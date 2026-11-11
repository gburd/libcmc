/*
 * paxos2.c: a SECOND, deliberately different implementation of
 * single-decree Paxos, to demonstrate the "one model, many
 * implementations" claim (Milestone C).  It is checked against the SAME
 * abstract model examples/spec/paxos.cml -- only its binding
 * (paxos2.bind) differs, because it observes differently.
 *
 * Where examples/paxos/paxos.c has 2 dedicated proposers and 3 dedicated
 * acceptors, this version has 3 SYMMETRIC nodes, each of which is both a
 * proposer and an acceptor (the single-shot multi-Paxos shape).  The
 * wire protocol and the state-hook layout are different too -- so a
 * reader can see the spec, not the code, is the shared artifact.
 *
 * Node i (0..2) listens on 127.0.0.1:(BASE+i).  Each node can be driven
 * to propose its own value (10 + i) for the single decree; it runs
 * Phase 1 (prepare) then Phase 2 (accept) against all three nodes
 * (itself included), picking the highest previously-accepted value seen
 * among a majority of promises, per Paxos.  A node DECIDES once a
 * majority has accepted its ballot's value.
 *
 * Messages, request/reply on a fresh connection (3 bytes):
 *   PREPARE 'P' ballot _      -> 'p' accepted_ballot accepted_value (promise)
 *                               or 'n' _ _ (refused: a higher ballot promised)
 *   ACCEPT  'A' ballot value  -> 'a' _ _ (accepted) or 'n' _ _ (refused)
 * A node's ballot is unique by construction: ballot = round*4 + (i+1),
 * so different nodes never share a ballot number.
 *
 * Canonical state, fd 3 (volatile) and fd 193 (durable):
 *   [count=4][promised_ballot][accepted_ballot][accepted_value][decided]
 * byte 4 (index 3) = decided value (0 = none).  Note this is a DIFFERENT
 * layout from paxos.c (there, a proposer's decided is byte 7); the
 * binding paxos2.bind maps it accordingly.  A node persists its state to
 * fd 193 before replying, and its decision when it decides.
 *
 * -DBUG: a node proposes its own value ignoring what a majority already
 * accepted (skipping the Phase-1 value-selection rule), AND accepts any
 * ballot without checking its promise.  Both are real Paxos mistakes;
 * together they can let two values be chosen.  (Paxos's own redundancy
 * makes two-values-chosen hard to reach at this small instance -- the
 * reliably-caught agreement bug is paxos.c's; this build exists to show
 * the second implementation builds and to document the mistake.)
 *
 *   cc -no-pie -o paxos2 examples/paxos/paxos2.c
 *   cmc --refine examples/spec/paxos.cml --bind examples/spec/paxos2.bind \
 *       --model-file examples/spec/paxos2-refine.model --binary ./paxos2
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
/* this node's acceptor state and its decision */
static unsigned char promised;		/* highest ballot promised */
static unsigned char acc_ballot;	/* ballot of the accepted value */
static unsigned char acc_value;		/* accepted value (0 = none) */
static unsigned char decided;		/* decided value (0 = none) */

static void
emit_fd(int fd)
{
	unsigned char rec[5] = {4, promised, acc_ballot, acc_value, decided};

	(void) !write(fd, rec, sizeof rec);
}
static void emit(void) { emit_fd(193); emit_fd(3); }	/* durable then volatile */

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
	int got = 0;

	while (got < n)
	{
		ssize_t r = read(fd, m + got, (size_t) (n - got));

		if (r <= 0)
			return -1;
		got += (int) r;
	}
	return 0;
}

static int
rpc(int to, const unsigned char req[3], unsigned char rep[3])
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + to);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		return -1;
	(void) !write(s, req, 3);
	int r = read_full(s, rep, 3);

	close(s);
	return r;
}

/* run one proposal round for this node's own value */
static void
propose(void)
{
	unsigned char b = (unsigned char) (4 + id + 1);	/* a unique ballot for round 1 */
	unsigned char my = (unsigned char) (10 + id);

	/* Phase 1: prepare at all nodes */
	int promises = 0;
	unsigned char best_b = 0, best_v = 0;

	for (int j = 0; j < N; j++)
	{
		unsigned char req[3] = {'P', b, 0}, rep[3];

		if (rpc(j, req, rep) == 0 && rep[0] == 'p')
		{
			promises++;
			if (rep[1] >= best_b) { best_b = rep[1]; best_v = rep[2]; }
		}
	}
	if (promises < MAJ)
		return;
	/* the value to propose: the highest accepted among promises, else mine */
#ifdef BUG
	/* always propose our own value, ignoring what a majority already
	 * accepted -- the classic Paxos mistake.  Combined with accepting any
	 * ballot below, two nodes can get different values chosen. */
	unsigned char v = my;
	(void) best_v;
#else
	unsigned char v = best_v ? best_v : my;
#endif

	/* Phase 2: accept at all nodes */
	int accepts = 0;

	for (int j = 0; j < N; j++)
	{
		unsigned char req[3] = {'A', b, v}, rep[3];

		if (rpc(j, req, rep) == 0 && rep[0] == 'a')
			accepts++;
	}
	if (accepts >= MAJ)
	{
		decided = v;
		emit();
	}
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	promised = acc_ballot = acc_value = decided = 0;
	emit();

	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);

	for (;;)
	{
		int c = accept(ls, NULL, NULL);

		if (c < 0)
			break;
		unsigned char m[3], r[3] = {'n', 0, 0};
		ssize_t n = read(c, m, 3);

		if (n == 1 && m[0] == 'T')		/* a timeout drives this node to propose */
		{
			close(c);
			if (!decided)
				propose();
			continue;
		}
		if (n != 3)
		{
			close(c);
			continue;
		}
		if (m[0] == 'P')				/* prepare(ballot) */
		{
			unsigned char b = m[1];

			if (b >= promised)
			{
				promised = b;
				emit();
				r[0] = 'p'; r[1] = acc_ballot; r[2] = acc_value;
			}
			/* else refuse: r stays 'n' */
		}
		else if (m[0] == 'A')			/* accept(ballot, value) */
		{
			unsigned char b = m[1], v = m[2];

#ifdef BUG
			/* accept any ballot, ignoring the promise -- unsound */
			acc_ballot = b; acc_value = v; promised = b;
			emit();
			r[0] = 'a';
#else
			if (b >= promised)
			{
				promised = b; acc_ballot = b; acc_value = v;
				emit();
				r[0] = 'a';
			}
			/* else refuse */
#endif
		}
		(void) !write(c, r, 3);
		close(c);
	}
	return 0;
}
