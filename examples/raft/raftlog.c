/*
 * raftlog.c: Raft log replication, checked as it ships (examples/raft).
 *
 * The companion to raft.c (leader election): this is the log-replication
 * half, and it checks Raft's State Machine Safety for a committed entry
 * -- a committed log entry is never lost, even if the leader crashes.
 * This is the failure Marc Brooker (AWS) animates: an acknowledged
 * write lost because the leader crashed before it was durable on a
 * quorum (https://brooker.co.za/blog/2026/10/23/animations.html).
 *
 * Reference: Diego Ongaro, raft.tla (https://github.com/ongardie/raft.tla);
 * State Machine Safety and Leader Completeness are Appendix B of his
 * dissertation.
 *
 * Three nodes over ordinary blocking TCP, no libcmc header; node i
 * listens on 127.0.0.1 port 20000 + i.  Node 0 is the leader (term 1),
 * nodes 1 and 2 followers.  A quorum is two of three.  A client submits
 * one command.  Raft commits an entry only once it is stored on a quorum
 * (the leader plus one follower); only then does the leader acknowledge
 * the client and apply it.
 *
 * Messages (3 bytes; the client submit is 2 bytes):
 *   SUBMIT  'W' v        client -> leader
 *   APPEND  'A' v term   leader -> follower (store in log, ack)
 *   reply 'k' = appended; client reply 'A' v = committed/acknowledged.
 *
 * Canonical state fd 3 (volatile log entry held) and fd 193 (committed,
 * durable): [count=2][term][committed value], value 0 = none.  A crash
 * keeps only the durable (committed) record.
 *
 * -DBUG: the leader commits and acknowledges the client as soon as the
 * entry is in its own log, before a quorum has it.  If the leader then
 * crashes, the committed entry is on no node's durable state -- a lost
 * committed entry, violating State Machine Safety.  cmc finds it.
 *
 *   cc -no-pie -o raftlog examples/raft/raftlog.c
 *   cmc --model-file examples/raft/raftlog.model --binary ./raftlog --crashes 1
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000
#define NFOLLOW 2

static int		id;
static unsigned char term = 1;
static unsigned char committed;	/* committed log value, 0 = none */

static void
emit_committed(void)
{
	unsigned char rec[3] = {2, term, committed};

	(void) !write(193, rec, sizeof rec);	/* durable: a committed entry */
	(void) !write(3, rec, sizeof rec);
}

#ifdef BUG
static void
emit_volatile(unsigned char v)
{
	unsigned char rec[3] = {2, term, v};

	(void) !write(3, rec, sizeof rec);		/* in log, not yet committed */
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

/* append the entry to the followers; return how many stored it */
static int
append_to_followers(unsigned char v)
{
	unsigned char req[3] = {'A', v, term}, rep[3];
	int			acks = 0;

	for (int f = 1; f <= NFOLLOW; f++)
		if (rpc(f, req, rep) == 0 && rep[0] == 'k')
			acks++;
	return acks;
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	committed = 0;
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
		unsigned char m[3], r[3] = {'k', 0, 0};
		ssize_t		n = read(c, m, 3);

		if (n == 2 && m[0] == 'W')		/* client submit to the leader */
		{
			unsigned char v = m[1];
			unsigned char ack[2] = {'A', v};

#ifdef BUG
			/* commit and ack as soon as the entry is in the leader's log,
			 * before a quorum has it: a crash now loses a committed entry */
			emit_volatile(v);
			(void) !write(c, ack, 2);
			close(c);
			if (append_to_followers(v) >= 1)
			{
				committed = v;			/* commits late, too late if crashed */
				emit_committed();
			}
#else
			/* Raft: an entry is committed only once a quorum (the leader
			 * plus one follower) stores it; then acknowledge the client */
			if (append_to_followers(v) >= 1)
			{
				committed = v;			/* committed on a quorum */
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

		if (m[0] == 'A')				/* APPEND: a follower stores the entry */
		{
			if (m[2] >= term)
			{
				term = m[2];
				committed = m[1];		/* the follower's store is durable */
				emit_committed();
			}
			r[0] = 'k';
		}
		(void) !write(c, r, 3);
		close(c);
	}
	return 0;
}
