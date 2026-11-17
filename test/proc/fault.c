/*
 * test/proc/fault.c: a node that kills itself with abort(), the way a
 * failed assert does.  The node is ptrace-seized, so the SIGABRT parks
 * the thread in a signal-delivery stop instead of ending the process;
 * the checker must notice the stop, end the node, and report the fault,
 * not wait for a system call that never comes.
 *
 *   main     the main thread emits, then aborts
 *   worker   a second thread emits, then aborts; the main thread waits
 *            in pthread_join, so the stop is on the worker's tid
 *
 * Canonical state fd 3: [one byte], 1 once the aborting thread has run.
 *
 *   cc -O1 -no-pie -o fault test/proc/fault.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
emit(int v)
{
	unsigned char rec[2] = {1, (unsigned char) v};

	(void) !write(3, rec, sizeof rec);
}

static void *
worker(void *arg)
{
	(void) arg;
	emit(1);
	abort();
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";
	pthread_t	t;

	emit(0);
	if (!strcmp(mode, "main"))
	{
		emit(1);
		abort();
	}
	pthread_create(&t, NULL, worker, NULL);
	pthread_join(t, NULL);
	return 0;
}
