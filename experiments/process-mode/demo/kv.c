/*
 * demo/kv.c: an ordinary one-connection TCP server with two independent
 * one-byte cells, a and b, in a DECLARED STATE REGION (cmc_state) the
 * checker hashes.  Written as it ships: blocking sockets, no libcmc
 * header, no step function.  The checker (../search.c) finds cmc_state by
 * its ELF symbol and model-checks every interleaving of the client's SETs.
 *
 * Protocol, one byte each way on the connection:
 *   client -> 'a'      : set cell a to 1, then acknowledge
 *   client -> 'b'      : set cell b to 1, then acknowledge
 *   server -> 'A'      : done
 *
 * SET a and SET b commute: the orders (a then b) and (b then a) reach the
 * same state, so the checker's fingerprint must merge that diamond.
 *
 * Built with -DBUG, SET a also clears b -- so "set b, then set a" loses
 * b.  The invariant the checker asserts is "a cell once set stays set",
 * which the bug violates only on that interleaving.
 *
 *   cc -O1 -no-pie -o kv     demo/kv.c
 *   cc -O1 -no-pie -DBUG -o kv-bug demo/kv.c
 *
 * -no-pie so cmc_state lands at a fixed address the checker reads with nm.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The declared state region: the checker hashes exactly these bytes.
 * cell 0 = a, cell 1 = b.  Zeroed at startup (it is in BSS; made explicit
 * in main so the contract "the program fully owns and initialises it" is
 * visible). */
unsigned char cmc_state[2];

int
main(int argc, char **argv)
{
	int			port = argc > 1 ? atoi(argv[1]) : 0;

	cmc_state[0] = 0;
	cmc_state[1] = 0;

	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	int			one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (bind(ls, (void *) &a, sizeof a) != 0)
		return 2;
	listen(ls, 1);

	int			cs = accept(ls, 0, 0);
	if (cs < 0)
		return 3;

	for (;;)
	{
		unsigned char m;
		ssize_t		n = read(cs, &m, 1);
		if (n <= 0)
			break;

		if (m == 'a')
		{
			cmc_state[0] = 1;
#ifdef BUG
			cmc_state[1] = 0;	/* the bug: setting a clobbers b */
#endif
		}
		else if (m == 'b')
			cmc_state[1] = 1;
		else
			continue;

		unsigned char ack = 'A';
		if (write(cs, &ack, 1) != 1)
			break;
	}
	return 0;
}
