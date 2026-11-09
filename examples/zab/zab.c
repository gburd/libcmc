/*
 * zab.c: ZooKeeper Atomic Broadcast (ZAB), checked as it ships
 * (examples/zab).  ZAB is the primary-backup atomic broadcast at the
 * heart of ZooKeeper: a leader proposes a transaction, the followers
 * acknowledge, and the leader commits -- and only then acknowledges the
 * client -- once a quorum has the transaction on durable storage.  The
 * safety property checked here is that a committed (client-acknowledged)
 * transaction is never lost, even if the leader crashes.
 *
 * Reference: Junqueira, Reed, Serafini, "Zab: High-performance broadcast
 * for primary-backup systems", DSN 2011.  The canonical TLA+ spec is
 * Apache ZooKeeper's Zab.tla
 * (apache/zookeeper, zookeeper-specifications/protocol-spec/Zab.tla),
 * whose named invariants include Leadership (at most one leader per
 * epoch), PrefixConsistency, Agreement and TotalOrder.  This miniature
 * checks the commit-durability property that underlies Agreement: a
 * delivered (committed) transaction survives.
 *
 * Three nodes over ordinary blocking TCP, no libcmc header; node i
 * listens on 127.0.0.1 port 20000 + i.  Node 0 is the leader; nodes 1
 * and 2 are followers.  A quorum is 2 of 3 (the leader plus one
 * follower).  A client submits one transaction to the leader.
 *
 * Commit rule: the leader proposes the transaction to the followers and
 * commits it -- storing it durably and acknowledging the client -- only
 * once a quorum (here: one follower, plus itself) has acknowledged
 * storing it.
 *
 * Messages, request/reply on a fresh connection (3 bytes; the client
 * submit is 2 bytes):
 *   SUBMIT   'W' v        client -> leader
 *   PROPOSE  'P' v epoch  leader -> follower (store and ack)
 *   reply 'a' = stored; client reply 'A' v = committed/acknowledged.
 *
 * Canonical state fd 3 (volatile) and fd 193 (durable): [count=2][epoch]
 * [value], value 0 = none.  A crash keeps only the durable record.
 *
 * -DBUG: the leader acknowledges the client as soon as the transaction
 * is in its own memory, before any follower has stored it.  If the
 * leader then crashes, the committed transaction is on no node's durable
 * state -- a lost commit.  cmc finds it.
 *
 *   cc -no-pie -o zab examples/zab/zab.c
 *   cmc --model-file examples/zab/zab.model --binary ./zab --crashes 1
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000
#define NFOLLOW 2				/* followers are nodes 1 and 2 */

static int		id;
static unsigned char epoch = 1;
static unsigned char value;		/* committed transaction value, 0 = none */

static void
emit_committed(void)
{
	unsigned char rec[3] = {2, epoch, value};

	(void) !write(193, rec, sizeof rec);	/* durable: survives a crash */
	(void) !write(3, rec, sizeof rec);
}

#ifdef BUG
static void
emit_volatile(unsigned char v)
{
	unsigned char rec[3] = {2, epoch, v};

	(void) !write(3, rec, sizeof rec);		/* in memory only, not committed */
}
#endif

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
read_full(int fd, unsigned char *m, int n)
{
	int			got = 0;

	while (got < n)
	{
		ssize_t		r = read(fd, m + got, (size_t) (n - got));

		if (r <= 0)
			return -1;
		got += (int) r;
	}
	return 0;
}

static int
rpc(int to, const unsigned char req[3], unsigned char rep[3])
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + to);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		return -1;
	(void) !write(s, req, 3);
	int			r = read_full(s, rep, 3);

	close(s);
	return r;
}

/* propose to the followers; return how many acknowledged storing it */
static int
propose_to_quorum(unsigned char v)
{
	unsigned char req[3] = {'P', v, epoch}, rep[3];
	int			acks = 0;

	for (int f = 1; f <= NFOLLOW; f++)
		if (rpc(f, req, rep) == 0 && rep[0] == 'a')
			acks++;
	return acks;
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	value = 0;
	emit_committed();

	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);

	for (;;)
	{
		int			c = accept(ls, NULL, NULL);

		if (c < 0)
			break;
		unsigned char m[3], r[3] = {'a', 0, 0};
		ssize_t		n = read(c, m, 3);

		if (n == 2 && m[0] == 'W')		/* client submit to the leader */
		{
			unsigned char v = m[1];
			unsigned char ack[2] = {'A', v};

#ifdef BUG
			/* acknowledge the client before any follower has stored it:
			 * a crash of the leader now loses a committed transaction */
			emit_volatile(v);
			(void) !write(c, ack, 2);
			close(c);
			if (propose_to_quorum(v) >= 1)
			{
				value = v;				/* commits late, too late if crashed */
				emit_committed();
			}
#else
			/* ZAB: commit only after a quorum (one follower + self) has
			 * stored the transaction, then acknowledge the client */
			if (propose_to_quorum(v) >= 1)
			{
				value = v;				/* committed on a quorum */
				emit_committed();
				(void) !write(c, ack, 2);
			}
			close(c);
#endif
			continue;
		}
		if (n != 3)
		{
			close(c);
			continue;
		}

		if (m[0] == 'P')				/* PROPOSE: a follower stores it */
		{
			if (m[2] >= epoch)
			{
				epoch = m[2];
				value = m[1];
				emit_committed();		/* the follower's store is durable */
			}
			r[0] = 'a';
		}
		(void) !write(c, r, 3);
		close(c);
	}
	return 0;
}
