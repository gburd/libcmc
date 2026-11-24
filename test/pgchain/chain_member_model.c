/*
 * test/mc/chain_member_model.c: the libcmc model of src/core/chain_member.c.
 *
 * Properties (PLAN section 11 invariants that apply to a static chain).
 * A reply's arg is CM_REPLY(s, value, outcome).
 *   NoLostAck       every COMMIT (s, v) is in the tail's durable log at s
 *                   (no acknowledged write is lost, 5.2; the tail's flush
 *                   is the commit point, 5.3)
 *   AckedEverywhere every COMMIT (s, v) is durable on every member,
 *                   restarted ones too: a log survives a crash
 *   DefiniteAbort   an ERROR (s, v) is never in any member's log or
 *                   memory: the noop compare-and-set (5.2 step 6) means
 *                   "definitely aborted" is true
 *   PrefixOrder     s at node i+1 implies the same value at node i, in
 *                   its log or its memory (S1, the update propagation
 *                   invariant), for every node i that has not restarted.
 *                   A restarted node may lack records it had sent on
 *                   before its own flush (5.3 allows that); it is out of
 *                   the chain until repair (8.3).
 *   LogContiguous   no member's log holds s without every s' < s: the
 *                   gap rule of 5.3 (records are taken only at recv + 1)
 *   WedgeAfterAmbiguity (action)  no new s is assigned while an 08007
 *                   outcome is unresolved (5.5 steps 1-3).  With no row
 *                   data in this model this is the mechanism, not its
 *                   consequence (a lost update on the head); the row-level
 *                   check is an isolation spec in M2.
 *   AnsweredOnce    each s is answered at most once
 *   Reach           a COMMIT happens (vacuity witness)
 *   EventuallyReplied  with no faults, every request gets an outcome
 *
 * The transport is libpq over TCP, so the model uses libcmc's FIFO
 * network (its default), where a lost message is a connection reset that
 * both ends see.  The resend after a reset is what fills a gap.  Under
 * --net bag a dropped record vanishes silently and EventuallyReplied
 * fails, which is expected: the core assumes FIFO connections.
 */
#include <string.h>

#include "cmc.h"
#include "pg_chain/chain_member.h"

#ifndef CM_MODEL_NAME
#define CM_MODEL_NAME chain_member_model
#endif
#define STR2(x) #x
#define STR(x) STR2(x)

static void
cfg_default(void *cfg)
{
	((ChainMemberCfg *) cfg)->members = 3;
	((ChainMemberCfg *) cfg)->reconfigs = 0;
}

static int
cfg_set(void *cfg, const char *k, const char *v)
{
	if (strcmp(k, "members") == 0 && v[0] >= '1' && v[0] <= '4' && !v[1])
	{
		((ChainMemberCfg *) cfg)->members = v[0] - '0';
		return 0;
	}
	if (strcmp(k, "reconfigs") == 0 && v[0] >= '0' && v[0] <= '4' && !v[1])
	{
		((ChainMemberCfg *) cfg)->reconfigs = v[0] - '0';
		return 0;
	}
	return -1;
}

/* the members, plus the configuration leader when it may act */
static int
nnodes(const void *cfg)
{
	const ChainMemberCfg *c = cfg;

	return c->members + (c->reconfigs > 0);
}

/* members only: the leader is not a chain member */
static int
nmembers(const CmcWorld *w)
{
	return ((const ChainMemberCfg *) cmc_cfg(w))->members;
}

static int
clients(const void *cfg, int node)
{
	(void) cfg;
	return node == 0;
}

static void
describe(const void *cfg, int node, const void *sv, FILE *f)
{
	const ChainMemberState *s = sv;

	if (node == ((const ChainMemberCfg *) cfg)->members)
	{
		fprintf(f, "leader epoch=%u upi=%#x rep=%#x phase=%u target=%u acks=%#x reconfs=%u joins=%#x need=%u repdone=%u",
				s->epoch, s->upi, s->rep, s->l_phase, s->l_target, s->l_acks, s->l_nreconf,
				s->l_joins, s->l_need, s->l_repdone);
		return;
	}
	fprintf(f, "n%d%s%s e=%u upi=%#x rep=%#x%s log=[", node, s->wedged ? " WEDGED" : "",
			s->removed ? " REMOVED" : "", s->epoch, s->upi, s->rep, s->sealed ? " sealed" : "");
	for (int i = 0; i < CM_MAX_S; i++)
		if (s->d.present & (1u << i))
			fprintf(f, "%s%d:%u", (s->d.present & ((1u << i) - 1)) ? "," : "", i + 1, s->d.log[i]);
	fprintf(f, "] recv=%u acked=%u ack_sent=%u", s->recv, s->acked, s->ack_sent);
	if (node == 0)
		fprintf(f, " next_s=%u sent=%u answered=%#x amb_s=%u", s->next_s, s->sent,
				s->answered, s->amb_s);
}

static void
describe_msg(const void *cfg, const uint8_t *b, FILE *f)
{
	static const char *const nm[] = {"?", "REC", "ACK", "HELLO_P", "HELLO_S", "SEAL",
		"SEALACK", "ACTIVATE", "REMOVED", "HEARTBEAT", "JOIN", "REPAIRDONE"};

	(void) cfg;
	fprintf(f, "%s(e=%u,%u,%u)", b[0] <= 11 ? nm[b[0]] : "?", b[1], b[2], b[3]);
}

static const CmcCore core = {
	.name = "chain_member",
	.state_size = sizeof(ChainMemberState),
	.durable_size = sizeof(ChainMemberDurable),
	.cfg_size = sizeof(ChainMemberCfg),
	.cfg_default = cfg_default,
	.cfg_set = cfg_set,
	.nnodes = nnodes,
	.init = chain_member_init,
	.step = chain_member_step,
	.apply_persist = chain_member_apply_persist,
	.describe = describe,
	.describe_msg = describe_msg,
	.clients = clients,
};

static const ChainMemberState *
st(const CmcWorld *w, int n)
{
	return cmc_node(w, n);
}

/* COMMIT (s, v) replies must be durable at s on the given nodes */
static bool
commits_durable_on(const CmcWorld *w, int from, int to, CmcWhy *why)
{
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (CM_REPLY_O(a) != CM_COMMIT)
			continue;
		for (int n = from; n <= to; n++)
		{
			const ChainMemberDurable *d = &st(w, n)->d;
			unsigned	q = CM_REPLY_S(a);

			if (!(d->present & (1u << (q - 1))) || d->log[q - 1] != CM_REPLY_V(a))
			{
				cmc_why(why, "COMMIT s=%u value %u not durable on node %d", q, CM_REPLY_V(a), n);
				return false;
			}
		}
	}
	return true;
}

/*
 * The latest UPI: the leader's if there is one, else epoch 1's.  Members
 * may lag behind it; the leader's view is the configuration.
 */
static uint8_t
current_upi(const CmcWorld *w)
{
	const ChainMemberCfg *c = cmc_cfg(w);

	if (c->reconfigs > 0 && !st(w, c->members)->wedged)
		return st(w, c->members)->upi;
	if (c->reconfigs > 0)
	{
		/* a crashed leader: its last UPI is the highest epoch any member adopted */
		uint8_t		e = 0,
					u = 0;

		for (int n = 0; n < c->members; n++)
			if (st(w, n)->epoch > e)
			{
				e = st(w, n)->epoch;
				u = st(w, n)->upi;
			}
		if (u)
			return u;
	}
	return (uint8_t) ((1u << c->members) - 1);
}

/*
 * No acknowledged write is lost: every COMMIT is durable on the tail of
 * the latest UPI, the member a new epoch's reads and commits rest on
 * (5.3, 5.6).
 */
static bool
no_lost_ack(const CmcWorld *w, CmcWhy *why)
{
	uint8_t		upi = current_upi(w);
	int			tail = 31 - __builtin_clz((unsigned) upi);

	return commits_durable_on(w, tail, tail, why);
}

/* Every COMMIT is durable on every member of the latest UPI (5.3). */
static bool
acked_everywhere(const CmcWorld *w, CmcWhy *why)
{
	uint8_t		upi = current_upi(w);

	for (int n = 0; n < nmembers(w); n++)
		if ((upi & (1u << n)) && !commits_durable_on(w, n, n, why))
			return false;
	return true;
}

static bool
definite_abort(const CmcWorld *w, CmcWhy *why)
{
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (CM_REPLY_O(a) != CM_ERROR)
			continue;
		for (int n = 0; n < nmembers(w); n++)
		{
			const ChainMemberState *m = st(w, n);
			unsigned	q = CM_REPLY_S(a);

			if (((m->d.present & (1u << (q - 1))) && m->d.log[q - 1] == CM_REPLY_V(a)) ||
				(!m->wedged && m->recv >= q && m->val[q - 1] == CM_REPLY_V(a)))
			{
				cmc_why(why, "ERROR s=%u value %u, but node %d holds it", q, CM_REPLY_V(a), n);
				return false;
			}
		}
	}
	return true;
}

static bool
prefix_order(const CmcWorld *w, CmcWhy *why)
{
	uint8_t		upi = current_upi(w);

	for (int n = 0; n < nmembers(w); n++)
	{
		const ChainMemberState *up = st(w, n),
				   *dn;
		int			m;

		if (!(upi & (1u << n)) || up->wedged)
			continue;
		for (m = n + 1; m < nmembers(w) && !(upi & (1u << m)); m++)
			;
		if (m >= nmembers(w))
			continue;
		dn = st(w, m);
		for (int i = 0; i < dn->d.nlog; i++)
			if (i >= up->d.nlog || up->d.log[i] != dn->d.log[i])
			{
				/* the upstream node may not have flushed it yet */
				if (i < up->recv && up->val[i] == dn->d.log[i])
					continue;
				cmc_why(why, "node %d logs s=%d=%u, node %d does not", m, i + 1,
						dn->d.log[i], n);
				return false;
			}
	}
	return true;
}

static bool
log_contiguous(const CmcWorld *w, CmcWhy *why)
{
	for (int n = 0; n < nmembers(w); n++)
	{
		const ChainMemberDurable *d = &st(w, n)->d;

		if (d->present != (uint8_t) ((1u << d->nlog) - 1))
		{
			cmc_why(why, "node %d's log has a hole (present %#x)", n, d->present);
			return false;
		}
	}
	return true;
}

/* an unresolved 08007: an AMBIGUOUS s node 0 (the only client head) has not seen acked */
static bool
unresolved_ambiguity(const CmcWorld *w)
{
	const ChainMemberState *h = st(w, 0);
	unsigned	upto = nmembers(w) == 1 ? h->d.nlog : (h->acked < h->d.nlog ? h->acked : h->d.nlog);

	if (h->wedged || h->removed)
		return false;

	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (CM_REPLY_O(a) == CM_AMBIGUOUS && CM_REPLY_S(a) > upto)
			return true;
	}
	return false;
}

static bool
wedge_after_ambiguity(const CmcWorld *pre, CmcAction a, const CmcWorld *post, CmcWhy *why)
{
	(void) a;
	/*
	 * A new s means a record a client sent: next_s grew while recv did not
	 * already cover it.  Re-adopting next_s = recv at activation is
	 * bookkeeping, not a new write, and the clients of a head that
	 * crashed are gone with it (lost_clients).
	 */
	if (!st(pre, 0)->wedged && !st(pre, 0)->lost_clients &&
		st(post, 0)->next_s > st(pre, 0)->next_s && st(post, 0)->next_s > st(pre, 0)->recv &&
		unresolved_ambiguity(pre))
	{
		cmc_why(why, "s=%u assigned while an 08007 is unresolved", st(post, 0)->next_s);
		return false;
	}
	return true;
}

/* A sealed member assigns no new s (5.6 step 1). */
static bool
sealed_no_new_s(const CmcWorld *pre, CmcAction a, const CmcWorld *post, CmcWhy *why)
{
	(void) a;
	for (int n = 0; n < nmembers(pre); n++)
		if (st(pre, n)->sealed && st(post, n)->epoch == st(pre, n)->epoch &&
			st(post, n)->next_s > st(pre, n)->next_s)
		{
			cmc_why(why, "node %d assigned s=%u while sealed", n, st(post, n)->next_s);
			return false;
		}
	return true;
}

/* A reconfiguration happens (vacuity, when the leader may act). */
static bool
reach_reconfig(const CmcWorld *w, CmcWhy *why)
{
	const ChainMemberCfg *c = cmc_cfg(w);

	(void) why;
	return c->reconfigs == 0 || st(w, c->members)->epoch > 1;
}

/*
 * A COMMIT answered by a member in epoch 2 or later: the reconfigured
 * chain still commits.  Only node 0 has clients, so this needs a removal
 * that keeps node 0, e.g. losing the tail.
 */
static bool
reach_commit_after_reconfig(const CmcWorld *w, CmcWhy *why)
{
	const ChainMemberCfg *c = cmc_cfg(w);
	const ChainMemberState *h = st(w, 0);

	(void) why;
	if (c->reconfigs == 0)
		return true;
	if (h->epoch < 2 || h->wedged || h->removed)
		return false;
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	r;

		cmc_reply(w, i, &node, &r);
		if (CM_REPLY_O(r) == CM_COMMIT && CM_REPLY_S(r) > h->epoch_first_s)
			return true;
	}
	return false;
}

/*
 * Repair finished: a member that restarted is back in the UPI, and its
 * log holds every s of the UPI tail (8.2, 8.3).
 */
static bool
reach_rejoined(const CmcWorld *w, CmcWhy *why)
{
	const ChainMemberCfg *c = cmc_cfg(w);
	const ChainMemberState *L;

	(void) why;
	if (c->reconfigs < 3)
		return true;
	L = st(w, c->members);
	return L->epoch >= 4 && __builtin_popcount(L->upi) == c->members;
}

/*
 * Once repair has emptied a member's log (the truncate is durable), the
 * log agrees with the UPI tail's wherever both hold an s: it reloads
 * from the tail (8.2 step 2), so nothing from before its crash survives.
 * Before that, its old log is the last copy of last resort (8.3) and may
 * disagree, e.g. hold a record the chain turned into a noop.  Checked
 * while it is repairing; once promoted, PrefixOrder covers it.
 */
static bool
repaired_prefix(const CmcWorld *w, CmcWhy *why)
{
	const ChainMemberCfg *c = cmc_cfg(w);
	uint8_t		upi = current_upi(w);
	int			tail;
	const ChainMemberDurable *t;

	if (c->reconfigs == 0 || upi == 0)
		return true;
	tail = 31 - __builtin_clz((unsigned) upi);
	t = &st(w, tail)->d;
	for (int n = 0; n < c->members; n++)
	{
		const ChainMemberState *m = st(w, n);

		if (n == tail || !m->d.reloaded || m->wedged || m->removed || !(m->rep & (1u << n)))
			continue;
		for (int i = 0; i < m->d.nlog; i++)
			if (i < t->nlog && m->d.log[i] != t->log[i])
			{
				cmc_why(why, "node %d holds s=%d=%u, the tail %d holds %u", n, i + 1,
						m->d.log[i], tail, t->log[i]);
				return false;
			}
	}
	return true;
}

static bool
answered_once(const CmcWorld *w, CmcWhy *why)
{
	unsigned	seen = 0;

	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;
		unsigned	q;

		cmc_reply(w, i, &node, &a);
		q = CM_REPLY_S(a);
		if (q == 0)
			continue;			/* REFUSED: no s */
		if (seen & (1u << q))
		{
			cmc_why(why, "s=%u answered twice", q);
			return false;
		}
		seen |= 1u << q;
	}
	return true;
}

static bool
reach(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (CM_REPLY_O(a) == CM_COMMIT)
			return true;
	}
	return false;
}

static bool
has_outcome(const CmcWorld *w, unsigned outcome)
{
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (CM_REPLY_O(a) == outcome)
			return true;
	}
	return false;
}

/* vacuity witnesses: each outcome the model claims to handle happens */
static bool
reach_error(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return has_outcome(w, CM_ERROR);
}

static bool
reach_ambiguous(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return has_outcome(w, CM_AMBIGUOUS);
}

static bool
reach_refused(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return has_outcome(w, CM_REFUSED);
}

/* an 08007 whose record the chain then commits: the case 5.5 exists for */
static bool
reach_ambiguous_committed(const CmcWorld *w, CmcWhy *why)
{
	int			tail = nmembers(w) - 1;

	(void) why;
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;
		unsigned	q;

		cmc_reply(w, i, &node, &a);
		q = CM_REPLY_S(a);
		if (CM_REPLY_O(a) == CM_AMBIGUOUS && (st(w, tail)->d.present & (1u << (q - 1))) &&
			st(w, tail)->d.log[q - 1] == CM_REPLY_V(a))
			return true;
	}
	return false;
}

static bool
eventually_replied(const CmcWorld *w, CmcWhy *why)
{
	/*
	 * A crashed member blocks progress until reconfiguration, and a
	 * crashed head's clients lose their connection (they see an error,
	 * not an outcome): exempt both.
	 */
	for (int n = 0; n < nmembers(w); n++)
		if (st(w, n)->wedged || st(w, n)->lost_clients)
			return true;
	if (cmc_nreplies(w) != cmc_requests(w, 0))
	{
		cmc_why(why, "%d of %d requests replied", cmc_nreplies(w), cmc_requests(w, 0));
		return false;
	}
	return true;
}

static const CmcProp props[] = {
	CMC_INVARIANT(NoLostAck, no_lost_ack),
	CMC_INVARIANT(AckedEverywhere, acked_everywhere),
	CMC_INVARIANT(DefiniteAbort, definite_abort),
	CMC_INVARIANT(PrefixOrder, prefix_order),
	CMC_INVARIANT(LogContiguous, log_contiguous),
	CMC_INVARIANT(RepairedPrefix, repaired_prefix),
	CMC_INVARIANT(AnsweredOnce, answered_once),
	CMC_ACTION_PROPERTY(WedgeAfterAmbiguity, wedge_after_ambiguity),
	CMC_ACTION_PROPERTY(SealedNoNewS, sealed_no_new_s),
	CMC_WITNESS_PROP(ReachReconfig, reach_reconfig),
	CMC_WITNESS_PROP(ReachCommitAfterReconfig, reach_commit_after_reconfig),
	CMC_WITNESS_PROP(ReachRejoined, reach_rejoined),
	CMC_WITNESS_PROP(Reach, reach),
	CMC_WITNESS_PROP(ReachError, reach_error),
	CMC_WITNESS_PROP(ReachAmbiguous, reach_ambiguous),
	CMC_WITNESS_PROP(ReachRefused, reach_refused),
	CMC_WITNESS_PROP(ReachAmbiguousCommitted, reach_ambiguous_committed),
	CMC_EVENTUALLY_PROP(EventuallyReplied, eventually_replied),
};

const CmcModel CM_MODEL_NAME = {
	.name = STR(CM_MODEL_NAME),
	.core = &core,
	.props = props,
	.nprops = sizeof(props) / sizeof(props[0]),
};
