/*
 * cmc_race.c: the race runtime for a node of cmc --model-file built with
 * ThreadSanitizer instrumentation (libcmc-race.a).
 *
 * Compile the program with -fsanitize=thread, and link it with this
 * archive instead of the sanitizer's own runtime:
 *
 *   cc -O1 -fno-pie -fsanitize=thread -c prog.c
 *   cc -no-pie -o prog prog.o libcmc-race.a -lpthread
 *
 * The compiler then calls into this file on every memory access and
 * atomic operation of the instrumented code.  Under cmc the runtime:
 *
 *   - announces itself to the checker at __tsan_init, so a node that ran
 *     more than one thread is known to have been checked for races;
 *   - keeps a vector clock per thread and per synchronisation object,
 *     with happens-before edges from thread creation and join, mutex,
 *     rwlock, spinlock, semaphore, condition variable, barrier and once
 *     calls (pthread and C11), and atomics by their memory order;
 *   - remembers, for each byte the program touches, its last write and
 *     the reads since it, and reports two accesses from different
 *     threads, at least one a write and not both atomic, that no
 *     happens-before edge orders: a data race;
 *   - makes each atomic operation a choice point for the search, as the
 *     preload does for each lock (a marker call the checker answers);
 *   - tells the checker the first time an atomic weaker than seq_cst
 *     runs, since the search explores a sequentially consistent machine
 *     only.
 *
 * Why this is enough: in a program free of data races, every execution
 * is equivalent to one that switches threads only at synchronisation
 * operations.  The search explores every order of those, and on each
 * explored path this runtime checks that the program has no race.  A
 * race on any path is reported; if none is found, the orders searched
 * cover the program's executions (on a sequentially consistent machine).
 *
 * Outside cmc (no CMC_STATE_FD in the environment) every entry point
 * returns at once, so the same binary runs as an ordinary program.
 *
 * Limits.  Code not compiled with the instrumentation (other libraries,
 * the C library) is not checked.  With gcc, memcpy, memset and memmove
 * calls are plain library calls and are not checked (clang reports them
 * through __tsan_memcpy and friends).  A synchronisation edge the
 * runtime does not model (a pipe, a signal, a raw futex) is missing, so
 * accesses it orders are reported as a race: a false alarm, never a
 * missed race.  pthread_exit is not followed: a thread must return from
 * its start function for its join to carry its writes.
 *
 * The runtime allocates nothing from the program's heap.  Its tables are
 * mapped at fixed addresses at start-up, so the program's own memory
 * layout is the same with and without it, and the same on every replay.
 *
 * The functions below have the instrumentation's names and prototypes,
 * so they are declared here rather than in a header.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <threads.h>
#include <unistd.h>

/* The marker the checker answers: sched_yield(CMC_RACE_MAGIC, op, a, b,
 * c).  cmc_proc.c has the same values. */
#define CMC_RACE_MAGIC 0x434d4352	/* "CMCR" */
enum
{
	RACE_HELLO = 1,				/* the runtime is linked in this node */
	RACE_ATOMIC = 2,			/* an atomic operation: a choice point */
	RACE_WEAK = 3,				/* an atomic weaker than seq_cst ran */
	RACE_REPORT = 4,			/* a data race: a = this pc, b = the other, c = address */
	RACE_FULL = 5				/* a table filled up: the check cannot go on */
};

#define MAXT 8					/* threads per node, as the checker caps them */

typedef uint32_t Clock[MAXT];

/* One byte of program memory: its last write and the reads since. */
typedef struct
{
	uintptr_t	addr;			/* 0: an empty slot */
	uintptr_t	wpc;
	uintptr_t	rpc[MAXT];
	uint32_t	wclk;			/* 0: never written */
	uint32_t	rclk[MAXT];		/* 0: not read by that thread since the write */
	uint8_t		wtid;
	uint8_t		watomic;
	uint8_t		ratomic;		/* bit t: thread t's read was atomic */
} Cell;

/* One synchronisation object (a mutex, an atomic's address): the clock
 * a release left in it. */
typedef struct
{
	uintptr_t	addr;
	Clock		vc;
} Sync;

#define CELLS (1u << 20)
#define SYNCS (1u << 14)
#define CELL_BASE ((void *) 0x580000000000ULL)
#define SYNC_BASE ((void *) 0x590000000000ULL)

static Cell *cells;
static Sync *syncs;
static int	active;				/* under cmc, and the tables are mapped */
static int	reported;			/* a race was reported: stop checking */
static int	weak_told;
static Clock tvc[MAXT];			/* each thread's vector clock */
static Clock exit_vc[MAXT];		/* a thread's clock when it returned */
static atomic_int nthreads = 1;
static _Thread_local int self = -1;	/* this thread's index; main is 0 */

static void
marker(int op, uintptr_t a, uintptr_t b, uintptr_t c)
{
	(void) syscall(SYS_sched_yield, CMC_RACE_MAGIC, op, a, b, c);
}

/* Map a table at its fixed address.  MAP_FIXED_NOREPLACE fails rather
 * than overwrite a mapping the program already has there. */
static void *
map_table(void *at, size_t bytes)
{
	void	   *p = mmap(at, bytes, PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);

	return p == at ? p : NULL;
}

void		__tsan_init(void);

void
__tsan_init(void)
{
	if (active || getenv("CMC_STATE_FD") == NULL)
		return;
	cells = map_table(CELL_BASE, CELLS * sizeof(Cell));
	syncs = map_table(SYNC_BASE, SYNCS * sizeof(Sync));
	if (cells == NULL || syncs == NULL)
	{
		marker(RACE_FULL, 0, 0, 0);
		return;
	}
	self = 0;
	tvc[0][0] = 1;
	active = 1;
	marker(RACE_HELLO, 0, 0, 0);
}

/* This thread's index, or -1 when the runtime is off.  A thread the
 * runtime did not see created (one made by a call it does not wrap) gets
 * a fresh index with an empty clock: nothing orders its accesses, so a
 * shared access is reported, never missed. */
static int
me(void)
{
	if (!active)
		return -1;
	if (self < 0)
	{
		int			t = atomic_fetch_add(&nthreads, 1);

		if (t >= MAXT)
		{
			marker(RACE_FULL, 1, 0, 0);
			active = 0;
			return -1;
		}
		self = t;
		tvc[t][t] = 1;
	}
	return self;
}

static void
vc_join(Clock into, const Clock from)
{
	for (int k = 0; k < MAXT; k++)
		if (from[k] > into[k])
			into[k] = from[k];
}

/* ---- synchronisation objects -------------------------------------- */

static Sync *
sync_of(const volatile void *obj)
{
	uintptr_t	a = (uintptr_t) obj;
	uint32_t	h = (uint32_t) ((a >> 3) * 0x9e3779b1u) & (SYNCS - 1);

	for (uint32_t i = 0; i < SYNCS; i++)
	{
		Sync	   *s = &syncs[(h + i) & (SYNCS - 1)];

		if (s->addr == a)
			return s;
		if (s->addr == 0)
		{
			s->addr = a;
			return s;
		}
	}
	marker(RACE_FULL, 2, 0, 0);
	active = 0;
	return NULL;
}

/* a release: the object takes this thread's clock, which then ticks */
static void
release(const volatile void *obj)
{
	int			t = me();
	Sync	   *s;

	if (t < 0 || (s = sync_of(obj)) == NULL)
		return;
	vc_join(s->vc, tvc[t]);
	tvc[t][t]++;
}

/* an acquire: this thread takes the clock the object holds */
static void
acquire(const volatile void *obj)
{
	int			t = me();
	Sync	   *s;

	if (t < 0 || (s = sync_of(obj)) == NULL)
		return;
	vc_join(tvc[t], s->vc);
}

/* ---- memory accesses ---------------------------------------------- */

static Cell *
cell_of(uintptr_t a)
{
	uint32_t	h = (uint32_t) (a * 0x9e3779b1u) & (CELLS - 1);

	for (uint32_t i = 0; i < CELLS; i++)
	{
		Cell	   *c = &cells[(h + i) & (CELLS - 1)];

		if (c->addr == a)
			return c;
		if (c->addr == 0)
		{
			c->addr = a;
			return c;
		}
	}
	marker(RACE_FULL, 3, 0, 0);
	active = 0;
	return NULL;
}

static void
race(uintptr_t pc, uintptr_t other, uintptr_t a)
{
	reported = 1;
	marker(RACE_REPORT, pc, other, a);
}

/* Thread t accesses byte a.  A prior access races with this one when it
 * came from another thread, is not ordered before this one (its clock
 * entry is ahead of what t has seen of that thread), and is not, with
 * this one, an atomic pair; for a read, only prior writes count. */
static void
access_byte(int t, uintptr_t a, int is_write, int is_atomic, uintptr_t pc)
{
	Cell	   *c = cell_of(a);

	if (c == NULL)
		return;
	if (c->wclk && c->wtid != t && c->wclk > tvc[t][c->wtid] && !(c->watomic && is_atomic))
	{
		race(pc, c->wpc, a);
		return;
	}
	if (is_write)
	{
		for (int u = 0; u < MAXT; u++)
			if (u != t && c->rclk[u] && c->rclk[u] > tvc[t][u] &&
				!(((c->ratomic >> u) & 1) && is_atomic))
			{
				race(pc, c->rpc[u], a);
				return;
			}
		memset(c->rclk, 0, sizeof c->rclk);
		c->ratomic = 0;
		c->wtid = (uint8_t) t;
		c->wclk = tvc[t][t];
		c->wpc = pc;
		c->watomic = (uint8_t) is_atomic;
	}
	else
	{
		c->rclk[t] = tvc[t][t];
		c->rpc[t] = pc;
		if (is_atomic)
			c->ratomic |= (uint8_t) (1u << t);
		else
			c->ratomic &= (uint8_t) ~(1u << t);
	}
}

static void
access_range(const void *p, size_t n, int is_write, int is_atomic, uintptr_t pc)
{
	int			t;

	if (!active || reported || (t = me()) < 0)
		return;
	for (size_t i = 0; i < n && !reported && active; i++)
		access_byte(t, (uintptr_t) p + i, is_write, is_atomic, pc);
}

#define PC() ((uintptr_t) __builtin_return_address(0))

#define ACCESS(n) \
void __tsan_read##n(void *p); \
void __tsan_write##n(void *p); \
void __tsan_unaligned_read##n(void *p); \
void __tsan_unaligned_write##n(void *p); \
void __tsan_read##n(void *p) { access_range(p, n, 0, 0, PC()); } \
void __tsan_write##n(void *p) { access_range(p, n, 1, 0, PC()); } \
void __tsan_unaligned_read##n(void *p) { access_range(p, n, 0, 0, PC()); } \
void __tsan_unaligned_write##n(void *p) { access_range(p, n, 1, 0, PC()); }

ACCESS(1)
ACCESS(2)
ACCESS(4)
ACCESS(8)
ACCESS(16)

void		__tsan_read_range(void *p, unsigned long n);
void		__tsan_write_range(void *p, unsigned long n);
void		__tsan_func_entry(void *pc);
void		__tsan_func_exit(void);
void		__tsan_vptr_read(void **p);
void		__tsan_vptr_update(void **p, void *v);
void	   *__tsan_memcpy(void *d, const void *s, unsigned long n);
void	   *__tsan_memmove(void *d, const void *s, unsigned long n);
void	   *__tsan_memset(void *d, int c, unsigned long n);

void
__tsan_read_range(void *p, unsigned long n)
{
	access_range(p, n, 0, 0, PC());
}

void
__tsan_write_range(void *p, unsigned long n)
{
	access_range(p, n, 1, 0, PC());
}

void
__tsan_func_entry(void *pc)
{
	(void) pc;
}

void
__tsan_func_exit(void)
{
}

void
__tsan_vptr_read(void **p)
{
	access_range(p, sizeof *p, 0, 0, PC());
}

void
__tsan_vptr_update(void **p, void *v)
{
	(void) v;
	access_range(p, sizeof *p, 1, 0, PC());
}

void *
__tsan_memcpy(void *d, const void *s, unsigned long n)
{
	access_range(s, n, 0, 0, PC());
	access_range(d, n, 1, 0, PC());
	for (unsigned long i = 0; i < n; i++)
		((volatile unsigned char *) d)[i] = ((const unsigned char *) s)[i];
	return d;
}

void *
__tsan_memmove(void *d, const void *s, unsigned long n)
{
	access_range(s, n, 0, 0, PC());
	access_range(d, n, 1, 0, PC());
	if ((uintptr_t) d < (uintptr_t) s)
		for (unsigned long i = 0; i < n; i++)
			((volatile unsigned char *) d)[i] = ((const unsigned char *) s)[i];
	else
		for (unsigned long i = n; i > 0; i--)
			((volatile unsigned char *) d)[i - 1] = ((const unsigned char *) s)[i - 1];
	return d;
}

void *
__tsan_memset(void *d, int c, unsigned long n)
{
	access_range(d, n, 1, 0, PC());
	for (unsigned long i = 0; i < n; i++)
		((volatile unsigned char *) d)[i] = (unsigned char) c;
	return d;
}

/* ---- atomics -------------------------------------------------------- */

/*
 * The memory-order argument follows the instrumentation's numbering:
 * 0 relaxed, 1 consume, 2 acquire, 3 release, 4 acq_rel, 5 seq_cst.
 * Every operation runs as seq_cst (the search's machine is sequentially
 * consistent); an order weaker than that is reported once, so a pass
 * that relied on it is not counted as exhaustive.
 */
enum { MO_RELAXED, MO_CONSUME, MO_ACQUIRE, MO_RELEASE, MO_ACQ_REL, MO_SEQ_CST };

/* Before an atomic access of n bytes at p with order mo: a choice point,
 * then the race check of the access itself (an atomic does not race
 * with another atomic), then the acquire half of its order. */
static void
atomic_pre(const volatile void *p, size_t n, int mo, int is_write, uintptr_t pc)
{
	if (!active || me() < 0)
		return;
	if (mo != MO_SEQ_CST && !weak_told)
	{
		weak_told = 1;
		marker(RACE_WEAK, pc, (uintptr_t) p, (uintptr_t) mo);
	}
	marker(RACE_ATOMIC, pc, (uintptr_t) p, 0);
	access_range((const void *) p, n, is_write, 1, pc);
	if (mo == MO_ACQUIRE || mo == MO_CONSUME || mo == MO_ACQ_REL || mo == MO_SEQ_CST)
		acquire(p);
}

/* after an atomic write of p with order mo: its release half */
static void
atomic_post(const volatile void *p, int mo, int is_write)
{
	if (!active || !is_write)
		return;
	if (mo == MO_RELEASE || mo == MO_ACQ_REL || mo == MO_SEQ_CST)
		release(p);
}

#define ATOMICS(n, T) \
T __tsan_atomic##n##_load(const volatile T *a, int mo); \
void __tsan_atomic##n##_store(volatile T *a, T v, int mo); \
T __tsan_atomic##n##_exchange(volatile T *a, T v, int mo); \
T __tsan_atomic##n##_fetch_add(volatile T *a, T v, int mo); \
T __tsan_atomic##n##_fetch_sub(volatile T *a, T v, int mo); \
T __tsan_atomic##n##_fetch_and(volatile T *a, T v, int mo); \
T __tsan_atomic##n##_fetch_or(volatile T *a, T v, int mo); \
T __tsan_atomic##n##_fetch_xor(volatile T *a, T v, int mo); \
T __tsan_atomic##n##_fetch_nand(volatile T *a, T v, int mo); \
int __tsan_atomic##n##_compare_exchange_strong(volatile T *a, T *c, T v, int mo, int fmo); \
int __tsan_atomic##n##_compare_exchange_weak(volatile T *a, T *c, T v, int mo, int fmo); \
T __tsan_atomic##n##_compare_exchange_val(volatile T *a, T c, T v, int mo, int fmo); \
T __tsan_atomic##n##_load(const volatile T *a, int mo) \
{ \
	atomic_pre(a, sizeof(T), mo, 0, PC()); \
	return __atomic_load_n(a, __ATOMIC_SEQ_CST); \
} \
void __tsan_atomic##n##_store(volatile T *a, T v, int mo) \
{ \
	atomic_pre(a, sizeof(T), mo, 1, PC()); \
	__atomic_store_n(a, v, __ATOMIC_SEQ_CST); \
	atomic_post(a, mo, 1); \
} \
T __tsan_atomic##n##_exchange(volatile T *a, T v, int mo) \
{ \
	atomic_pre(a, sizeof(T), mo, 1, PC()); \
	T r = __atomic_exchange_n(a, v, __ATOMIC_SEQ_CST); \
	atomic_post(a, mo, 1); \
	return r; \
} \
ATOMIC_RMW(n, T, fetch_add) \
ATOMIC_RMW(n, T, fetch_sub) \
ATOMIC_RMW(n, T, fetch_and) \
ATOMIC_RMW(n, T, fetch_or) \
ATOMIC_RMW(n, T, fetch_xor) \
ATOMIC_RMW(n, T, fetch_nand) \
int __tsan_atomic##n##_compare_exchange_strong(volatile T *a, T *c, T v, int mo, int fmo) \
{ \
	(void) fmo; \
	atomic_pre(a, sizeof(T), mo, 1, PC()); \
	int ok = __atomic_compare_exchange_n(a, c, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	atomic_post(a, mo, ok); \
	return ok; \
} \
int __tsan_atomic##n##_compare_exchange_weak(volatile T *a, T *c, T v, int mo, int fmo) \
{ \
	(void) fmo; \
	atomic_pre(a, sizeof(T), mo, 1, PC()); \
	int ok = __atomic_compare_exchange_n(a, c, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	atomic_post(a, mo, ok); \
	return ok; \
} \
T __tsan_atomic##n##_compare_exchange_val(volatile T *a, T c, T v, int mo, int fmo) \
{ \
	(void) fmo; \
	atomic_pre(a, sizeof(T), mo, 1, PC()); \
	T old = c; \
	int ok = __atomic_compare_exchange_n(a, &old, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
	atomic_post(a, mo, ok); \
	return old; \
}

#define ATOMIC_RMW(n, T, op) \
T __tsan_atomic##n##_##op(volatile T *a, T v, int mo) \
{ \
	atomic_pre(a, sizeof(T), mo, 1, PC()); \
	T r = __atomic_##op(a, v, __ATOMIC_SEQ_CST); \
	atomic_post(a, mo, 1); \
	return r; \
}

ATOMICS(8, uint8_t)
ATOMICS(16, uint16_t)
ATOMICS(32, uint32_t)
ATOMICS(64, uint64_t)

/*
 * 16-byte atomics, which a program built with -mcx16 uses (a pointer and
 * its generation count, say).  This file is built without -mcx16, where
 * the __atomic builtins on 16 bytes would call libatomic, which a node
 * need not link; so every operation is a loop on cmpxchg16b, which the
 * cx16 target attribute lets the compiler emit.  A load is a
 * compare-exchange of 0 with 0, which writes nothing visible, as
 * libatomic's own 16-byte load does on x86-64.
 */
#if defined(__x86_64__) && defined(__SIZEOF_INT128__)
typedef unsigned __int128 u128;

/* noinline: clang drops the target attribute when it inlines the
 * function, and then calls libatomic after all */
__attribute__((target("cx16"), noinline)) static u128
cas128(volatile u128 *a, u128 expect, u128 v)
{
	return __sync_val_compare_and_swap(a, expect, v);
}

/* the operations of the fetch_ entry points, by name */
static u128 op_add(u128 x, u128 v) { return x + v; }
static u128 op_sub(u128 x, u128 v) { return x - v; }
static u128 op_and(u128 x, u128 v) { return x & v; }
static u128 op_or(u128 x, u128 v) { return x | v; }
static u128 op_xor(u128 x, u128 v) { return x ^ v; }
static u128 op_nand(u128 x, u128 v) { return ~(x & v); }
static u128 op_set(u128 x, u128 v) { (void) x; return v; }

/* replace *a by op(*a, v) atomically; the old value */
static u128
rmw128(volatile u128 *a, u128 v, u128 (*op) (u128, u128))
{
	u128		old = cas128(a, 0, 0);

	for (;;)
	{
		u128		seen = cas128(a, old, op(old, v));

		if (seen == old)
			return old;
		old = seen;
	}
}

u128		__tsan_atomic128_load(const volatile u128 *a, int mo);
void		__tsan_atomic128_store(volatile u128 *a, u128 v, int mo);
u128		__tsan_atomic128_exchange(volatile u128 *a, u128 v, int mo);
u128		__tsan_atomic128_fetch_add(volatile u128 *a, u128 v, int mo);
u128		__tsan_atomic128_fetch_sub(volatile u128 *a, u128 v, int mo);
u128		__tsan_atomic128_fetch_and(volatile u128 *a, u128 v, int mo);
u128		__tsan_atomic128_fetch_or(volatile u128 *a, u128 v, int mo);
u128		__tsan_atomic128_fetch_xor(volatile u128 *a, u128 v, int mo);
u128		__tsan_atomic128_fetch_nand(volatile u128 *a, u128 v, int mo);
int			__tsan_atomic128_compare_exchange_strong(volatile u128 *a, u128 *c, u128 v, int mo, int fmo);
int			__tsan_atomic128_compare_exchange_weak(volatile u128 *a, u128 *c, u128 v, int mo, int fmo);
u128		__tsan_atomic128_compare_exchange_val(volatile u128 *a, u128 c, u128 v, int mo, int fmo);

u128
__tsan_atomic128_load(const volatile u128 *a, int mo)
{
	atomic_pre(a, sizeof(u128), mo, 0, PC());
	return cas128((volatile u128 *) a, 0, 0);
}

void
__tsan_atomic128_store(volatile u128 *a, u128 v, int mo)
{
	atomic_pre(a, sizeof(u128), mo, 1, PC());
	(void) rmw128(a, v, op_set);
	atomic_post(a, mo, 1);
}

#define RMW128(name, op) \
u128 \
__tsan_atomic128_##name(volatile u128 *a, u128 v, int mo) \
{ \
	atomic_pre(a, sizeof(u128), mo, 1, PC()); \
	u128 r = rmw128(a, v, op); \
	atomic_post(a, mo, 1); \
	return r; \
}

RMW128(exchange, op_set)
RMW128(fetch_add, op_add)
RMW128(fetch_sub, op_sub)
RMW128(fetch_and, op_and)
RMW128(fetch_or, op_or)
RMW128(fetch_xor, op_xor)
RMW128(fetch_nand, op_nand)

int
__tsan_atomic128_compare_exchange_strong(volatile u128 *a, u128 *c, u128 v, int mo, int fmo)
{
	(void) fmo;
	atomic_pre(a, sizeof(u128), mo, 1, PC());
	u128		old = cas128(a, *c, v);
	int			ok = old == *c;

	*c = old;
	atomic_post(a, mo, ok);
	return ok;
}

int
__tsan_atomic128_compare_exchange_weak(volatile u128 *a, u128 *c, u128 v, int mo, int fmo)
{
	(void) fmo;
	atomic_pre(a, sizeof(u128), mo, 1, PC());
	u128		old = cas128(a, *c, v);
	int			ok = old == *c;

	*c = old;
	atomic_post(a, mo, ok);
	return ok;
}

u128
__tsan_atomic128_compare_exchange_val(volatile u128 *a, u128 c, u128 v, int mo, int fmo)
{
	(void) fmo;
	atomic_pre(a, sizeof(u128), mo, 1, PC());
	u128		old = cas128(a, c, v);

	atomic_post(a, mo, old == c);
	return old;
}
#endif

void		__tsan_atomic_thread_fence(int mo);
void		__tsan_atomic_signal_fence(int mo);

/* A fence orders only weaker atomics, which are reported as weak; a
 * seq_cst fence adds nothing to a sequentially consistent machine. */
void
__tsan_atomic_thread_fence(int mo)
{
	if (active && mo != MO_SEQ_CST && !weak_told)
	{
		weak_told = 1;
		marker(RACE_WEAK, PC(), 0, (uintptr_t) mo);
	}
}

void
__tsan_atomic_signal_fence(int mo)
{
	(void) mo;
}

/* ---- synchronisation calls ------------------------------------------ */

/*
 * These replace the C library's functions in the program (the archive is
 * linked into the executable, which comes first in symbol lookup).  Each
 * calls the next definition, the preload's (whose marker makes an
 * acquisition a choice point) or else the C library's, and adds the
 * happens-before edge of the call.
 */
static void *
next_fn(void **slot, const char *name)
{
	if (*slot == NULL)
		*slot = dlsym(RTLD_NEXT, name);
	if (*slot == NULL)
	{
		static const char msg[] = "cmc-race: a thread function is missing from the C library\n";

		(void) !write(2, msg, sizeof msg - 1);
		_exit(2);
	}
	return *slot;
}

#define NEXT(ret, name, params) \
	static void *real_##name; \
	ret (*fn) params; \
	*(void **) &fn = next_fn(&real_##name, #name)

/* an acquisition: the call, then on success the object's clock */
#define ACQ_WRAP(name, T, extra_params, extra_args) \
int name(T *o extra_params) \
{ \
	NEXT(int, name, (T *o extra_params)); \
	int r = fn(o extra_args); \
	if (r == 0) acquire(o); \
	return r; \
}

/* a release: this thread's clock into the object, then the call */
#define REL_WRAP(name, T) \
int name(T *o) \
{ \
	NEXT(int, name, (T *o)); \
	release(o); \
	return fn(o); \
}

#define COMMA ,
ACQ_WRAP(pthread_mutex_lock, pthread_mutex_t, , )
ACQ_WRAP(pthread_mutex_trylock, pthread_mutex_t, , )
ACQ_WRAP(pthread_mutex_timedlock, pthread_mutex_t, COMMA const struct timespec *t, COMMA t)
REL_WRAP(pthread_mutex_unlock, pthread_mutex_t)
ACQ_WRAP(pthread_rwlock_rdlock, pthread_rwlock_t, , )
ACQ_WRAP(pthread_rwlock_tryrdlock, pthread_rwlock_t, , )
ACQ_WRAP(pthread_rwlock_timedrdlock, pthread_rwlock_t, COMMA const struct timespec *t, COMMA t)
ACQ_WRAP(pthread_rwlock_wrlock, pthread_rwlock_t, , )
ACQ_WRAP(pthread_rwlock_trywrlock, pthread_rwlock_t, , )
ACQ_WRAP(pthread_rwlock_timedwrlock, pthread_rwlock_t, COMMA const struct timespec *t, COMMA t)
REL_WRAP(pthread_rwlock_unlock, pthread_rwlock_t)
ACQ_WRAP(pthread_spin_lock, pthread_spinlock_t, , )	/* GCOVR_EXCL_LINE: glibc's spin lock always returns 0 */
ACQ_WRAP(pthread_spin_trylock, pthread_spinlock_t, , )
REL_WRAP(pthread_spin_unlock, pthread_spinlock_t)
ACQ_WRAP(sem_wait, sem_t, , )
ACQ_WRAP(sem_trywait, sem_t, , )
ACQ_WRAP(sem_timedwait, sem_t, COMMA const struct timespec *t, COMMA t)
REL_WRAP(sem_post, sem_t)
REL_WRAP(pthread_cond_signal, pthread_cond_t)
REL_WRAP(pthread_cond_broadcast, pthread_cond_t)
ACQ_WRAP(mtx_lock, mtx_t, , )
ACQ_WRAP(mtx_trylock, mtx_t, , )
ACQ_WRAP(mtx_timedlock, mtx_t, COMMA const struct timespec *t, COMMA t)
REL_WRAP(mtx_unlock, mtx_t)
REL_WRAP(cnd_signal, cnd_t)
REL_WRAP(cnd_broadcast, cnd_t)

/* A condition wait releases the mutex, waits, and reacquires it, inside
 * the C library; the edges are the mutex's release before it and, after
 * it, the mutex and the condition variable's acquire. */
int
pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	NEXT(int, pthread_cond_wait, (pthread_cond_t *, pthread_mutex_t *));
	release(m);
	int			r = fn(c, m);

	acquire(m);
	acquire(c);
	return r;
}

int
pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t)
{
	NEXT(int, pthread_cond_timedwait, (pthread_cond_t *, pthread_mutex_t *, const struct timespec *));
	release(m);
	int			r = fn(c, m, t);

	acquire(m);
	acquire(c);
	return r;
}

int
cnd_wait(cnd_t *c, mtx_t *m)
{
	NEXT(int, cnd_wait, (cnd_t *, mtx_t *));
	release(m);
	int			r = fn(c, m);

	acquire(m);
	acquire(c);
	return r;
}

int
cnd_timedwait(cnd_t *restrict c, mtx_t *restrict m, const struct timespec *restrict t)
{
	NEXT(int, cnd_timedwait, (cnd_t *, mtx_t *, const struct timespec *));
	release(m);
	int			r = fn(c, m, t);

	acquire(m);
	acquire(c);
	return r;
}

/* every thread releases into the barrier before waiting, and none
 * returns until all have, so each acquires every arrival's clock */
int
pthread_barrier_wait(pthread_barrier_t *b)
{
	NEXT(int, pthread_barrier_wait, (pthread_barrier_t *));
	release(b);
	int			r = fn(b);

	acquire(b);
	return r;
}

/* once: the initialiser's completion releases, every caller acquires */
static _Thread_local void (*once_init)(void);
static _Thread_local const volatile void *once_obj;

static void
once_tramp(void)
{
	once_init();
	release(once_obj);
}

int
pthread_once(pthread_once_t *o, void (*init)(void))
{
	NEXT(int, pthread_once, (pthread_once_t *, void (*)(void)));
	once_init = init;
	once_obj = o;
	int			r = fn(o, once_tramp);

	acquire(o);
	return r;
}

void
call_once(once_flag *o, void (*init)(void))
{
	NEXT(void, call_once, (once_flag *, void (*)(void)));
	once_init = init;
	once_obj = o;
	fn(o, once_tramp);
	acquire(o);
}

/* ---- thread creation and join ---------------------------------------- */

typedef struct
{
	void	   *(*start) (void *);
	void	   *arg;
	int			t;
} Start;

/* one start record per thread index: no heap, and index t is the
 * thread's for its whole life */
static Start starts[MAXT];

static void *
thread_tramp(void *p)
{
	Start	   *s = p;

	self = s->t;
	void	   *r = s->start(s->arg);

	if (active)
		memcpy(exit_vc[s->t], tvc[s->t], sizeof(Clock));
	return r;
}

/* A new thread starts with its creator's clock, which then ticks.  The
 * pthread_t names the index, so a join knows whose exit clock to take. */
static pthread_t tid_of[MAXT];

int
pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*start) (void *), void *arg)
{
	NEXT(int, pthread_create, (pthread_t *, const pthread_attr_t *, void *(*)(void *), void *));
	int			p = me();

	if (p < 0)
		return fn(th, attr, start, arg);
	int			t = atomic_fetch_add(&nthreads, 1);

	if (t >= MAXT)
	{
		marker(RACE_FULL, 1, 0, 0);
		active = 0;
		return fn(th, attr, start, arg);
	}
	memcpy(tvc[t], tvc[p], sizeof(Clock));
	tvc[t][t] = 1;
	tvc[p][p]++;
	starts[t].start = start;
	starts[t].arg = arg;
	starts[t].t = t;
	int			r = fn(th, attr, thread_tramp, &starts[t]);

	if (r == 0)
		tid_of[t] = *th;
	return r;
}

int
pthread_join(pthread_t th, void **ret)
{
	NEXT(int, pthread_join, (pthread_t, void **));
	int			r = fn(th, ret);
	int			t = me();

	if (r == 0 && t >= 0)
		for (int k = 1; k < MAXT; k++)
			if (pthread_equal(tid_of[k], th))
				vc_join(tvc[t], exit_vc[k]);
	return r;
}

/* C11 threads: glibc's thrd_create and thrd_join call its pthread
 * functions directly, not through the PLT, so they are wrapped too */
typedef struct
{
	thrd_start_t start;
	void	   *arg;
	int			t;
} Cstart;

static Cstart cstarts[MAXT];

static int
cthread_tramp(void *p)
{
	Cstart	   *s = p;

	self = s->t;
	int			r = s->start(s->arg);

	if (active)
		memcpy(exit_vc[s->t], tvc[s->t], sizeof(Clock));
	return r;
}

int
thrd_create(thrd_t *th, thrd_start_t start, void *arg)
{
	NEXT(int, thrd_create, (thrd_t *, thrd_start_t, void *));
	int			p = me();

	if (p < 0)
		return fn(th, start, arg);
	int			t = atomic_fetch_add(&nthreads, 1);

	if (t >= MAXT)
	{
		marker(RACE_FULL, 1, 0, 0);
		active = 0;
		return fn(th, start, arg);
	}
	memcpy(tvc[t], tvc[p], sizeof(Clock));
	tvc[t][t] = 1;
	tvc[p][p]++;
	cstarts[t].start = start;
	cstarts[t].arg = arg;
	cstarts[t].t = t;
	int			r = fn(th, cthread_tramp, &cstarts[t]);

	if (r == thrd_success)
		tid_of[t] = (pthread_t) *th;
	return r;
}

int
thrd_join(thrd_t th, int *ret)
{
	NEXT(int, thrd_join, (thrd_t, int *));
	int			r = fn(th, ret);
	int			t = me();

	if (r == thrd_success && t >= 0)
		for (int k = 1; k < MAXT; k++)
			if (pthread_equal(tid_of[k], (pthread_t) th))
				vc_join(tvc[t], exit_vc[k]);
	return r;
}
