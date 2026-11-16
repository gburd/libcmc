/*
 * crossrpc.c: two connections between one pair of nodes, one dialled
 * each way.  Each node listens, dials the other and sends a request 'Q'.
 * Node 0 then serves the connection it accepts (answering 'A') before it
 * reads the answer on its own; node 1 reads its answer first, then
 * serves.  Each records the byte read back on the connection it dialled.
 * TCP keeps the two connections apart, so that byte is the other node's
 * 'A', never its 'Q'.
 *
 * A checker that keys channels by node pair, not by connection, hands
 * node 1 node 0's request (queued first from node 0 to node 1, on the
 * other connection) as its answer, a violation no Linux run can
 * produce.
 *
 * Canonical state on fd 3: [count=1][got], got = 0 before the reply,
 * else the byte read.
 *
 *   cc -O1 -no-pie -o crossrpc test/proc/crossrpc.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
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
emit(unsigned char got)
{
	unsigned char rec[2] = {1, got};

	(void) !write(3, rec, sizeof rec);
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in me = addr(BASE + id), other = addr(BASE + 1 - id);

	emit(0);
	bind(ls, (struct sockaddr *) &me, sizeof me);
	listen(ls, 8);

	int out = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(out, (struct sockaddr *) &other, sizeof other) != 0)
		return 3;
	(void) !write(out, "Q", 1);

	unsigned char b;

	if (id == 1 && read(out, &b, 1) == 1)
		emit(b);

	int in = accept(ls, NULL, NULL);

	if (read(in, &b, 1) == 1 && b == 'Q')
		(void) !write(in, "A", 1);
	if (id == 0 && read(out, &b, 1) == 1)
		emit(b);
	return 0;
}
