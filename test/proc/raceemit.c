/*
 * test/proc/raceemit.c: a lost update between two threads, the race a
 * single fixed thread order misses (soundness plan 3.8).  One node.
 *
 * The main thread starts two workers and joins both.  Each worker adds
 * one to a shared counter: it reads the counter, logs the value it read
 * on the state hook (a write to fd 3, a system call), then stores the
 * value plus one.  Under -DBUG the read, the log and the store are not
 * under the mutex, so a worker that runs between the other's read and
 * store makes one increment overwrite the other: the counter ends at 1.
 * The correct build holds the mutex across all three.
 *
 * The log write is what puts the race at a system call: the checker
 * explores which thread runs at each one, not interleavings of plain
 * memory accesses between two calls.  Running each worker to completion
 * in turn, as an engine with one schedule does, never loses an update.
 *
 * Canonical state fd 3: [done][count].  The main thread emits done = 1
 * with the counter once it has joined both workers; a worker's log
 * leaves done = 0.
 *
 *   cc -O1 -no-pie -o raceemit     test/proc/raceemit.c -lpthread
 *   cc -O1 -no-pie -DBUG -o raceemit-bug test/proc/raceemit.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int counter;

static void
emit(unsigned char done, unsigned char count)
{
	unsigned char rec[3] = {2, done, count};
	(void) !write(3, rec, sizeof rec);
}

static void *
worker(void *arg)
{
	(void) arg;
#ifndef BUG
	pthread_mutex_lock(&mtx);
#endif
	int			v = counter;
	emit(0, (unsigned char) v);	/* log the value read: a system call */
	counter = v + 1;
#ifndef BUG
	pthread_mutex_unlock(&mtx);
#endif
	return NULL;
}

int
main(void)
{
	pthread_t	a, b;

	emit(0, 0);
	pthread_create(&a, NULL, worker, NULL);
	pthread_create(&b, NULL, worker, NULL);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	emit(1, (unsigned char) counter);
	return 0;
}
