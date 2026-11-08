/*
 * clocks demo for the process front end's interception gate
 * (test/proc_gate.sh).  It exercises the syscalls no other fixture
 * pins: gettimeofday and getrandom, and sendto/recvfrom rather than
 * write/read.  It reports what the checker answered, so the invariants
 * are the contract:
 *
 *   - gettimeofday returns the checker's LOGICAL clock, the same time
 *     clock_gettime gives, advancing one second per fired timeout;
 *   - getrandom returns the engine-chosen fixed pattern (all zero) and
 *     the full length asked for;
 *   - a timeout satisfies a blocked recvfrom, as it does a blocked read.
 *
 * Two nodes.  Node 0 reads the clock and random at start (logical second
 * 1), sends one byte to node 1 with sendto, then blocks in recvfrom;
 * the checker fires a timeout, so node 0 wakes at logical second 2 and
 * reports the second clock read.  Node 1 just receives the byte.
 *
 * Canonical state on fd 3, node 0:
 *   [count=4][tv_sec at start][random byte at start][tv_sec after wake][woke_by_timeout]
 * so node[0] = 1,0,2,1 for a correct checker.
 *
 *   cc -O1 -no-pie -o clocks demo/clocks.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define BASE 20000

static unsigned char st[4];		/* the four reported bytes */

static void
emit(void)
{
	unsigned char rec[5] = {4, st[0], st[1], st[2], st[3]};

	(void) !write(3, rec, sizeof rec);
}

static long
clock_sec(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long) tv.tv_sec;
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

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;

	if (id == 1)
	{
		/* receiver: accept, recvfrom one byte, then block forever so the
		 * only way to wake node 0's recv is a timeout, not our EOF */
		int			ls = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);

		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 4);
		int			c = accept(ls, NULL, NULL);
		unsigned char m[4];

		(void) !recvfrom(c, m, sizeof m, 0, NULL, NULL);
		(void) !recvfrom(c, m, sizeof m, 0, NULL, NULL);	/* never answered */
		return 0;
	}

	/* node 0 */
	unsigned char rnd[8];
	ssize_t		n = getrandom(rnd, sizeof rnd, 0);

	st[0] = (unsigned char) clock_sec();		/* logical second at start */
	st[1] = (n == (ssize_t) sizeof rnd) ? rnd[0] : 0xff;	/* 0 if engine-zeroed */
	st[2] = 0;
	st[3] = 0;
	emit();

	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + 1);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		return 3;
	unsigned char one = 'x';

	(void) !sendto(s, &one, 1, 0, NULL, 0);

	/* block for a reply that never comes; the checker fires a timeout */
	unsigned char buf[4];

	(void) !recvfrom(s, buf, sizeof buf, 0, NULL, NULL);

	st[2] = (unsigned char) clock_sec();		/* logical second after waking */
	st[3] = 1;									/* woke (by the timeout) */
	emit();
	return 0;
}
