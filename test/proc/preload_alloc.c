/*
 * preload_alloc.c: the preload's allocator (src/cmc_preload.c) under its
 * own names, checked directly.  calloc's size product and the arena
 * bound must not wrap, so an impossible request fails with NULL instead
 * of returning memory already in use; realloc copies the old block's
 * size, not the new one, so growing a block never copies its neighbour.
 * The aligned allocators honour their alignment and refuse a bad one;
 * the arena's size knob falls back to the default on a bad value; the
 * clock and random shims give the kernel's answers outside the checker;
 * and a synchronisation wrapper makes its marker and then the real call.
 * Exits 0 when every check holds.
 */
#define malloc pa_malloc
#define calloc pa_calloc
#define realloc pa_realloc
#define free pa_free
#define posix_memalign pa_posix_memalign
#define aligned_alloc pa_aligned_alloc
#define memalign pa_memalign
#define clock_gettime pa_clock_gettime
#define gettimeofday pa_gettimeofday
#define time pa_time
#define getrandom pa_getrandom
/* the preload's _exit, made an exit, so a forked child's counts are
 * written when it ends the node */
#define _exit(code) exit(code)
#include "../../src/cmc_preload.c"
#undef _exit
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef posix_memalign
#undef aligned_alloc
#undef memalign
#undef clock_gettime
#undef gettimeofday
#undef time
#undef getrandom

#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>

/* run fn in a child and return its exit status, or -1 if it did not exit */
static int
in_child(void (*fn)(void))
{
	int			st = 0;

	fflush(stdout);
	pid_t		pid = fork();

	if (pid == 0)
	{
		fn();
		exit(0);
	}
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void
realloc_stranger(void)
{
	static char not_ours[64];

	close(2);
	void	   *r = pa_realloc(not_ours + 32, 8);

	exit(r == NULL ? 1 : 0);
}

/* a pointer inside the arena but past every block it handed out */
static void
realloc_past(void)
{
	close(2);
	void	   *r = pa_realloc((unsigned char *) ARENA_BASE + cmc_arena_bump + 4096, 8);

	exit(r == NULL ? 1 : 0);
}

/* a pointer inside the arena's base header, below any block */
static void
realloc_below(void)
{
	close(2);
	void	   *r = pa_realloc(ARENA_BASE, 8);

	exit(r == NULL ? 1 : 0);
}


static void
arena_taken(void)
{
	/* a fresh arena whose base another mapping already holds: the fixed
	 * map fails (MAP_FIXED over a read-only mapping succeeds, so make the
	 * size impossible instead) */
	ready = 0;
	setenv("CMC_ARENA_MB", "4096", 1);
	close(2);
	struct rlimit low = {1, 1};

	setrlimit(RLIMIT_AS, &low);
	arena_init();
}

static size_t
bytes_for(const char *v)
{
	if (v)
		setenv("CMC_ARENA_MB", v, 1);
	else
		unsetenv("CMC_ARENA_MB");
	return arena_bytes();
}

static int	failed;
static int	once_runs;

static void
once_init(void)
{
	once_runs++;
}

static void
missing_fn(void)
{
	static void *slot;

	close(2);
	(void) real_fn(&slot, "cmc_no_such_function");
}

static void
check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failed = 1;
}

int
main(void)
{
	volatile size_t huge = SIZE_MAX;

	check(pa_calloc(huge / 2 + 1, 2) == NULL, "calloc whose product overflows is NULL");
	check(pa_calloc(huge, 0) != NULL && pa_calloc(3, 5) != NULL, "calloc of a zero or a small product succeeds");
	check(pa_calloc(huge / 2, 1) == NULL, "calloc past the arena is NULL");
	check(pa_realloc(NULL, huge) == NULL, "realloc past the arena is NULL");
	check(pa_malloc(huge) == NULL, "malloc(SIZE_MAX) is NULL");
	check(pa_malloc(huge - 8) == NULL, "malloc near SIZE_MAX is NULL");

	unsigned char *a = pa_malloc(16), *b = pa_malloc(64);
	memset(a, 'A', 16);
	memset(b, 'B', 64);
	unsigned char *c = pa_realloc(a, 64);
	int		copied = c != NULL;
	for (int i = 0; copied && i < 16; i++)
		copied = c[i] == 'A';
	int		clean = c != NULL;
	for (int i = 16; clean && i < 64; i++)
		clean = c[i] == 0;
	check(copied, "realloc keeps the old block's bytes");
	check(clean, "realloc copies only the old block's size");
	check(pa_realloc(c, 8) != NULL && c[0] == 'A', "realloc to a smaller size keeps its prefix");
	check(pa_realloc(NULL, 4) != NULL, "realloc of NULL allocates");
	check(pa_malloc(0) != NULL && pa_calloc(0, 4) != NULL && pa_realloc(NULL, 0) != NULL,
		  "a zero-size request gets a block");
	pa_free(b);

	/* the aligned allocators */
	void	   *q = NULL;

	check(pa_posix_memalign(&q, 64, 10) == 0 && ((uintptr_t) q & 63) == 0, "posix_memalign aligns");
	check(pa_posix_memalign(&q, 4096, 0) == 0 && ((uintptr_t) q & 4095) == 0, "posix_memalign of 0 bytes aligns");
	check(pa_posix_memalign(&q, 3, 8) == EINVAL, "posix_memalign refuses a non-power of two");
	check(pa_posix_memalign(&q, 2, 8) == EINVAL, "posix_memalign refuses less than a pointer");
	check(pa_posix_memalign(&q, 24, 8) == EINVAL, "posix_memalign refuses a pointer multiple that is not a power of two");
	check(pa_posix_memalign(&q, 64, huge) == ENOMEM, "posix_memalign of too much is ENOMEM");
	check(pa_posix_memalign(&q, (size_t) 1 << 40, 8) == ENOMEM, "an alignment past the arena is ENOMEM");
	check(((uintptr_t) pa_aligned_alloc(128, 1) & 127) == 0 && pa_aligned_alloc(8, 0) != NULL,
		  "aligned_alloc aligns, below the arena's own alignment too");
	check(pa_aligned_alloc(0, 8) == NULL && pa_aligned_alloc(24, 8) == NULL, "aligned_alloc refuses a bad alignment");
	check(((uintptr_t) pa_memalign(256, 1) & 255) == 0 && pa_memalign(16, 0) != NULL, "memalign aligns");
	check(pa_memalign(0, 8) == NULL && pa_memalign(12, 8) == NULL, "memalign refuses a bad alignment");

	/* the arena's size: CMC_ARENA_MB, or the default on a bad value */
	size_t		def = ARENA_MB_DEFAULT * 1024 * 1024;

	check(bytes_for(NULL) == def && bytes_for("") == def, "no CMC_ARENA_MB is the default");
	check(bytes_for("8") == 8 * 1024 * 1024, "CMC_ARENA_MB sets the arena in MiB");
	check(bytes_for("x") == def && bytes_for("8x") == def && bytes_for("0") == def &&
		  bytes_for("5000") == def, "a bad CMC_ARENA_MB falls back to the default");
	unsetenv("CMC_ARENA_MB");

	/* the arena running out */
	size_t		left = cmc_arena_size - cmc_arena_bump;

	check(pa_malloc(left) == NULL && errno == ENOMEM, "a request past what is left is ENOMEM");
	cmc_arena_bump = cmc_arena_size;
	check(pa_malloc(1) == NULL, "a full arena is ENOMEM");
	cmc_arena_bump = 4096;

	/* ending the node visibly */
	check(in_child(realloc_stranger) == 97, "realloc of a block the arena did not make ends the node");
	check(in_child(realloc_below) == 97, "realloc of the arena's base, below any block, ends the node");
	check(in_child(realloc_past) == 97, "realloc past the arena's last block ends the node");
	check(in_child(arena_taken) == 97, "an arena that cannot be mapped ends the node");

	/* the shims, outside the checker, give the kernel's answers */
	struct timespec ts = {0};
	struct timeval tv = {0};
	time_t		t0 = 0;
	unsigned char rnd[8];

	check(pa_clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 0, "clock_gettime reads the clock");
	check(pa_gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 0, "gettimeofday reads the clock");
	time_t		t1 = pa_time(&t0);

	check(t1 > 0 && t1 == t0, "time returns and stores the clock");
	check(pa_time(NULL) >= t1, "time(NULL) reads it");
	check(pa_getrandom(rnd, sizeof rnd, 0) == (ssize_t) sizeof rnd, "getrandom fills its buffer");

	/* the synchronisation wrappers: each calls the C library's function */
	pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
	pthread_spinlock_t sp;
	pthread_barrier_t bar;
	pthread_once_t once = PTHREAD_ONCE_INIT;
	once_flag conce = ONCE_FLAG_INIT;
	sem_t		sem;
	mtx_t		cm;
	struct timespec later = {0};
	int			ok = 1;

	pa_clock_gettime(CLOCK_REALTIME, &later);
	later.tv_sec += 5;
	ok &= pthread_mutex_lock(&m) == 0 && pthread_mutex_trylock(&m) == EBUSY;
	pthread_mutex_unlock(&m);
	ok &= pthread_mutex_timedlock(&m, &later) == 0;
	pthread_mutex_unlock(&m);
	ok &= pthread_rwlock_rdlock(&rw) == 0 && pthread_rwlock_tryrdlock(&rw) == 0;
	pthread_rwlock_unlock(&rw);
	pthread_rwlock_unlock(&rw);
	ok &= pthread_rwlock_timedrdlock(&rw, &later) == 0;
	pthread_rwlock_unlock(&rw);
	ok &= pthread_rwlock_wrlock(&rw) == 0 && pthread_rwlock_trywrlock(&rw) == EBUSY;
	pthread_rwlock_unlock(&rw);
	ok &= pthread_rwlock_timedwrlock(&rw, &later) == 0;
	pthread_rwlock_unlock(&rw);
	pthread_spin_init(&sp, PTHREAD_PROCESS_PRIVATE);
	ok &= pthread_spin_lock(&sp) == 0 && pthread_spin_trylock(&sp) == EBUSY;
	pthread_spin_unlock(&sp);
	sem_init(&sem, 0, 2);
	ok &= sem_wait(&sem) == 0 && sem_trywait(&sem) == 0 && sem_timedwait(&sem, &later) == -1;
	pthread_barrier_init(&bar, NULL, 1);
	ok &= pthread_barrier_wait(&bar) == PTHREAD_BARRIER_SERIAL_THREAD;
	ok &= pthread_once(&once, once_init) == 0 && pthread_once(&once, once_init) == 0 && once_runs == 1;
	call_once(&conce, once_init);
	call_once(&conce, once_init);
	ok &= once_runs == 2;
	mtx_init(&cm, mtx_timed);
	ok &= mtx_lock(&cm) == thrd_success && mtx_trylock(&cm) == thrd_busy;
	mtx_unlock(&cm);
	ok &= mtx_timedlock(&cm, &later) == thrd_success;
	mtx_unlock(&cm);
	check(ok, "every synchronisation wrapper calls the real function");
	check(in_child(missing_fn) == 2, "a function the C library lacks ends the node");
	return failed;
}
