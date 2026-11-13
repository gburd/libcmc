/*
 * test/proc/pollloop.c: a minimal event-loop node, the example for
 * async-I/O support (docs/plans/async-io.md).  It is written the way an
 * event-loop runtime works: a listening socket set non-blocking, a
 * poll() over it, and a non-blocking accept/read driven by poll
 * readiness.  Before libcmc modelled poll/fcntl the checker died in
 * drive_quiescent ("blocked in a call the checker does not model"); now
 * it drives the poll loop through the search like any deferred recv.
 *
 * Two nodes.  Node 1 is a server: it listens, sets the listener
 * non-blocking, and loops in poll() waiting for a connection, then a
 * request; it stores the one request byte it receives.  Node 0 is a
 * client: it connects and sends one byte ('x' = 120).  The invariant
 * (pollloop.model) is that once node 1 has received a byte, the byte it
 * stored is the one node 0 sent.
 *
 * Canonical state fd 3, node 1: [recvd][stored request byte].  recvd is
 * 0 until a read delivers a byte, so the invariant can tell "nothing
 * received yet" from "received and stored".
 *
 * -DBUG: the server stores a fixed wrong byte (0) instead of the byte it
 * read, so once it has received a request the stored byte is not what
 * the client sent.  The invariant catches it; cmc prints the shortest
 * schedule.
 *
 *   cc -no-pie -o pollloop test/proc/pollloop.c
 *   cmc --model-file test/proc/pollloop.model --binary ./pollloop
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char st;		/* the stored request byte */
static unsigned char recvd;		/* 1 once a read has delivered a byte */

static void
emit(void)
{
	unsigned char rec[3] = {2, recvd, st};

	(void) !write(3, rec, sizeof rec);
}

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a = {0};

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	return a;
}

static void
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);

	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;

	if (id == 0)
	{
		/* client: connect and send one byte */
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);

		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
			return 3;
		unsigned char b = 'x';

		(void) !write(s, &b, 1);
		return 0;
	}

	/* server: non-blocking listener, poll loop */
	st = 0;
	recvd = 0;
	emit();
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + 1);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);
	set_nonblock(ls);

	int conn = -1;

	for (;;)
	{
		struct pollfd pf;

		pf.fd = conn < 0 ? ls : conn;
		pf.events = POLLIN;
		pf.revents = 0;
		int r = poll(&pf, 1, -1);

		if (r <= 0)
			continue;
		if (conn < 0)
		{
			conn = accept(ls, NULL, NULL);
			if (conn >= 0)
				set_nonblock(conn);
			continue;
		}
		unsigned char buf[4];
		ssize_t n = read(conn, buf, sizeof buf);

		if (n > 0)
		{
#ifdef BUG
			st = 0;				/* stores a fixed wrong byte, not buf[0] */
#else
			st = buf[0];
#endif
			recvd = 1;
			emit();
			break;
		}
		if (n == 0)
			break;			/* EOF */
	}
	return 0;
}
