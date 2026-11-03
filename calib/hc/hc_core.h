/*
 * hc_core.h: private state of the HummingConsensus calibration core.
 * Included by hc_core.c, hc_inv.c and hc_describe.c only, never by the
 * engine (cmc.c), which sees the state as opaque bytes.
 *
 * Source: calib/hc/HummingConsensus.tla (357 lines).  The struct is a
 * bijection with the TLA+ variables <<net, pub, priv, cur, props, hist,
 * stamp, acked, conf>>: sets are sorted arrays with zeroed tails, records
 * are packed integers with zeroed unused fields, so two states have the
 * same bytes iff they are the same TLA+ state.
 */
#ifndef CMC_HC_CORE_H
#define CMC_HC_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define HC_MAXN		4
#define HC_MAXE		4			/* MaxEpoch bound; the packed form allows 7 */
#define HC_MAXW		4
#define HC_SEQ		4			/* max sequence length (= HC_MAXN) */
#define HC_PROPS_CAP 12		/* >= N * (MaxEpoch - 1): pub is write-once */
#define HC_STAMP_CAP 8		/* overflow sets bad (HC_Wellformed) */
#define HC_ACKED_CAP 8

/*
 * A projection [e, a, upi, rep] packed into 28 bits; NoProj = 0.
 *   bits 0-2 e, 3-5 a, 6-8 Len(upi), 9-11 Len(rep),
 *   12-19 upi[i]-1 at 12+2i, 20-27 rep[i]-1 at 20+2i; unused bits zero.
 */
typedef uint32_t HcProj;

/* <<w, P>> packed: w (1..HC_MAXW) in bits 28-31, P below. */
typedef uint32_t HcWP;

#define HC_WP(w, p)		(((uint32_t)(w) << 28) | (p))
#define HC_WP_W(x)		((int)((x) >> 28))
#define HC_WP_P(x)		((x) & 0x0fffffffu)

typedef struct HcCfg
{
	int			n;				/* N */
	int			maxe;			/* MaxEpoch */
	int			writes;			/* |Writes|; ids 1..writes */
	bool		cp;				/* CPMode */
	bool		code;			/* UnanimityRule = "code" (else "paper") */
	bool		confirm;		/* Confirm */
	bool		zerf;			/* Zerf */
	/* not a TLA+ constant: list Adopt once per true leaf of the disjunctions
	 * in MajOK and FinalReview, as TLC's action evaluator branches on them,
	 * so "states generated" matches TLC too.  Distinct states are unaffected. */
	bool		tlc_mult;
} HcCfg;

typedef struct HcState
{
	HcProj		pub[HC_MAXN][HC_MAXE];	/* [n-1][e-1] */
	HcProj		priv[HC_MAXN][HC_MAXE];
	HcProj		cur[HC_MAXN];
	HcProj		props[HC_PROPS_CAP];	/* sorted, unique, zero tail */
	HcWP		stamp[HC_MAXN][HC_STAMP_CAP];	/* sorted, unique, zero tail */
	HcWP		acked[HC_ACKED_CAP];	/* sorted, unique, zero tail */
	uint8_t		nprops;
	uint8_t		nacked;
	uint8_t		nstamp[HC_MAXN];
	uint8_t		hist[HC_MAXN];	/* bit w-1 */
	uint8_t		conf[HC_MAXN];	/* bit e-1 */
	uint8_t		net;			/* bit per pair, see hc_pair_index */
	/* not a TLA+ variable: set when a capacity above is exceeded or a TLA+
	 * operator would have been undefined (TLC would stop with an error);
	 * hc_inv.c reports it as the HC_Wellformed invariant. */
	uint8_t		bad;
	uint8_t		pad[4];
} HcState;

/* Unpacked projection, node ids 1-based like the TLA+. */
typedef struct HcPj
{
	int			e,
				a,
				ulen,
				rlen;
	int			upi[HC_SEQ],
				rep[HC_SEQ];
} HcPj;

/* Action kinds (top byte of CmcAction).  The TLA+ action names. */
enum
{
	HC_NETCHANGE = 1,			/* d = new net mask */
	HC_PROPOSE,					/* a = n */
	HC_ADOPT,					/* a = n, d = P */
	HC_DOCONFIRM,				/* a = n */
	HC_DELIVER,					/* a = m, d = P */
	HC_READREPAIR,				/* a = n, b = m, d = V */
	HC_REPAIR,					/* a = t, b = r */
	HC_CLIENTWRITE,				/* a = h, c = w */
	HC_FORWARD,					/* a = n, b = m, c = w, d = P */
	HC_ACK						/* c = w, d = P */
};

/* shared with hc_inv.c / hc_describe.c */
void		hc_unpack(HcProj p, HcPj *o);
HcProj		hc_pack(const HcPj *p, uint8_t *bad);
HcProj		hc_p1(const HcCfg *c);
unsigned	hc_members(HcProj p);	/* node bitmask, bit n-1 */
unsigned	hc_upi_mask(HcProj p);
unsigned	hc_rep_mask(HcProj p);
bool		hc_reach(const HcCfg *c, const HcState *s, int a, int b);
bool		hc_wedged(const HcCfg *c, const HcState *s, int n);
bool		hc_is_head(HcProj cur, int n);
bool		hc_is_tail(HcProj cur, int n);
int			hc_pair_index(const HcCfg *c, int i, int j);
int			hc_npairs(const HcCfg *c);
bool		hc_set_has(const uint32_t *a, int n, uint32_t v);
/* Base(n)'s CHOOSE had more than one candidate at the maximal epoch */
bool		hc_base_tie(const HcCfg *c, const HcState *s, int n);

/* the vtable entry points (hc_core.c), wired up in hc_glue.c */
struct CmcEvent;
struct CmcOut;
void		hc_init(const void *cfgv, int node, void *sv);
int			hc_actions(const void *cfgv, const void *sv, uint64_t *acts, int max);
void		hc_step(const void *cfgv, int node, void *sv, const struct CmcEvent *ev,
				struct CmcOut *out);

static inline int
hc_pe(HcProj p)
{
	return (int) (p & 7);
}

static inline int
hc_pa(HcProj p)
{
	return (int) ((p >> 3) & 7);
}

static inline int
hc_pulen(HcProj p)
{
	return (int) ((p >> 6) & 7);
}

#endif							/* CMC_HC_CORE_H */
