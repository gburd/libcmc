/*
 * test/proc/syncpoint.c: an ordering bug that needs a thread switch at
 * an uncontended lock, where no system call is made (plan 2.1, item 3).
 *
 * The main thread publishes a value in two steps, each under a mutex:
 * it sets `len`, unlocks, then locks again and sets `ready`.  The worker
 * takes the same mutex once and reads both.  A worker that runs between
 * main's two critical sections sees len set and ready clear, which is
 * the state the BUG build mishandles: it records len as if it were
 * ready.  The correct build checks ready first.
 *
 * Neither thread makes a system call between main's two critical
 * sections: an uncontended pthread_mutex_lock is an atomic exchange in
 * user space.  An engine whose only choice points are system calls never
 * runs the worker there, and passes the BUG build.  With the preload's
 * marker before every lock, the search does.
 *
 * Built with -DC11 the locks are C11 mtx_lock and mtx_unlock, which
 * glibc implements without calling pthread_mutex_lock through the PLT,
 * so the preload wraps them on their own.
 *
 * Canonical state fd 3: [phase][seen].  phase 2 once the worker has
 * read; seen is the length it accepted (0 if it saw nothing ready).
 *
 *   cc -O1 -no-pie [-DC11] [-DBUG] -o syncpoint test/proc/syncpoint.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <threads.h>
#include <unistd.h>

#ifdef C11
static mtx_t mu;
#define LOCK() mtx_lock(&mu)
#define UNLOCK() mtx_unlock(&mu)
#else
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mu)
#define UNLOCK() pthread_mutex_unlock(&mu)
#endif
static int	len, ready;

static void
emit(int phase, int seen)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) seen};

	(void) !write(3, rec, sizeof rec);
}

static void *
worker(void *arg)
{
	int			seen = 0;

	(void) arg;
	LOCK();
#ifdef BUG
	if (len)
		seen = ready ? len : len + 100;	/* a half-published value */
#else
	if (ready)
		seen = len;
#endif
	UNLOCK();
	emit(2, seen);
	return NULL;
}

int
main(void)
{
	pthread_t	t;

#ifdef C11
	mtx_init(&mu, mtx_plain);
#endif
	emit(0, 0);
	pthread_create(&t, NULL, worker, NULL);
	LOCK();
	len = 5;
	UNLOCK();
	LOCK();
	ready = 1;
	UNLOCK();
	pthread_join(t, NULL);
	return 0;
}
