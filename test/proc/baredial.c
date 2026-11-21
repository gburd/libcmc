/*
 * test/proc/baredial.c: node 0's mid-step connect or non-blocking
 * accept, whose outcome another node can change with nothing sent to
 * it.  The mode, from the target args:
 *   redial  node 0 dials node 1 (no bytes) and dials again: node 1
 *           accepts and exits, so the second dial is refused (1) or
 *           not (2)
 *   dialbk  node 0 dials node 1 (no bytes), then a non-blocking accept
 *           on its own listener: node 1 accepts and dials back (1), or
 *           not yet (2)
 *   pollbk  as dialbk, but a zero-timeout poll(POLLIN) of the listener
 *           instead of the accept
 *   relay   three nodes: node 0 writes "q" to node 2, then dials node
 *           1; node 2 tells node 1, which exits, so the dial is refused
 *           (1) or not (2)
 * Node 0 reports the value as node[0][0] and then waits.  Natively,
 * SPIN in the environment makes node 0 spin before the second call,
 * standing for its being descheduled: Linux gives 1 then, 2 without.
 *
 *   cc -O1 -no-pie -o baredial test/proc/baredial.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
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
		for (volatile long q = 0; q < 300000000; q++)
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
lsn(int port)
{
	struct sockaddr_in a = addr(port);
	int			one = 1, ls = socket(AF_INET, SOCK_STREAM, 0);

	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 4);
	return ls;
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

/* nodes 1 and 2 */
static void
peer(int id, const char *mode)
{
	unsigned char b;
	int			ls = lsn(20000 + id), x = accept(ls, NULL, NULL);

	if (!strcmp(mode, "relay") && id == 2)
	{
		(void) !read(x, &b, 1);
		(void) !write(dial(20001), "x", 1);
		(void) !read(x, &b, 1);
	}
	else if (!strcmp(mode, "relay"))
		(void) !read(x, &b, 1);
	else if (!strcmp(mode, "dialbk") || !strcmp(mode, "pollbk"))
		(void) !read(dial(20000), &b, 1);
	exit(0);
}

int
main(int argc, char **argv)
{
	int			id = atoi(argv[1]), ls = -1, s;
	const char *mode = argc > 2 ? argv[2] : "";
	unsigned char b;

	signal(SIGPIPE, SIG_IGN);
	emit(0);
	if (id != 0)
		peer(id, mode);
	if (!strcmp(mode, "dialbk") || !strcmp(mode, "pollbk"))
		ls = lsn(20000);
	if (!strcmp(mode, "relay"))
	{
		sleep(1);
		s = dial(20002);
		(void) !write(s, "q", 1);
	}
	else
		s = dial(20001);
	if (s < 0)
		return 1;
	spin();
	if (!strcmp(mode, "dialbk"))
	{
		fcntl(ls, F_SETFL, O_NONBLOCK);
		emit(accept(ls, NULL, NULL) >= 0 ? 1 : 2);
	}
	else if (!strcmp(mode, "pollbk"))
	{
		struct pollfd p = {ls, POLLIN, 0};

		poll(&p, 1, 0);
		emit(p.revents & POLLIN ? 1 : 2);
	}
	else
		emit(dial(20001) < 0 ? 1 : 2);
	(void) !read(s, &b, 1);
	return 0;
}
