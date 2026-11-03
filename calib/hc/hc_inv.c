/*
 * hc_inv.c: the HummingConsensus properties (calib/hc/HummingConsensus.tla
 * l.297-357), written against the private state.  They are evaluated on
 * states and transitions only, never to compute enabled actions.
 *
 * TLA+ "vacuity witnesses" W_* are invariants that must be VIOLATED.  They
 * are registered twice: as CMC_INVARIANT under the TLA+ name (so a run
 * reports "violated at depth d" exactly as TLC does) and as CMC_WITNESS
 * with the predicate negated, under the name Reach_<W>.
 */
#include "cmc.h"
#include "hc_core.h"

#define BIT(n)	(1u << ((n) - 1))

extern const CmcCore hc_core;

static const HcCfg *
C(const CmcWorld *w)
{
	return cmc_cfg(w);
}

static const HcState *
S(const CmcWorld *w)
{
	return cmc_node(w, 0);
}

static void
put(CmcWhy *why, const char *what, int a, int b, int e)
{
	cmc_why(why, "%s (a=%d b=%d e=%d)", what, a, b, e);
}

/* HC_Wellformed: no capacity overflowed (where TLC has none) and Base's
 * CHOOSE had a unique answer in every state the run reached. */
static bool
hc_wellformed(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	if (s->bad)
	{
		cmc_why(why, "capacity exceeded in the C encoding (raise HC_*_CAP)");
		return false;
	}
	if (c->cp && c->zerf)
		for (int n = 1; n <= c->n; n++)
			if (hc_base_tie(c, s, n))
			{
				cmc_why(why, "Base(%d): CHOOSE over two confirmed projections at one epoch", n);
				return false;
			}
	return true;
}

/* TypeOK (l.352-356) */
static bool
type_ok(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);
	HcProj		p1 = hc_p1(c);

	if (s->net >> hc_npairs(c))
	{
		cmc_why(why, "net not a subset of Pairs");
		return false;
	}
	for (int n = 1; n <= c->n; n++)
	{
		int			mx = 0;

		for (int e = 1; e <= c->maxe; e++)
			if (s->priv[n - 1][e - 1])
				mx = e;
		if (!mx || s->cur[n - 1] != s->priv[n - 1][mx - 1])
		{
			cmc_why(why, "cur[%d] is not its latest private projection", n);
			return false;
		}
	}
	for (int i = 0; i < s->nacked; i++)
	{
		HcWP		x = s->acked[i];
		int			wid = HC_WP_W(x);
		HcProj		P = HC_WP_P(x);

		if (wid < 1 || wid > c->writes ||
			!(P == p1 || hc_set_has(s->props, s->nprops, P)))
		{
			cmc_why(why, "acked entry %d is not <<Writes, props \\cup {P1}>>", i);
			return false;
		}
	}
	return true;
}

/* S1_Agreement (l.300-302) */
static bool
s1_agreement(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	for (int e = 1; e <= c->maxe; e++)
		for (int a = 1; a <= c->n; a++)
			for (int b = a + 1; b <= c->n; b++)
			{
				HcProj		x = s->priv[a - 1][e - 1],
							y = s->priv[b - 1][e - 1];

				if (x && y && x != y)
				{
					put(why, "priv[a][e] /= priv[b][e]", a, b, e);
					return false;
				}
			}
	return true;
}

/* S1_AgreementNonNone (l.306-308) */
static bool
s1_nonnone(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	for (int e = 1; e <= c->maxe; e++)
		for (int a = 1; a <= c->n; a++)
			for (int b = a + 1; b <= c->n; b++)
			{
				HcProj		x = s->priv[a - 1][e - 1],
							y = s->priv[b - 1][e - 1];

				if (hc_pulen(x) && hc_pulen(y) && x != y)
				{
					put(why, "non-none priv[a][e] /= priv[b][e]", a, b, e);
					return false;
				}
			}
	return true;
}

static bool
in_upi(const HcState *s, int n)
{
	return (hc_upi_mask(s->cur[n - 1]) & BIT(n)) != 0;
}

/* S2_NoSplitBrainServing (l.312-314) */
static bool
s2(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);
	bool		ht[HC_MAXN + 1];

	for (int n = 1; n <= c->n; n++)
		ht[n] = in_upi(s, n) && !hc_wedged(c, s, n) &&
			(hc_is_head(s->cur[n - 1], n) || hc_is_tail(s->cur[n - 1], n));
	for (int a = 1; a <= c->n; a++)
		for (int b = a + 1; b <= c->n; b++)
			if (ht[a] && ht[b] && hc_pe(s->cur[a - 1]) != hc_pe(s->cur[b - 1]))
			{
				put(why, "two serving heads/tails in different epochs", a, b, 0);
				return false;
			}
	return true;
}

static bool
ack_capable(const HcCfg *c, const HcState *s, int n)
{
	HcProj		cur = s->cur[n - 1];
	unsigned	mem = hc_members(cur);

	if (!hc_pulen(cur))
		return false;
	for (int m = 1; m <= c->n; m++)
		if ((mem & BIT(m)) && (s->cur[m - 1] != cur || hc_wedged(c, s, m)))
			return false;
	return true;
}

/* S2w_NoConcurrentWriteChains (l.317-319) */
static bool
s2w(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	for (int a = 1; a <= c->n; a++)
		for (int b = a + 1; b <= c->n; b++)
			if (ack_capable(c, s, a) && ack_capable(c, s, b) &&
				s->cur[a - 1] != s->cur[b - 1])
			{
				put(why, "two different ack-capable projections", a, b, 0);
				return false;
			}
	return true;
}

static bool
holds(const HcState *s, int n, HcWP x)
{
	return (s->hist[n - 1] & BIT(HC_WP_W(x))) != 0;
}

/* S2r_NoStaleRead (l.322-323) */
static bool
s2r(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	for (int t = 1; t <= c->n; t++)
		if (hc_is_tail(s->cur[t - 1], t) && !hc_wedged(c, s, t))
			for (int i = 0; i < s->nacked; i++)
				if (!holds(s, t, s->acked[i]))
				{
					put(why, "unwedged tail misses an acked write", t, 0,
						hc_pe(s->cur[t - 1]));
					return false;
				}
	return true;
}

/* S3_NoLostAck (l.328-330) */
static bool
s3(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	for (int i = 0; i < s->nacked; i++)
	{
		HcWP		x = s->acked[i];
		HcProj		P = HC_WP_P(x);

		for (int n = 1; n <= c->n; n++)
			if (in_upi(s, n) &&
				(hc_pe(s->cur[n - 1]) > hc_pe(P) || s->cur[n - 1] == P) &&
				!holds(s, n, x))
			{
				put(why, "UPI member of a later/same projection misses an acked write",
					n, 0, hc_pe(s->cur[n - 1]));
				return false;
			}
	}
	return true;
}

static bool
active(const HcCfg *c, const HcState *s, HcProj P)
{
	unsigned	mem = hc_members(P);

	if (!hc_pulen(P))
		return false;
	for (int m = 1; m <= c->n; m++)
		if ((mem & BIT(m)) && s->cur[m - 1] != P)
			return false;
	return true;
}

/* S3a_NoLostAckActive (l.335-337) */
static bool
s3a(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	for (int i = 0; i < s->nacked; i++)
	{
		HcWP		x = s->acked[i];
		HcProj		P = HC_WP_P(x);

		for (int n = 1; n <= c->n; n++)
			if (in_upi(s, n) && active(c, s, s->cur[n - 1]) &&
				hc_pe(s->cur[n - 1]) > hc_pe(P) && !holds(s, n, x))
			{
				put(why, "UPI member of a later active projection misses an acked write",
					n, 0, hc_pe(s->cur[n - 1]));
				return false;
			}
	}
	return true;
}

/* S3_NoLostAckSameEpoch (l.340-341) */
static bool
s3same(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	for (int i = 0; i < s->nacked; i++)
	{
		HcWP		x = s->acked[i];

		for (int n = 1; n <= c->n; n++)
			if (in_upi(s, n) && hc_pe(s->cur[n - 1]) >= hc_pe(HC_WP_P(x)) &&
				!holds(s, n, x))
			{
				put(why, "UPI member at >= the ack's epoch misses it", n, 0, 0);
				return false;
			}
	}
	return true;
}

/* S4_EpochMonotone (l.344) */
static bool
s4_mono(const CmcWorld *pre, CmcAction a, const CmcWorld *post, CmcWhy *why)
{
	const HcCfg *c = C(pre);
	const HcState *p = S(pre),
			   *q = S(post);

	(void) a;
	for (int n = 1; n <= c->n; n++)
		if (hc_pe(q->cur[n - 1]) < hc_pe(p->cur[n - 1]))
		{
			put(why, "cur[n].e decreased", n, 0, hc_pe(q->cur[n - 1]));
			return false;
		}
	return true;
}

/* S4_PrivWriteOnce (l.345-346) */
static bool
s4_once(const CmcWorld *pre, CmcAction a, const CmcWorld *post, CmcWhy *why)
{
	const HcCfg *c = C(pre);
	const HcState *p = S(pre),
			   *q = S(post);

	(void) a;
	for (int n = 1; n <= c->n; n++)
		for (int e = 1; e <= c->maxe; e++)
			if (p->priv[n - 1][e - 1] && q->priv[n - 1][e - 1] != p->priv[n - 1][e - 1])
			{
				put(why, "priv[n][e] rewritten", n, 0, e);
				return false;
			}
	return true;
}

/* W_AckAfterReconfig == ~(\E x \in acked : x[2].e >= 2) (l.349) */
static bool
reach_ack_after_reconfig(const CmcWorld *w, CmcWhy *why)
{
	const HcState *s = S(w);

	(void) why;
	for (int i = 0; i < s->nacked; i++)
		if (hc_pe(HC_WP_P(s->acked[i])) >= 2)
			return true;
	return false;
}

/* W_RepairJoinsUPI == ~(\E n : cur[n].e >= 3 /\ Len(cur[n].upi) = N /\ acked /= {}) (l.350) */
static bool
reach_repair_joins_upi(const CmcWorld *w, CmcWhy *why)
{
	const HcCfg *c = C(w);
	const HcState *s = S(w);

	(void) why;
	if (!s->nacked)
		return false;
	for (int n = 1; n <= c->n; n++)
		if (hc_pe(s->cur[n - 1]) >= 3 && hc_pulen(s->cur[n - 1]) == c->n)
			return true;
	return false;
}

static bool
w_ack_after_reconfig(const CmcWorld *w, CmcWhy *why)
{
	if (reach_ack_after_reconfig(w, NULL))
	{
		cmc_why(why, "a write was acked under an epoch >= 2");
		return false;
	}
	return true;
}

static bool
w_repair_joins_upi(const CmcWorld *w, CmcWhy *why)
{
	if (reach_repair_joins_upi(w, NULL))
	{
		cmc_why(why, "a full-length UPI at epoch >= 3 after an ack");
		return false;
	}
	return true;
}

static const CmcProp hc_props[] = {
	CMC_INVARIANT(HC_Wellformed, hc_wellformed),
	CMC_INVARIANT(TypeOK, type_ok),
	CMC_INVARIANT(S1_Agreement, s1_agreement),
	CMC_INVARIANT(S1_AgreementNonNone, s1_nonnone),
	CMC_INVARIANT(S2_NoSplitBrainServing, s2),
	CMC_INVARIANT(S2w_NoConcurrentWriteChains, s2w),
	CMC_INVARIANT(S2r_NoStaleRead, s2r),
	CMC_INVARIANT(S3_NoLostAck, s3),
	CMC_INVARIANT(S3a_NoLostAckActive, s3a),
	CMC_INVARIANT(S3_NoLostAckSameEpoch, s3same),
	CMC_ACTION_PROPERTY(S4_EpochMonotone, s4_mono),
	CMC_ACTION_PROPERTY(S4_PrivWriteOnce, s4_once),
	CMC_INVARIANT(W_AckAfterReconfig, w_ack_after_reconfig),
	CMC_INVARIANT(W_RepairJoinsUPI, w_repair_joins_upi),
	CMC_WITNESS_PROP(Reach_AckAfterReconfig, reach_ack_after_reconfig),
	CMC_WITNESS_PROP(Reach_RepairJoinsUPI, reach_repair_joins_upi),
};

const CmcModel hc_model = {
	.name = "hc",
	.core = &hc_core,
	.props = hc_props,
	.nprops = sizeof(hc_props) / sizeof(hc_props[0]),
	.nalways = 1,				/* HC_Wellformed */
};
