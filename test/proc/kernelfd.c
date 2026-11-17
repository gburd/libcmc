/*
 * test/proc/kernelfd.c: calls on kernel fds (pipes) that the checker
 * does not model.  Each mode is a reviewer's probe of the thread
 * scheduler (soundness plan 3.8).
 *
 *   alone    one thread reads an empty pipe and blocks in the kernel.
 *            The checker sees no call from it, so it must end with the
 *            lull error, not wait forever.
 *   block    a worker blocks reading a pipe that the main thread then
 *            writes (a self-pipe).  A thread of a multi-threaded node
 *            uses a kernel fd, which is refused.
 *   order    two workers each write their letter to one pipe; the main
 *            thread joins both and reads the first letter.  The pipe's
 *            contents, not memory, tell the two orders apart, and the
 *            world does not hold them; refused.
 *   mixed    a worker writes a pipe, then an eventfd; another reads
 *            both, non-blocking.  Refused for its pipe.
 *   stdio    a worker prints to stdout and stderr and emits 1: allowed,
 *            since nothing reads them back.
 *
 * Canonical state fd 3: [one byte], 0 at the start and 1 once a read
 * returned (in order, the letter read).
 *
 *   cc -O1 -no-pie -o kernelfd test/proc/kernelfd.c -lpthread
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int	p[2], efd;

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

static void *
writer(void *arg)
{
	(void) !write(p[1], arg, 1);
	return NULL;
}

static void *
pipe_then_efd(void *arg)
{
	uint64_t	one = 1;

	(void) arg;
	(void) !write(p[1], "a", 1);
	(void) !write(efd, &one, 8);
	return NULL;
}

static void *
read_both(void *arg)
{
	unsigned char c;
	uint64_t	v;

	(void) arg;
	int			gp = read(p[0], &c, 1) == 1;
	int			ge = read(efd, &v, 8) == 8;

	emit(gp * 2 + ge);
	return NULL;
}

static void *
print(void *arg)
{
	(void) arg;
	printf("a worker prints\n");
	fflush(stdout);
	fprintf(stderr, "a worker prints to stderr\n");
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
	if (!strcmp(mode, "order"))
	{
		pthread_t	u;
		unsigned char c = 0;

		pthread_create(&t, NULL, writer, "a");
		pthread_create(&u, NULL, writer, "b");
		pthread_join(t, NULL);
		pthread_join(u, NULL);
		(void) !read(p[0], &c, 1);
		emit(c);
	}
	if (!strcmp(mode, "mixed"))
	{
		pthread_t	u;

		(void) !pipe2(p, O_NONBLOCK);
		efd = eventfd(0, EFD_NONBLOCK);
		pthread_create(&t, NULL, pipe_then_efd, NULL);
		pthread_create(&u, NULL, read_both, NULL);
		pthread_join(t, NULL);
		pthread_join(u, NULL);
	}
	if (!strcmp(mode, "stdio"))
	{
		pthread_create(&t, NULL, print, NULL);
		pthread_join(t, NULL);
	}
	return 0;
}
