/*
 * raft.c: Raft leader election, checked as it ships (examples/raft).
 *
 * Three nodes, each its own process over ordinary blocking TCP, no
 * libcmc header.  Node i listens on 127.0.0.1 port 20000 + i.  This is
 * the election half of Raft -- the part that proves Election Safety: at
 * most one leader per term.  (Log replication is a separate example.)
 *
 * Each node is a follower that sits in accept(), waiting.  The checker
 * drives time: a follower whose election timer fires (a TIMEOUT the
 * checker chooses) becomes a candidate -- it bumps its term, votes for
 * itself, and dials the other two with RequestVote.  A node grants its
 * vote at most once per term, to the first candidate whose term is at
 * least its own.  A candidate with a majority (2 of 3) becomes leader.
 * An incoming RequestVote or AppendEntries with a higher term makes the
 * node step down to follower and adopt that term.
 *
 * Because the checker owns the timers and the message order, it explores
 * every election race: simultaneous candidates, split votes, a candidate
 * that crashes mid-election, a lost vote.  Election Safety must hold in
 * all of them.
 *
 * Messages, 3 bytes, request/reply on a fresh connection (Raft RPCs):
 *   RequestVote   'V' term _          -> reply 'v' term granted(0|1)
 *   AppendEntries 'A' term _          -> reply 'a' term _   (heartbeat)
 *
 * Persistent Raft state survives a crash, so it is emitted on fd 193:
 * currentTerm and votedFor.  Both the volatile (fd 3) and the durable
 * (fd 193) records use one layout, [count=3][role][currentTerm]
 * [votedFor+1]; a recovering node is always a follower, so its durable
 * role byte is 0.  role 0 = follower, 1 = candidate, 2 = leader;
 * votedFor -1 (none) is stored as 0, a node id as id+1.
 *
 * -DBUG: a node grants a vote without recording votedFor, so it can vote
 * for two different candidates in the same term.  Both can then reach a
 * majority and become leader in that term -- the classic election bug.
 * cmc finds it.
 *
 *   cc -no-pie -o raft examples/raft/raft.c
 *   cmc --model-file examples/raft/raft.model --binary ./raft
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000
#define N 3
#define MAJ 2

static int id;
static unsigned char role;		/* 0 follower, 1 candidate, 2 leader */
static unsigned char term;		/* currentTerm (persistent) */
static int votedFor = -1;		/* persistent */

static void
emit(void)
{
	/* Volatile (fd 3) carries the role; durable (fd 193) is Raft's
	 * persistent state -- currentTerm and votedFor -- and a recovering
	 * node is always a follower, so its durable role byte is 0.  Both
	 * use the same layout so node[i] reads the same after a crash. */
	unsigned char vol[4] = {3, role, term, (unsigned char) (votedFor + 1)};
	unsigned char dur[4] = {3, 0, term, (unsigned char) (votedFor + 1)};

	(void) !write(3, vol, sizeof vol);
	(void) !write(193, dur, sizeof dur);
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

/* one RequestVote RPC to peer `to`; returns 1 if the vote was granted */
static int
request_vote(int to)
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + to);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		return 0;
	unsigned char req[3] = {'V', term, (unsigned char) id}, rep[3];

	(void) !write(s, req, 3);
	int			ok = read_full(s, rep, 3) == 0;

	close(s);
	if (ok && rep[0] == 'v')
	{
		if (rep[1] > term)		/* saw a higher term: step down */
		{
			term = rep[1];
			role = 0;
			votedFor = -1;
			emit();
			return 0;
		}
		return rep[2] == 1;
	}
	return 0;
}

/* become a candidate and run one election round */
static void
start_election(void)
{
	term = term + 1;
	role = 1;
	votedFor = id;				/* vote for self */
	emit();
	int			votes = 1;

	for (int p = 0; p < N; p++)
		if (p != id && role == 1)
			votes += request_vote(p);
	if (role == 1 && votes >= MAJ)
	{
		role = 2;				/* won: leader for this term */
		emit();
	}
}

/* serve one incoming RPC on an accepted connection */
/* serve one RPC (3 bytes in m) and write the 3-byte reply on c */
static void
serve(int c, const unsigned char *m)
{
	unsigned char r[3] = {0, term, 0};
	unsigned char mterm = m[1];

	if (mterm > term)			/* higher term: step down and adopt it */
	{
		term = mterm;
		role = 0;
		votedFor = -1;
		emit();
	}
	if (m[0] == 'V')			/* RequestVote(term, candidateId=m[2]) */
	{
		int			grant = 0;
		int			cand = m[2];

		/* Raft: grant iff the candidate's term is current and we have not
		 * yet voted this term for anyone else */
		if (mterm >= term && (votedFor == -1 || votedFor == cand))
		{
			grant = 1;
#ifndef BUG
			votedFor = cand;	/* record whom we voted for this term */
#endif
			term = mterm;
			emit();
		}
		r[0] = 'v';
		r[1] = term;
		r[2] = (unsigned char) grant;
	}
	else						/* 'A': AppendEntries heartbeat */
	{
		if (mterm >= term)
		{
			role = 0;
			term = mterm;
			emit();
		}
		r[0] = 'a';
		r[1] = term;
	}
	(void) !write(c, r, 3);
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	role = 0;
	term = 0;
	votedFor = -1;
	emit();

	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);

	for (;;)
	{
		int			c = accept(ls, NULL, NULL);

		if (c < 0)
			break;
		/* one read: a timer connection delivers a single 'T' byte; an
		 * RPC delivers three bytes.  The checker delivers each write as
		 * one message, so the count tells them apart. */
		unsigned char m[3];
		ssize_t		n = read(c, m, 3);

		if (n == 1 && m[0] == 'T')
			start_election();
		else if (n == 3)
			serve(c, m);
		close(c);
	}
	return 0;
}
