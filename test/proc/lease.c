/*
 * elect/lease demo for process-mode M5b (test/proc).
 *
 * A single leaseholder that may write to shared storage only while its
 * lease is valid.  The lease is valid until `expiry` on the (logical)
 * clock; the checker owns that clock and advances it only when it fires
 * the lease-expiry timeout.  The node blocks on recv waiting for a
 * go-ahead; the checker instead fires the timeout (advancing time past
 * expiry), and the node must then refuse to write.
 *
 * Correct: after waking, the node RE-READS the clock and writes only if
 * now < expiry -- so once the lease expires it never writes.
 *
 * -DBUG: the node caches `now` read before it blocked and decides from
 * that stale value -- so it writes even though its lease expired while it
 * was blocked.  A classic stale-lease / fencing bug, visible only because
 * the checker controls absolute time.
 *
 * Ordinary blocking sockets, no libcmc header.  Node 0 is the only node;
 * it listens on BASE+0 so the checker can drive its recv and its clock.
 * Canonical state on fd 3: [count=2][wrote][expired], 1 = true.
 *
 *   cc -O1 -no-pie -o lease     test/proc/lease.c
 *   cc -O1 -no-pie -DBUG -o lease-bug test/proc/lease.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BASE 20000

static long long
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
emit(int wrote, int expired)
{
	unsigned char rec[3] = {2, (unsigned char) wrote, (unsigned char) expired};
	write(3, rec, sizeof rec);
}

int
main(void)
{
	/* listen so the checker can deliver this node a message or a timeout */
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = {0};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(BASE + 0);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(ls, (struct sockaddr *) &sa, sizeof sa);
	listen(ls, 8);
	int c = accept(ls, 0, 0);

	/* the lease: valid until one clock tick from the start epoch */
	long long start = now_ns();
	long long expiry = start + 1000000000LL;		/* CLK_TICK on the checker */

	emit(0, 0);

	/* block waiting for a go-ahead; the checker will instead fire the
	 * lease-expiry timeout, which advances the logical clock past expiry */
	unsigned char buf[8];
	read(c, buf, sizeof buf);

#ifdef BUG
	/* stale: decide from the clock value read before we blocked */
	long long seen = start;
#else
	/* correct: re-read the clock after waking */
	long long seen = now_ns();
#endif

	int expired = (seen >= expiry);
	int wrote = 0;
	if (seen < expiry)			/* only write while the lease is valid */
		wrote = 1;

	/* report whether the lease had actually expired (true clock) vs what
	 * we did; the checker's invariant is: never wrote after expiry */
	int really_expired = (now_ns() >= expiry);
	emit(wrote, really_expired);
	(void) expired;

	close(c);
	close(ls);
	return 0;
}
