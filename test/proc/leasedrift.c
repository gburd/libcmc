/*
 * leasedrift: a lease whose safety depends on bounded clock drift
 * (test/proc, 2.2).
 *
 * Node 1 is a lock server.  Node 0 asks it for a lease of L, reading its
 * own clock first (t_send), and on the grant serves reads until
 * valid_until = t_send + L(1 - rho), waiting out the lease in poll() with
 * a finite timeout.  Node 1 grants, then waits L(1 + rho) and one more
 * millisecond on its own clock in nanosleep() before it may grant the
 * lease again.  Each node reads only its own clock, and the clocks may
 * drift apart by the model's `drift` (per thousand), so the margin is
 * what keeps the two from overlapping: node 0's L(1 - rho) ends first
 * for any drift up to about 180 per thousand.
 *
 * -DBUG: node 0 uses L(1 + rho), the sign error.  On equal clocks it
 * still ends a millisecond before node 1's wait, so the bug shows only
 * when node 0's clock runs slow against node 1's, which the checker
 * finds by firing node 1's deadline with node 0's clock slow.
 *
 * Ordinary blocking sockets, no libcmc header.  Canonical state on fd 3:
 * [count=1][flag], flag = serving (node 0) or granted again (node 1).
 *
 *   cc -O1 -no-pie -o leasedrift     test/proc/leasedrift.c
 *   cc -O1 -no-pie -DBUG -o leasedrift-bug test/proc/leasedrift.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BASE 20000
#define LEASE_MS 1000LL
#define RHO 100					/* per thousand: the drift the protocol allows for */

static long long
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
emit(int flag)
{
	unsigned char rec[2] = {1, (unsigned char) flag};
	write(3, rec, sizeof rec);
}

static int
connect_to(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, (void *) &a, sizeof a) != 0) _exit(3);
	return s;
}

static int
listen_on(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(s, (void *) &a, sizeof a);
	listen(s, 8);
	return s;
}

/* node 0: take the lease, then serve until it ends on our own clock */
static void
holder(void)
{
	int c = connect_to(BASE + 1);
	unsigned char b = 'Q';
	long long t_send = now_ns();
	write(c, &b, 1);
	if (read(c, &b, 1) != 1 || b != 'G') _exit(4);
#ifndef BUG
	long long valid_until = t_send + LEASE_MS * 1000000LL * (1000 - RHO) / 1000;
#else
	long long valid_until = t_send + LEASE_MS * 1000000LL * (1000 + RHO) / 1000;
#endif
	emit(1);
	int fd = c;
	for (;;)
	{
		long long left = valid_until - now_ns();
		if (left <= 0) break;
		struct pollfd p = {fd, POLLIN, 0};
		int r = poll(&p, 1, (int) ((left + 999999) / 1000000));
		/* serve a read; once the server has gone, poll only for the time */
		if (r > 0 && read(fd, &b, 1) == 1) write(fd, "Y", 1);
		else if (r > 0) fd = -1;
	}
	emit(0);
}

/* node 1: grant the lease, outwait it on our own clock, then it is free */
static void
granter(void)
{
	int ls = listen_on(BASE + 1);
	int c = accept(ls, 0, 0);
	unsigned char b;
	if (read(c, &b, 1) != 1 || b != 'Q') _exit(4);
	write(c, "G", 1);
	emit(0);
	long long wait_ns = LEASE_MS * 1000000LL * (1000 + RHO) / 1000 + 1000000LL;
	struct timespec l = {wait_ns / 1000000000LL, wait_ns % 1000000000LL};
	nanosleep(&l, NULL);
	emit(1);
}

int
main(int argc, char **argv)
{
	if (argc > 1 && atoi(argv[1]) == 1) granter();
	else holder();
	return 0;
}
