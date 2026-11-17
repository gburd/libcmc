/*
 * test/proc/racepipe.c: two threads share two kernel pipes, no futex and
 * no checker-owned fd (soundness plan 3.8).  The worker writes 'a' to
 * the first pipe and then 'b' to the second; the main thread reads the
 * second pipe and then the first, both non-blocking, and records what it
 * got.  On Linux main can miss 'b' and still read 'a': it reads the
 * second pipe while it is empty, the worker writes both, and main then
 * reads 'a'.
 *
 * The pipes' contents are kernel state the world encoding does not
 * hold, so the checker cannot search these orders soundly, and a thread
 * of a multi-threaded node that calls on a kernel fd is refused (exit 3).
 *
 * Canonical state fd 3: [phase][got], phase 2 once main has read, got =
 * 2 * (read 'a') + (read 'b').
 *
 *   cc -O1 -no-pie -o racepipe test/proc/racepipe.c -lpthread
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

static int	p1[2], p2[2];

static void
emit(int phase, int got)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) got};

	(void) !write(3, rec, sizeof rec);
}

static void *
worker(void *x)
{
	(void) x;
	(void) !write(p1[1], "a", 1);
	(void) !write(p2[1], "b", 1);
	emit(1, 0);
	return NULL;
}

int
main(void)
{
	pthread_t	t;
	unsigned char c;

	pipe2(p1, O_NONBLOCK);
	pipe2(p2, O_NONBLOCK);
	emit(0, 0);
	pthread_create(&t, NULL, worker, NULL);
	int			got_b = read(p2[0], &c, 1) == 1;
	int			got_a = read(p1[0], &c, 1) == 1;

	emit(2, got_a * 2 + got_b);
	pthread_join(t, NULL);
	return 0;
}
