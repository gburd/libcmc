/*
 * hc.c: Humming Consensus, checked as it ships (examples/hc).
 *
 * Humming Consensus (Scott Lystig Fritchie, PaPoC 2016) is a GENERIC
 * agreement protocol -- like Paxos, Raft or ZAB -- for agreeing on a
 * SEQUENCE OF CONFIGURATIONS, one per epoch.  The configuration is an
 * opaque, system-specific value; the paper's examples are "a list of
 * replicas of a key-value store or a flat ASCII file".  Chain
 * replication (Machi's use of HC) is just one such configuration; HC
 * itself knows nothing about chains.  This example agrees on an opaque
 * one-byte configuration, to show HC as the agreement protocol it is.
 *
 * Reference: "Coordinating Distributed System Configuration Changes
 * with Humming Consensus", PaPoC 2016,
 * http://snookles.com/scott/publications/papoc2016-slf.pdf (the
 * algorithm is Figure 2).  The only implementation is inside Machi:
 * https://github.com/basho/machi/blob/master/src/machi_chain_manager1.erl
 * (the ranking and unanimity rules).  See the README for how the
 * invariant maps to HC's stated safety property.
 *
 * Components (paper section 3):
 *   - the opaque configuration (here one byte);
 *   - the epoch number, strictly increasing logical time;
 *   - a WRITE-ONCE epoch register store, one per participant, keyed by
 *     epoch, readable by all.  All inter-participant communication is
 *     indirect, through reads and writes of these stores.
 *
 * Three participants (nodes 0, 1, 2), each a process; cmc is the
 * network between them.  Node i listens on 127.0.0.1 port 20000 + i and
 * holds its own write-once store.  The store API (paper Figure 1):
 *   READ  'R' epoch _     -> 'r' epoch config, or 'r' epoch 0 = unwritten
 *   WRITE 'W' epoch config-> 'w' (ok) or 'x' (already written: ignored)
 *   LATEST'L' _ _         -> 'l' epoch config (its highest written epoch)
 *
 * A participant takes one iteration of the algorithm when the
 * environment fires a TIMEOUT at it (the paper's periodic tick).  An
 * iteration (Figure 2): read every participant's latest public
 * projection; let E_max be the greatest epoch seen and C_max a config
 * at E_max; if some participant has no value at E_max, repair by
 * copying C_max there (write-once, so an existing value is never
 * changed); then, if the configs at E_max are UNANIMOUS across a quorum
 * and E_max is newer than what we have adopted, ADOPT C_max; else
 * suggest a new config at epoch E_max + 1.  While it waits on a peer's
 * reply a participant keeps serving its own store, as every peer is
 * iterating by asking the others.
 *
 * To make two participants race, each starts preferring its own config
 * (node i prefers value 10 + i), so at epoch 1 they suggest DIFFERENT
 * configs.  HC must never let two participants ADOPT different configs
 * for the same epoch: dissent at an epoch is visible to every reader
 * (write-once + read-all), so a dissenting epoch is ignored and the
 * participants advance to a later epoch and converge.
 *
 * Canonical state, fd 3 (volatile) and fd 193 (durable):
 *   [count=2][adopted_epoch][adopted_config]   0 = nothing adopted yet.
 * The store's write-once contents are durable; the adopted projection
 * is what the participant has committed to.
 *
 * -DBUG: a participant adopts a projection it sees as unanimous WITHOUT
 * a quorum majority of reachable stores (HC's eventual-consistency
 * mode).  Under an asymmetric partition two participants each adopt from
 * a different minority and end up with different configs at the same
 * epoch -- the agreement violation the paper notes is invisible until
 * the partition heals.  cmc finds it (a timeout on a reply stands for an
 * unreachable peer):
 *   cmc --model-file examples/hc/hc.model --binary ./hc-bug \
 *       --timeouts 12 --simulate --seed 2 --walks 30000 --depth 600
 *
 *   cc -no-pie -o hc examples/hc/hc.c
 *   cmc --model-file examples/hc/hc.model --binary ./hc
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000
#define NPART 3
#define MAJ 2					/* a quorum majority of NPART participants */
#define MAXEPOCH 32

static int id;
static unsigned char adopted_epoch;		/* highest epoch we have adopted */
static unsigned char adopted_config;	/* its config, 0 = none */

/* our own write-once store: store[e] = config at epoch e, 0 = unwritten */
static unsigned char store[MAXEPOCH];

static void
emit_fd(int fd)
{
	unsigned char rec[3] = {2, adopted_epoch, adopted_config};

	(void) !write(fd, rec, sizeof rec);
}

/* the adopted projection is what we commit to; durable + volatile */
static void
emit(void)
{
	emit_fd(193);
	emit_fd(3);
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

static int ls;			/* this participant's listener */

/* answer one store request m (READ, WRITE or LATEST) into r */
static void
answer_store(const unsigned char m[3], unsigned char r[3])
{
	r[0] = r[1] = r[2] = 0;
	if (m[0] == 'R')				/* READ epoch: our store's value */
	{
		r[0] = 'r';
		r[1] = m[1];
		r[2] = m[1] < MAXEPOCH ? store[m[1]] : 0;
	}
	else if (m[0] == 'W')			/* WRITE epoch config (write-once) */
	{
		if (m[1] < MAXEPOCH && store[m[1]] == 0)
		{
			store[m[1]] = m[2];
			r[0] = 'w';
		}
		else
			r[0] = 'x';				/* already written: ignored */
	}
	else if (m[0] == 'L')			/* LATEST: highest written epoch */
	{
		int hi = 0;

		for (int e = 1; e < MAXEPOCH; e++)
			if (store[e])
				hi = e;
		r[0] = 'l';
		r[1] = (unsigned char) hi;
		r[2] = hi ? store[hi] : 0;
	}
}

/* accept one waiting connection and answer the store request on it; a
 * timer 'T' that arrives mid-iteration is dropped (one iteration at a
 * time) */
static void
serve_one(void)
{
	int c = accept(ls, NULL, NULL);
	unsigned char m[3], r[3];

	if (c < 0)
		return;
	if (read(c, m, 3) == 3)
	{
		answer_store(m, r);
		(void) !write(c, r, 3);
	}
	close(c);
}

/* One request/reply on a fresh connection to participant `to`.  While
 * the reply is outstanding the participant keeps serving its own store,
 * in a poll() loop over the listener and the connection: every
 * participant iterates by asking the others, so one that stopped
 * serving while it waited would deadlock with a peer doing the same.
 * Returns 0 with the reply in rep, or -1 (no reply: refused, reset, or
 * a timeout standing for a partition). */
static int
rpc(int to, const unsigned char req[3], unsigned char rep[3])
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + to);
	int r = -1;

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
	{
		close(s);
		return -1;
	}
	(void) !write(s, req, 3);
	for (;;)
	{
		struct pollfd pf[2] = {{.fd = ls, .events = POLLIN}, {.fd = s, .events = POLLIN}};

		if (poll(pf, 2, -1) <= 0)
			continue;
		if (pf[0].revents & POLLIN)
			serve_one();
		if (pf[1].revents & POLLIN)
		{
			r = read(s, rep, 3) == 3 ? 0 : -1;
			break;
		}
	}
	close(s);
	return r;
}

/* read participant `to`'s latest public projection into *ep, *cfg;
 * returns 0 on success (a value exists), -1 on none/partition */
static int
read_latest(int to, unsigned char *ep, unsigned char *cfg)
{
	unsigned char req[3] = {'L', 0, 0}, rep[3];

	if (to == id)							/* our own store, read directly */
	{
		int hi = 0;

		for (int e = 1; e < MAXEPOCH; e++)
			if (store[e])
				hi = e;
		if (!hi)
			return -1;
		*ep = (unsigned char) hi;
		*cfg = store[hi];
		return 0;
	}
	if (rpc(to, req, rep) != 0 || rep[0] != 'l' || rep[1] == 0)
		return -1;
	*ep = rep[1];
	*cfg = rep[2];
	return 0;
}

/* read participant `to`'s value at a specific epoch; 0 = unwritten */
static unsigned char
read_at(int to, unsigned char ep)
{
	unsigned char req[3] = {'R', ep, 0}, rep[3];

	if (to == id)
		return store[ep];
	if (rpc(to, req, rep) != 0 || rep[0] != 'r')
		return 0;
	return rep[2];
}

/* write-once: put `cfg` at epoch `ep` in participant `to`'s store */
static void
write_at(int to, unsigned char ep, unsigned char cfg)
{
	unsigned char req[3] = {'W', ep, cfg}, rep[3];

	if (to == id)
	{
		if (store[ep] == 0)			/* write-once: never overwrite */
			store[ep] = cfg;
		return;
	}
	(void) !rpc(to, req, rep);
}

/* one iteration of Humming Consensus (paper Figure 2) */
static void
iterate(void)
{
	unsigned char e_max = 0, best = 0;
	int have_e_max = 0;

	/* read every participant's latest public projection; E_max is the
	 * greatest epoch seen, `best` the best config at E_max (the ranking
	 * is a total order -- here, the greatest config value wins; every
	 * participant that can read all stores computes the same best) */
	for (int p = 0; p < NPART; p++)
	{
		unsigned char ep, cfg;

		if (read_latest(p, &ep, &cfg) == 0)
		{
			if (ep > e_max) { e_max = ep; best = cfg; have_e_max = 1; }
			else if (ep == e_max && cfg > best) best = cfg;
		}
	}

	unsigned char mine = (unsigned char) (10 + id);

	if (!have_e_max)
	{
		/* nothing suggested yet: suggest our own config at epoch 1 in our
		 * OWN store only, so participants genuinely diverge (write-once) */
		write_at(id, 1, mine);
		return;
	}

	/* read every store's value at E_max over a QUORUM: HC in strongly
	 * consistent mode (paper 6.3) requires a quorum majority of
	 * successful reads before a projection may be adopted.  `seen`
	 * counts stores we could read a value from at E_max; `agree` counts
	 * those equal to the best.  A store we cannot reach (the checker may
	 * time a read out) does not count toward the quorum. */
	int seen = 0, agree = 0, hole = 0;

	for (int p = 0; p < NPART; p++)
	{
		unsigned char v = read_at(p, e_max);

		if (v == 0)
			hole = 1;
		else
		{
			seen++;
			if (v == best)
				agree++;
		}
	}
	int unanimous = (seen == agree) && (seen > 0);

	if (hole && unanimous)
	{
		/* read repair (HummingConsensus.tla's ReadRepair): copy the best
		 * value seen at E_max to the reachable stores missing it
		 * (write-once).  Repair needs no quorum, since it only spreads a
		 * value already suggested, and it does not end the iteration:
		 * the adoption rule below, counted on the reads above, decides
		 * whether to adopt, as Adopt is a separate action in the spec. */
		for (int p = 0; p < NPART; p++)
			if (read_at(p, e_max) == 0)
				write_at(p, e_max, best);
	}
	if (hole && !unanimous)
	{
		/* a reachable store already dissents; fall through to abandon */
	}

#ifdef BUG
	/* AP mode: adopt on a visible unanimity WITHOUT a quorum majority --
	 * a participant isolated with a minority of reachable stores adopts
	 * anyway (HC's eventual-consistency mode, where "groups of one" may
	 * adopt).  Under an asymmetric partition two participants each see a
	 * different minority and adopt different configs at the same epoch;
	 * the paper notes the conflict is invisible until the partition
	 * heals.  cmc finds it (try --simulate). */
	if (unanimous && e_max > adopted_epoch)
	{
		adopted_epoch = e_max;
		adopted_config = best;
		emit();
		return;
	}
#else
	/* CP mode: adopt only a projection a QUORUM MAJORITY agrees on, and
	 * only when it is strictly newer.  Quorums intersect, so two
	 * participants cannot adopt different configs at one epoch. */
	if (unanimous && seen >= MAJ && e_max > adopted_epoch)
	{
		adopted_epoch = e_max;
		adopted_config = best;
		emit();
		return;
	}
#endif

	if (seen >= MAJ && !unanimous)
	{
		/* dissent across a quorum at E_max: abandon it and converge on a
		 * single new suggestion at epoch E_max + 1.  The paper's flapping
		 * remedy is to fall back to one safe value all agree on; here the
		 * lowest-id participant authors the next epoch's value (the
		 * globally-best config), so E_max + 1 has a single author and
		 * becomes unanimous once repair propagates it. */
		if (id == 0 && e_max + 1 < MAXEPOCH)
			write_at(id, (unsigned char) (e_max + 1), best);
	}
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	adopted_epoch = 0;
	adopted_config = 0;
	emit();

	ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);

	for (;;)
	{
		int c = accept(ls, NULL, NULL);

		if (c < 0)
			break;
		unsigned char m[3], r[3];
		ssize_t n = read(c, m, 3);

		if (n == 1 && m[0] == 'T')		/* timeout: take one HC iteration */
		{
			close(c);
			iterate();
			continue;
		}
		if (n != 3)
		{
			close(c);
			continue;
		}
		answer_store(m, r);
		(void) !write(c, r, 3);
		close(c);
	}
	return 0;
}
