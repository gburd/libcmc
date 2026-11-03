/*
 * hc_core.c: C port of calib/hc/HummingConsensus.tla, cmc's
 * calibration core (docs/research/01 4b.1).
 *
 * Calibration exception: HC is a shared-register model with no messages,
 * so this core sets direct_actions and its action set IS the TLA+ Next
 * relation: NetChange, Propose, Adopt, DoConfirm, Deliver, ReadRepair,
 * Repair, ClientWrite, Forward, Ack.  Actor-mode cores may not do
 * this; they get actions only from their outputs (cmc.h).
 *
 * Each action is enumerated once per distinct binding of TLA+'s \E
 * (the same bindings TLC enumerates), so "states generated" can be
 * compared with TLC as well as "distinct states".  Line refs below are
 * to HummingConsensus.tla.
 *
 * Planted bugs for the calibration's own teeth (-DCMC_BUG=n,
 * hc_bug_gate.sh): 101 drops A21's "every member answered" (S1 must fail
 * for code-cp); 102 MajOK ignores the repairing list (a one-token slip:
 * exact counts must move); 103 keeps sets in insertion order (a
 * canonical-form bug: dedup splits states, so distinct counts must grow).
 */
#include "cmc.h"
#include "hc_core.h"

#include <string.h>

#ifndef CMC_BUG
#define CMC_BUG 0
#endif

#define BIT(n)	(1u << ((n) - 1))		/* node or epoch id (1-based) to bit */

static inline int
popc(unsigned x)
{
	int			k = 0;

	for (; x; x &= x - 1)
		k++;
	return k;
}

/* ---- projections ---------------------------------------------------- */

void
hc_unpack(HcProj p, HcPj *o)
{
	o->e = (int) (p & 7);
	o->a = (int) ((p >> 3) & 7);
	o->ulen = (int) ((p >> 6) & 7);
	o->rlen = (int) ((p >> 9) & 7);
	for (int i = 0; i < HC_SEQ; i++)
	{
		o->upi[i] = i < o->ulen ? (int) ((p >> (12 + 2 * i)) & 3) + 1 : 0;
		o->rep[i] = i < o->rlen ? (int) ((p >> (20 + 2 * i)) & 3) + 1 : 0;
	}
}

HcProj
hc_pack(const HcPj *p, uint8_t *bad)
{
	HcProj		v;

	if (p->e < 0 || p->e > 7 || p->a < 0 || p->a > 7 ||
		p->ulen < 0 || p->ulen > HC_SEQ || p->rlen < 0 || p->rlen > HC_SEQ)
	{
		*bad = 1;
		return 0;
	}
	v = (uint32_t) p->e | ((uint32_t) p->a << 3) | ((uint32_t) p->ulen << 6) |
		((uint32_t) p->rlen << 9);
	for (int i = 0; i < p->ulen; i++)
		v |= (uint32_t) (p->upi[i] - 1) << (12 + 2 * i);
	for (int i = 0; i < p->rlen; i++)
		v |= (uint32_t) (p->rep[i] - 1) << (20 + 2 * i);
	return v;
}

/* P1 == [e |-> 1, a |-> 1, upi |-> SetToSeq(Nodes), rep |-> <<>>] (l.54) */
HcProj
hc_p1(const HcCfg *c)
{
	HcPj		p = {1, 1, c->n, 0, {0}, {0}};
	uint8_t		bad = 0;

	for (int i = 0; i < c->n; i++)
		p.upi[i] = i + 1;
	return hc_pack(&p, &bad);
}

unsigned
hc_upi_mask(HcProj p)
{
	unsigned	m = 0;
	int			ul = (int) ((p >> 6) & 7);

	for (int i = 0; i < ul; i++)
		m |= 1u << ((p >> (12 + 2 * i)) & 3);
	return m;
}

/* Range(P.rep) */
unsigned
hc_rep_mask(HcProj p)
{
	unsigned	m = 0;
	int			rl = (int) ((p >> 9) & 7);

	for (int i = 0; i < rl; i++)
		m |= 1u << ((p >> (20 + 2 * i)) & 3);
	return m;
}

/* Members(P) == Range(P.upi) \cup Range(P.rep) (l.52) */
unsigned
hc_members(HcProj p)
{
	unsigned	m = hc_upi_mask(p);
	int			rl = (int) ((p >> 9) & 7);

	for (int i = 0; i < rl; i++)
		m |= 1u << ((p >> (20 + 2 * i)) & 3);
	return m;
}

/* the (ulen, rlen, upi, rep) bits: equal iff upi and rep are equal */
static inline uint32_t
chain_bits(HcProj p)
{
	return p >> 6;
}

/* ---- network --------------------------------------------------------- */

int
hc_pair_index(const HcCfg *c, int i, int j)
{
	int			k = 0;

	if (i > j)
	{
		int			t = i;

		i = j;
		j = t;
	}
	for (int a = 1; a <= c->n; a++)
		for (int b = a + 1; b <= c->n; b++, k++)
			if (a == i && b == j)
				return k;
	return -1;
}

int
hc_npairs(const HcCfg *c)
{
	return c->n * (c->n - 1) / 2;
}

/* Reach(a, b) == a = b \/ {a, b} \in net (l.68) */
bool
hc_reach(const HcCfg *c, const HcState *s, int a, int b)
{
	if (a == b)
		return true;
	if (a < 1 || b < 1 || a > c->n || b > c->n)
		return false;
	return (s->net >> hc_pair_index(c, a, b)) & 1;
}

static unsigned
up_mask(const HcCfg *c, const HcState *s, int n)
{
	unsigned	m = 0;

	for (int k = 1; k <= c->n; k++)
		if (hc_reach(c, s, n, k))
			m |= BIT(k);
	return m;
}

/* LatestE(n) == Max(UNION {WrittenAt(m) : m \in Up(n)}) (l.71-72) */
static int
latest_e(const HcCfg *c, const HcState *s, unsigned up)
{
	int			best = 0;

	for (int m = 1; m <= c->n; m++)
		if (up & BIT(m))
			for (int e = c->maxe; e > best; e--)
				if (s->pub[m - 1][e - 1])
				{
					best = e;
					break;
				}
	return best;
}

/* Wedged(n) (l.76-77) */
bool
hc_wedged(const HcCfg *c, const HcState *s, int n)
{
	int			ce = hc_pe(s->cur[n - 1]);

	for (int e = ce + 1; e <= c->maxe; e++)
		if (s->pub[n - 1][e - 1])
			return true;
	return c->confirm && ce >= 1 && !(s->conf[n - 1] & BIT(ce));
}

bool
hc_is_head(HcProj cur, int n)
{
	HcPj		p;

	hc_unpack(cur, &p);
	return p.ulen > 0 && p.upi[0] == n;
}

bool
hc_is_tail(HcProj cur, int n)
{
	HcPj		p;

	hc_unpack(cur, &p);
	return p.ulen > 0 && p.upi[p.ulen - 1] == n;
}

bool
hc_set_has(const uint32_t *a, int n, uint32_t v)
{
	int			lo = 0,
				hi = n - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (a[mid] == v)
			return true;
		if (a[mid] < v)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/* sorted unique insert; overflow sets *bad (TLC has no capacity) */
static void
set_insert(uint32_t *a, uint8_t *n, int cap, uint32_t v, uint8_t *bad)
{
	int			i = 0;

#if CMC_BUG == 103
	/* planted: sets kept in insertion order, not sorted (non-canonical) */
	for (i = 0; i < *n; i++)
		if (a[i] == v)
			return;
	if (*n >= cap)
	{
		*bad = 1;
		return;
	}
	a[(*n)++] = v;
	return;
#endif
	while (i < *n && a[i] < v)
		i++;
	if (i < *n && a[i] == v)
		return;
	if (*n >= cap)
	{
		*bad = 1;
		return;
	}
	memmove(a + i + 1, a + i, (size_t) (*n - i) * sizeof(uint32_t));
	a[i] = v;
	(*n)++;
}

/* ---- Unanimous, Base, Calc, Sane, Relevant -------------------------- */

/* Unanimous(n, P) (l.79-85) */
static bool
unanimous(const HcCfg *c, const HcState *s, unsigned up, int E, HcProj P)
{
	unsigned	eq = 0;

	if (hc_pe(P) != E)
		return false;
	for (int m = 1; m <= c->n; m++)
	{
		HcProj		v;

		if (!(up & BIT(m)))
			continue;
		v = s->pub[m - 1][E - 1];
		if (c->code)
		{
			if (v != 0 && v != P)
				return false;
			if (v == P)
				eq |= BIT(m);
		}
		else if (v != P)
			return false;
	}
#if CMC_BUG == 101
	/* planted: A21 without "every member answered" (calibration teeth) */
	(void) eq;
	return true;
#else
	return c->code ? (hc_members(P) & ~eq) == 0 : true;
#endif
}

/* NoneP(n) == [e |-> cur[n].e, a |-> n, upi |-> <<>>, rep |-> <<>>] (l.92) */
static HcProj
none_p(const HcState *s, int n)
{
	return (HcProj) hc_pe(s->cur[n - 1]) | ((HcProj) n << 3);
}

/*
 * Base(n) (l.96-102).  CHOOSE P \in C : \A Q \in C : Q.e <= P.e is unique
 * unless two distinct confirmed projections share the maximal epoch; TLC
 * would then pick by its internal value order, which this port does not
 * reproduce, so *tie is reported (hc_base_tie, HC_Wellformed).
 */
static HcProj
base_p(const HcCfg *c, const HcState *s, int n, unsigned up, bool *tie)
{
	int			maj = c->n / 2 + 1,
				best_e = -1;
	HcProj		best = 0;

	if (tie)
		*tie = false;
	if (!(c->cp && c->zerf))
		return s->cur[n - 1];
	if (popc(up) < maj)
		return none_p(s, n);
	for (int m = 1; m <= c->n; m++)
	{
		if (!(up & BIT(m)))
			continue;
		for (int e = 1; e <= c->maxe; e++)
		{
			HcProj		P;

			if (!(s->conf[m - 1] & BIT(e)))
				continue;
			P = s->priv[m - 1][e - 1];
			if (popc(hc_members(P)) < maj)	/* ZerfOK (l.94) */
				continue;
			if (hc_pe(P) > best_e)
			{
				best_e = hc_pe(P);
				best = P;
				if (tie)
					*tie = false;
			}
			else if (hc_pe(P) == best_e && P != best)
			{
				if (tie)
					*tie = true;
				if (P < best)
					best = P;
			}
		}
	}
	return best_e < 0 ? none_p(s, n) : best;
}

bool
hc_base_tie(const HcCfg *c, const HcState *s, int n)
{
	bool		tie;

	base_p(c, s, n, up_mask(c, s, n), &tie);
	return tie;
}

/* Calc(n, E) (l.104-129) */
static HcProj
calc(const HcCfg *c, const HcState *s, int n, int E, unsigned up, HcProj basep,
	 uint8_t *bad)
{
	HcPj		old,
				r;
	int			upi1[HC_SEQ],
				rep1[HC_SEQ],
				u1 = 0,
				r1 = 0,
				maj = c->n / 2 + 1;
	int			upi2[2 * HC_SEQ],
				rep2[2 * HC_SEQ],
				u2 = 0,
				r2 = 0;
	unsigned	newup;
	bool		promote,
				apfix,
				ok;

	hc_unpack(basep, &old);
	for (int i = 0; i < old.ulen; i++)
		if (up & BIT(old.upi[i]))
			upi1[u1++] = old.upi[i];
	for (int i = 0; i < old.rlen; i++)
		if (up & BIT(old.rep[i]))
			rep1[r1++] = old.rep[i];
	newup = up & ~hc_members(basep);
	promote = newup == 0 && r1 > 0 && old.ulen > 0 && n == old.upi[old.ulen - 1];
	for (int i = 0; promote && i < r1; i++)
		if (s->hist[n - 1] & ~s->hist[rep1[i] - 1])
			promote = false;
	for (int i = 0; i < u1; i++)
		upi2[u2++] = upi1[i];
	if (promote)
		for (int i = 0; i < r1; i++)
			upi2[u2++] = rep1[i];
	else
		for (int i = 0; i < r1; i++)
			rep2[r2++] = rep1[i];
	for (int k = 1; k <= c->n; k++)	/* SetToSeq(newUp): ascending */
		if (newup & BIT(k))
			rep2[r2++] = k;
	apfix = !c->cp && u2 == 0 && r2 > 0;
	memset(&r, 0, sizeof(r));
	r.e = E;
	r.a = n;
	if (apfix)
	{
		r.ulen = 1;
		r.upi[0] = rep2[0];
		r.rlen = r2 - 1;
		for (int i = 1; i < r2; i++)
			r.rep[i - 1] = rep2[i];
	}
	else
	{
		if (u2 > HC_SEQ || r2 > HC_SEQ)
		{
			*bad = 1;
			return 0;
		}
		r.ulen = u2;
		r.rlen = r2;
		memcpy(r.upi, upi2, sizeof(int) * u2);
		memcpy(r.rep, rep2, sizeof(int) * r2);
	}
	ok = !c->cp || r.ulen + r.rlen >= maj;
	if (!ok)
	{
		r.ulen = r.rlen = 0;
		memset(r.upi, 0, sizeof(r.upi));
		memset(r.rep, 0, sizeof(r.rep));
	}
	return hc_pack(&r, bad);
}

/* SimpleSane(old, new) (l.135-142) */
static bool
simple_sane(const HcPj *o, const HcPj *nw)
{
	unsigned	nm = 0,
				am = 0;
	int			kept[HC_SEQ],
				k = 0,
				f[HC_SEQ],
				fl = 0,
				alen;

	for (int i = 0; i < nw->ulen; i++)
		nm |= BIT(nw->upi[i]);
	for (int i = 0; i < o->ulen; i++)
		if (nm & BIT(o->upi[i]))
			kept[k++] = o->upi[i];
	if (k > nw->ulen)
		return false;
	for (int i = 0; i < k; i++)
		if (nw->upi[i] != kept[i])
			return false;
	alen = nw->ulen - k;
	for (int i = k; i < nw->ulen; i++)
		am |= BIT(nw->upi[i]);
	/* IsSubseqOf(added, old.rep) == added = SelectSeq(old.rep, \in Range(added)) */
	for (int i = 0; i < o->rlen; i++)
		if (am & BIT(o->rep[i]))
			f[fl++] = o->rep[i];
	if (fl != alen)
		return false;
	for (int i = 0; i < alen; i++)
		if (f[i] != nw->upi[k + i])
			return false;
	if (alen > 0 && !(o->ulen > 0 && nw->a == o->upi[o->ulen - 1]))
		return false;
	return true;
}

/* Sane(old, new, n) (l.144-163) */
static bool
sane(const HcCfg *c, HcProj oldp, HcProj newp, int n)
{
	HcPj		o,
				nw;
	unsigned	om = hc_upi_mask(oldp),
				nm = hc_upi_mask(newp);
	int			maj = c->n / 2 + 1;
	bool		chain;

	hc_unpack(oldp, &o);
	hc_unpack(newp, &nw);
	if (!(nw.e > o.e))
		return false;
	/* MajOK */
#if CMC_BUG == 102
	/* planted: MajOK counts only the UPI (a one-token slip) */
	if (!(!c->cp || nw.ulen == 0 || nw.ulen >= maj))
		return false;
#else
	if (!(!c->cp || nw.ulen == 0 || nw.ulen + nw.rlen >= maj))
		return false;
#endif
	/* ChainSane */
	if (nw.ulen == 1 && nw.upi[0] == nw.a)
		chain = true;
	else if ((om & nm) == 0)
		chain = !(nm & BIT(n));
	else
		chain = simple_sane(&o, &nw);
	if (!chain)
		return false;
	/* FinalReview */
	return !c->cp || nw.ulen == 0 || (c->zerf && o.ulen == 0) || (om & nm) != 0;
}

/*
 * TLC evaluates \/ inside an action as a branch: Adopt yields its successor
 * once per true leaf of MajOK (3 leaves) times once per true leaf of
 * FinalReview (4 leaves).  Used only when tlc_mult is set.
 */
static int
tlc_adopt_mult(const HcCfg *c, HcProj oldp, HcProj newp)
{
	unsigned	om = hc_upi_mask(oldp),
				nm = hc_upi_mask(newp);
	int			nu = hc_pulen(newp),
				nr = (int) ((newp >> 9) & 7),
				maj = c->n / 2 + 1;
	int			m1 = !c->cp + (nu == 0) + (nu + nr >= maj);
	int			m2 = !c->cp + (nu == 0) + (c->zerf && hc_pulen(oldp) == 0) + ((om & nm) != 0);

	return m1 * m2;
}

/* Relevant(n, P) (l.166-172) */
static bool
relevant(const HcState *s, int n, HcProj P)
{
	HcPj		p,
				cu;

	if (hc_pa(P) == n)
		return true;
	hc_unpack(P, &p);
	if (p.ulen == 0)
		return false;
	if (!(hc_members(P) & BIT(n)))
		return false;
	for (int i = 0; i < p.rlen; i++)
		if (p.rep[i] == n)
		{
			hc_unpack(s->cur[n - 1], &cu);
			return !((hc_upi_mask(s->cur[n - 1]) & BIT(n)) && cu.ulen > p.ulen);
		}
	return true;
}

/* Rank(P) (l.87) */
static int
rank(const HcCfg *c, HcProj P)
{
	HcPj		p;

	hc_unpack(P, &p);
	return p.ulen == 0 ? 0 : 1 + c->n * c->n * c->n * p.ulen + c->n * p.rlen;
}

static bool
stamp_has(const HcState *s, int n, HcWP x)
{
	return hc_set_has(s->stamp[n - 1], s->nstamp[n - 1], x);
}

/* Init (l.175-184) */
void
hc_init(const void *cfgv, int node, void *sv)
{
	const HcCfg *c = cfgv;
	HcState    *s = sv;
	HcProj		p1 = hc_p1(c);

	(void) node;
	memset(s, 0, sizeof(*s));
	s->net = (uint8_t) ((1u << hc_npairs(c)) - 1);
	for (int n = 0; n < c->n; n++)
	{
		s->pub[n][0] = p1;
		s->priv[n][0] = p1;
		s->cur[n] = p1;
		s->conf[n] = 1;
	}
}

#define PUSH(x) do { if (k >= max) return -1; acts[k++] = (x); } while (0)

/* Next (l.284-292): every enabled TLA+ action instance. */
int
hc_actions(const void *cfgv, const void *sv, CmcAction *acts, int max)
{
	const HcCfg *c = cfgv;
	const HcState *s = sv;
	unsigned	up[HC_MAXN + 1];
	int			late[HC_MAXN + 1];
	bool		wed[HC_MAXN + 1];
	int			k = 0,
				np = hc_npairs(c);

	if (s->bad)
		return 0;
	for (int n = 1; n <= c->n; n++)
	{
		up[n] = up_mask(c, s, n);
		late[n] = latest_e(c, s, up[n]);
		wed[n] = hc_wedged(c, s, n);
	}

	/* NetChange (l.187-189) */
	for (unsigned m = 0; m < (1u << np); m++)
		if (m != s->net)
			PUSH(CMC_ACT(HC_NETCHANGE, 0, 0, 0, m));

	for (int n = 1; n <= c->n; n++)
	{
		HcProj		cur = s->cur[n - 1];
		int			ce = hc_pe(cur);
		int			E;

		/* Propose(n) (l.192-200) */
		E = (ce > late[n] ? ce : late[n]) + 1;
		if (E <= c->maxe)
		{
			uint8_t		bad = 0;
			HcProj		P = calc(c, s, n, E, up[n], base_p(c, s, n, up[n], NULL), &bad);

			if (bad)
				PUSH(CMC_ACT(HC_PROPOSE, n, 0, 0, 0));	/* step flags it */
			else if (!(chain_bits(P) == chain_bits(cur) &&
					   unanimous(c, s, up[n], late[n], cur)))
				PUSH(CMC_ACT(HC_PROPOSE, n, 0, 0, P));
		}

		/* Adopt(n) (l.221-233): \E P \in {pub[m][LatestE(n)] : m \in Up(n)} */
		{
			HcProj		seen[HC_MAXN];
			int			ns = 0;
			int			L = late[n];

			for (int m = 1; m <= c->n; m++)
			{
				HcProj		P,
							B,
							old;
				bool		dup = false;

				if (!(up[n] & BIT(m)))
					continue;
				P = s->pub[m - 1][L - 1];
				for (int i = 0; i < ns; i++)
					if (seen[i] == P)
						dup = true;
				if (dup)
					continue;
				seen[ns++] = P;
				if (!unanimous(c, s, up[n], L, P))
					continue;
				if (hc_pa(P) < 1 || !(up[n] & BIT(hc_pa(P))))
					continue;
				if (!relevant(s, n, P))
					continue;
				/* Sane([B EXCEPT !.e = IF B.e < cur[n].e THEN B.e ELSE cur[n].e], P, n) */
				B = base_p(c, s, n, up[n], NULL);
				old = (B & ~(HcProj) 7) | (HcProj) (hc_pe(B) < ce ? hc_pe(B) : ce);
				if (!sane(c, old, P, n))
					continue;
				if (!(hc_pe(P) > ce))
					continue;
				if (s->priv[n - 1][hc_pe(P) - 1] != 0)
					continue;
				for (int r = c->tlc_mult ? tlc_adopt_mult(c, old, P) : 1; r > 0; r--)
					PUSH(CMC_ACT(HC_ADOPT, n, 0, 0, P));
			}
		}

		/* DoConfirm(n) (l.275-281) */
		if (c->confirm && hc_pulen(cur) > 0 && !(s->conf[n - 1] & BIT(ce)))
		{
			bool		ok = true;
			unsigned	um = hc_upi_mask(cur);

			for (int m = 1; m <= c->n; m++)
				if (um & (1u << (m - 1)))
					if (!hc_reach(c, s, n, m) || s->cur[m - 1] != cur)
						ok = false;
			if (ok)
				PUSH(CMC_ACT(HC_DOCONFIRM, n, 0, 0, 0));
		}
	}

	/* Deliver(P, m) (l.203-207) */
	for (int i = 0; i < s->nprops; i++)
	{
		HcProj		P = s->props[i];

		for (int m = 1; m <= c->n; m++)
			if (hc_reach(c, s, hc_pa(P), m) && s->pub[m - 1][hc_pe(P) - 1] == 0)
				PUSH(CMC_ACT(HC_DELIVER, m, 0, 0, P));
	}

	/* ReadRepair(n, m) and Repair(n, m) (l.211-218, l.266-272) */
	for (int n = 1; n <= c->n; n++)
	{
		int			L = late[n];
		HcProj		vals[HC_MAXN];
		int			nv = 0,
					best = -1;

		for (int q = 1; q <= c->n; q++)
		{
			HcProj		v;
			bool		dup = false;

			if (!(up[n] & BIT(q)))
				continue;
			v = s->pub[q - 1][L - 1];
			if (!v)
				continue;
			for (int i = 0; i < nv; i++)
				if (vals[i] == v)
					dup = true;
			if (!dup)
				vals[nv++] = v;
		}
		for (int i = 0; i < nv; i++)
			if (rank(c, vals[i]) > best)
				best = rank(c, vals[i]);
		for (int m = 1; m <= c->n; m++)
		{
			HcProj		curn = s->cur[n - 1];

			if ((up[n] & BIT(m)) && s->pub[m - 1][L - 1] == 0)
				for (int i = 0; i < nv; i++)
					if (rank(c, vals[i]) == best)
						PUSH(CMC_ACT(HC_READREPAIR, n, m, 0, vals[i]));

			/* Repair(t = n, r = m) */
			if (hc_is_tail(curn, n) && (hc_rep_mask(curn) & BIT(m)) &&
				s->cur[m - 1] == curn && hc_reach(c, s, n, m) &&
				(s->hist[n - 1] & ~s->hist[m - 1]))
				PUSH(CMC_ACT(HC_REPAIR, n, m, 0, 0));
		}
	}

	/* ClientWrite(h, w) (l.241-246) */
	for (int h = 1; h <= c->n; h++)
		for (int w = 1; w <= c->writes; w++)
			if (hc_is_head(s->cur[h - 1], h) && !wed[h] &&
				!stamp_has(s, h, HC_WP(w, s->cur[h - 1])))
				PUSH(CMC_ACT(HC_CLIENTWRITE, h, 0, w, 0));

	/* Forward(n, m, w) (l.250-258) */
	for (int n = 1; n <= c->n; n++)
		for (int i = 0; i < s->nstamp[n - 1]; i++)
		{
			HcWP		x = s->stamp[n - 1][i];
			HcProj		P = HC_WP_P(x);
			HcPj		p;
			int			ch[2 * HC_SEQ],
						cl = 0;

			hc_unpack(P, &p);
			for (int j = 0; j < p.ulen; j++)
				ch[cl++] = p.upi[j];
			for (int j = 0; j < p.rlen; j++)
				ch[cl++] = p.rep[j];
			for (int j = 0; j + 1 < cl; j++)
			{
				int			m = ch[j + 1];

				if (ch[j] != n)
					continue;
				if (s->cur[m - 1] == P && !wed[m] && !stamp_has(s, m, x))
					PUSH(CMC_ACT(HC_FORWARD, n, m, HC_WP_W(x), P));
			}
		}

	/* \E x \in AllStamped : Ack(x[1], x[2]) (l.260-264, 292) */
	{
		HcWP		all[HC_MAXN * HC_STAMP_CAP];
		int			na = 0;

		for (int n = 1; n <= c->n; n++)
			for (int i = 0; i < s->nstamp[n - 1]; i++)
			{
				HcWP		x = s->stamp[n - 1][i];
				bool		dup = false;

				for (int j = 0; j < na; j++)
					if (all[j] == x)
						dup = true;
				if (!dup)
					all[na++] = x;
			}
		for (int i = 0; i < na; i++)
		{
			HcWP		x = all[i];
			HcProj		P = HC_WP_P(x);
			unsigned	mem = hc_members(P);
			bool		ok = hc_pulen(P) > 0;

			for (int m = 1; ok && m <= c->n; m++)
				if ((mem & (1u << (m - 1))) && !stamp_has(s, m, x))
					ok = false;
			if (ok && !hc_set_has(s->acked, s->nacked, x))
				PUSH(CMC_ACT(HC_ACK, 0, 0, HC_WP_W(x), P));
		}
	}
	return k;
}

#undef PUSH

/* Applies one TLA+ action.  The engine only passes actions hc_actions
 * listed for this state. */
void
hc_step(const void *cfgv, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	const HcCfg *c = cfgv;
	HcState    *s = sv;
	CmcAction	a = ev->action;
	int			A = (int) CMC_ACT_A(a),
				B = (int) CMC_ACT_B(a),
				W = (int) CMC_ACT_C(a);
	uint32_t	D = CMC_ACT_D(a);

	(void) node;
	out->n = 0;
	if (ev->kind != CMC_EV_ACTION)
	{
		s->bad = 1;
		return;
	}
	switch (CMC_ACT_KIND(a))
	{
		case HC_NETCHANGE:
			s->net = (uint8_t) D;
			break;
		case HC_PROPOSE:
			{
				HcProj		P = D;

				if (!P)
				{
					s->bad = 1;	/* Calc overflowed the packed form */
					break;
				}
				set_insert(s->props, &s->nprops, HC_PROPS_CAP, P, &s->bad);
				s->pub[A - 1][hc_pe(P) - 1] = P;
				break;
			}
		case HC_ADOPT:
			s->priv[A - 1][hc_pe(D) - 1] = D;
			s->cur[A - 1] = D;
			break;
		case HC_DOCONFIRM:
			s->conf[A - 1] |= (uint8_t) BIT(hc_pe(s->cur[A - 1]));
			break;
		case HC_DELIVER:
			s->pub[A - 1][hc_pe(D) - 1] = D;
			break;
		case HC_READREPAIR:
			{
				int			L = latest_e(c, s, up_mask(c, s, A));

				s->pub[B - 1][L - 1] = D;
				break;
			}
		case HC_REPAIR:
			s->hist[B - 1] |= s->hist[A - 1];
			break;
		case HC_CLIENTWRITE:
			s->hist[A - 1] |= (uint8_t) BIT(W);
			set_insert(s->stamp[A - 1], &s->nstamp[A - 1], HC_STAMP_CAP,
					   HC_WP(W, s->cur[A - 1]), &s->bad);
			break;
		case HC_FORWARD:
			s->hist[B - 1] |= (uint8_t) BIT(W);
			set_insert(s->stamp[B - 1], &s->nstamp[B - 1], HC_STAMP_CAP,
					   HC_WP(W, D), &s->bad);
			break;
		case HC_ACK:
			set_insert(s->acked, &s->nacked, HC_ACKED_CAP, HC_WP(W, D), &s->bad);
			break;
		default:
			s->bad = 1;
	}
}

