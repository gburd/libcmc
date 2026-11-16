/*
 * twoconn.c: two connections from one peer.  Node 1 dials node 0 twice,
 * sends 'b' on the second connection and then 'a' on the first.  Node 0
 * accepts both, polls both, and records the first byte it reads, then
 * waits.  TCP orders bytes within a connection, not across two, so node
 * 0 may read 'b' first.
 *
 * A checker that names a connection by its peer merges "waiting on the
 * first connection" with "waiting on the second" and never explores the
 * run in which 'b' arrives first.
 *
 * Canonical state on fd 3: [count=1][first byte read, or 0].
 *
 *   cc -O1 -no-pie -o twoconn test/proc/twoconn.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
emit(int v)
{
	unsigned char rec[2] = {1, (unsigned char) v};

	(void) !write(3, rec, sizeof rec);
}

static int
dial(int id)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);

	return connect(s, (struct sockaddr *) &a, sizeof a) == 0 ? s : -1;
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	struct sockaddr_in me = addr(BASE + id);
	int ls = socket(AF_INET, SOCK_STREAM, 0);

	emit(0);
	bind(ls, (struct sockaddr *) &me, sizeof me);
	listen(ls, 8);
	if (id == 1)
	{
		int a = dial(0), b = dial(0);

		(void) !write(b, "b", 1);
		(void) !write(a, "a", 1);
		(void) accept(ls, NULL, NULL);
		return 0;
	}
	int x = accept(ls, NULL, NULL), y = accept(ls, NULL, NULL);
	struct pollfd p[2] = {{.fd = x, .events = POLLIN}, {.fd = y, .events = POLLIN}};

	fcntl(x, F_SETFL, O_NONBLOCK);
	fcntl(y, F_SETFL, O_NONBLOCK);
	for (;;)
	{
		unsigned char c;

		(void) poll(p, 2, -1);
		for (int i = 0; i < 2; i++)
			if (read(p[i].fd, &c, 1) == 1)
			{
				emit(c);
				(void) accept(ls, NULL, NULL);
				return 0;
			}
	}
}
