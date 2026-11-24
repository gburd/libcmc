/*
 * test/mc/paxos_model.c: the libcmc model of src/core/paxos.c.
 *
 * Properties (PLAN section 11: "one value per config slot", research/03
 * section 4.1):
 *   Agreement       no two nodes learn different values for one slot
 *   OneValuePerBallot  no two acceptors durably hold different values for
 *                   one (ballot, slot): ballots are unique
 *   OneValuePerSlot no two values are chosen for one slot: a value is
 *                   chosen when q2 acceptors durably accepted it at one
 *                   ballot.  Read from the acceptors' disks, not from any
 *                   leader, so it holds even if no one learned it.
 *   Agreement       also: every COMMITTED (slot, v) agrees with what every
 *                   node learned at slot
 *   LearnedChosen   every learned value and COMMITTED reply is safe on the
 *                   acceptors' disks: for some ballot b, q2 of them hold it
 *                   at a ballot >= b and none holds another value there
 *                   (see safe_value(); no learner runs ahead of the disks)
 *   ChosenStable (action)  a value chosen at one ballot stays safe
 *   RefusedNeverChosen  a REFUSED value was never proposed, so no
 *                   acceptor holds it (AMBIGUOUS values may be chosen later)
 *   AnsweredOnce    each request hears at most one outcome
 *   Reach*          a value is chosen; a value is chosen after a leader
 *                   change (a second ballot); AMBIGUOUS happens;
 *                   a recovered value is re-proposed and chosen
 *   EventuallyDecided  with no faults, every client hears an outcome
 *
 * Client values are PX_VALUE(node, request), unique for up to 4 requests
 * per candidate; shortcut: --clients 5 or more makes values of two nodes
 * collide, raise the stride in PX_VALUE before running that.
 *
 * Bounds: 3 voters, 2 candidates, majorities, 2 slots.  A crash of an
 * acceptor keeps a prefix of its pending persists; a DROP resets a
 * connection (FIFO).
 */
#include <stdlib.h>
#include <string.h>

#include "cmc.h"
#include "pg_chain/paxos.h"

static void
cfg_default(void *cfgv)
{
	PaxosCfg   *cfg = cfgv;

	cfg->nodes = 3;
	cfg->voters = 3;
	cfg->candidates = 2;
	cfg->change = 0;
	cfg->change2 = 0;
	cfg->q1 = 2;
	cfg->q2 = 2;
	cfg->slots = 2;
	cfg->max_round = 2;
}

static int
digit(const char *v, int lo, int hi)
{
	if (v[0] < '0' + lo || v[0] > '0' + hi || v[1])
		return -1;
	return v[0] - '0';
}

static int
cfg_set(void *cfgv, const char *k, const char *v)
{
	PaxosCfg   *cfg = cfgv;
	int			x;

	if (strcmp(k, "voters") == 0 && (x = digit(v, 1, PX_MAX_NODES)) >= 0)
	{
		cfg->voters = x;
		if (cfg->nodes < x)
			cfg->nodes = x;
	}
	else if (strcmp(k, "nodes") == 0 && (x = digit(v, 1, PX_MAX_NODES)) >= 0)
		cfg->nodes = x;
	else if (strcmp(k, "candidates") == 0 && (x = digit(v, 1, PX_MAX_NODES)) >= 0)
		cfg->candidates = x;
	else if (strcmp(k, "q1") == 0 && (x = digit(v, 1, PX_MAX_NODES)) >= 0)
		cfg->q1 = x;
	else if (strcmp(k, "q2") == 0 && (x = digit(v, 1, PX_MAX_NODES)) >= 0)
		cfg->q2 = x;
	else if (strcmp(k, "change") == 0 || strcmp(k, "change2") == 0)
	{
		/* a member bitmap in hex, 1..1f: the VoterCfg a candidate proposes */
		char	   *end;
		long		m = strtol(v, &end, 16);

		if (*end || m < 1 || m > 0x1f)
			return -1;
		if (k[6])
			cfg->change2 = (int) m;
		else
			cfg->change = (int) m;
	}
	else if (strcmp(k, "slots") == 0 && (x = digit(v, 1, PX_MAX_SLOTS)) >= 0)
		cfg->slots = x;
	else if (strcmp(k, "rounds") == 0 && (x = digit(v, 1, 9)) >= 0)
		cfg->max_round = x;
	else
		return -1;
	if (cfg->candidates > cfg->voters || cfg->q1 > cfg->voters || cfg->q2 > cfg->voters ||
		cfg->voters > cfg->nodes || cfg->change >= (1 << cfg->nodes) ||
		cfg->change2 >= (1 << cfg->nodes))
		return -1;
	/*
	 * q1 + q2 > V is Flexible Paxos's condition; a smaller pair is accepted
	 * on purpose, so the model can show the agreement violation it allows.
	 */
	return 0;
}

static int
nnodes(const void *cfg)
{
	return ((const PaxosCfg *) cfg)->nodes;
}

static int
clients(const void *cfg, int node)
{
	return node < ((const PaxosCfg *) cfg)->candidates;
}

static void
describe(const void *cfgv, int node, const void *sv, FILE *f)
{
	const PaxosCfg *cfg = cfgv;
	const PaxosState *s = sv;

	fprintf(f, "n%d prom=%d.%d/%d.%d last=%d.%d acc=[", node,
			PX_ROUND(s->d.promised), PX_NODE(s->d.promised),
			PX_ROUND(s->prom_v), PX_NODE(s->prom_v),
			PX_ROUND(s->d.last_ballot), PX_NODE(s->d.last_ballot));
	for (int k = 0; k < cfg->slots; k++)
		fprintf(f, "%s%d.%d:%d", k ? " " : "", PX_ROUND(s->d.acc_b[k]),
				PX_NODE(s->d.acc_b[k]), s->d.acc_v[k]);
	fprintf(f, "] learned=[");
	for (int k = 0; k < cfg->slots; k++)
		fprintf(f, "%s%d", k ? " " : "", s->learned[k]);
	fprintf(f, "]");
	if (node < cfg->candidates)
		fprintf(f, " phase=%d ballot=%d.%d p1=%#x prop=[%d %d %d] pending=%d@%d%s",
				s->phase, PX_ROUND(s->ballot), PX_NODE(s->ballot), s->p1,
				s->prop_v[0], s->prop_v[1], s->prop_v[2], s->pending,
				s->pending_slot == PX_NO_SLOT ? -1 : s->pending_slot,
				s->gave_up ? " GAVE_UP" : "");
	fprintf(f, "\n");
}

static void
describe_msg(const void *cfg, const uint8_t *b, FILE *f)
{
	static const char *const names[] = {"?", "PREPARE", "PROMISE", "ACCEPT",
	"ACCEPTED", "NACK", "COMMIT"};

	(void) cfg;
	fprintf(f, "%s(b=%d.%d,%d,%d)", b[0] <= PX_COMMIT ? names[b[0]] : "?",
			PX_ROUND(b[1]), PX_NODE(b[1]), b[2], b[3]);
}

/*
 * Symmetry (libcmc's copy only: libcmc 2.4, CmcCore.symmetry).  The
 * candidates 0..C-1 are distinguished: ballots and client values carry
 * their ids, and only they take clients.  The other nodes are plain
 * acceptors and learners, interchangeable when they agree on everything
 * cfg says about them: whether they are initial voters, and whether the
 * VoterCfg of change and change2 includes them, so permuting within a
 * class leaves every member bitmap in a value unchanged.  No message or
 * persist body names a non-candidate (they carry ballots, slots and
 * values), so permute_msg() has nothing to do; the node ids in a state
 * are the promise and accepted bitmaps, p1 and p2, and the rows of pb and
 * pv, which are indexed by acceptor.  These helpers live here, not in
 * paxos.c, so the shipped core file stays as pg_chain's; they know its
 * state layout through paxos.h, as the properties below do.
 */
static int
symmetry(const void *cfgv, int *cls, int max)
{
	const PaxosCfg *cfg = cfgv;

	for (int n = 0; n < cfg->nodes && n < max; n++)
		cls[n] = n < cfg->candidates ? 0 :
			1 + (n < cfg->voters) + 2 * ((cfg->change >> n) & 1) +
			4 * ((cfg->change2 >> n) & 1);
	return cfg->nodes;
}

static uint8_t
permute_mask(const PaxosCfg *cfg, const uint8_t *perm, uint8_t m)
{
	uint8_t		x = 0;

	for (int a = 0; a < cfg->nodes; a++)
		if (m & (1u << a))
			x |= (uint8_t) (1u << perm[a]);
	return x;
}

static void
permute(const void *cfgv, const uint8_t *perm, int node, void *sv)
{
	const PaxosCfg *cfg = cfgv;
	PaxosState *s = sv;
	PaxosState	old = *s;

	(void) node;
	s->p1 = permute_mask(cfg, perm, old.p1);
	for (int k = 0; k < PX_MAX_SLOTS; k++)
		s->p2[k] = permute_mask(cfg, perm, old.p2[k]);
	for (int a = 0; a < cfg->nodes; a++)
	{
		memcpy(s->pb[perm[a]], old.pb[a], PX_MAX_SLOTS);
		memcpy(s->pv[perm[a]], old.pv[a], PX_MAX_SLOTS);
	}
}

static void
permute_msg(const void *cfg, const uint8_t *perm, uint8_t *body, uint16_t len)
{
	(void) cfg;
	(void) perm;
	(void) body;
	(void) len;
}

static const CmcCore core = {
	.name = "paxos",
	.state_size = sizeof(PaxosState),
	.durable_size = sizeof(PaxosDurable),
	.cfg_size = sizeof(PaxosCfg),
	.cfg_default = cfg_default,
	.cfg_set = cfg_set,
	.nnodes = nnodes,
	.init = paxos_init,
	.step = paxos_step,
	.apply_persist = paxos_apply_persist,
	.describe = describe,
	.describe_msg = describe_msg,
	.clients = clients,
	.symmetry = symmetry,
	.permute = permute,
	.permute_msg = permute_msg,
};

/* ---- properties ------------------------------------------------------- */

static const PaxosCfg *
cfg_of(const CmcWorld *w)
{
	return cmc_cfg(w);
}

static const PaxosState *
st(const CmcWorld *w, int n)
{
	return cmc_node(w, n);
}

static bool safe_value(const CmcWorld *w, int k, uint8_t v);

/*
 * The configuration governing slot k, as the truth of this world: the
 * values safe on the acceptors' disks at slots below k, passed through
 * the core's own paxos_config_at().  A leader proposes at k only after
 * learning every slot below it, so a correct core agrees with this; a
 * core that uses a stale configuration (bugs 107, 109, 110) does not, and
 * the properties below judge it by this one.
 */
static void
slot_cfg(const CmcWorld *w, int k, uint8_t *mem, int *q2)
{
	const PaxosCfg *cfg = cfg_of(w);
	uint8_t		log[PX_MAX_SLOTS] = {0};
	int			q1;

	for (int j = 0; j < k; j++)
		for (int n = 0; n < cfg->nodes && !log[j]; n++)
		{
			uint8_t		v = st(w, n)->d.acc_v[j];

			if (st(w, n)->d.acc_b[j] && safe_value(w, j, v))
				log[j] = v;
		}
	paxos_config_at(cfg, log, k, mem, &q1, q2);
}

/*
 * The value chosen at slot k now: some ballot at which a q2 of slot k's
 * configuration durably holds one value.  0 = none.  Returns -1 (and
 * explains) if two values are.
 */
static int
chosen_now(const CmcWorld *w, int k, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);
	uint8_t		mem;
	int			q2,
				v = 0;

	slot_cfg(w, k, &mem, &q2);
	for (int i = 0; i < cfg->nodes; i++)
	{
		uint8_t		b = st(w, i)->d.acc_b[k],
					val = st(w, i)->d.acc_v[k];
		int			n = 0;

		if (!b || !(mem & (1u << i)))
			continue;
		for (int j = 0; j < cfg->nodes; j++)
			if ((mem & (1u << j)) && st(w, j)->d.acc_b[k] == b && st(w, j)->d.acc_v[k] == val)
				n++;
		if (n < q2)
			continue;
		if (v && v != val)
		{
			if (why)
				cmc_why(why, "slot %d: %d and %d are both chosen", k, v, val);
			return -1;
		}
		v = val;
	}
	return v;
}

static bool
one_value_per_slot(const CmcWorld *w, CmcWhy *why)
{
	for (int k = 0; k < cfg_of(w)->slots; k++)
		if (chosen_now(w, k, why) < 0)
			return false;
	return true;
}

/*
 * One ballot, one value per slot, on every acceptor's disk.  Every
 * argument for Paxos safety rests on this; a leader whose ballot is not
 * unique (reused after a restart) breaks it long before two learners
 * disagree.
 */
static bool
one_value_per_ballot(const CmcWorld *w, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);

	for (int k = 0; k < cfg->slots; k++)
		for (int i = 0; i < cfg->nodes; i++)
			for (int j = i + 1; j < cfg->nodes; j++)
			{
				const PaxosDurable *a = &st(w, i)->d,
						   *b = &st(w, j)->d;

				if (a->acc_b[k] && a->acc_b[k] == b->acc_b[k] && a->acc_v[k] != b->acc_v[k])
				{
					cmc_why(why, "slot %d ballot %d.%d: node %d holds %d, node %d holds %d",
							k, PX_ROUND(a->acc_b[k]), PX_NODE(a->acc_b[k]), i, a->acc_v[k],
							j, b->acc_v[k]);
					return false;
				}
			}
	return true;
}

static bool
agreement(const CmcWorld *w, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);

	for (int k = 0; k < cfg->slots; k++)
	{
		int			v = 0;

		for (int n = 0; n < cfg->nodes; n++)
		{
			int			l = st(w, n)->learned[k];

			if (!l)
				continue;
			if (v && v != l)
			{
				cmc_why(why, "slot %d: one node learned %d, node %d learned %d",
						k, v, n, l);
				return false;
			}
			v = l;
		}
		/* and every COMMITTED reply for k agrees with them */
		for (int i = 0; i < cmc_nreplies(w); i++)
		{
			int			node;
			uint32_t	a;

			cmc_reply(w, i, &node, &a);
			if (PX_REPLY_O(a) != PX_COMMITTED || (int) PX_REPLY_S(a) != k)
				continue;
			if (v && (int) PX_REPLY_V(a) != v)
			{
				cmc_why(why, "slot %d: node %d told its client %u was chosen, a node learned %d",
						k, node, PX_REPLY_V(a), v);
				return false;
			}
			v = (int) PX_REPLY_V(a);
		}
	}
	return true;
}

/*
 * Is v safe at slot k, judged from the acceptors' disks alone?  If v was
 * chosen at ballot b (a q2 of slot k's configuration accepted (b, v)),
 * Paxos keeps it: every proposal at a ballot above b carries v, so those
 * q2 acceptors hold v at some ballot >= b from then on, and no member of
 * the configuration ever holds another value at a ballot >= b.  So v is
 * safe iff some ballot b has both.  A learner that counted ACCEPTEDs from
 * several ballots (A at b1, B and D at b2) fails this as soon as a third
 * acceptor holds another value between b1 and b2.  Counting acceptors
 * that hold v at any ballot, with no second condition, passes such a
 * learner (found in review).
 */
static bool
safe_value(const CmcWorld *w, int k, uint8_t v)
{
	const PaxosCfg *cfg = cfg_of(w);
	uint8_t		mem;
	int			q2;

	slot_cfg(w, k, &mem, &q2);
	for (int i = 0; i < cfg->nodes; i++)
	{
		uint8_t		b = st(w, i)->d.acc_b[k];
		int			hold = 0;
		bool		other = false;

		if (!b || st(w, i)->d.acc_v[k] != v || !(mem & (1u << i)))
			continue;
		for (int a = 0; a < cfg->nodes; a++)
		{
			const PaxosDurable *d = &st(w, a)->d;

			if (!(mem & (1u << a)))
				continue;
			if (d->acc_b[k] >= b && d->acc_v[k] == v)
				hold++;
			else if (d->acc_b[k] >= b)
				other = true;
		}
		if (hold >= q2 && !other)
			return true;
	}
	return false;
}

/* Every learned value and every COMMITTED reply is safe at its slot. */
static bool
learned_chosen(const CmcWorld *w, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);

	for (int n = 0; n < cfg->nodes; n++)
		for (int k = 0; k < cfg->slots; k++)
		{
			uint8_t		l = st(w, n)->learned[k];

			if (l && !safe_value(w, k, l))
			{
				cmc_why(why, "node %d learned %d at slot %d, which no ballot's quorum holds", n, l, k);
				return false;
			}
		}
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (PX_REPLY_O(a) == PX_COMMITTED &&
			!safe_value(w, (int) PX_REPLY_S(a), (uint8_t) PX_REPLY_V(a)))
		{
			cmc_why(why, "node %d told its client %u is chosen at slot %u, which no ballot's quorum holds",
					node, PX_REPLY_V(a), PX_REPLY_S(a));
			return false;
		}
	}
	return true;
}

/*
 * A value chosen now stays safe after the next step, crashes included.
 * "Chosen now" is the exact condition, a q2 of the slot's configuration
 * holding (b, v) at one ballot b; safe_value() is only necessary for
 * having been chosen (value 1 at ballots 1 and 3 on two disks was never
 * chosen), so it cannot be the premise.  This sees a choice nobody
 * learned that a later leader overwrites, which the state invariants
 * above, reading the disks as they are now, may miss (found in review).
 * shortcut: the premise is checked only while the choosing quorum still
 * holds (b, v) exactly; a ghost history of choices would cover later
 * steps, which libcmc's library engine does not offer yet (the libcmc
 * asks, item 5).
 */
static bool
chosen_stable(const CmcWorld *pre, CmcAction act, const CmcWorld *post, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(pre);

	(void) act;
	for (int k = 0; k < cfg->slots; k++)
	{
		int			v = chosen_now(pre, k, NULL);

		if (v > 0 && !safe_value(post, k, (uint8_t) v))
		{
			cmc_why(why, "slot %d: %d was chosen and no longer is safe", k, v);
			return false;
		}
	}
	return true;
}

/* A REFUSED value was never proposed, so no acceptor holds it. */
static bool
refused_never_chosen(const CmcWorld *w, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);

	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (PX_REPLY_O(a) != PX_REFUSED)
			continue;
		for (int n = 0; n < cfg->nodes; n++)
			for (int k = 0; k < cfg->slots; k++)
				if (st(w, n)->d.acc_b[k] && st(w, n)->d.acc_v[k] == PX_REPLY_V(a))
				{
					cmc_why(why, "value %u was REFUSED, node %d accepted it at slot %d",
							PX_REPLY_V(a), n, k);
					return false;
				}
	}
	return true;
}

/* each client request hears at most one outcome */
static bool
answered_once(const CmcWorld *w, CmcWhy *why)
{
	for (int i = 0; i < cmc_nreplies(w); i++)
		for (int j = i + 1; j < cmc_nreplies(w); j++)
		{
			int			ni,
						nj;
			uint32_t	ai,
						aj;

			cmc_reply(w, i, &ni, &ai);
			cmc_reply(w, j, &nj, &aj);
			if (PX_REPLY_V(ai) == PX_REPLY_V(aj))
			{
				cmc_why(why, "value %u answered twice", PX_REPLY_V(ai));
				return false;
			}
		}
	return true;
}

static bool
reach_chosen(const CmcWorld *w, CmcWhy *why)
{
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (PX_REPLY_O(a) == PX_COMMITTED)
		{
			cmc_why(why, "a value is chosen");
			return true;
		}
	}
	return false;
}

/* a value is chosen by a leader in round 2: the log survived a leader change */
static bool
reach_second_leader(const CmcWorld *w, CmcWhy *why)
{
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (PX_REPLY_O(a) == PX_COMMITTED && PX_ROUND(PX_REPLY_B(a)) >= 2)
		{
			cmc_why(why, "chosen in round 2");
			return true;
		}
	}
	return false;
}

/* a client's value is chosen at a slot that a VoterCfg below governs */
static bool
reach_reconfigured(const CmcWorld *w, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);

	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;
		uint8_t		mem;
		int			q2;

		cmc_reply(w, i, &node, &a);
		if (PX_REPLY_O(a) != PX_COMMITTED || PX_IS_VCFG(PX_REPLY_V(a)))
			continue;
		slot_cfg(w, (int) PX_REPLY_S(a), &mem, &q2);
		if (mem != (uint8_t) ((1u << cfg->voters) - 1))
		{
			cmc_why(why, "%u chosen at slot %u under members %#x", PX_REPLY_V(a),
					PX_REPLY_S(a), mem);
			return true;
		}
	}
	return false;
}

static bool
has_outcome(const CmcWorld *w, unsigned o)
{
	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			node;
		uint32_t	a;

		cmc_reply(w, i, &node, &a);
		if (PX_REPLY_O(a) == o)
			return true;
	}
	return false;
}

static bool
reach_ambiguous(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return has_outcome(w, PX_AMBIGUOUS);
}

static bool
reach_refused(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return has_outcome(w, PX_REFUSED);
}

/* a leader learned a value some other candidate's client submitted */
static bool
reach_recovered(const CmcWorld *w, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);

	for (int n = 0; n < cfg->candidates; n++)
		for (int k = 0; k < cfg->slots; k++)
		{
			int			l = st(w, n)->learned[k];

			if (l && l != PX_NOOP && !PX_IS_VCFG(l) && PX_VALUE_NODE(l) != n &&
				st(w, n)->phase == PX_PH_LEAD &&
				st(w, n)->prop_v[k] == l)
			{
				cmc_why(why, "node %d chose node %d's value %d at slot %d", n,
						PX_VALUE_NODE(l), l, k);
				return true;
			}
		}
	return false;
}

/*
 * With no faults left, every request hears an outcome.  A candidate that
 * restarted lost its client with it, and its pending value with it.
 */
static bool
eventually_decided(const CmcWorld *w, CmcWhy *why)
{
	const PaxosCfg *cfg = cfg_of(w);

	for (int n = 0; n < cfg->candidates; n++)
		if (st(w, n)->pending)
		{
			cmc_why(why, "node %d's value %d has no outcome", n, st(w, n)->pending);
			return false;
		}
	return true;
}

static const CmcProp props[] = {
	CMC_INVARIANT(OneValuePerSlot, one_value_per_slot),
	CMC_INVARIANT(Agreement, agreement),
	CMC_INVARIANT(OneValuePerBallot, one_value_per_ballot),
	CMC_INVARIANT(LearnedChosen, learned_chosen),
	CMC_INVARIANT(RefusedNeverChosen, refused_never_chosen),
	CMC_INVARIANT(AnsweredOnce, answered_once),
	CMC_ACTION_PROPERTY(ChosenStable, chosen_stable),
	CMC_WITNESS_PROP(ReachChosen, reach_chosen),
	CMC_WITNESS_PROP(ReachSecondLeader, reach_second_leader),
	CMC_WITNESS_PROP(ReachAmbiguous, reach_ambiguous),
	CMC_WITNESS_PROP(ReachRefused, reach_refused),
	CMC_WITNESS_PROP(ReachRecovered, reach_recovered),
	CMC_WITNESS_PROP(ReachReconfigured, reach_reconfigured),
	CMC_EVENTUALLY_PROP(EventuallyDecided, eventually_decided),
};

const CmcModel paxos_model = {
	.name = "paxos",
	.core = &core,
	.props = props,
	.nprops = sizeof(props) / sizeof(props[0]),
	.nalways = 0,
};

/*
 * libcmc's copy only (not in pg_chain): ChosenStable as an invariant over
 * a history variable (libcmc 2.4, CmcModel.history).  The ghost holds,
 * per slot, the first value ever chosen there; HistChosen requires it to
 * stay safe in every later state, not only one step after the choice,
 * which is the shortcut chosen_stable() names.
 */
static void
chosen_history(const CmcWorld *pre, CmcAction act, const CmcWorld *post, uint8_t *ghost)
{
	(void) pre;
	(void) act;
	for (int k = 0; k < cfg_of(post)->slots; k++)
	{
		int			v = chosen_now(post, k, NULL);

		if (!ghost[k] && v > 0)
			ghost[k] = (uint8_t) v;
	}
}

/* the ghost holds values, and a permutation within a class changes no
 * value (symmetry() above), so it names no node */
static void
permute_history(const void *cfg, const uint8_t *perm, uint8_t *ghost)
{
	(void) cfg;
	(void) perm;
	(void) ghost;
}

static bool
hist_chosen(const CmcWorld *w, CmcWhy *why)
{
	const uint8_t *g = cmc_history(w);

	for (int k = 0; k < cfg_of(w)->slots; k++)
		if (g[k] && !safe_value(w, k, g[k]))
		{
			cmc_why(why, "slot %d: %d was chosen in an earlier state and is no longer safe",
					k, g[k]);
			return false;
		}
	return true;
}

static const CmcProp hist_props[] = {
	CMC_INVARIANT(HistChosen, hist_chosen),
	CMC_ACTION_PROPERTY(ChosenStable, chosen_stable),
	CMC_WITNESS_PROP(ReachChosen, reach_chosen),
};

const CmcModel paxos_hist_model = {
	.name = "paxos_hist",
	.core = &core,
	.props = hist_props,
	.nprops = sizeof(hist_props) / sizeof(hist_props[0]),
	.nalways = 0,
	.history_size = PX_MAX_SLOTS,
	.history = chosen_history,
	.permute_history = permute_history,
};
