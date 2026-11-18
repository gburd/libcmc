/*
 * cmc_preload.c: the library every node of cmc --model-file runs under,
 * LD_PRELOADed by the process front end (cmc_proc.c).  It makes a node's
 * run a pure function of the actions the checker chose:
 *
 *   - a deterministic bump allocator: malloc/calloc/realloc and the
 *     aligned allocators (posix_memalign/aligned_alloc/memalign) carve
 *     sequentially from an arena mapped MAP_FIXED at a fixed base, so the
 *     Nth allocation lands at the same address every run, whatever ASLR
 *     does.  free() is a no-op: a bounded run never needs the memory
 *     back, and a freelist would reintroduce order effects.  The arena is
 *     64 MiB by default; CMC_ARENA_MB overrides it for a larger target.
 *   - clock_gettime, gettimeofday, time and getrandom through a real
 *     syscall.  glibc serves the clock from the vDSO, which seccomp never
 *     sees; as syscalls, the checker answers them with its logical clock
 *     (it advances only when the search fires a timeout) and a fixed
 *     random pattern, so a replay reproduces the same times.
 *
 * Its constructor announces it to the checker with a clock_gettime on
 * the clock id CMC_PRELOAD_MAGIC, which no real clock uses; a node that
 * never announces it is refused, rather than checked against the real
 * clock.
 *
 *   - the pthread synchronisation calls (mutex, rwlock, spinlock,
 *     condition variable, semaphore, barrier, once) each make a marker
 *     call first, a sched_yield whose first argument is CMC_SYNC_MAGIC,
 *     before the real function runs.  The checker answers it without the
 *     kernel, and in a node with more than one thread it is a choice
 *     point: the search picks which thread runs next.  So a lock taken
 *     on its uncontended fast path, which makes no futex call, is still
 *     an ordering point the search explores.
 *
 * The functions below replace libc's, so they take libc's prototypes.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <threads.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ARENA_BASE ((void *) 0x600000000000ULL)
#define ARENA_MB_DEFAULT 64UL
#define ARENA_MB_MAX     4096UL		/* a sane ceiling; the base leaves room */
#define ALIGN      16UL

/* exported at fixed, nm-findable symbols */
extern void *cmc_arena;
extern size_t cmc_arena_bump, cmc_arena_size;
void	   *cmc_arena = ARENA_BASE;
size_t		cmc_arena_bump = 0;
size_t		cmc_arena_size = ARENA_MB_DEFAULT * 1024 * 1024;

static int	ready;

/* the arena size in bytes, from CMC_ARENA_MB (MiB) or the default.  A
 * value out of range, or non-numeric, falls back to the default: a
 * target should not fail to check over a mistyped tuning knob. */
static size_t
arena_bytes(void)
{
	const char *mb = getenv("CMC_ARENA_MB");
	if (mb && *mb)
	{
		char	   *e;
		unsigned long	v = strtoul(mb, &e, 10);
		if (!*e && v >= 1 && v <= ARENA_MB_MAX)
			return (size_t) v * 1024 * 1024;
	}
	return ARENA_MB_DEFAULT * 1024 * 1024;
}

static void
arena_init(void)
{
	if (ready)
		return;
	cmc_arena_size = arena_bytes();
	void	   *p = mmap(ARENA_BASE, cmc_arena_size, PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p != ARENA_BASE)
	{
		static const char msg[] = "cmc-preload: cannot map the arena at its fixed base\n";

		(void) !write(2, msg, sizeof msg - 1);
		_exit(97);
	}
	ready = 1;
}

#define CMC_PRELOAD_MAGIC 0x434d43	/* "CMC"; cmc_proc.c has the same */

__attribute__((constructor))
static void
ctor(void)
{
	struct timespec ts;

	arena_init();
	(void) syscall(SYS_clock_gettime, CMC_PRELOAD_MAGIC, &ts);
}

/* carve n bytes aligned to `align` (a power of two, at least ALIGN),
 * preceded by an ALIGN-byte header that records n, so realloc knows how
 * much of the old block to copy.  The arena base is 2 MiB aligned, so an
 * aligned offset yields an aligned pointer.  Returns NULL with errno
 * ENOMEM when the request does not fit; every sum is checked, so a huge
 * n cannot wrap around and hand out memory already in use. */
static void *
bump_aligned(size_t align, size_t n)
{
	arena_init();
	if (align < ALIGN)
		align = ALIGN;
	size_t		used = cmc_arena_bump + ALIGN;
	if (used > cmc_arena_size || align - 1 > cmc_arena_size - used)
		goto nomem;
	size_t		off = (used + (align - 1)) & ~(align - 1);
	if (n > cmc_arena_size - off)
		goto nomem;
	cmc_arena_bump = off + n;
	*(size_t *) ((unsigned char *) ARENA_BASE + off - ALIGN) = n;
	return (unsigned char *) ARENA_BASE + off;
nomem:
	errno = ENOMEM;
	return NULL;
}

static void *
bump(size_t n)
{
	return bump_aligned(ALIGN, n);
}

/* the size recorded for a block bump_aligned returned.  A pointer from
 * anywhere else has no header to read, so it ends the node visibly
 * rather than copying a guessed length. */
static size_t
block_size(const void *p)
{
	const unsigned char *u = p, *base = ARENA_BASE;

	if (u < base + ALIGN || u > base + cmc_arena_bump)
	{
		static const char msg[] = "cmc-preload: realloc of a block the arena did not allocate\n";

		(void) !write(2, msg, sizeof msg - 1);
		_exit(97);
	}
	return *(const size_t *) (u - ALIGN);
}

void *
malloc(size_t n)
{
	return bump(n ? n : 1);
}

void
free(void *p)
{
	(void) p;					/* no-op: see the header comment */
}

void *
calloc(size_t a, size_t b)
{
	if (b != 0 && a > SIZE_MAX / b)
	{
		errno = ENOMEM;
		return NULL;
	}
	size_t		n = a * b;
	void	   *p = bump(n ? n : 1);
	/* the arena is freshly mmap'd zero and never reused, so it is already
	 * zero; memset anyway to honour the contract exactly */
	if (p)
		memset(p, 0, n);
	return p;
}

void *
realloc(void *old, size_t n)
{
	void	   *p = bump(n ? n : 1);
	if (p && old)
	{
		size_t		k = block_size(old);

		memcpy(p, old, k < n ? k : n);
	}
	return p;
}

/* Aligned allocation from the arena, so a target that aligns buffers
 * (an O_DIRECT path, a SIMD buffer) still gets a fixed, replayable
 * address rather than one from glibc's ASLR'd heap.  alignment must be
 * a power of two; posix_memalign also requires a multiple of
 * sizeof(void *). */
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
		return EINVAL;
	void	   *p = bump_aligned(alignment, size ? size : 1);
	if (!p)
		return ENOMEM;
	*memptr = p;
	return 0;
}

void *
aligned_alloc(size_t alignment, size_t size)
{
	if (alignment == 0 || (alignment & (alignment - 1)) != 0)
		return NULL;
	return bump_aligned(alignment, size ? size : 1);
}

void *
memalign(size_t alignment, size_t size)
{
	if (alignment == 0 || (alignment & (alignment - 1)) != 0)
		return NULL;
	return bump_aligned(alignment, size ? size : 1);
}

/*
 * M5b -- absolute-time and randomness shims.  glibc serves clock_gettime
 * and gettimeofday from the vDSO, so seccomp never sees them; forcing
 * them through a real syscall() makes them visible to the checker, which
 * answers with a LOGICAL clock it advances only at timeout events (so
 * stateless replay stays deterministic).  getrandom is routed the same
 * way so the value a target reads is engine-chosen, not real entropy.
 * These override the libc symbols because this .so is LD_PRELOAD'd first.
 */
int
clock_gettime(clockid_t id, struct timespec *ts)
{
	return syscall(SYS_clock_gettime, id, ts);
}

int
gettimeofday(struct timeval *restrict tv, void *restrict tz)
{
	return syscall(SYS_gettimeofday, tv, tz);
}

/* time() reads the same logical clock as clock_gettime, so a target that
 * seeds from time(NULL) at startup is deterministic rather than taking
 * the wall clock.  glibc's time() reads the vDSO, which seccomp never
 * sees, so it must be overridden here and routed through clock_gettime. */
time_t
time(time_t *tloc)
{
	struct timespec ts = {0};
	if (syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts) != 0)	/* GCOVR_EXCL_LINE: ts is on the stack, so this cannot fail */
		return (time_t) -1;
	if (tloc)
		*tloc = ts.tv_sec;
	return ts.tv_sec;
}

ssize_t
getrandom(void *buf, size_t len, unsigned int flags)
{
	return syscall(SYS_getrandom, buf, len, flags);
}

/*
 * Synchronisation calls as choice points.
 *
 * Only acquisitions are wrapped: a lock, a semaphore wait, a barrier and
 * a once.  In a program free of data races a release (an unlock, a
 * signal, a post) moves left, past the accesses before it, and an
 * acquisition moves right, so every schedule is equivalent to one that
 * switches threads only before an acquisition (Lipton's reduction).  A
 * marker on releases too would multiply the schedules searched without
 * adding one.  A condition wait releases its mutex and reacquires it
 * when woken; the wake is a futex answer the search already orders, so
 * it has no marker of its own.  That the program is free of data races
 * is what the race runtime (cmc_race.c) checks; without it, a
 * multi-threaded node cannot pass (cmc(7)).
 *
 * Each wrapper makes the marker call and then calls the C library's own
 * function, found with dlsym(RTLD_NEXT) on first use.  The marker
 * carries the operation and the object's address, so a counterexample
 * names them.  Outside cmc the preload is not loaded at all, so nothing
 * here runs.  C11's mtx_lock, mtx_trylock and mtx_timedlock are wrapped
 * on their own: glibc's call __pthread_mutex_lock directly, not through
 * the PLT, so a pthread wrapper never sees them.
 *
 * A wrapper that runs before its real function is found (dlsym itself
 * may take a lock) goes straight to the marker and then the lookup;
 * dlsym does not call back into these wrappers.
 */
#define CMC_SYNC_MAGIC 0x434d4353	/* "CMCS"; cmc_proc.c has the same */

enum
{
	SYNC_MUTEX_LOCK = 1, SYNC_MUTEX_TRYLOCK, SYNC_MUTEX_TIMEDLOCK,
	SYNC_RW_RDLOCK, SYNC_RW_TRYRDLOCK, SYNC_RW_TIMEDRDLOCK, SYNC_RW_WRLOCK,
	SYNC_RW_TRYWRLOCK, SYNC_RW_TIMEDWRLOCK, SYNC_SPIN_LOCK, SYNC_SPIN_TRYLOCK,
	SYNC_SEM_WAIT, SYNC_SEM_TRYWAIT, SYNC_SEM_TIMEDWAIT, SYNC_BARRIER_WAIT,
	SYNC_ONCE, SYNC_MTX_LOCK, SYNC_MTX_TRYLOCK, SYNC_MTX_TIMEDLOCK, SYNC_CALL_ONCE
};

static void
sync_point(int op, const void *obj)
{
	(void) syscall(SYS_sched_yield, CMC_SYNC_MAGIC, op, obj);
}

/* the C library's function `name`, cached in *slot */
static void *
real_fn(void **slot, const char *name)
{
	if (*slot == NULL)
		*slot = dlsym(RTLD_NEXT, name);
	if (*slot == NULL)
	{
		static const char msg[] = "cmc-preload: a pthread function is missing from the C library\n";

		(void) !write(2, msg, sizeof msg - 1);
		_exit(2);
	}
	return *slot;
}

/* one wrapper: a marker, then the real call with the same arguments */
#define SYNC_WRAP(ret, name, op, params, args, obj) \
ret name params \
{ \
	static void *real; \
	ret (*fn) params; \
	sync_point(op, obj); \
	*(void **) &fn = real_fn(&real, #name); \
	return fn args; \
}

SYNC_WRAP(int, pthread_mutex_lock, SYNC_MUTEX_LOCK, (pthread_mutex_t *m), (m), m)
SYNC_WRAP(int, pthread_mutex_trylock, SYNC_MUTEX_TRYLOCK, (pthread_mutex_t *m), (m), m)
SYNC_WRAP(int, pthread_mutex_timedlock, SYNC_MUTEX_TIMEDLOCK,
		  (pthread_mutex_t *restrict m, const struct timespec *restrict t), (m, t), m)
SYNC_WRAP(int, pthread_rwlock_rdlock, SYNC_RW_RDLOCK, (pthread_rwlock_t *l), (l), l)
SYNC_WRAP(int, pthread_rwlock_tryrdlock, SYNC_RW_TRYRDLOCK, (pthread_rwlock_t *l), (l), l)
SYNC_WRAP(int, pthread_rwlock_timedrdlock, SYNC_RW_TIMEDRDLOCK,
		  (pthread_rwlock_t *restrict l, const struct timespec *restrict t), (l, t), l)
SYNC_WRAP(int, pthread_rwlock_wrlock, SYNC_RW_WRLOCK, (pthread_rwlock_t *l), (l), l)
SYNC_WRAP(int, pthread_rwlock_trywrlock, SYNC_RW_TRYWRLOCK, (pthread_rwlock_t *l), (l), l)
SYNC_WRAP(int, pthread_rwlock_timedwrlock, SYNC_RW_TIMEDWRLOCK,
		  (pthread_rwlock_t *restrict l, const struct timespec *restrict t), (l, t), l)
SYNC_WRAP(int, pthread_spin_lock, SYNC_SPIN_LOCK, (pthread_spinlock_t *l), (l), (const void *) l)
SYNC_WRAP(int, pthread_spin_trylock, SYNC_SPIN_TRYLOCK, (pthread_spinlock_t *l), (l), (const void *) l)
SYNC_WRAP(int, sem_wait, SYNC_SEM_WAIT, (sem_t *x), (x), x)
SYNC_WRAP(int, sem_trywait, SYNC_SEM_TRYWAIT, (sem_t *x), (x), x)
SYNC_WRAP(int, sem_timedwait, SYNC_SEM_TIMEDWAIT,
		  (sem_t *restrict x, const struct timespec *restrict t), (x, t), x)
SYNC_WRAP(int, pthread_barrier_wait, SYNC_BARRIER_WAIT, (pthread_barrier_t *b), (b), b)
SYNC_WRAP(int, pthread_once, SYNC_ONCE, (pthread_once_t *o, void (*init)(void)), (o, init), o)
SYNC_WRAP(int, mtx_lock, SYNC_MTX_LOCK, (mtx_t *m), (m), m)
SYNC_WRAP(int, mtx_trylock, SYNC_MTX_TRYLOCK, (mtx_t *m), (m), m)
SYNC_WRAP(int, mtx_timedlock, SYNC_MTX_TIMEDLOCK,
		  (mtx_t *restrict m, const struct timespec *restrict t), (m, t), m)

/* call_once returns nothing, so it does not fit SYNC_WRAP */
void
call_once(once_flag *o, void (*init)(void))
{
	static void *real;
	void		(*fn) (once_flag *, void (*)(void));

	sync_point(SYNC_CALL_ONCE, o);
	*(void **) &fn = real_fn(&real, "call_once");
	fn(o, init);
}
