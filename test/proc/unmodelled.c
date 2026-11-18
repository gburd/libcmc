/*
 * unmodelled.c: node 0 exercises, in the mode named by its [target] arg,
 * a construct the process engine once answered wrongly.  Without a
 * recognised mode the node does nothing and the check passes, which
 * shows each verdict comes from the construct.
 *
 * Modes:
 *   badptr     write() from an unmapped buffer.  Linux returns EFAULT, so
 *              the node records 2; the engine used to send zero bytes and
 *              report success (1), which the model's invariant forbids.
 *   pollto     poll() with a finite timeout       (refused: exit 3)
 *   epollet    an edge-triggered epoll registration (refused: exit 3)
 *   semaphore  an EFD_SEMAPHORE eventfd           (refused: exit 3)
 *   longrec    a 65-byte state record, past the 64 stored (refused)
 *   partrec    a state record cut short before it parks  (refused)
 *   refused    node 1 exits at once; node 0's later connect() to it is
 *              refused (ECONNREFUSED), so node 0 records 2, never 1
 *   rstwrite   node 1 closes the connection with node 0's second byte
 *              unread, an RST: node 0's read gets ECONNRESET, its write
 *              EPIPE, and the default SIGPIPE ends it before it records
 *   rstignore  the same with SIGPIPE ignored: node 0 records 2
 *   rsthandler the same with a SIGPIPE handler (refused)
 *   fork       node 0 forks; the child exits at once (refused)
 *   writev     a writev() on a checker-owned socket (refused)
 *   sendmsg    a sendmsg() on a checker-owned socket (refused)
 *   peek       a recv() with MSG_PEEK (refused)
 *   nbaccept   an accept4() with SOCK_NONBLOCK (refused)
 *   udp        a UDP socket (refused)
 *   dialexit   node 1 exits at once; node 0 makes 16 checked calls, then
 *              dials it and records 1 if the dial waited (node 1 had not
 *              yet run), 2 if it was refused (node 1 had exited).  Linux
 *              gives either, by which process runs first.  2.0.0 drove
 *              both nodes at once and took one of the two at random.
 *
 *   cc -O1 -no-pie -o unmodelled test/proc/unmodelled.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define BASE 20000

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

static void
on_pipe(int sig)
{
	(void) sig;
}

static void
emit(int v)
{
	unsigned char rec[2] = {1, (unsigned char) v};

	(void) !write(3, rec, sizeof rec);
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";

	emit(0);
	if (!strcmp(mode, "badptr"))
	{
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);

		if (connect(s, (struct sockaddr *) &a, sizeof a) == 0)
			emit(write(s, (void *) 16, 1) < 0 ? 2 : 1);
		return 0;
	}
	if (!strcmp(mode, "pollto"))
	{
		struct pollfd pf = {.fd = socket(AF_INET, SOCK_STREAM, 0), .events = POLLIN};
		struct sockaddr_in a = addr(BASE + 0);

		bind(pf.fd, (struct sockaddr *) &a, sizeof a);
		listen(pf.fd, 8);
		emit(poll(&pf, 1, 100) == 0 ? 1 : 2);
		return 0;
	}
	if (!strcmp(mode, "epollet"))
	{
		int ep = epoll_create1(0), e = eventfd(0, 0);
		struct epoll_event ev = {.events = EPOLLIN | EPOLLET};

		epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev);
		return 0;
	}
	if (!strcmp(mode, "semaphore"))
	{
		(void) eventfd(2, EFD_SEMAPHORE);
		return 0;
	}
	if (!strcmp(mode, "refused"))
	{
		/* node 0 waits for one timeout so node 1 has exited, then dials */
		int id = atoi(argv[1]);
		if (id == 1)
			return 0;
		int ls = socket(AF_INET, SOCK_STREAM, 0), s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in me = addr(BASE + 0), a = addr(BASE + 1);

		bind(ls, (struct sockaddr *) &me, sizeof me);
		listen(ls, 8);
		(void) accept(ls, NULL, NULL);	/* the timer fires */
		emit(connect(s, (struct sockaddr *) &a, sizeof a) == 0 ? 1 : 2);
		return 0;
	}
	if (!strncmp(mode, "rst", 3))
	{
		int id = atoi(argv[1]);
		struct sockaddr_in a = addr(BASE + 1);

		if (id == 1)
		{
			/* take the connection and close it with node 0's byte unread */
			int ls = socket(AF_INET, SOCK_STREAM, 0);

			bind(ls, (struct sockaddr *) &a, sizeof a);
			listen(ls, 8);
			int c = accept(ls, NULL, NULL);
			unsigned char b;

			(void) !read(c, &b, 1);
			close(c);
			return 0;
		}
		if (!strcmp(mode, "rstignore"))
			signal(SIGPIPE, SIG_IGN);
		else if (!strcmp(mode, "rsthandler"))
			signal(SIGPIPE, on_pipe);
		int s = socket(AF_INET, SOCK_STREAM, 0);
		unsigned char b;

		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
			return 0;
		(void) !write(s, "x", 1);
		(void) !write(s, "y", 1);	/* left unread: node 1's close is an RST */
		/* the RST arrives as this read's ECONNRESET, then writes fail */
		int e0 = read(s, &b, 1) < 0 ? errno : 0;
		int e1 = write(s, "z", 1) < 0 ? errno : 0;

		emit(e0 == ECONNRESET && e1 == EPIPE ? 2 : 1);
		return 0;
	}
	if (!strcmp(mode, "writev") || !strcmp(mode, "sendmsg") || !strcmp(mode, "peek") || !strcmp(mode, "nbaccept") || !strcmp(mode, "udp"))
	{
		/* node 0 dials node 1, which serves one connection */
		int id = atoi(argv[1]);
		struct sockaddr_in a = addr(BASE + 1);
		unsigned char b;

		if (!strcmp(mode, "udp"))
		{
			(void) socket(AF_INET, SOCK_DGRAM, 0);
			return 0;
		}
		if (id == 1)
		{
			int ls = socket(AF_INET, SOCK_STREAM, 0);

			bind(ls, (struct sockaddr *) &a, sizeof a);
			listen(ls, 8);
			int c = !strcmp(mode, "nbaccept") ? accept4(ls, NULL, NULL, SOCK_NONBLOCK) : accept(ls, NULL, NULL);

			if (!strcmp(mode, "peek"))
				(void) !recv(c, &b, 1, MSG_PEEK);
			(void) !read(c, &b, 1);
			return 0;
		}
		int s = socket(AF_INET, SOCK_STREAM, 0);

		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
			return 0;
		if (!strcmp(mode, "writev") || !strcmp(mode, "sendmsg"))
		{
			struct iovec v = {.iov_base = "x", .iov_len = 1};
			struct msghdr mh = {.msg_iov = &v, .msg_iovlen = 1};

			if (!strcmp(mode, "writev"))
				(void) !writev(s, &v, 1);
			else
				(void) !sendmsg(s, &mh, 0);
		}
		else
			(void) !write(s, "x", 1);
		return 0;
	}
	if (!strcmp(mode, "dialexit"))
	{
		int id = atoi(argv[1]), s;
		struct sockaddr_in a = addr(BASE + 1);
		unsigned char b;

		if (id == 1)
			return 0;
		for (int i = 0; i < 16; i++)
			(void) !getrandom(&b, 1, 0);
		s = socket(AF_INET, SOCK_STREAM, 0);
		emit(connect(s, (struct sockaddr *) &a, sizeof a) == 0 ? 1 : 2);
		return 0;
	}
	if (!strcmp(mode, "fork"))
	{
		/* the child exits at once, so even an unrefused fork leaves no
		 * process behind */
		if (fork() == 0)
			_exit(0);
		emit(1);
		return 0;
	}
	if (!strcmp(mode, "longrec") || !strcmp(mode, "partrec"))
	{
		/* a record whose count byte promises 65 bytes: written whole, or
		 * cut to its first ten bytes, then the node parks in accept() */
		unsigned char rec[66];
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 0);

		memset(rec, 1, sizeof rec);
		rec[0] = 65;
		(void) !write(3, rec, !strcmp(mode, "longrec") ? sizeof rec : 10);
		bind(s, (struct sockaddr *) &a, sizeof a);
		listen(s, 8);
		(void) accept(s, NULL, NULL);
		return 0;
	}
	return 0;
}
