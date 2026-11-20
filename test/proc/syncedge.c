/*
 * test/proc/syncedge.c: schedules and happens-before edges the race
 * runtime got wrong in 2.1.0 (docs/research/04-review-race-runtime.md).
 * Each mode is a two-thread program, from the target args:
 *
 *   rwread   two readers under a rwlock read lock, one of which writes
 *            x.  Read locks do not exclude each other and POSIX orders
 *            no read unlock before another read lock: a data race.
 *   casfail  the reader's compare-exchange fails (the writer set the
 *            flag first) with a relaxed failure order, then reads the
 *            payload.  A relaxed load orders nothing: a data race.
 *   casok    the same with a seq_cst failure order: no race.
 *   bargen   a barrier of two, used twice.  Between the rounds one
 *            thread writes x and the other reads it: a data race.
 *   sem2     a semaphore of count two taken as a lock: both threads can
 *            be inside at once, one writing x and one reading it.
 *   trymutex a trylock that fails needs the other thread to hold the
 *            lock at that moment.  The record says whether it failed;
 *            the invariant claims it never does, which Linux refutes.
 *   trysem   the same with sem_trywait on a semaphore of count one.
 *   tryrw    the same with a tryrdlock against a writer.
 *   readbuf  one thread reads an eventfd into a buffer, which the read
 *            writes; the other reads the buffer with a plain load.
 *            Nothing orders the two: a data race.  Built -DCHK the read
 *            is the fortified __read_chk.
 *
 * Canonical state fd 3: [phase][value]; phase 2 once main has joined,
 * value 1 unless a try failed.
 *
 *   cc -O1 -fno-pie -fsanitize=thread -c test/proc/syncedge.c
 *   cc -no-pie -o syncedge syncedge.o libcmc-race.a -lpthread
 */
#undef _FORTIFY_SOURCE
#ifdef CHK
#define _FORTIFY_SOURCE 2
#endif
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t bar;
static sem_t sem;
static atomic_int flag;
static int	x;
static volatile int sink;
static int	failed;
static int	efd;
static uint64_t ebuf;
static volatile size_t elen = sizeof(uint64_t);	/* not a constant, so fortify checks it */
static const char *mode = "rwread";

static void
emit(int phase, int value)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) value};

	(void) !write(3, rec, sizeof rec);
}

static int
is(const char *m)
{
	return strcmp(mode, m) == 0;
}

/* thread a writes x (or holds the lock), thread b reads x (or tries) */
static void
touch(int a)
{
	if (a)
		x = 1;
	else
		sink = x;
}

static void
cas_side(int a)
{
	int			expect = 0;
	memory_order fail = is("casfail") ? memory_order_relaxed : memory_order_seq_cst;

	if (a)
	{
		x = 7;
		atomic_store(&flag, 1);
	}
	else if (!atomic_compare_exchange_strong_explicit(&flag, &expect, 2, memory_order_seq_cst, fail))
		sink = x;
}

static void
try_side(int a)
{
	if (is("trymutex"))
	{
		if (a)
		{
			pthread_mutex_lock(&mu);
			pthread_mutex_unlock(&mu);
		}
		else if (pthread_mutex_trylock(&mu) != 0)
			failed = 1;
		else
			pthread_mutex_unlock(&mu);
	}
	else if (is("trysem"))
	{
		if (a)
		{
			sem_wait(&sem);
			sem_post(&sem);
		}
		else if (sem_trywait(&sem) != 0)
			failed = 1;
		else
			sem_post(&sem);
	}
	else
	{
		if (a)
		{
			pthread_rwlock_wrlock(&rw);
			pthread_rwlock_unlock(&rw);
		}
		else if (pthread_rwlock_tryrdlock(&rw) != 0)
			failed = 1;
		else
			pthread_rwlock_unlock(&rw);
	}
}

static void *
side(void *arg)
{
	int			a = arg != NULL;

	if (is("rwread"))
	{
		pthread_rwlock_rdlock(&rw);
		touch(a);
		pthread_rwlock_unlock(&rw);
	}
	else if (is("casfail") || is("casok"))
		cas_side(a);
	else if (is("bargen"))
	{
		pthread_barrier_wait(&bar);
		touch(a);
		pthread_barrier_wait(&bar);
	}
	else if (is("readbuf"))
	{
		if (a)
			(void) !read(efd, &ebuf, elen);
		else
			sink = (int) ebuf;
	}
	else if (is("sem2"))
	{
		sem_wait(&sem);
		touch(a);
		sem_post(&sem);
	}
	else
		try_side(a);
	return NULL;
}

int
main(int argc, char **argv)
{
	pthread_t	a, b;

	if (argc > 2)
		mode = argv[2];
	pthread_barrier_init(&bar, NULL, 2);
	sem_init(&sem, 0, is("sem2") ? 2 : 1);
	if (is("readbuf"))
	{
		uint64_t	one = 1;

		efd = eventfd(0, 0);
		(void) !write(efd, &one, sizeof one);
	}
	emit(0, 0);
	pthread_create(&a, NULL, side, (void *) 1);
	pthread_create(&b, NULL, side, NULL);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	emit(2, failed ? 0 : 1);
	return 0;
}
