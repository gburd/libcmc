/*
 * race_rt.c: the race runtime (src/cmc_race.c) checked directly, in
 * process.  The runtime normally runs inside a checked node, where the
 * marker calls it makes (a sched_yield the checker answers) carry its
 * reports out.  Here the marker is redirected to a function that records
 * each report, so every edge and every report can be asserted without a
 * checker.
 *
 * The harness orders its threads with atomic builtins the runtime does
 * not see (this file is not built with the instrumentation), so a step
 * that "runs after" another has no happens-before edge unless the
 * scenario adds one through a wrapped call.  Each scenario asserts
 * whether a race is reported.  Exits 0 when every check holds.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>

/* the runtime's marker, recorded instead of made; and its _exit, made an
 * exit so a forked child's counts are written */
static long race_marker(long nr, ...);
#define syscall race_marker
#define _exit(code) exit(code)
#include "../../src/cmc_race.c"
#undef syscall
#undef _exit

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>

static int	nreport, nweak, natomic, nhello, nfull, last_full;

static long
race_marker(long nr, ...)
{
	va_list		ap;
	long		magic, op, a;

	va_start(ap, nr);
	magic = va_arg(ap, long);
	op = va_arg(ap, long);
	a = va_arg(ap, long);
	va_end(ap);
	if (nr != SYS_sched_yield || magic != CMC_RACE_MAGIC)
		return -1;
	switch (op)
	{
		case RACE_HELLO: nhello++; break;
		case RACE_ATOMIC: natomic++; break;
		case RACE_WEAK: nweak++; break;
		case RACE_REPORT: nreport++; break;
		case RACE_FULL: nfull++; last_full = (int) a; break;
		default: break;
	}
	return 0;
}

static int	failed;

static void
check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failed = 1;
}

/* Forget every access, clock and thread, so a scenario starts clean.
 * The runtime stays active; the calling thread (main) is index 0. */
static void
reset(void)
{
	memset(cells, 0, CELLS * sizeof(Cell));
	memset(syncs, 0, SYNCS * sizeof(Sync));
	memset(tvc, 0, sizeof tvc);
	memset(exit_vc, 0, sizeof exit_vc);
	memset(tid_of, 0, sizeof tid_of);
	atomic_store(&nthreads, 1);
	tvc[0][0] = 1;
	self = 0;
	reported = 0;
	weak_told = 0;
	nreport = nweak = natomic = nfull = 0;
}

/* ---- a two-thread harness ------------------------------------------ */

/* turn: whose step runs now.  Spinning on it with builtins adds no edge
 * the runtime sees. */
static int	turn;

static void
wait_turn(int t)
{
	while (__atomic_load_n(&turn, __ATOMIC_SEQ_CST) != t)
		;
}

static void
pass_turn(int t)
{
	__atomic_store_n(&turn, t, __ATOMIC_SEQ_CST);
}

typedef void (*Step) (void);

static Step	worker_steps[4];

static void *
worker(void *arg)
{
	(void) arg;
	for (int i = 0; worker_steps[i]; i++)
	{
		wait_turn(2 * i + 1);
		worker_steps[i]();
		pass_turn(2 * i + 2);
	}
	return NULL;
}

/* Run main's steps and the worker's in alternation: main step 0, worker
 * step 0, main step 1, and so on; then join (unless join is 0). */
static void
alternate(const Step *mine, const Step *theirs, int join)
{
	pthread_t	t;

	memcpy(worker_steps, theirs, sizeof worker_steps);
	pass_turn(0);
	pthread_create(&t, NULL, worker, NULL);
	for (int i = 0; mine[i] || theirs[i]; i++)
	{
		wait_turn(2 * i);
		if (mine[i])
			mine[i]();
		if (!theirs[i])
		{
			/* the worker is done: main runs the rest of its steps alone */
			for (int k = i + 1; mine[k]; k++)
				mine[k]();
			break;
		}
		pass_turn(2 * i + 1);
		wait_turn(2 * i + 2);
	}
	if (join)
		pthread_join(t, NULL);
	else
	{
		/* a join through the C library's own function: no edge */
		int			(*real_join) (pthread_t, void **) = dlsym(RTLD_NEXT, "pthread_join");

		real_join(t, NULL);
	}
}

static int	x, y;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_spinlock_t sp;
static sem_t sem;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t bar;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static mtx_t cmtx;
static cnd_t ccnd;
static once_flag conce = ONCE_FLAG_INIT;
static _Atomic int flag;

static void w_x(void) { __tsan_write4(&x); }
static void r_x(void) { __tsan_read4(&x); }
static void lock_w_unlock(void) { pthread_mutex_lock(&mu); __tsan_write4(&x); pthread_mutex_unlock(&mu); }
static void lock_r_unlock(void) { pthread_mutex_lock(&mu); __tsan_read4(&x); pthread_mutex_unlock(&mu); }
static void hold(void) { pthread_mutex_lock(&mu); }
static void unhold(void) { pthread_mutex_unlock(&mu); }
static void try_fail_then_r(void)
{
	if (pthread_mutex_trylock(&mu) == 0)
		pthread_mutex_unlock(&mu);
	__tsan_read4(&x);
}
static void try_r(void)
{
	if (pthread_mutex_trylock(&mu) == 0)
	{
		__tsan_read4(&x);
		pthread_mutex_unlock(&mu);
	}
}
static void timed_w(void)
{
	struct timespec t = {0};

	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += 5;
	pthread_mutex_timedlock(&mu, &t);
	__tsan_write4(&x);
	pthread_mutex_unlock(&mu);
}
static void wr_w(void) { pthread_rwlock_wrlock(&rw); __tsan_write4(&x); pthread_rwlock_unlock(&rw); }
static void rd_r(void) { pthread_rwlock_rdlock(&rw); __tsan_read4(&x); pthread_rwlock_unlock(&rw); }
static void trywr_w(void) { if (pthread_rwlock_trywrlock(&rw) == 0) { __tsan_write4(&x); pthread_rwlock_unlock(&rw); } }
static void tryrd_r(void) { if (pthread_rwlock_tryrdlock(&rw) == 0) { __tsan_read4(&x); pthread_rwlock_unlock(&rw); } }
static void timedwr_w(void)
{
	struct timespec t = {0};

	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += 5;
	pthread_rwlock_timedwrlock(&rw, &t);
	__tsan_write4(&x);
	pthread_rwlock_unlock(&rw);
}
static void timedrd_r(void)
{
	struct timespec t = {0};

	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += 5;
	pthread_rwlock_timedrdlock(&rw, &t);
	__tsan_read4(&x);
	pthread_rwlock_unlock(&rw);
}
static void spin_w(void) { pthread_spin_lock(&sp); __tsan_write4(&x); pthread_spin_unlock(&sp); }
static void spin_r(void) { if (pthread_spin_trylock(&sp) == 0) { __tsan_read4(&x); pthread_spin_unlock(&sp); } }
static void w_post(void) { __tsan_write4(&x); sem_post(&sem); }
static void wait_r(void) { sem_wait(&sem); __tsan_read4(&x); }
static void trywait_r(void) { if (sem_trywait(&sem) == 0) __tsan_read4(&x); }
static void timedwait_r(void)
{
	struct timespec t = {0};

	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += 5;
	if (sem_timedwait(&sem, &t) == 0)
		__tsan_read4(&x);
}
static void w_signal(void) { __tsan_write4(&x); pthread_cond_signal(&cv); }
static void w_broadcast(void) { __tsan_write4(&x); pthread_cond_broadcast(&cv); }
static void cond_r(void)
{
	/* a timed wait that returns at once: the edge from the signal's
	 * release is what orders the write */
	struct timespec t = {0};

	pthread_mutex_lock(&mu);
	pthread_cond_timedwait(&cv, &mu, &t);
	pthread_mutex_unlock(&mu);
	__tsan_read4(&x);
}
static void init_once(void) { __tsan_write4(&y); }
static void once_w(void) { pthread_once(&once, init_once); }
static void once_r(void) { pthread_once(&once, init_once); __tsan_read4(&y); }
static void cinit_once(void) { __tsan_write4(&y); }
static void conce_w(void) { call_once(&conce, cinit_once); }
static void conce_r(void) { call_once(&conce, cinit_once); __tsan_read4(&y); }
static void c_lock_w(void) { mtx_lock(&cmtx); __tsan_write4(&x); mtx_unlock(&cmtx); }
static void c_lock_r(void) { if (mtx_trylock(&cmtx) == thrd_success) { __tsan_read4(&x); mtx_unlock(&cmtx); } }
static void c_timed_r(void)
{
	struct timespec t = {0};

	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_sec += 5;
	if (mtx_timedlock(&cmtx, &t) == thrd_success)
	{
		__tsan_read4(&x);
		mtx_unlock(&cmtx);
	}
}
static void w_csignal(void) { __tsan_write4(&x); cnd_signal(&ccnd); }
static void w_cbroadcast(void) { __tsan_write4(&x); cnd_broadcast(&ccnd); }
static void ccond_r(void)
{
	struct timespec t = {0};

	mtx_lock(&cmtx);
	cnd_timedwait(&ccnd, &cmtx, &t);
	mtx_unlock(&cmtx);
	__tsan_read4(&x);
}
static void store_sc(void) { __tsan_write4(&x); __tsan_atomic32_store((volatile uint32_t *) &flag, 1, MO_SEQ_CST); }
static void load_sc_r(void) { if (__tsan_atomic32_load((volatile uint32_t *) &flag, MO_SEQ_CST)) __tsan_read4(&x); }
static void store_rel(void) { __tsan_write4(&x); __tsan_atomic32_store((volatile uint32_t *) &flag, 1, MO_RELEASE); }
static void load_acq_r(void) { if (__tsan_atomic32_load((volatile uint32_t *) &flag, MO_ACQUIRE)) __tsan_read4(&x); }
static void store_rlx(void) { __tsan_write4(&x); __tsan_atomic32_store((volatile uint32_t *) &flag, 1, MO_RELAXED); }
static void load_rlx_r(void) { if (__tsan_atomic32_load((volatile uint32_t *) &flag, MO_RELAXED)) __tsan_read4(&x); }
static void atomic_add(void) { __tsan_atomic32_fetch_add((volatile uint32_t *) &x, 1, MO_SEQ_CST); }
static void plain_r(void) { __tsan_read4(&x); }
static void atomic_load_x(void) { __tsan_atomic32_load((volatile uint32_t *) &x, MO_SEQ_CST); }
static void plain_w(void) { __tsan_write4(&x); }

/* run one two-thread scenario and check whether it reported a race */
static void
scenario(const char *what, const Step *mine, const Step *theirs, int join, int want_race)
{
	reset();
	alternate(mine, theirs, join);
	if (want_race)
		check(nreport == 1, what);
	else
		check(nreport == 0, what);
}

#define S(...) ((const Step[]) {__VA_ARGS__, NULL, NULL, NULL})

/* ---- a barrier ------------------------------------------------------- */

/* Each thread writes its own word, waits at the barrier, then reads the
 * other's: the barrier orders every write before every read.  The
 * worker is joined through the C library's own function, so the join
 * adds no edge that could hide a missing one. */
static void *
barrier_child(void *arg)
{
	(void) arg;
	__tsan_write4(&y);
	pthread_barrier_wait(&bar);
	__tsan_read4(&x);
	return NULL;
}

static void
barrier(void)
{
	int			(*real_join) (pthread_t, void **) = dlsym(RTLD_NEXT, "pthread_join");
	pthread_t	t;

	reset();
	pthread_barrier_init(&bar, NULL, 2);
	pthread_create(&t, NULL, barrier_child, NULL);
	__tsan_write4(&x);
	pthread_barrier_wait(&bar);
	__tsan_read4(&y);
	real_join(t, NULL);
	check(nreport == 0, "a barrier orders each thread's writes before the other's reads");
	pthread_barrier_destroy(&bar);
}

/* ---- create and join -------------------------------------------------- */

static void *
child_reads(void *arg)
{
	(void) arg;
	__tsan_read4(&x);
	return NULL;
}

static void *
child_writes(void *arg)
{
	(void) arg;
	__tsan_write4(&x);
	return NULL;
}

static int
cchild_writes(void *arg)
{
	(void) arg;
	__tsan_write4(&x);
	return 0;
}

static void
create_join(void)
{
	pthread_t	t;
	thrd_t		ct;

	reset();
	__tsan_write4(&x);
	pthread_create(&t, NULL, child_reads, NULL);
	pthread_join(t, NULL);
	check(nreport == 0, "a write before pthread_create is ordered before the child's read");

	reset();
	pthread_create(&t, NULL, child_writes, NULL);
	pthread_join(t, NULL);
	__tsan_read4(&x);
	check(nreport == 0, "a child's write is ordered before a read after pthread_join");

	reset();
	pthread_create(&t, NULL, child_writes, NULL);
	{
		int			(*real_join) (pthread_t, void **) = dlsym(RTLD_NEXT, "pthread_join");

		real_join(t, NULL);
	}
	__tsan_read4(&x);
	check(nreport == 1, "without the join's edge the child's write races a later read");

	reset();
	thrd_create(&ct, cchild_writes, NULL);
	thrd_join(ct, NULL);
	__tsan_read4(&x);
	check(nreport == 0, "C11 thrd_create and thrd_join order a child's write");
}

/* ---- the instrumentation's entry points ------------------------------ */

typedef void (*Acc) (void *);

static void
entry_points(void)
{
	static const Acc reads[] = {__tsan_read1, __tsan_read2, __tsan_read4, __tsan_read8, __tsan_read16,
		__tsan_unaligned_read1, __tsan_unaligned_read2, __tsan_unaligned_read4,
		__tsan_unaligned_read8, __tsan_unaligned_read16};
	static const Acc writes[] = {__tsan_write1, __tsan_write2, __tsan_write4, __tsan_write8, __tsan_write16,
		__tsan_unaligned_write1, __tsan_unaligned_write2, __tsan_unaligned_write4,
		__tsan_unaligned_write8, __tsan_unaligned_write16};
	static unsigned char buf[64], dst[64];
	static void *vp;

	reset();
	for (size_t i = 0; i < sizeof reads / sizeof *reads; i++)
	{
		reads[i](buf);
		writes[i](buf + 32);
	}
	__tsan_read_range(buf, 8);
	__tsan_write_range(buf, 8);
	__tsan_func_entry(NULL);
	__tsan_func_exit();
	__tsan_vptr_update(&vp, NULL);
	__tsan_vptr_read(&vp);
	memset(buf, 'a', sizeof buf);
	__tsan_memcpy(dst, buf, 16);
	check(memcmp(dst, buf, 16) == 0, "__tsan_memcpy copies");
	__tsan_memset(dst, 'z', 4);
	check(dst[0] == 'z' && dst[3] == 'z' && dst[4] == 'a', "__tsan_memset sets n bytes");
	memcpy(dst, "0123456789", 10);
	__tsan_memmove(dst + 2, dst, 6);
	check(memcmp(dst, "0101234589", 10) == 0, "__tsan_memmove forward overlap");
	memcpy(dst, "0123456789", 10);
	__tsan_memmove(dst, dst + 2, 6);
	check(memcmp(dst, "2345676789", 10) == 0, "__tsan_memmove backward overlap");
	check(nreport == 0, "one thread's accesses never race");
}

/* every atomic entry point, at every size, with seq_cst: no weak report */
#define ATOMIC_SIZE(n, T) \
	do { \
		static volatile T v; \
		T c = 0; \
		__tsan_atomic##n##_store(&v, 1, MO_SEQ_CST); \
		ok &= __tsan_atomic##n##_load(&v, MO_SEQ_CST) == 1; \
		ok &= __tsan_atomic##n##_exchange(&v, 2, MO_SEQ_CST) == 1; \
		ok &= __tsan_atomic##n##_fetch_add(&v, 3, MO_SEQ_CST) == 2; \
		ok &= __tsan_atomic##n##_fetch_sub(&v, 1, MO_SEQ_CST) == 5; \
		ok &= __tsan_atomic##n##_fetch_and(&v, 6, MO_SEQ_CST) == 4; \
		ok &= __tsan_atomic##n##_fetch_or(&v, 1, MO_SEQ_CST) == 4; \
		ok &= __tsan_atomic##n##_fetch_xor(&v, 1, MO_SEQ_CST) == 5; \
		ok &= __tsan_atomic##n##_fetch_nand(&v, 1, MO_SEQ_CST) == 4; \
		c = __tsan_atomic##n##_load(&v, MO_SEQ_CST); \
		ok &= __tsan_atomic##n##_compare_exchange_strong(&v, &c, 9, MO_SEQ_CST, MO_SEQ_CST) == 1; \
		c = 0; \
		ok &= __tsan_atomic##n##_compare_exchange_weak(&v, &c, 7, MO_SEQ_CST, MO_SEQ_CST) == 0 && c == 9; \
		ok &= __tsan_atomic##n##_compare_exchange_val(&v, 9, 8, MO_SEQ_CST, MO_SEQ_CST) == 9; \
		ok &= __tsan_atomic##n##_load(&v, MO_SEQ_CST) == 8; \
	} while (0)

static void
atomics(void)
{
	int			ok = 1;

	reset();
	ATOMIC_SIZE(8, uint8_t);
	ATOMIC_SIZE(16, uint16_t);
	ATOMIC_SIZE(32, uint32_t);
	ATOMIC_SIZE(64, uint64_t);
	check(ok, "every atomic entry point computes its operation");
	check(nweak == 0 && nreport == 0, "seq_cst atomics report nothing");
	/* 14 calls at each of four sizes, the last a load that checks the result */
	check(natomic == 4 * 14, "each atomic operation is a choice point");
	__tsan_atomic_thread_fence(MO_SEQ_CST);
	__tsan_atomic_signal_fence(MO_RELAXED);
	check(nweak == 0, "a seq_cst fence is not weak");
	__tsan_atomic_thread_fence(MO_ACQUIRE);
	check(nweak == 1, "an acquire fence is reported weak");
	__tsan_atomic_thread_fence(MO_RELEASE);
	check(nweak == 1, "weak is reported once");
	reset();
	__tsan_atomic32_load((volatile uint32_t *) &x, MO_RELAXED);
	check(nweak == 1, "a relaxed load is reported weak");
	reset();
	__tsan_atomic32_load((volatile uint32_t *) &x, MO_CONSUME);
	__tsan_atomic32_fetch_add((volatile uint32_t *) &x, 1, MO_ACQ_REL);
	__tsan_atomic32_store((volatile uint32_t *) &x, 1, MO_ACQ_REL);
	check(nweak == 1 && nreport == 0, "consume and acq_rel are weak, reported once");
}

/* ---- limits ------------------------------------------------------------ */

static void
limits(void)
{
	pthread_t	t[MAXT];
	int			n = 0;

	reset();
	for (int i = 0; i < MAXT; i++)
		if (pthread_create(&t[n], NULL, child_reads, NULL) == 0)
			n++;
	for (int i = 0; i < n; i++)
		pthread_join(t[i], NULL);
	check(nfull == 1 && last_full == 1, "a thread past the cap is reported, not tracked");
	active = 1;

	/* the sync table: one more object than it holds */
	reset();
	static volatile uint32_t many[SYNCS + 1];

	for (unsigned i = 0; i <= SYNCS && nfull == 0; i++)
		__tsan_atomic32_store(&many[i], 1, MO_SEQ_CST);
	check(nfull == 1 && last_full == 2, "a full sync table is reported");
	active = 1;
	reset();
	for (unsigned i = 0; i < SYNCS; i++)
		__tsan_atomic32_load(&many[i], MO_SEQ_CST);
	check(nfull == 0, "the sync table holds exactly its size");
	{
		sem_t		s1;

		sem_init(&s1, 0, 0);
		sem_post(&s1);
		check(nfull == 1, "a release that finds the sync table full is reported");
		sem_destroy(&s1);
	}
	active = 1;

	/* the cell table: one more byte than it holds */
	reset();
	size_t		len = (size_t) CELLS + 16;
	unsigned char *big = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (big != MAP_FAILED)
	{
		__tsan_write_range(big, len);
		munmap(big, len);
	}
	check(nfull == 1 && last_full == 3, "a full cell table is reported");
	active = 1;
	reset();
}

/* ---- off outside cmc, and a table that cannot be mapped -------------- */

static void
startup(void)
{
	fflush(stdout);
	fflush(stdout);
	pid_t		pid = fork();

	if (pid == 0)
	{
		/* no CMC_STATE_FD: nothing is mapped and nothing is checked.  The
		 * parent's runtime is on, so turn it off as a fresh node starts. */
		unsetenv("CMC_STATE_FD");
		pthread_t	t;

		thrd_t		ct;

		active = 0;
		nhello = nweak = natomic = nreport = 0;
		__tsan_init();
		__tsan_write4(&x);
		__tsan_atomic32_store((volatile uint32_t *) &x, 1, MO_RELAXED);
		__tsan_atomic_thread_fence(MO_RELAXED);
		pthread_mutex_lock(&mu);
		pthread_mutex_unlock(&mu);
		pthread_create(&t, NULL, child_writes, NULL);
		pthread_join(t, NULL);
		thrd_create(&ct, cchild_writes, NULL);
		thrd_join(ct, NULL);
		exit(active || nhello || nweak || natomic || nreport ? 10 + active + 2 * nhello + 4 * nreport : 0);
	}
	int			st = 0;

	waitpid(pid, &st, 0);
	check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "outside cmc the runtime does nothing");

	fflush(stdout);
	pid = fork();
	if (pid == 0)
	{
		/* the cell table's address is taken: init reports it and stays off */
		munmap(CELL_BASE, CELLS * sizeof(Cell));
		munmap(SYNC_BASE, SYNCS * sizeof(Sync));
		void	   *p = mmap(CELL_BASE, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

		active = 0;
		nfull = 0;
		setenv("CMC_STATE_FD", "3", 1);
		__tsan_init();
		exit(p == CELL_BASE && !active && nfull == 1 ? 0 : 1);
	}
	waitpid(pid, &st, 0);
	check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a table that cannot be mapped is reported");

	fflush(stdout);
	pid = fork();
	if (pid == 0)
	{
		/* a function the C library lacks ends the node (its message on
		 * stderr is the one a node would print) */
		static void *slot;

		close(2);

		(void) next_fn(&slot, "cmc_no_such_function");
		exit(0);
	}
	waitpid(pid, &st, 0);
	check(WIFEXITED(st) && WEXITSTATUS(st) == 2, "a missing C library function ends the node");
}

/* ---- calls that fail add no edge ----------------------------------- */

static int	try_results, ctry_result;

static void *
try_rwlocks(void *arg)
{
	pthread_rwlock_t *l = arg;

	try_results = (pthread_rwlock_tryrdlock(l) == EBUSY) + (pthread_rwlock_trywrlock(l) == EBUSY) +
		(pthread_mutex_trylock(&mu) == 0);
	pthread_mutex_unlock(&mu);
	return NULL;
}

static int
ctry_lock(void *arg)
{
	ctry_result = mtx_trylock(arg);
	return 0;
}

static void
on_signal(int sig)
{
	(void) sig;
}

static void *
interrupter(void *arg)
{
	struct timespec t = {0, 50 * 1000 * 1000};

	nanosleep(&t, NULL);
	pthread_kill(*(pthread_t *) arg, SIGUSR1);
	return NULL;
}

static void
failures(void)
{
	pthread_mutexattr_t ma;
	pthread_mutex_t em;
	pthread_rwlock_t rl = PTHREAD_RWLOCK_INITIALIZER;
	pthread_spinlock_t sl;
	sem_t		s0;
	struct timespec past = {0};
	int			ok = 1;

	reset();
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&em, &ma);
	pthread_mutex_lock(&em);
	ok &= pthread_mutex_lock(&em) == EDEADLK;
	ok &= pthread_mutex_timedlock(&em, &past) == EDEADLK;
	pthread_mutex_unlock(&em);
	pthread_rwlock_wrlock(&rl);
	ok &= pthread_rwlock_wrlock(&rl) == EDEADLK;
	ok &= pthread_rwlock_rdlock(&rl) == EDEADLK;
	ok &= pthread_rwlock_timedwrlock(&rl, &past) == EDEADLK;
	ok &= pthread_rwlock_timedrdlock(&rl, &past) == EDEADLK;
	pthread_rwlock_unlock(&rl);
	{
		/* a try that finds the lock held by another thread */
		pthread_t	t;

		pthread_rwlock_wrlock(&rl);
		pthread_create(&t, NULL, try_rwlocks, &rl);
		pthread_join(t, NULL);
		pthread_rwlock_unlock(&rl);
		ok &= try_results == 3;
	}
	pthread_spin_init(&sl, PTHREAD_PROCESS_PRIVATE);
	pthread_spin_lock(&sl);
	ok &= pthread_spin_trylock(&sl) == EBUSY;
	pthread_spin_unlock(&sl);
	sem_init(&s0, 0, 0);
	ok &= sem_trywait(&s0) == -1 && errno == EAGAIN;
	ok &= sem_timedwait(&s0, &past) == -1 && errno == ETIMEDOUT;
	{
		/* sem_wait interrupted by a signal returns -1 with EINTR */
		struct sigaction sa = {0};
		pthread_t	me_t = pthread_self(), t;

		sa.sa_handler = on_signal;
		sigaction(SIGUSR1, &sa, NULL);
		pthread_create(&t, NULL, interrupter, &me_t);
		ok &= sem_wait(&s0) == -1 && errno == EINTR;
		pthread_join(t, NULL);
	}
	{
		/* C11 calls on an error-checking mutex (glibc's mtx_t is a
		 * pthread mutex) */
		mtx_t	   *cm = (mtx_t *) &em;
		thrd_t		ct;

		mtx_lock(cm);
		ok &= mtx_lock(cm) == thrd_error;
		ok &= mtx_timedlock(cm, &past) == thrd_error;
		mtx_unlock(cm);
		thrd_create(&ct, ctry_lock, cm);
		mtx_lock(cm);
		thrd_join(ct, NULL);
		mtx_unlock(cm);
		ok &= ctry_result == thrd_busy;
		ok &= thrd_join(thrd_current(), NULL) == thrd_error;
	}
	ok &= pthread_join(pthread_self(), NULL) == EDEADLK;
	{
		/* a thread that cannot be made: a stack larger than memory */
		pthread_attr_t at;
		pthread_t	t;

		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, (size_t) 1 << 46);
		ok &= pthread_create(&t, &at, child_reads, NULL) != 0;
		pthread_attr_destroy(&at);
	}
	check(ok, "a failed lock, wait, create or join returns its error");
	check(nreport == 0, "and reports nothing");
	pthread_mutex_destroy(&em);
	sem_destroy(&s0);
}

/* a thread the runtime did not see created gets a fresh index */
static void *
stranger(void *arg)
{
	(void) arg;
	__tsan_write4(&x);
	return NULL;
}

static void *
stranger_atomic(void *arg)
{
	(void) arg;
	__tsan_atomic32_fetch_add((volatile uint32_t *) &x, 1, MO_SEQ_CST);
	return NULL;
}

/* a C11 thread that finishes after the runtime turned off records no
 * exit clock */
static int
late_child(void *arg)
{
	(void) arg;
	wait_turn(1);
	return 0;
}

static void
c11_late(void)
{
	thrd_t		ct;

	reset();
	pass_turn(0);
	thrd_create(&ct, late_child, NULL);
	active = 0;
	pass_turn(1);
	thrd_join(ct, NULL);
	check(exit_vc[1][1] == 0, "a C11 thread that ends while the runtime is off records no exit clock");
	active = 1;
	reset();

	/* a C11 thread the C library cannot make returns thrd_nomem and no
	 * create edge is recorded for it */
	fflush(stdout);
	pid_t		pid = fork();

	if (pid == 0)
	{
		/* RLIMIT_NPROC of 1 for a user who has more processes: clone fails
		 * with EAGAIN (glibc reuses a cached stack, so an address-space
		 * limit would not do).  root is exempt from the limit. */
		struct rlimit one = {1, 1};

		if (geteuid() == 0)
			exit(3);
		setrlimit(RLIMIT_NPROC, &one);
		int			r = thrd_create(&ct, late_child, NULL);

		if (r == thrd_success)
		{
			pass_turn(1);
			thrd_join(ct, NULL);
		}
		exit(r != thrd_success && tid_of[1] == 0 ? 0 : 1);
	}
	int			st = 0;

	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 3)
		printf("skip a C11 thread that cannot be made (root ignores RLIMIT_NPROC)\n");
	else
		check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a C11 thread that cannot be made is not recorded");
}

static void
unseen_thread(void)
{
	int			(*real_create) (pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) =
		dlsym(RTLD_NEXT, "pthread_create");
	int			(*real_join) (pthread_t, void **) = dlsym(RTLD_NEXT, "pthread_join");
	pthread_t	t;

	reset();
	__tsan_write4(&x);
	real_create(&t, NULL, stranger, NULL);
	real_join(t, NULL);
	check(nreport == 1, "a thread created out of sight has no edge, so its write races");

	reset();
	atomic_store(&nthreads, MAXT);
	real_create(&t, NULL, stranger, NULL);
	real_join(t, NULL);
	check(nfull == 1 && last_full == 1, "an unseen thread past the cap is reported");
	active = 1;
	reset();
	atomic_store(&nthreads, MAXT);
	real_create(&t, NULL, stranger_atomic, NULL);
	real_join(t, NULL);
	check(nfull == 1 && natomic == 0, "an unseen thread past the cap makes no atomic choice point");
	active = 1;
	reset();
	atomic_store(&nthreads, MAXT);
	{
		thrd_t		ct;

		check(thrd_create(&ct, cchild_writes, NULL) == thrd_success, "thrd_create past the cap still runs the thread");
		thrd_join(ct, NULL);
		check(nfull == 1, "and is reported");
	}
	active = 1;
}

int
main(void)
{
	setenv("CMC_STATE_FD", "3", 1);
	__tsan_init();
	check(active && nhello == 1, "under cmc the runtime announces itself");
	__tsan_init();
	check(nhello == 1, "a second init does nothing");
	pthread_spin_init(&sp, PTHREAD_PROCESS_PRIVATE);
	sem_init(&sem, 0, 0);
	mtx_init(&cmtx, mtx_timed);
	cnd_init(&ccnd);

	scenario("an unordered write and write race", S(w_x), S(w_x), 1, 1);
	scenario("an unordered write and read race", S(w_x), S(r_x), 1, 1);
	scenario("an unordered read and write race", S(r_x), S(w_x), 1, 1);
	scenario("two reads never race", S(r_x), S(r_x), 1, 0);
	scenario("a mutex orders a write and a read", S(lock_w_unlock), S(lock_r_unlock), 1, 0);
	scenario("a timed lock orders like a lock", S(timed_w), S(lock_r_unlock), 1, 0);
	scenario("a successful trylock orders", S(lock_w_unlock), S(try_r), 1, 0);
	scenario("a failed trylock adds no edge", S(hold, w_x, unhold), S(try_fail_then_r), 1, 1);
	scenario("a rwlock orders a writer and a reader", S(wr_w), S(rd_r), 1, 0);
	scenario("try and timed rwlocks order", S(trywr_w), S(tryrd_r), 1, 0);
	scenario("timed rwlocks order", S(timedwr_w), S(timedrd_r), 1, 0);
	scenario("a spinlock orders", S(spin_w), S(spin_r), 1, 0);
	scenario("sem_post orders before sem_wait", S(w_post), S(wait_r), 1, 0);
	scenario("sem_post orders before sem_trywait", S(w_post), S(trywait_r), 1, 0);
	scenario("sem_post orders before sem_timedwait", S(w_post), S(timedwait_r), 1, 0);
	scenario("a condition signal orders a waiter", S(w_signal), S(cond_r), 1, 0);
	scenario("a condition broadcast orders a waiter", S(w_broadcast), S(cond_r), 1, 0);
	scenario("pthread_once orders its initialiser", S(once_w), S(once_r), 1, 0);
	scenario("C11 call_once orders its initialiser", S(conce_w), S(conce_r), 1, 0);
	scenario("a C11 mutex orders", S(c_lock_w), S(c_lock_r), 1, 0);
	scenario("a C11 timed mutex orders", S(c_lock_w), S(c_timed_r), 1, 0);
	scenario("a C11 condition signal orders", S(w_csignal), S(ccond_r), 1, 0);
	scenario("a C11 condition broadcast orders", S(w_cbroadcast), S(ccond_r), 1, 0);
	scenario("a seq_cst store and load order a payload", S(store_sc), S(load_sc_r), 1, 0);
	scenario("a release store and acquire load order a payload", S(store_rel), S(load_acq_r), 1, 0);
	scenario("relaxed atomics do not order a payload", S(store_rlx), S(load_rlx_r), 1, 1);
	scenario("two atomics on one word never race", S(atomic_add), S(atomic_add), 1, 0);
	scenario("an atomic and a plain read race", S(atomic_add), S(plain_r), 1, 1);
	scenario("only the first race is reported", S(w_x, w_x), S(w_x, w_x), 1, 1);
	scenario("a read ordered before a write does not race", S(lock_r_unlock), S(lock_w_unlock), 1, 0);
	scenario("an atomic read and an atomic write never race", S(atomic_load_x), S(atomic_add), 1, 0);
	scenario("an atomic read and a plain write race", S(atomic_load_x), S(plain_w), 1, 1);
	barrier();
	create_join();
	entry_points();
	atomics();
	unseen_thread();
	c11_late();
	failures();
	limits();
	startup();
	return failed;
}
