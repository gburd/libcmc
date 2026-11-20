/*
 * test/proc/midstep.c: node 0 dials node 1, sends "0a" in one write,
 * and then makes a call whose answer on Linux depends on what node 1
 * did meanwhile.
 * Node 1 accepts, reads one byte and closes, leaving bytes unread, so
 * its stack answers with an RST.  The mode, from the target args:
 *   rstwake   poll(POLLIN) blocks until the RST, then reports its
 *             revents: Linux gives POLLIN|POLLERR|POLLHUP (25)
 *   rstewake  the same through epoll_wait (EPOLLIN|EPOLLERR|EPOLLHUP)
 * Node 0 reports the value as node[0][0] and then waits.  Natively, SPIN
 * in the environment makes node 0 spin after its write, standing for
 * its being descheduled, so node 1 acts first.
 *
 *   cc -O1 -no-pie -o midstep test/proc/midstep.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static void
emit(unsigned char v)
{
	unsigned char r[2] = {1, v};

	(void) !write(3, r, 2);
}

static void
spin(void)
{
	if (getenv("SPIN"))
		for (volatile long q = 0; q < 200000000; q++)
			;
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

static int
dial(int port)
{
	struct sockaddr_in a = addr(port);
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
	{
		close(s);
		return -1;
	}
	return s;
}

/* node 1: read one byte of the first connection, close it (an RST, as
 * bytes are left unread), then close every later one */
static void
peer(void)
{
	struct sockaddr_in a = addr(20001);
	int			ls = socket(AF_INET, SOCK_STREAM, 0), x;
	unsigned char b;

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 4);
	x = accept(ls, NULL, NULL);
	(void) !read(x, &b, 1);
	close(x);
	while ((x = accept(ls, NULL, NULL)) >= 0)
		close(x);
}

int
main(int argc, char **argv)
{
	int			id = atoi(argv[1]);
	const char *mode = argc > 2 ? argv[2] : "";
	unsigned char b;

	signal(SIGPIPE, SIG_IGN);
	emit(0);
	if (id == 1)
	{
		peer();
		return 0;
	}
	int			s = dial(20001);

	if (s < 0)
		return 1;
	/* one write, so the RST can only follow it: node 1 reads "0" and
	 * leaves "a" unread */
	(void) !write(s, "0a", 2);
	spin();
	if (!strcmp(mode, "rstwake"))
	{
		struct pollfd p = {s, POLLIN, 0};

		poll(&p, 1, -1);
		emit((unsigned char) p.revents);
	}
	else if (!strcmp(mode, "rstewake"))
	{
		struct epoll_event ev = {EPOLLIN, {0}}, out = {0, {0}};
		int			ep = epoll_create1(0);

		epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev);
		epoll_wait(ep, &out, 1, -1);
		emit((unsigned char) out.events);
	}
	(void) !read(s, &b, 1);
	return 0;
}
