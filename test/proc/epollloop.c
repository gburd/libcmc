/*
 * test/proc/epollloop.c: a minimal event-loop node built on epoll, the
 * example for the epoll backend (davros eval section 3, ask 9).  It is
 * written the way libxtc's default Linux backend works: a listening
 * socket set non-blocking, an epoll instance the listener is registered
 * in, epoll_wait as the parking point, and a non-blocking accept/read
 * driven by epoll readiness.  Before libcmc modelled epoll the checker
 * ran the loop on real fds (epoll_create1/epoll_ctl/epoll_wait fell
 * through to the kernel), so the search drove nothing and the pass was a
 * fiction; now it drives the epoll loop through the search like any
 * deferred recv.
 *
 * Two nodes.  Node 1 is a server: it listens, sets the listener
 * non-blocking, registers it in an epoll instance, and loops in
 * epoll_wait waiting for a connection, then a request; it stores the one
 * request byte it receives.  Node 0 is a client: it connects and sends
 * one byte ('x' = 120).  The invariant (epollloop.model) is that once
 * node 1 has received a byte, the byte it stored is the one node 0 sent.
 *
 * The handler is found through the epoll_event.data the loop stored at
 * EPOLL_CTL_ADD: epoll_wait returns that data verbatim, and the loop
 * switches on it to tell the listener from a connection.  Here data.fd
 * holds the fd, so the model must return the stored data for a ready fd,
 * not a pollfd.
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
 *   cc -no-pie -o epollloop test/proc/epollloop.c
 *   cmc --model-file test/proc/epollloop.model --binary ./epollloop
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/epoll.h>
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

static void
ep_add(int ep, int fd)
{
	struct epoll_event ev = {0};

	ev.events = EPOLLIN;
	ev.data.fd = fd;			/* the handler key: epoll_wait returns it */
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
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

	/* server: non-blocking listener registered in an epoll instance */
	st = 0;
	recvd = 0;
	emit();
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + 1);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);
	set_nonblock(ls);

	int ep = epoll_create1(0);

	ep_add(ep, ls);

	int conn = -1;

	for (;;)
	{
		struct epoll_event ev;
		int r = epoll_wait(ep, &ev, 1, -1);

		if (r <= 0)
			continue;
		if (ev.data.fd == ls)
		{
			conn = accept(ls, NULL, NULL);
			if (conn >= 0)
			{
				set_nonblock(conn);
				ep_add(ep, conn);
			}
			continue;
		}
		/* the ready fd is the connection: its data.fd routes us here */
		unsigned char buf[4];
		ssize_t n = read(ev.data.fd, buf, sizeof buf);

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
