/*-------------------------------------------------------------------------
 *
 * chain_member.c
 *	  Data-path protocol core of one chain member, with a configuration
 *	  leader that removes members (PLAN sections 5.2-5.6, 8.3).
 *
 * Members are nodes 0..R-1.  The chain in an epoch is the adopted UPI (a
 * bitmap), in node-id order: head = lowest member, tail = highest, node
 * i forwards to the next member above it and acks to the next below.
 * Epoch 1's UPI is every member.
 *
 * On the head (5.2):
 *   CLIENT       refuse if wedged, sealed or ambiguous (5.5); else assign
 *                s = ++next_s and persist (s, v); arm the sender's timer
 *                and the transaction's commit timeout
 *   TIMER 0      the sender takes every assigned record not yet taken and
 *                ships it to its successor, in s order
 *   ACK / FLUSH  answer COMMIT for every s <= min(acked, durable) whose
 *                client is still waiting (step 5)
 *   TIMER 1      commit timeout (step 6) for the oldest waiting s: if the
 *                sender has not taken it, replace it by a noop (the
 *                compare-and-set) and answer ERROR once the noop is
 *                durable; otherwise answer AMBIGUOUS (08007) and wedge the
 *                chain until acked >= s (5.5)
 *
 * NB: ERROR waits for the noop's flush.  The original record was
 * persisted first, so answering at once would let a crash between the two
 * flushes leave a "definitely aborted" record in the head's log, which
 * PLAN 8.3 keeps as the last copy of last resort (rev 3.4).
 *
 * Downstream (5.3):
 *   middle  REC(s, v)    s == recv + 1: take it, persist it, forward it
 *   tail    REC(s, v)    take it and persist it; the tail's flush is the
 *                        commit point
 *   any     FLUSHED(s)   s is in the log; a non-head acks upstream
 *   middle  ACK(s')      acked = s'; ack upstream min(s', durable)
 * Every message carries the sender's epoch; a member drops messages of
 * another epoch (5.3: "a message carries the sender's adopted epoch").
 *
 * After a connection reset (FIFO transport, CMC_EV_CONN_RESET), or on
 * activating a new epoch, the predecessor sends HELLO_P; the successor
 * answers HELLO_S(recv, ackable); the predecessor resends from recv + 1
 * and takes the ack.  A record past a gap is dropped: the resend fills it.
 *
 * Reconfiguration (5.6, Vertical Paxos II; reconfigs > 0).  The leader is
 * node R and stands in for the Paxos log, so it does not crash here.
 * When a member it may remove is down (a crash: the leader learns of it
 * from the connection reset), it seals the survivors of the current
 * epoch, waits for every survivor's SealAck, then activates epoch + 1
 * with the survivors as UPI and tells the removed member.  A sealed
 * member takes no new s.  On activation a survivor adopts the UPI, and a
 * new head resends (acked, recv] and keeps the waiting clients' outcomes
 * (it is the same node or a node that never had clients: only node 0's
 * clients are modelled, so removing node 0 ends them with 08007).
 *
 * Only one transaction waits at a time in this model (one commit-timeout
 * timer); shortcut: a real head has one waiter per backend, model several
 * with per-waiter timers once libcmc's timer ids are per waiter.
 *
 * Planted bugs, -DPGCHAIN_MC_BUG=n, for scripts/mc-bug-gate.sh:
 *   1  the tail acks before its own flush (ack before durable)
 *   2  a middle acks what it has received, not what is durable
 *   3  the head replies before the ack (head commit before the chain)
 *   4  a middle takes a record past a gap (no gap check)
 *   5  timeout always answers ERROR, even after the sender took the
 *      record (no compare-and-set: a definite abort the chain commits)
 *   6  no ambiguity wedge: new writes are accepted after an 08007
 *   7  ERROR is answered when the noop is written, before it is durable
 *   8  a promoted head numbers new records from acked, not recv: it
 *      reuses sequence numbers its successors already hold
 *   9  a repairing member reports its old log as repaired, before its
 *      truncate and reload are durable
 *  10  a restarted member accepts an ACTIVATE that names it in the UPI
 *      (sent before its crash was noticed), with its memory gone
 *
 * NB: the seal is not load-bearing for writes in this model, and no
 * planted bug claims it is.  Activating without the SealAcks passes every
 * property here (checked, with crashes and false suspicions): the epoch
 * on every message and acks of durable records only already keep a write
 * committed through the old epoch on the survivors.  The seal's job is the
 * read path, a removed tail serving stale reads (PLAN 5.6, 7.2, model S2r);
 * that needs tail reads and leases, which need clocks this model lacks.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "pg_chain/chain_member.h"

#ifndef PGCHAIN_MC_BUG
#define PGCHAIN_MC_BUG 0
#endif

#define TIMER_SENDER	0
#define TIMER_COMMIT	1

static void
send4(CmcOut *out, int dst, uint8_t type, uint8_t epoch, uint8_t a, uint8_t b)
{
	CmcOutput  *o = cmc_out_push(out, CMC_OUT_SEND);

	o->dst = (uint8_t) dst;
	o->len = 4;
	o->body[0] = type;
	o->body[1] = epoch;
	o->body[2] = a;
	o->body[3] = b;
}

static void
persist(CmcOut *out, uint8_t s, uint8_t v)
{
	CmcOutput  *o = cmc_out_push(out, CMC_OUT_PERSIST);

	o->len = 2;
	o->body[0] = s;
	o->body[1] = v;
}

static uint8_t
min8(uint8_t a, uint8_t b)
{
	return a < b ? a : b;
}

/* ---- chain order within the adopted UPI ------------------------------- */

static int
pred_of(uint8_t upi, int node)
{
	for (int i = node - 1; i >= 0; i--)
		if (upi & (1u << i))
			return i;
	return -1;
}

static int
succ_of(uint8_t upi, int node, int members)
{
	for (int i = node + 1; i < members; i++)
		if (upi & (1u << i))
			return i;
	return -1;
}

static bool
is_head(const ChainMemberState *s, int node)
{
	return (s->upi & (1u << node)) && pred_of(s->upi, node) < 0;
}

/* the UPI tail: its flush is the commit point (5.3) */
static bool
is_tail(const ChainMemberState *s, int node, int members)
{
	return (s->upi & (1u << node)) && succ_of(s->upi, node, members) < 0;
}

/*
 * Forwarding: UPI members forward to the next UPI member; the UPI tail
 * forwards to the repairing member, if any (8.2 step 1: "the tail starts
 * forwarding live records to N").  The repairing member forwards to no
 * one and acks only to the leader.
 */
static int
next_hop(const ChainMemberState *s, int node, int members)
{
	int			n = succ_of(s->upi, node, members);

	if (n < 0 && (s->upi & (1u << node)) && s->rep)
		return __builtin_ctz(s->rep);
	return n;
}

static int
prev_hop(const ChainMemberState *s, int node)
{
	if (s->rep & (1u << node))
	{
		int			t = -1;

		for (int i = 0; i < 8; i++)
			if (s->upi & (1u << i))
				t = i;
		return t;
	}
	return pred_of(s->upi, node);
}

static void
answer(ChainMemberState *st, CmcOut *out, uint8_t s, uint8_t outcome)
{
	st->answered |= (uint8_t) (1u << (s - 1));
	cmc_out_push(out, CMC_OUT_REPLY)->arg = CM_REPLY(s, st->val[s - 1], outcome);
}

void
chain_member_init(const void *cfgv, int node, void *state)
{
	const ChainMemberCfg *cfg = cfgv;
	ChainMemberState *s = state;

	(void) node;
	memset(s, 0, sizeof(ChainMemberState));
	s->epoch = 1;
	s->upi = (uint8_t) ((1u << cfg->members) - 1);
}

/*
 * The engine's half of a flush: the record is now in the log.  A noop
 * may overwrite the same s (the compare-and-set replaced the record
 * before anyone else saw it).
 */
void
chain_member_apply_persist(const void *cfg, int node, void *durable,
						   const uint8_t *body, uint16_t len)
{
	ChainMemberDurable *d = durable;

	(void) cfg;
	(void) node;
	if (len == 2 && body[0] == 0)
	{
		memset(d, 0, sizeof(*d));	/* CM_TRUNCATE: repair empties the log */
		d->reloaded = 1;
		return;
	}
	if (len != 2 || body[0] < 1 || body[0] > CM_MAX_S)
		return;					/* the core never writes these */
	d->log[body[0] - 1] = body[1];
	d->present |= (uint8_t) (1u << (body[0] - 1));
	while (d->nlog < CM_MAX_S && (d->present & (1u << d->nlog)))
		d->nlog++;
}

/* What this member may ack upstream: durable here and acked downstream. */
static uint8_t
ackable(const ChainMemberState *s, int node, int members)
{
	bool		tail = is_tail(s, node, members);

#if PGCHAIN_MC_BUG == 1
	if (tail)
		return s->recv;
#endif
#if PGCHAIN_MC_BUG == 2
	if (!tail)
		return s->acked;
#endif
	if (tail)
		return s->d.nlog;
	return min8(s->acked, s->d.nlog);
}

static void head_progress(ChainMemberState *s, int node, int members, CmcOut *out);

/* Send the predecessor any newly ackable s (non-head only). */
static void
ack_up(ChainMemberState *s, int node, int members, CmcOut *out)
{
	uint8_t		a = ackable(s, node, members);
	int			p = pred_of(s->upi, node);

	if (s->rep & (1u << node))
	{
		/*
		 * Repairing: tell the leader what is durable here (8.2 step 3).
		 * Only after the truncate is durable, and only records received
		 * in this repair: until then the log is the old incarnation's, and
		 * reporting its length let the leader promote a member whose
		 * reload had not reached disk (found by RepairedPrefix and
		 * PrefixOrder; planted bug 9 restores it).
		 */
#if PGCHAIN_MC_BUG == 9
		uint8_t		have = s->d.nlog;
#else
		uint8_t		have = s->d.reloaded ? min8(s->d.nlog, s->recv) : 0;

		if (!s->d.reloaded)
			return;
#endif
		if (have + 1 > s->repdone_sent)
		{
			s->repdone_sent = (uint8_t) (have + 1);
			send4(out, members, CM_REPAIRDONE, s->epoch, have, 0);
		}
		return;
	}

	if (p >= 0 && a > s->ack_sent)
	{
		s->ack_sent = a;
		send4(out, p, CM_ACK, s->epoch, a, 0);
	}
}

static void
progress(ChainMemberState *s, int node, int members, CmcOut *out)
{
	if (is_head(s, node))
		head_progress(s, node, members, out);
	else
		ack_up(s, node, members, out);
}

/*
 * Head: answer the outcomes the chain has settled, and end the ambiguity
 * wedge once its record is acked (5.5 step 3; this model's head applies
 * nothing, so acked is the whole condition).
 */
static void
head_progress(ChainMemberState *s, int node, int members, CmcOut *out)
{
	uint8_t		upto = is_tail(s, node, members) ? s->d.nlog : min8(s->acked, s->d.nlog);

	/* a noop that is now durable: its transaction definitely aborted */
	for (uint8_t q = 1; q <= s->next_s; q++)
		if ((s->abort_wait & (1u << (q - 1))) && (s->d.present & (1u << (q - 1))) &&
			s->d.log[q - 1] == CM_NOOP)
		{
			s->abort_wait &= (uint8_t) ~(1u << (q - 1));
			cmc_out_push(out, CMC_OUT_REPLY)->arg = CM_REPLY(q, s->aborted_val[q - 1], CM_ERROR);
		}
	for (uint8_t q = 1; q <= upto && q <= s->next_s; q++)
		if (!(s->answered & (1u << (q - 1))) && s->val[q - 1] != CM_NOOP)
			answer(s, out, q, CM_COMMIT);
	if (s->amb_s && upto >= s->amb_s)
		s->amb_s = 0;
	if (s->next_s && s->answered == (uint8_t) ((1u << s->next_s) - 1))
		cmc_out_push(out, CMC_OUT_CANCEL_TIMER)->arg = TIMER_COMMIT;
}

/* Head: the sender takes every assigned record it has not taken yet. */
static void
sender_take(ChainMemberState *s, int node, int members, CmcOut *out)
{
	int			n = next_hop(s, node, members);

	while (s->sent < s->next_s)
	{
		s->sent++;
		if (n >= 0)
			send4(out, n, CM_REC, s->epoch, s->sent, s->val[s->sent - 1]);
	}
}

/* Head: the commit timeout of the oldest waiting transaction (5.2 step 6). */
static void
commit_timeout(ChainMemberState *s, CmcOut *out)
{
	uint8_t		q;

	for (q = 1; q <= s->next_s && (s->answered & (1u << (q - 1))); q++)
		;
	if (q > s->next_s)
		return;
#if PGCHAIN_MC_BUG == 5
	if (1)
#else
	if (q > s->sent)
#endif
	{
		/* the compare-and-set: the record never left; make it a noop */
#if PGCHAIN_MC_BUG == 7
		answer(s, out, q, CM_ERROR);
#else
		s->answered |= (uint8_t) (1u << (q - 1));	/* no more timeouts for q */
		s->abort_wait |= (uint8_t) (1u << (q - 1));
#endif
		s->aborted_val[q - 1] = s->val[q - 1];
		s->val[q - 1] = CM_NOOP;
		persist(out, q, CM_NOOP);
	}
	else
	{
		answer(s, out, q, CM_AMBIGUOUS);
#if PGCHAIN_MC_BUG != 6
		s->amb_s = q;
#endif
	}
	/* the next waiter, if any, gets its own timeout */
	if (s->answered != (uint8_t) ((1u << s->next_s) - 1))
		cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = TIMER_COMMIT;
}

/* Take record (rs, v) from the predecessor. */
static void
on_rec(ChainMemberState *s, int node, int members, uint8_t rs, uint8_t v, CmcOut *out)
{
	int			n = next_hop(s, node, members);

#if PGCHAIN_MC_BUG == 4
	if (rs <= s->recv || rs > CM_MAX_S)
		return;
#else
	if (rs != s->recv + 1 || rs > CM_MAX_S)
		return;					/* duplicate or gap: the resend fills it */
#endif
	s->recv = rs;
	s->val[rs - 1] = v;
	persist(out, rs, v);
	if (n >= 0)
		send4(out, n, CM_REC, s->epoch, rs, v);
}

static void
on_client(ChainMemberState *s, int node, uint32_t req, CmcOut *out)
{
	uint8_t		v = (uint8_t) (10 + req);

	if (!is_head(s, node) || s->sealed || s->amb_s || s->next_s >= CM_MAX_S)
	{
		/* not the head, or resolving (5.5 step 1): no s is assigned */
		cmc_out_push(out, CMC_OUT_REPLY)->arg = CM_REPLY(0, v, CM_REFUSED);
		return;
	}
	s->next_s++;
	s->recv = s->next_s;
	s->val[s->next_s - 1] = v;
	persist(out, s->next_s, v);
	cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = TIMER_SENDER;
	cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = TIMER_COMMIT;
#if PGCHAIN_MC_BUG == 3
	answer(s, out, s->next_s, CM_COMMIT);
#endif
}

/* ---- reconfiguration -------------------------------------------------- */

/* Adopt epoch e with UPI upi and repairing set rep (5.6 step 5). */
static void
activate(ChainMemberState *s, int node, int members, uint8_t e, uint8_t upi, uint8_t rep,
		 CmcOut *out)
{
	int			n;
	bool		was_head = is_head(s, node);

	s->epoch = e;
	s->upi = upi;
	s->rep = rep;
	s->sealed = 0;
	s->epoch_first_s = s->recv;
	/* positions are relative to the new neighbours: start over with them */
	s->ack_sent = 0;
	if (is_head(s, node))
	{
		/*
		 * The clients of records numbered before this epoch belong to the
		 * old head: they got their outcome there, or lost the connection
		 * when it went (08007 from the client's side).  A promoted head
		 * answers no one for them; it only carries them to the tail.
		 */
		if (!was_head)
			s->answered = (uint8_t) ((1u << s->recv) - 1);
#if PGCHAIN_MC_BUG == 8
		s->recv = s->acked;
		s->next_s = s->acked;
#else
		s->next_s = s->recv;	/* a promoted head continues after what it holds */
#endif
		s->sent = min8(s->sent, s->recv);
		s->acked = min8(s->acked, s->recv);
	}
	n = next_hop(s, node, members);
	if (n >= 0)
		send4(out, n, CM_HELLO_P, s->epoch, 0, 0);
	if (succ_of(s->upi, node, members) < 0)
		s->acked = s->recv;		/* the UPI tail acks what is durable here */
	progress(s, node, members, out);
}

/*
 * The leader changes the chain one epoch at a time (5.6): seal every
 * member of the current chain that stays, wait for all their SealAcks,
 * then activate the next epoch.  Three kinds of change:
 *   1 remove  a member it lost (a connection reset)
 *   2 add     a restarted member that asked to rejoin, as repairing at
 *             the tail (8.2 step 1, 8.3)
 *   3 promote the repairing member to UPI tail, once it holds every s
 *             the sealed UPI tail reported (8.2 step 3)
 */
static void
leader_begin(ChainMemberState *L, int members, int kind, int t, CmcOut *out)
{
	L->l_phase = (uint8_t) kind;
	L->l_target = (uint8_t) t;
	L->l_acks = 0;
	L->l_need = 0;
	for (int i = 0; i < members; i++)
		if (((L->upi | L->rep) & (1u << i)) && !(kind == 1 && i == t))
			send4(out, i, CM_SEAL, L->epoch, 0, 0);
}

static void
leader_maybe_activate(ChainMemberState *L, int members, CmcOut *out)
{
	uint8_t		stay = (uint8_t) ((L->upi | L->rep) & ~(L->l_phase == 1 ? 1u << L->l_target : 0u));
	uint8_t		upi = L->upi,
				rep = L->rep;

	if (L->l_phase == 0 || (L->l_acks & stay) != stay)
		return;
	if (L->l_phase == 3 && L->l_repdone < L->l_need + 1)
		return;					/* not caught up with the sealed tail */
	switch (L->l_phase)
	{
		case 1:
			upi &= (uint8_t) ~(1u << L->l_target);
			rep &= (uint8_t) ~(1u << L->l_target);
			break;
		case 2:
			rep = (uint8_t) (1u << L->l_target);
			break;
		case 3:
			upi |= rep;
			rep = 0;
			break;
	}
	L->epoch++;
	L->upi = upi;
	L->rep = rep;
	L->l_nreconf++;
	for (int i = 0; i < members; i++)
		if ((upi | rep) & (1u << i))
			send4(out, i, CM_ACTIVATE, L->epoch, upi, rep);
	if (L->l_phase == 1)
		send4(out, L->l_target, CM_REMOVED, L->epoch, 0, 0);
	L->l_phase = 0;
	L->l_repdone = 0;
}

/* Leader: start the next change, if one is due and the budget allows. */
static void
leader_next(const ChainMemberCfg *cfg, ChainMemberState *L, CmcOut *out)
{
	int			members = cfg->members;

	if (L->l_phase != 0 || L->l_nreconf >= cfg->reconfigs)
		return;
	if (L->rep && L->l_repdone > 0)
		leader_begin(L, members, 3, __builtin_ctz(L->rep), out);
	else if (!L->rep && L->l_joins)
	{
		int			t = __builtin_ctz(L->l_joins);

		L->l_joins &= (uint8_t) ~(1u << t);
		leader_begin(L, members, 2, t, out);
	}
	leader_maybe_activate(L, members, out);
}

static void
leader_step(const ChainMemberCfg *cfg, ChainMemberState *L, const CmcEvent *ev, CmcOut *out)
{
	int			members = cfg->members;

	if (ev->kind == CMC_EV_CONN_RESET)
	{
		int			t = ev->src;

		/*
		 * A reset of the leader's connection to a member is its failure
		 * detector: the member crashed, or a drop cut it off while it
		 * lives on (a false suspicion).  Safety must not depend on which
		 * (PLAN 7.2), so the leader removes it either way.
		 */
		if (t < members && ((L->upi | L->rep) & (1u << t)) && L->l_phase == 0 &&
			L->l_nreconf < cfg->reconfigs && __builtin_popcount(L->upi & ~(1u << t)) > 0)
		{
			leader_begin(L, members, 1, t, out);
			leader_maybe_activate(L, members, out);
		}
		return;
	}
	if (ev->kind != CMC_EV_RECV || ev->len != 4)
		return;
	switch (ev->body[0])
	{
		case CM_SEALACK:
			if (ev->body[1] == L->epoch && L->l_phase != 0)
			{
				L->l_acks |= (uint8_t) (1u << ev->src);
				/* the sealed UPI tail's recv is what the newcomer must reach */
				if ((L->upi & (1u << ev->src)) && ev->body[2] > L->l_need)
					L->l_need = ev->body[2];
				leader_maybe_activate(L, members, out);
			}
			return;
		case CM_JOIN:
			if (!((L->upi | L->rep) & (1u << ev->src)))
				L->l_joins |= (uint8_t) (1u << ev->src);
			break;
		case CM_REPAIRDONE:
			if (ev->body[1] == L->epoch && (L->rep & (1u << ev->src)) &&
				ev->body[2] + 1 > L->l_repdone)
				L->l_repdone = (uint8_t) (ev->body[2] + 1);
			if (L->l_phase == 3)
				leader_maybe_activate(L, members, out);
			break;
	}
	leader_next(cfg, L, out);
}

/* ---- the step function ------------------------------------------------ */

static bool
epoch_ok(const ChainMemberState *s, uint8_t e)
{
	return e == s->epoch;
}

static void
on_recv(const ChainMemberCfg *cfg, ChainMemberState *s, int node, const CmcEvent *ev, CmcOut *out)
{
	int			members = cfg->members;
	uint8_t		type = ev->body[0],
				e = ev->body[1],
				a = ev->body[2],
				b = ev->body[3];

	if (ev->src == members)
	{
		/* from the leader */
		if (type == CM_SEAL && e == s->epoch && !s->wedged && !s->removed)
		{
			s->sealed = 1;
			send4(out, members, CM_SEALACK, s->epoch, s->recv, 0);
		}
		else if (type == CM_ACTIVATE && e > s->epoch && ((a | b) & (1u << node)))
		{
			/*
			 * A crashed member is never activated into the UPI: its SealAck,
			 * if any, came from before the crash, and its volatile state is
			 * gone.  The leader only adds a restarted member as repairing
			 * (b), so an ACTIVATE naming it in the UPI (a) is stale; drop it
			 * and wait for repair.  Accepting it put an empty log into the
			 * UPI (found by PrefixOrder; the leader crossed with the crash).
			 */
#if PGCHAIN_MC_BUG != 10
			if (s->wedged && !(b & (1u << node)))
				return;
#endif
			if (s->wedged || s->removed)
			{
				/*
				 * Added back as repairing (8.2 step 1, 8.3): forget the old
				 * chain and empty the log; the tail resends from s = 1.  The
				 * model has no rows, so the base copy of 8.2 step 2 is the
				 * log replay itself (the tail's log is never truncated here).
				 */
				uint8_t		keep_hb = s->hb_sent,
							keep_lost = s->lost_clients;

				memset((char *) s + sizeof(ChainMemberDurable), 0,
					   sizeof(*s) - sizeof(ChainMemberDurable));
				s->hb_sent = keep_hb;
				s->lost_clients = keep_lost;
				persist(out, 0, CM_TRUNCATE);
			}
			activate(s, node, members, e, a, b, out);
		}
		else if (type == CM_REMOVED && !s->removed)
		{
			/*
			 * Removed from the chain (5.6): "the old head's waiters end in
			 * 08007".  A waiter whose record never left is aborted for sure,
			 * but only once its noop is durable, and a removed node writes
			 * nothing more, so it too is 08007 here.
			 */
			s->removed = 1;
			for (uint8_t q = 1; q <= s->next_s; q++)
				if (!(s->answered & (1u << (q - 1))) || (s->abort_wait & (1u << (q - 1))))
				{
					s->answered |= (uint8_t) (1u << (q - 1));
					s->abort_wait &= (uint8_t) ~(1u << (q - 1));
					cmc_out_push(out, CMC_OUT_REPLY)->arg =
						CM_REPLY(q, s->val[q - 1] == CM_NOOP ? s->aborted_val[q - 1] : s->val[q - 1],
								 CM_AMBIGUOUS);
				}
			cmc_out_push(out, CMC_OUT_CANCEL_TIMER)->arg = TIMER_COMMIT;
		}
		return;
	}
	if (!epoch_ok(s, e) || s->wedged || s->removed)
		return;
	switch (type)
	{
		case CM_REC:
			if (ev->src == prev_hop(s, node))
				on_rec(s, node, members, a, b, out);
			return;
		case CM_ACK:
			if (ev->src == succ_of(s->upi, node, members) && a > s->acked)
			{
				s->acked = min8(a, is_head(s, node) ? s->sent : s->recv);
				progress(s, node, members, out);
			}
			return;
		case CM_HELLO_P:
			if (ev->src == prev_hop(s, node))
			{
				s->ack_sent = ackable(s, node, members);
				send4(out, ev->src, CM_HELLO_S, s->epoch, s->recv, s->ack_sent);
			}
			return;
		case CM_HELLO_S:
			if (ev->src == next_hop(s, node, members))
			{
				uint8_t		have = is_head(s, node) ? s->sent : s->recv;

				/* a repairing successor's position is not an ack (8.2) */
				if (b > s->acked && ev->src == succ_of(s->upi, node, members))
				{
					s->acked = min8(b, have);
					progress(s, node, members, out);
				}
				for (uint8_t rs = a + 1; rs <= have; rs++)
					send4(out, ev->src, CM_REC, s->epoch, rs, s->val[rs - 1]);
			}
			return;
	}
}

void
chain_member_step(const void *cfgv, int node, void *state,
				  const CmcEvent *ev, CmcOut *out)
{
	const ChainMemberCfg *cfg = cfgv;
	ChainMemberState *s = state;
	int			members = cfg->members;

	out->n = 0;
	if (node == members)
	{
		/*
		 * The leader stands in for the Paxos configuration log, which is
		 * replicated and does not lose its state.  libcmc's crash budget
		 * can still pick this node, and zeroes its volatile bytes; the
		 * leader's state is all volatile here, so a crash of the leader is
		 * a model error: freeze it (it takes no further part, like a
		 * leader that lost its quorum), instead of acting on zeroes.
		 */
		if (ev->kind == CMC_EV_RESTART)
			s->wedged = 1;
		if (!s->wedged)
			leader_step(cfg, s, ev, out);
		return;
	}
	if (ev->kind == CMC_EV_RESTART)
	{
		s->wedged = 1;			/* restart means rejoin (PLAN 8.3) */
		s->lost_clients = 1;
		if (cfg->reconfigs > 0)
			send4(out, members, CM_JOIN, 0, 0, 0);
		return;
	}
	if (ev->kind == CMC_EV_RECV && ev->len == 4 && ev->src == members)
	{
		on_recv(cfg, s, node, ev, out);
		return;
	}
	if (s->wedged || s->removed)
	{
		if (ev->kind == CMC_EV_CLIENT)
			cmc_out_push(out, CMC_OUT_REPLY)->arg =
				CM_REPLY(0, (uint8_t) (10 + ev->arg), CM_REFUSED);
		return;
	}

	/*
	 * One heartbeat to the leader, on the first event a member sees, so the
	 * member-leader connection carries a message a drop can cut.  libcmc
	 * offers DROP only for messages in flight; this is what makes a false
	 * suspicion (a live member removed) part of the search.
	 */
	if (cfg->reconfigs > 0 && !s->hb_sent)
	{
		s->hb_sent = 1;
		send4(out, members, CM_HEARTBEAT, s->epoch, 0, 0);
	}

	switch (ev->kind)
	{
		case CMC_EV_CLIENT:
			on_client(s, node, ev->arg, out);
			return;
		case CMC_EV_TIMER:
			if (!is_head(s, node))
				return;
			if (ev->arg == TIMER_SENDER)
				sender_take(s, node, members, out);
			else if (ev->arg == TIMER_COMMIT)
				commit_timeout(s, out);
			return;
		case CMC_EV_FLUSHED:
			progress(s, node, members, out);
			return;
		case CMC_EV_RECV:
			if (ev->len == 4)
				on_recv(cfg, s, node, ev, out);
			return;
		case CMC_EV_CONN_RESET:
			/* the predecessor end restarts the conversation */
			if (ev->src == next_hop(s, node, members))
				send4(out, ev->src, CM_HELLO_P, s->epoch, 0, 0);
			return;
		default:
			return;
	}
}
