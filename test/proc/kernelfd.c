/*
 * test/proc/kernelfd.c: calls on kernel fds (pipes) that the checker
 * does not model.  Each mode is a reviewer's probe of the thread
 * scheduler (soundness plan 3.8).
 *
 *   alone    one thread reads an empty pipe and blocks in the kernel.
 *            The checker sees no call from it, so it must end with the
 *            lull error, not wait forever.
 *   block    a worker blocks reading a pipe that the main thread then
 *            writes (a self-pipe).
 *
 * Canonical state fd 3: [one byte], 0 at the start and 1 once a read
 * returned.
 *
 *   cc -O1 -no-pie -o kernelfd test/proc/kernelfd.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static int	p[2];

static void
emit(int v)
{
	unsigned char rec[2] = {1, (unsigned char) v};

	(void) !write(3, rec, sizeof rec);
}

static void *
reader(void *arg)
{
	char		c;

	(void) arg;
	(void) !read(p[0], &c, 1);
	emit(1);
	return NULL;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";
	pthread_t	t;

	(void) !pipe(p);
	emit(0);
	if (!strcmp(mode, "alone"))
	{
		reader(NULL);
		return 0;
	}
	if (!strcmp(mode, "block"))
	{
		pthread_create(&t, NULL, reader, NULL);
		(void) !write(p[1], "x", 1);
		pthread_join(t, NULL);
	}
	return 0;
}
