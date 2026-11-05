/*
 * chain probe for process-mode M7 (test/proc).
 *
 * An engine probe for a topology that is not primary/backup: node 0
 * sends a value to node 1, which forwards it to node 2.  `not_reached`
 * says node 2 never holds the value; it is deliberately false, so the
 * right verdict is a violation.  A checker that pairs every accepted
 * connection with node 0 never delivers node 1's forward, and wrongly
 * passes.
 *
 * Canonical state on fd 3: [count=1][value], 0 = nothing yet.
 *
 *   cc -O1 -no-pie -o chain test/proc/chain.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char value;

static void
emit(void)
{
	unsigned char rec[2] = {1, value};
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

static int
send_to(int node, unsigned char v)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + node);
	if (connect(s, (void *) &a, sizeof a) != 0)
		return -1;
	unsigned char m[2] = {'V', v};
	(void) !write(s, m, 2);
	return 0;
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	emit();
	if (id == 0)
		return send_to(1, 7) ? 3 : 0;

	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);
	bind(ls, (void *) &a, sizeof a);
	listen(ls, 4);
	int c = accept(ls, 0, 0);
	unsigned char m[2];
	if (read(c, m, 2) != 2 || m[0] != 'V')
		return 0;
	value = m[1];
	emit();
	if (id == 1)
		return send_to(2, value) ? 3 : 0;
	return 0;
}
