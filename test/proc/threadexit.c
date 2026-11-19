/*
 * test/proc/threadexit.c: a thread that ends by pthread_exit (or C11
 * thrd_exit) instead of returning from its start function.  POSIX and
 * C11 give its join the same edge either way: everything the thread did
 * happens before the join returns.  The mode, from the target args:
 *
 *   exit     the worker stores the payload and calls pthread_exit; main
 *            joins it and reads the payload.  No race.
 *   thrd     the same through thrd_create, thrd_exit and thrd_join.
 *   nested   the worker calls pthread_exit from a function it called,
 *            not from its start function.  No race.
 *   early    main reads the payload before the join: a race, which an
 *            edge taken from the exit must not hide.
 *
 * Canonical state fd 3: [phase][value], phase 2 once main has read.
 *
 *   cc -O1 -fno-pie -fsanitize=thread -c test/proc/threadexit.c
 *   cc -no-pie -o threadexit threadexit.o libcmc-race.a -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

static int	payload;
static volatile int sink;	/* keeps the early read: payload is only ever 0 or 7 */
static const char *mode = "exit";

static void
emit(int phase, int value)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) value};

	(void) !write(3, rec, sizeof rec);
}

static void
finish(void)
{
	payload = 7;
	pthread_exit(NULL);
}

static void *
worker(void *arg)
{
	(void) arg;
	if (strcmp(mode, "nested") == 0)
		finish();
	payload = 7;
	pthread_exit(NULL);
}

static int
cworker(void *arg)
{
	(void) arg;
	payload = 7;
	thrd_exit(0);
}

int
main(int argc, char **argv)
{
	int			seen;

	if (argc > 2)
		mode = argv[2];
	emit(0, 0);
	if (strcmp(mode, "thrd") == 0)
	{
		thrd_t		t;

		thrd_create(&t, cworker, NULL);
		thrd_join(t, NULL);
		seen = payload;
	}
	else
	{
		pthread_t	t;

		pthread_create(&t, NULL, worker, NULL);
		if (strcmp(mode, "early") == 0)
		{
			sink = payload;
			pthread_join(t, NULL);
			seen = 7;
		}
		else
		{
			pthread_join(t, NULL);
			seen = payload;
		}
	}
	emit(2, seen);
	return 0;
}
