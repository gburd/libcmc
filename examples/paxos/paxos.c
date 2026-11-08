/*
 * paxos.c: single-decree Paxos, checked as it ships (examples/paxos).
 *
 * Two proposers (nodes 0 and 1) and three acceptors (nodes 2, 3 and 4),
 * each its own process, written with ordinary blocking TCP and no
 * libcmc header.  Every request is its own connection, a common server
 * pattern: the proposer dials, sends, and waits for the reply; the
 * acceptor accepts, reads, replies, and goes back to accept.  The order
 * in which an acceptor takes requests is therefore the network's -- the
 * checker's -- choice, which is what makes the search meaningful.
 *
 * Proposer i uses ballot i+1 and proposes value i+1.  Phase 1 sends
 * PREPARE to acceptors 2, 3, 4 in turn until a majority has promised;
 * phase 2 sends ACCEPT(ballot, v) to those that promised, where v is
 * the value of the highest-ballot acceptance among the promises, else
 * its own.  It decides v once a majority accepted.  The safety property
 * is agreement: the two proposers never decide different values.
 *
 * -DBUG: the proposer ignores the values in the promises and always
 * proposes its own -- the classic mistake that lets the two proposers
 * decide different values.  cmc finds it.
 *
 * Canonical state on fd 3 (it must hold everything the node's future
 * depends on, its program position included):
 *   proposer: [count=8][phase][k][ok][prom][hb][hv][decided]
 *   acceptor: [count=3][promised][accepted ballot][accepted value]
 * The acceptor persists its state (fd 193) before it replies, as Paxos
 * requires; a proposer persists its decision.  A crashed node keeps
 * only that durable state.
 *
 *   cc -no-pie -o paxos examples/paxos/paxos.c
 *   cmc --model-file examples/paxos/paxos.model --binary ./paxos
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000
#define NACC 3
#define MAJ 2

static void
emit_fd(int fd, const unsigned char *b, int n)
{
	unsigned char rec[16];
	rec[0] = (unsigned char) n;
	for (int i = 0; i < n; i++)
		rec[1 + i] = b[i];
	(void) !write(fd, rec, (size_t) n + 1);
}

/* volatile state on fd 3; durable state (what survives a crash) on 193 */
static void emit(const unsigned char *b, int n) { emit_fd(3, b, n); }
static void emit_durable(const unsigned char *b, int n) { emit_fd(193, b, n); }

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

/* one request/reply on a fresh connection to node `to` */
static int
rpc(int to, const unsigned char req[3], unsigned char rep[3])
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + to);
	if (connect(s, (void *) &a, sizeof a) != 0)
		return -1;
	(void) !write(s, req, 3);
	int r = read_full(s, rep, 3);
	close(s);
	return r;
}

static void
acceptor(int id)
{
	unsigned char st[3] = {0, 0, 0};	/* promised, accepted ballot, value */
	emit(st, 3);
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);
	bind(ls, (void *) &a, sizeof a);
	listen(ls, 8);
	for (;;)
	{
		int c = accept(ls, 0, 0);
		unsigned char m[3], r[3] = {'n', 0, 0};
		if (read_full(c, m, 3) == 0)
		{
			unsigned char b = m[1];
			if (m[0] == 'P' && b > st[0])
			{
				st[0] = b;
				r[0] = 'p'; r[1] = st[1]; r[2] = st[2];
			}
			else if (m[0] == 'A' && b >= st[0])
			{
				st[0] = b; st[1] = b; st[2] = m[2];
				r[0] = 'a'; r[1] = b; r[2] = m[2];
			}
			else
				r[1] = st[0];
			emit_durable(st, 3);		/* Paxos: persist before replying */
			emit(st, 3);
			(void) !write(c, r, 3);
		}
		close(c);
	}
}

static void
proposer(int id)
{
	/* phase, next acceptor, promises, promise bitmap, highest accepted
	 * ballot and value seen, acceptances, decided value */
	unsigned char p[8] = {1, 0, 0, 0, 0, 0, 0, 0};
	unsigned char ballot = (unsigned char) (id + 1);
	emit(p, 8);
	for (int k = 0; k < NACC && p[2] < MAJ; k++)
	{
		unsigned char req[3] = {'P', ballot, 0}, rep[3];
		p[1] = (unsigned char) k;
		emit(p, 8);
		if (rpc(2 + k, req, rep) == 0 && rep[0] == 'p')
		{
			p[2]++;
			p[3] |= (unsigned char) (1 << k);
			if (rep[1] > p[4]) { p[4] = rep[1]; p[5] = rep[2]; }
		}
	}
	if (p[2] < MAJ)
	{
		p[0] = 9;						/* gave up */
		emit(p, 8);
		return;
	}
	unsigned char v = (unsigned char) (id + 1);
#ifndef BUG
	if (p[4])
		v = p[5];						/* adopt the highest accepted value */
#endif
	p[0] = 2;
	for (int k = 0; k < NACC; k++)
		if (p[3] & (1 << k))
		{
			unsigned char req[3] = {'A', ballot, v}, rep[3];
			p[1] = (unsigned char) k;
			emit(p, 8);
			if (rpc(2 + k, req, rep) == 0 && rep[0] == 'a')
				p[6]++;
		}
	p[0] = 3;
	if (p[6] >= MAJ)
	{
		p[7] = v;
		emit_durable(p, 8);				/* a decision survives a crash */
	}
	emit(p, 8);
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	if (id < 2)
		proposer(id);
	else
		acceptor(id);
	return 0;
}
