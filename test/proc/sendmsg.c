/*
 * test/proc/sendmsg.c: sendmsg() on a kernel socket, an fd below the
 * checker's 500 (2.1 plan, item 1).  The launch child sends the seccomp
 * listener with sendmsg, so 2.0.0's filter let every sendmsg below fd
 * 500 reach the kernel unseen.
 *
 *   worker   a worker thread sends 'a' and then 'c' with sendmsg on a
 *            Unix socketpair, and the main thread sends 'b' on it while
 *            the worker runs.  Once it has joined the worker, main reads
 *            the three bytes and records the middle one.  On Linux main's
 *            'b' can land between the worker's two sends; a checker that
 *            does not see the sends never orders them that way.  The
 *            socket's contents are the kernel's, not the world's, so a
 *            node with more than one thread that does this is refused.
 *            2.0.0 passed it.
 *   alone    one thread sends a byte to itself with sendmsg on a Unix
 *            socketpair and reads it back: a kernel socket in a node
 *            with one thread goes to the kernel.
 *
 * Canonical state fd 3: [phase][byte]: phase 1 once main has read; the
 * byte is the middle one (worker) or the one read back (alone).
 *
 *   cc -O1 -no-pie -o sendmsg test/proc/sendmsg.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int	sv[2];

static void
emit(int phase, int byte)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) byte};

	(void) !write(3, rec, sizeof rec);
}

static void
send_byte(const char *b)
{
	struct iovec v = {.iov_base = (void *) b, .iov_len = 1};
	struct msghdr mh = {.msg_iov = &v, .msg_iovlen = 1};

	(void) !sendmsg(sv[0], &mh, 0);
}

static void *
worker(void *arg)
{
	(void) arg;
	send_byte("a");
	send_byte("c");
	return NULL;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";
	unsigned char got[3] = {0};
	pthread_t	t;

	emit(0, 0);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return 1;
	if (!strcmp(mode, "worker"))
	{
		pthread_create(&t, NULL, worker, NULL);
		send_byte("b");
		pthread_join(t, NULL);
		for (size_t n = 0; n < sizeof got;)
		{
			ssize_t		k = read(sv[1], got + n, sizeof got - n);

			if (k <= 0)
				return 1;
			n += (size_t) k;
		}
		emit(1, got[1]);
	}
	if (!strcmp(mode, "alone"))
	{
		send_byte("x");
		emit(1, read(sv[1], got, 1) == 1 ? got[0] : 0);
	}
	return 0;
}
