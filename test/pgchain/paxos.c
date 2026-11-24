/*-------------------------------------------------------------------------
 *
 * paxos.c
 *	  The configuration log's consensus core: Multi-Paxos over fixed voters
 *	  (PLAN section 7.2, research/03 sections 3.1-3.3, 3.8).
 *
 * Every node is an acceptor and a learner; nodes 0..C-1 are also
 * candidates.  A candidate with a client value and no leadership
 * campaigns: it makes its next ballot durable and sends one PREPARE, for
 * every slot, to every node.  It then drives the log one slot at a time
 * (advance()): at the first slot it has not learned, once a q1 of that
 * slot's configuration has promised, it proposes the value accepted at
 * the highest ballot those promises reported, else its client's value,
 * else a noop if a later slot holds something.  A value is chosen once a
 * q2 of the slot's configuration accepted it at the leader's ballot; the
 * leader learns it, answers its client, tells the other nodes, and moves
 * to the next slot.  Configuration changes are in paxos.h.
 *
 * Every reply that reports acceptor state (PROMISE, ACCEPTED) goes out
 * from the FLUSHED event of the record that made that state durable, so
 * a reply never runs ahead of the disk (research/03 3.2).  A NACK carries
 * the acceptor's promise, so a preempted candidate jumps past it.
 *
 * Client outcomes: COMMITTED once the value is chosen; AMBIGUOUS when the
 * leader is preempted after it sent the value in an ACCEPT (some acceptor
 * may hold it, and a later leader may choose it); REFUSED when it was
 * never proposed (no rounds left, or the log is full).  The PostgreSQL
 * glue maps AMBIGUOUS to "unknown, re-read the log", as 08007 is for a
 * chain commit.
 *
 * The transport is libpq over TCP (FIFO).  After a connection reset the
 * proposer resends whatever that peer has not answered, and a leader
 * resends COMMITs, so a reset loses nothing but time.
 *
 * Planted bugs, -DPGCHAIN_MC_BUG=n (src/core/ numbers them per core; the
 * paxos variants are 101-...), for scripts/mc-bug-gate.sh:
 *   101  an acceptor sends PROMISE before its promise is durable
 *   102  a leader proposes its own value, ignoring recovered ones
 *   103  a leader keeps the recovered value of the LOWEST ballot
 *   104  a candidate does not make its ballot durable: after a restart
 *        it can campaign again with a ballot it already used
 *   105  an acceptor sends ACCEPTED before its accept is durable
 *   106  an acceptor accepts below its promise
 *   107  a leader proposes at slot k before it has learned slot k - 1, so
 *        it can use a configuration a VoterCfg below has replaced (V1)
 *   108  an acceptor answers a resent PREPARE with PROMISE before the
 *        first one's promise is durable
 *   109  phase 2 uses the initial configuration, ignoring a VoterCfg (V3)
 *   110  phase 1 is tallied against the initial configuration, ignoring
 *        a VoterCfg (V2)
 *
 * NB: two checks here are redundant under the transport the glue uses,
 * and stay as defence in depth.  The leader counts an ACCEPTED only at
 * its current ballot, and only from on_accepted.  Under FIFO connections
 * and in-order flushes, an acceptor's ACCEPTED for an older ballot always
 * arrives before its PROMISE for the newer one, so it lands while the
 * leader is still preparing and is ignored; dropping either check passed
 * complete searches of two candidates by two rounds (36-42 M states,
 * review of 2026-11-23).  A bag network would need them.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "pg_chain/paxos.h"

#ifndef PGCHAIN_MC_BUG
#define PGCHAIN_MC_BUG 0
#endif

static CmcOutput *
send_msg(CmcOut *out, int dst, uint8_t type, uint8_t ballot, int len)
{
	CmcOutput  *o = cmc_out_push(out, CMC_OUT_SEND);

	o->dst = (uint8_t) dst;
	o->len = (uint16_t) len;
	o->body[0] = type;
	o->body[1] = ballot;
	return o;
}

static void
send_promise(const PaxosCfg *cfg, const PaxosState *s, CmcOut *out, uint8_t b)
{
	CmcOutput  *o = send_msg(out, PX_NODE(b), PX_PROMISE, b, 2 + 2 * cfg->slots);

	for (int k = 0; k < cfg->slots; k++)
	{
		o->body[2 + 2 * k] = s->d.acc_b[k];
		o->body[3 + 2 * k] = s->d.acc_v[k];
	}
}

static void
send_accept(CmcOut *out, int dst, uint8_t b, int slot, uint8_t v)
{
	CmcOutput  *o = send_msg(out, dst, PX_ACCEPT, b, 4);

	o->body[2] = (uint8_t) slot;
	o->body[3] = v;
}

static void
send_commit(CmcOut *out, int dst, int slot, uint8_t v)
{
	CmcOutput  *o = send_msg(out, dst, PX_COMMIT, 0, 4);

	o->body[2] = (uint8_t) slot;
	o->body[3] = v;
}

static void
persist(CmcOut *out, uint8_t type, uint8_t b, uint8_t slot, uint8_t v)
{
	CmcOutput  *o = cmc_out_push(out, CMC_OUT_PERSIST);

	o->len = 4;
	o->body[0] = type;
	o->body[1] = b;
	o->body[2] = slot;
	o->body[3] = v;
}

static void
reply(PaxosState *s, CmcOut *out, uint8_t slot, uint8_t outcome)
{
	cmc_out_push(out, CMC_OUT_REPLY)->arg =
		PX_REPLY(slot, s->pending, outcome, s->ballot);
	s->pending = 0;
	s->pending_slot = PX_NO_SLOT;
}

static int
popcount8(uint8_t x)
{
	int			n = 0;

	for (; x; x &= (uint8_t) (x - 1))
		n++;
	return n;
}

void
paxos_init(const void *cfg, int node, void *state)
{
	PaxosState *s = state;

	(void) cfg;
	(void) node;
	memset(s, 0, sizeof(*s));
	s->pending_slot = PX_NO_SLOT;
}

/*
 * The engine applies a record when it reaches disk, and on a crash for
 * the records that made it.  The core never persists a promise below one
 * it holds, so taking the maximum only guards replays of a duplicate.
 */
void
paxos_apply_persist(const void *cfg, int node, void *durable,
					const uint8_t *body, uint16_t len)
{
	PaxosDurable *d = durable;
	uint8_t		b = body[1];

	(void) cfg;
	(void) node;
	(void) len;
	if (body[0] == PX_P_BALLOT)
	{
		if (b > d->last_ballot)
			d->last_ballot = b;
		return;
	}
	if (b > d->promised)
		d->promised = b;
	if (body[0] == PX_P_ACCEPT)
	{
		d->acc_b[body[2]] = b;
		d->acc_v[body[2]] = body[3];
	}
}

/* ---- configurations ---------------------------------------------------- */

void
paxos_config_at(const PaxosCfg *cfg, const uint8_t *log, int k,
				uint8_t *members, int *q1, int *q2)
{
	*members = (uint8_t) ((1u << cfg->voters) - 1);
	*q1 = cfg->q1;
	*q2 = cfg->q2;
	for (int j = 0; j < k; j++)
		if (PX_IS_VCFG(log[j]))
		{
			*members = PX_VCFG_MASK(log[j]);
			*q1 = *q2 = popcount8(*members) / 2 + 1;
		}
}

/* ---- proposer --------------------------------------------------------- */

/*
 * Start a campaign with a ballot above every one this node has used or
 * seen; with no rounds left, give up and refuse the client.
 */
static void
campaign(const PaxosCfg *cfg, PaxosState *s, int node, CmcOut *out)
{
	int			round = PX_ROUND(s->d.last_ballot);

	if (PX_ROUND(s->ballot) > round)
		round = PX_ROUND(s->ballot);
	if (s->max_round > round)
		round = s->max_round;
	if (PX_ROUND(s->prom_v) > round)
		round = PX_ROUND(s->prom_v);
	round++;
	if (round > cfg->max_round)
	{
		s->phase = PX_PH_FOLLOWER;
		s->gave_up = 1;
		if (s->pending)
			reply(s, out, PX_NO_SLOT, PX_REFUSED);
		return;
	}
	s->ballot = PX_BALLOT(round, node);
	s->p1 = 0;
	memset(s->pb, 0, sizeof(s->pb));
	memset(s->pv, 0, sizeof(s->pv));
	memset(s->prop_v, 0, sizeof(s->prop_v));
	memset(s->p2, 0, sizeof(s->p2));
#if PGCHAIN_MC_BUG == 104
	s->phase = PX_PH_PREPARE;
	for (int a = 0; a < cfg->nodes; a++)
		send_msg(out, a, PX_PREPARE, s->ballot, 2);
#else
	s->phase = PX_PH_BALLOT;
	persist(out, PX_P_BALLOT, s->ballot, 0, 0);
#endif
}

/*
 * Phase 1 for slot k: the promisers that count are members of k's
 * configuration, tallied against that configuration alone (V2).  Returns
 * false while too few have promised.  On true, *rec is the value accepted
 * at the highest ballot any of them reported at k, or 0.
 */
static bool
phase1_slot(const PaxosCfg *cfg, const PaxosState *s, int k, uint8_t *rec)
{
	uint8_t		mem,
				best = 0;
	int			q1,
				q2;

	paxos_config_at(cfg, s->learned, k, &mem, &q1, &q2);
#if PGCHAIN_MC_BUG == 110
	paxos_config_at(cfg, s->learned, 0, &mem, &q1, &q2);
#endif
	if (popcount8(s->p1 & mem) < q1)
		return false;
	*rec = 0;
	for (int a = 0; a < cfg->nodes; a++)
	{
		if (!(s->p1 & mem & (1u << a)))
			continue;
#if PGCHAIN_MC_BUG == 103
		if (s->pb[a][k] && (best == 0 || s->pb[a][k] < best))
#else
		if (s->pb[a][k] > best)
#endif
		{
			best = s->pb[a][k];
			*rec = s->pv[a][k];
		}
	}
	return true;
}

/* Did any promise report a value at a slot after k?  Then k needs a noop. */
static bool
reported_after(const PaxosCfg *cfg, const PaxosState *s, int k)
{
	for (int a = 0; a < cfg->nodes; a++)
		for (int j = k + 1; j < cfg->slots; j++)
			if ((s->p1 & (1u << a)) && s->pb[a][j])
				return true;
	return false;
}

/*
 * Drive the log: at the first slot this node has not learned, once phase
 * 1 has a quorum of that slot's configuration, propose the recovered
 * value, else the client's, else a noop if a later slot needs filling.
 * One slot is in flight at a time, so a slot's configuration is always
 * known before anything is proposed there (V1, V3).
 */
static void
advance(const PaxosCfg *cfg, PaxosState *s, CmcOut *out)
{
	int			k = 0;
	uint8_t		rec,
				v,
				mem;
	int			q1,
				q2;

#if PGCHAIN_MC_BUG == 107
	while (k < cfg->slots && (s->learned[k] || s->prop_v[k]))
		k++;
#else
	while (k < cfg->slots && s->learned[k])
		k++;
#endif
	if (k == cfg->slots)
	{
		if (s->pending && s->pending_slot == PX_NO_SLOT)
			reply(s, out, PX_NO_SLOT, PX_REFUSED);	/* the log is full */
		return;
	}
	if (s->prop_v[k] || !phase1_slot(cfg, s, k, &rec))
		return;
#if PGCHAIN_MC_BUG == 102
	v = s->pending ? s->pending : rec;
#else
	v = rec ? rec : (s->pending_slot == PX_NO_SLOT ? s->pending : 0);
#endif
	if (!v && reported_after(cfg, s, k))
		v = PX_NOOP;
	if (!v)
		return;					/* nothing to do: an idle leader */
	if (v == s->pending)
		s->pending_slot = (uint8_t) k;
	s->phase = PX_PH_LEAD;
	s->prop_v[k] = v;
	s->p2[k] = 0;
	paxos_config_at(cfg, s->learned, k, &mem, &q1, &q2);
#if PGCHAIN_MC_BUG == 109
	paxos_config_at(cfg, s->learned, 0, &mem, &q1, &q2);
#endif
	for (int a = 0; a < cfg->nodes; a++)
		if (mem & (1u << a))
			send_accept(out, a, s->ballot, k, v);
}

static void
on_promise(const PaxosCfg *cfg, PaxosState *s, int src, const uint8_t *b, CmcOut *out)
{
	if ((s->phase != PX_PH_PREPARE && s->phase != PX_PH_LEAD) || b[1] != s->ballot ||
		(s->p1 & (1u << src)))
		return;
	s->p1 |= (uint8_t) (1u << src);
	for (int k = 0; k < cfg->slots; k++)
	{
		s->pb[src][k] = b[2 + 2 * k];
		s->pv[src][k] = b[3 + 2 * k];
	}
	advance(cfg, s, out);
}

/* A value is chosen at k: learn it, tell everyone, answer, move on. */
static void
chosen(const PaxosCfg *cfg, PaxosState *s, int node, int k, uint8_t v, CmcOut *out)
{
	s->learned[k] = v;
	for (int a = 0; a < cfg->nodes; a++)
		if (a != node)
			send_commit(out, a, k, v);
	if (s->pending && v == s->pending)
		reply(s, out, (uint8_t) k, PX_COMMITTED);
	advance(cfg, s, out);
}

static void
on_accepted(const PaxosCfg *cfg, PaxosState *s, int node, int src,
			const uint8_t *b, CmcOut *out)
{
	int			k = b[2];
	uint8_t		mem;
	int			q1,
				q2;

	if (s->phase != PX_PH_LEAD || b[1] != s->ballot || s->prop_v[k] != b[3] ||
		s->learned[k])
		return;
	paxos_config_at(cfg, s->learned, k, &mem, &q1, &q2);
#if PGCHAIN_MC_BUG == 109
	paxos_config_at(cfg, s->learned, 0, &mem, &q1, &q2);
#endif
	s->p2[k] |= (uint8_t) (1u << src);
	if (popcount8(s->p2[k] & mem) >= q2)
		chosen(cfg, s, node, k, b[3], out);
}

/*
 * Preempted: a value already sent in an ACCEPT may yet be chosen by
 * another leader, so its client hears AMBIGUOUS; one not yet proposed is
 * tried again under a higher ballot.  Either way the candidate campaigns
 * again while it has rounds: leadership does not depend on a client
 * waiting (the failure detector starts campaigns in the glue, 7.2).
 */
static void
on_nack(const PaxosCfg *cfg, PaxosState *s, int node, const uint8_t *b, CmcOut *out)
{
	if ((s->phase != PX_PH_PREPARE && s->phase != PX_PH_LEAD) || b[1] != s->ballot)
		return;
	if (PX_ROUND(b[2]) > s->max_round)
		s->max_round = PX_ROUND(b[2]);
	s->phase = PX_PH_FOLLOWER;
	if (s->pending && s->pending_slot != PX_NO_SLOT)
		reply(s, out, s->pending_slot, PX_AMBIGUOUS);
	campaign(cfg, s, node, out);
}

/*
 * Learned from another leader.  Our client's value may be the one chosen,
 * at any slot (a later leader recovers it); otherwise, if another value
 * took the slot we proposed ours in, our ballot is dead: preempted, as on
 * a NACK.
 */
static void
on_commit(const PaxosCfg *cfg, PaxosState *s, int k, uint8_t v, CmcOut *out)
{
	if (s->learned[k])
		return;
	s->learned[k] = v;
	if (s->pending && v == s->pending)
		reply(s, out, (uint8_t) k, PX_COMMITTED);
	else if (s->pending && s->pending_slot == k)
	{
		reply(s, out, (uint8_t) k, PX_AMBIGUOUS);
		s->phase = PX_PH_FOLLOWER;
	}
	if (s->phase == PX_PH_PREPARE || s->phase == PX_PH_LEAD)
		advance(cfg, s, out);
}

static void
on_client(const PaxosCfg *cfg, PaxosState *s, int node, uint32_t req, CmcOut *out)
{
	uint8_t		v = PX_VALUE(node, req);

	/* the model's configuration changes: a candidate's first request */
	if (req == 0 && node == 0 && cfg->change)
		v = PX_VCFG(cfg->change);
	if (req == 0 && node == 1 && cfg->change2)
		v = PX_VCFG(cfg->change2);

	if (s->pending || s->gave_up)
	{
		/* one request at a time per candidate: refuse the second */
		cmc_out_push(out, CMC_OUT_REPLY)->arg = PX_REPLY(PX_NO_SLOT, v, PX_REFUSED, s->ballot);
		return;
	}
	s->pending = v;
	s->pending_slot = PX_NO_SLOT;
	if (s->phase == PX_PH_PREPARE || s->phase == PX_PH_LEAD)
		advance(cfg, s, out);
	else if (s->phase == PX_PH_FOLLOWER)
		campaign(cfg, s, node, out);
}

/* A peer's connection was reset: resend what it has not answered. */
static void
on_reset(const PaxosCfg *cfg, PaxosState *s, int peer, CmcOut *out)
{
	uint8_t		mem;
	int			q1,
				q2;

	if (s->phase != PX_PH_PREPARE && s->phase != PX_PH_LEAD)
		return;
	if (!(s->p1 & (1u << peer)))
		send_msg(out, peer, PX_PREPARE, s->ballot, 2);
	for (int k = 0; k < cfg->slots; k++)
	{
		paxos_config_at(cfg, s->learned, k, &mem, &q1, &q2);
		if (s->learned[k])
			send_commit(out, peer, k, s->learned[k]);
		else if (s->prop_v[k] && (mem & (1u << peer)) && !(s->p2[k] & (1u << peer)))
			send_accept(out, peer, s->ballot, k, s->prop_v[k]);
	}
}

/* ---- acceptor --------------------------------------------------------- */

static void
on_prepare(const PaxosCfg *cfg, PaxosState *s, uint8_t b, CmcOut *out)
{
	if (b < s->prom_v)
	{
		CmcOutput  *o = send_msg(out, PX_NODE(b), PX_NACK, b, 3);

		o->body[2] = s->prom_v;
		return;
	}
#if PGCHAIN_MC_BUG == 108
	if (b == s->prom_v)
#else
	if (b == s->prom_v && s->d.promised == b)
#endif
	{
		send_promise(cfg, s, out, b);	/* a resend: already durable */
		return;
	}
	if (b > s->prom_v)
	{
		s->prom_v = b;
#if PGCHAIN_MC_BUG == 101
		send_promise(cfg, s, out, b);
#endif
		persist(out, PX_P_PROMISE, b, 0, 0);
	}
}

static void
on_accept(PaxosState *s, const uint8_t *body, CmcOut *out)
{
	uint8_t		b = body[1];

#if PGCHAIN_MC_BUG == 106
	if (false)
#else
	if (b < s->prom_v)
#endif
	{
		CmcOutput  *o = send_msg(out, PX_NODE(b), PX_NACK, b, 3);

		o->body[2] = s->prom_v;
		return;
	}
	if (b > s->prom_v)
		s->prom_v = b;
	if (s->d.acc_b[body[2]] == b && s->d.acc_v[body[2]] == body[3])
	{
		CmcOutput  *o = send_msg(out, PX_NODE(b), PX_ACCEPTED, b, 4);	/* a resend */

		o->body[2] = body[2];
		o->body[3] = body[3];
		return;
	}
#if PGCHAIN_MC_BUG == 105
	{
		CmcOutput  *o = send_msg(out, PX_NODE(b), PX_ACCEPTED, b, 4);

		o->body[2] = body[2];
		o->body[3] = body[3];
	}
#endif
	persist(out, PX_P_ACCEPT, b, body[2], body[3]);
}

/* A record is durable: now the reply it backs may go out. */
static void
on_flushed(const PaxosCfg *cfg, PaxosState *s, int node, const uint8_t *body, CmcOut *out)
{
	uint8_t		b = body[1];

	switch (body[0])
	{
		case PX_P_BALLOT:
			if (s->phase == PX_PH_BALLOT && b == s->ballot)
			{
				s->phase = PX_PH_PREPARE;
				for (int a = 0; a < cfg->nodes; a++)
					send_msg(out, a, PX_PREPARE, b, 2);
			}
			break;
		case PX_P_PROMISE:
#if PGCHAIN_MC_BUG != 101
			if (s->prom_v == b)
				send_promise(cfg, s, out, b);
			else
			{
				CmcOutput  *o = send_msg(out, PX_NODE(b), PX_NACK, b, 3);

				o->body[2] = s->prom_v;
			}
#endif
			break;
		case PX_P_ACCEPT:
#if PGCHAIN_MC_BUG != 105
			{
				CmcOutput  *o = send_msg(out, PX_NODE(b), PX_ACCEPTED, b, 4);

				o->body[2] = body[2];
				o->body[3] = body[3];
			}
#endif
			break;
	}
	(void) node;
}

void
paxos_step(const void *cfgv, int node, void *state, const CmcEvent *ev, CmcOut *out)
{
	const PaxosCfg *cfg = cfgv;
	PaxosState *s = state;
	const uint8_t *b = ev->body;

	switch (ev->kind)
	{
		case CMC_EV_RESTART:
			/* volatile state is zero; the acceptor resumes from disk */
			s->prom_v = s->d.promised;
			s->pending_slot = PX_NO_SLOT;
			break;
		case CMC_EV_CLIENT:
			on_client(cfg, s, node, ev->arg, out);
			break;
		case CMC_EV_FLUSHED:
			on_flushed(cfg, s, node, b, out);
			break;
		case CMC_EV_CONN_RESET:
			on_reset(cfg, s, ev->src, out);
			break;
		case CMC_EV_RECV:
			switch (b[0])
			{
				case PX_PREPARE:
					on_prepare(cfg, s, b[1], out);
					break;
				case PX_PROMISE:
					on_promise(cfg, s, ev->src, b, out);
					break;
				case PX_ACCEPT:
					on_accept(s, b, out);
					break;
				case PX_ACCEPTED:
					on_accepted(cfg, s, node, ev->src, b, out);
					break;
				case PX_NACK:
					on_nack(cfg, s, node, b, out);
					break;
				case PX_COMMIT:
					on_commit(cfg, s, b[2], b[3], out);
					break;
			}
			break;
	}
}
