/*
 * timedwait: a timed condition wait in a node with two threads
 * (test/proc, 2.2).
 *
 * The main thread waits up to a second on its node's clock for a flag
 * the worker sets and signals.  Which comes first, the signal or the
 * timeout, is a search choice: the timeout is a deadline the search may
 * fire (A_EXPIRE) at any point the waiter is parked, and the signal
 * wakes it whenever the worker runs.  Both outcomes are reachable.
 *
 * Canonical state on fd 3: [count=2][done][outcome], outcome 1 = woken
 * by the signal, 2 = timed out.
 *
 *   cc -O1 -no-pie -o timedwait test/proc/timedwait.c -lpthread
 */
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int flag;

static void *
signaller(void *x)
{
	pthread_mutex_lock(&m);
	flag = 1;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&m);
	return x;
}

int
main(void)
{
	unsigned char rec[3] = {2, 0, 0};
	(void) !write(3, rec, sizeof rec);
	pthread_t t;
	pthread_create(&t, NULL, signaller, NULL);
	struct timespec at;
	clock_gettime(CLOCK_REALTIME, &at);
	at.tv_sec += 1;
	int r = 0;
	pthread_mutex_lock(&m);
	while (!flag && r == 0) r = pthread_cond_timedwait(&cv, &m, &at);
	pthread_mutex_unlock(&m);
	pthread_join(t, NULL);
	rec[1] = 1; rec[2] = r == ETIMEDOUT ? 2 : 1;
	(void) !write(3, rec, sizeof rec);
	return 0;
}
