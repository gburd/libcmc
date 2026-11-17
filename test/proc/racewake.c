/*
 * test/proc/racewake.c: a wake sent before the data it announces is
 * stored, the thread race the scheduler exists to find (soundness plan
 * 3.8).  One node, two threads, no network.
 *
 * The main thread starts a worker that waits on a futex word until it
 * is set, then reads `data` and emits what it read.  The main thread
 * sets the word, wakes the worker (FUTEX_WAKE), and stores data = 7.
 * The bug is the order: under -DBUG the store comes after the wake, so
 * a worker that runs between the wake and the store reads 0.  The
 * correct build stores first.
 *
 * The futex calls are raw syscalls so the race sits exactly at system
 * calls, the boundary the checker explores: the worker's step from its
 * woken wait to its emit, and the main thread's step from its wake to
 * its next call, can run in either order.  An engine that runs a woken
 * waiter only after the waker parks never sees the worker read 0.
 *
 * Canonical state fd 3: [phase][seen].  phase 1 once the worker has
 * read, seen is what it read.  The worker's emit is the race's witness.
 *
 *   cc -O1 -no-pie -o racewake     test/proc/racewake.c -lpthread
 *   cc -O1 -no-pie -DBUG -o racewake-bug test/proc/racewake.c -lpthread
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/syscall.h>
#include <unistd.h>

static atomic_int go;			/* the futex word: 1 once data is announced */
static volatile int data;		/* what the worker must read: 7 */

static void
emit(unsigned char phase, unsigned char seen)
{
	unsigned char rec[3] = {2, phase, seen};
	(void) !write(3, rec, sizeof rec);
}

static void *
worker(void *arg)
{
	(void) arg;
	while (atomic_load(&go) == 0)
		syscall(SYS_futex, &go, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
	emit(1, (unsigned char) data);
	return NULL;
}

int
main(void)
{
	pthread_t	t;

	emit(0, 0);
	pthread_create(&t, NULL, worker, NULL);
#ifndef BUG
	data = 7;
#endif
	atomic_store(&go, 1);
	syscall(SYS_futex, &go, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#ifdef BUG
	data = 7;					/* too late: the worker may already have read */
#endif
	pthread_join(t, NULL);
	return 0;
}
