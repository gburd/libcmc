/*
 * accept_timeout.c: a server idle in accept() can have its election
 * timer fire.  It pins the engine capability real Raft needs: a node
 * waiting for a connection, not for a message, is still timeout-eligible.
 *
 * The node sits in accept().  With a timeout budget, the checker fires
 * the timer: the node accepts a synthetic timer connection and reads the
 * one byte 'T', then records that it fired.  never_fires is deliberately
 * false, so the right verdict is a violation -- proof the timeout reached
 * an accept-blocked node.
 *
 * Canonical state on fd 3: [count=1][fired].
 *
 *   cc -no-pie -o accept_timeout test/proc/accept_timeout.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char fired;

static void
emit(void)
{
	unsigned char r[2] = {1, fired};

	(void) !write(3, r, sizeof r);
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;

	emit();
	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) (BASE + id));
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 4);
	int			c = accept(ls, NULL, NULL);

	if (c < 0)
		return 0;
	unsigned char m[1];

	if (read(c, m, 1) == 1 && m[0] == 'T')
	{
		fired = 1;
		emit();
	}
	return 0;
}
