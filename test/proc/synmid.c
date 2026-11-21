/*
 * test/proc/synmid.c: three nodes; no time passes between two calls of
 * one step.  Node 1 sleeps, dials node 0 (blocking, A) and node 2
 * (non-blocking, B), writes "x" and "y" to A, then reads B's SO_ERROR.
 * Nothing between the connect of B and the getsockopt waits, so on
 * Linux B is still connecting (SO_ERROR 0) and no time has passed.
 * Node 1 reports, as node[1][0] and node[1][1], 1 for ETIMEDOUT, 2 for
 * 0 or 3 for another error, and the CLOCK_MONOTONIC seconds between
 * the connect and the getsockopt.  A crash of node 2's host or a cut
 * link starts B's 127 s SYN deadline; the mode, from the target args:
 *   own    as above: B's own deadline must not fire mid-step
 *   other  node 2 sleeps 50 s before it listens and node 1 does not
 *          wait first: node 2's deadline must not fire mid-step either
 *
 *   cc -O1 -no-pie -o synmid test/proc/synmid.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void
emit(unsigned char v, unsigned char w)
{
	unsigned char r[3] = {2, v, w};

	(void) !write(3, r, 3);
}

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	return a;
}

int
main(int argc, char **argv)
{
	int			id = atoi(argv[1]), other = argc > 2 && !strcmp(argv[2], "other");
	struct timespec pause = {other ? 0 : 1, other ? 1000 : 0}, wait = {50, 0}, t0, t1;
	struct sockaddr_in a0 = addr(20000), a2 = addr(20002);
	int			e = 0, s0, s2, x;
	socklen_t	l = sizeof e;
	char		b[4];

	emit(0, 0);
	if (id != 1)
	{
		struct sockaddr_in a = addr(20000 + id);
		int			ls = socket(AF_INET, SOCK_STREAM, 0);

		if (id == 2 && other)
			nanosleep(&wait, NULL);
		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 4);
		while ((x = accept(ls, NULL, NULL)) >= 0)
			while (read(x, b, 4) > 0)
				;
		return 0;
	}
	nanosleep(&pause, NULL);
	s0 = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(s0, (struct sockaddr *) &a0, sizeof a0) != 0)
		return 1;
	s2 = socket(AF_INET, SOCK_STREAM, 0);
	fcntl(s2, F_SETFL, O_NONBLOCK);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	connect(s2, (struct sockaddr *) &a2, sizeof a2);
	(void) !write(s0, "x", 1);
	(void) !write(s0, "y", 1);
	getsockopt(s2, SOL_SOCKET, SO_ERROR, &e, &l);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	emit(e == ETIMEDOUT ? 1 : e == 0 ? 2 : 3, (unsigned char) (t1.tv_sec - t0.tv_sec));
	(void) !read(s0, b, 1);
	return 0;
}
