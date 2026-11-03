/*
 * test_cmc.c: unit tests of the cmc engine (the engine's own exit criterion).
 * Plain asserts, no framework.  Run by `make check-mc-toy`.
 */
#include "cmc.h"

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern const CmcModel toy_model;
extern const CmcModel toy_bug1_model;
extern const CmcModel toy_bug2_model;
extern const CmcModel toy_bug3_model;

static int	failures;

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static CmcRun *
mk(const CmcModel *m, CmcSearch s, const char *const *props, CmcOptions *o_in)
{
	CmcOptions	o;
	char		err[256];
	CmcRun	   *r;

	if (o_in)
		o = *o_in;
	else
		cmc_options_default(&o);
	o.search = s;
	o.quiet = true;
	r = cmc_run_new(m, "", props, &o, err, sizeof(err));
	if (!r)
		fprintf(stderr, "cmc_run_new: %s\n", err);
	return r;
}

/* ---- fingerprint ------------------------------------------------------ */

static void
test_fingerprint(void)
{
	uint8_t		a[64],
				b[64];
	uint64_t	fa[2],
				fb[2];
	int			same0 = 0,
				same1 = 0;

	memset(a, 0, sizeof(a));
	cmc_fingerprint(a, sizeof(a), fa);
	cmc_fingerprint(a, sizeof(a), fb);
	CHECK(fa[0] == fb[0] && fa[1] == fb[1]);
	/* every single-bit flip changes both halves */
	for (int i = 0; i < 64 * 8; i++)
	{
		memcpy(b, a, sizeof(a));
		b[i / 8] ^= (uint8_t) (1u << (i % 8));
		cmc_fingerprint(b, sizeof(b), fb);
		same0 += fa[0] == fb[0];
		same1 += fa[1] == fb[1];
	}
	CHECK(same0 == 0 && same1 == 0);
	/* the two halves are independent: equal low bits rarely coincide */
	{
		int			agree = 0;

		for (int i = 0; i < 4096; i++)
		{
			memset(b, 0, sizeof(b));
			memcpy(b, &i, sizeof(i));
			cmc_fingerprint(b, sizeof(b), fb);
			agree += (fb[0] & 0xff) == (fb[1] & 0xff);
		}
		CHECK(agree < 64);		/* expected 16 */
	}
	/* length is hashed: zero-padded prefixes differ */
	cmc_fingerprint(a, 56, fb);
	CHECK(fa[0] != fb[0]);
}

/* ---- toy: exact counts, depth, bug variants -------------------------- */

static void
test_toy_pass(void)
{
	const char *props[] = {"Mutex", "PhaseMonotone", "AllReplied", "ReachCS", NULL};
	CmcRun	   *r = mk(&toy_model, CMC_BFS, props, NULL);
	CmcResult	res;

	cmc_check(r, &res);
	CHECK(res.verdict == CMC_PASS);
	/* 33 (client pairs, see toy_core.c) + 1: (done, done) splits by reply order */
	CHECK(res.distinct == 34);
	CHECK(res.depth == 11);
	CHECK(res.left == 0);
	CHECK(res.nwitness == 1 && res.witness_depth[0] == 4);	/* CLIENT, REQ, GRANT */
	if (res.distinct != 34 || res.depth != 11)
		fprintf(stderr, "  toy: distinct %llu depth %d\n",
				(unsigned long long) res.distinct, res.depth);
	cmc_result_free(&res);
	cmc_run_free(r);

	/* the same space under bounded DFS and with the determinism gate */
	{
		CmcOptions	o;

		cmc_options_default(&o);
		o.max_depth = 40;
		o.det = true;
		r = mk(&toy_model, CMC_DFS, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_PASS);
		CHECK(res.distinct == 34);
		CHECK(res.depth == 11);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
	/* a DFS bound below the depth is INCOMPLETE, never PASS */
	{
		CmcOptions	o;

		cmc_options_default(&o);
		o.max_depth = 6;
		r = mk(&toy_model, CMC_DFS, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_INCOMPLETE);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

static void
test_toy_bug1(void)
{
	const char *props[] = {"Mutex", "AllReplied", NULL};
	CmcRun	   *r = mk(&toy_bug1_model, CMC_BFS, props, NULL);
	CmcResult	res;
	char		path[512];

	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION);
	CHECK(res.prop && strcmp(res.prop, "Mutex") == 0);
	/* shortest: CLIENT x2, REQ x2 (both granted), GRANT x2 */
	CHECK(res.trace_len == 7);
	CHECK(res.depth == 7);

	/* replay file round trip: the trace replays to the same violation */
	snprintf(path, sizeof(path), "%s/toy_bug1.replay",
			 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
	CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
	cmc_result_free(&res);
	cmc_run_free(r);
	{
		CmcOptions	o;

		cmc_options_default(&o);
		o.replay_in = path;
		r = mk(&toy_bug1_model, CMC_REPLAY, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION);
		CHECK(res.prop && strcmp(res.prop, "Mutex") == 0);
		CHECK(res.trace_len == 7);
		cmc_result_free(&res);
		cmc_run_free(r);
		/* the same file against the correct core replays cleanly or is refused */
		r = mk(&toy_model, CMC_REPLAY, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict != CMC_VIOLATION);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
	/* random walks find it too, and the seed reproduces it */
	{
		CmcOptions	o;
		uint64_t	seed;

		cmc_options_default(&o);
		o.walks = 2000;
		o.max_depth = 30;
		o.seed = 7;
		r = mk(&toy_bug1_model, CMC_SIMULATE, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION);
		CHECK(res.prop && strcmp(res.prop, "Mutex") == 0);
		seed = strstr(res.why, "--seed ") ? strtoull(strstr(res.why, "--seed ") + 7, NULL, 10) : 0;
		cmc_result_free(&res);
		cmc_run_free(r);
		o.seed = seed;
		o.walks = 1;
		r = mk(&toy_bug1_model, CMC_SIMULATE, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
	/* DFS finds it as well (not necessarily shortest) */
	{
		CmcOptions	o;

		cmc_options_default(&o);
		o.max_depth = 30;
		r = mk(&toy_bug1_model, CMC_DFS, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION);
		CHECK(res.trace_len >= 7);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

static void
test_toy_bug2(void)
{
	const char *props[] = {"Mutex", "AllReplied", NULL};
	CmcRun	   *r = mk(&toy_bug2_model, CMC_BFS, props, NULL);
	CmcResult	res;

	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION);
	CHECK(res.prop && strcmp(res.prop, "AllReplied") == 0);
	/* CLIENT1, REQ1, CLIENT2, REQ2 (queued), GRANT1, FIRE1, REL1 */
	CHECK(res.trace_len == 8);
	cmc_result_free(&res);
	cmc_run_free(r);
	/* the Mutex invariant alone does not see a lost wake */
	{
		const char *p2[] = {"Mutex", NULL};

		r = mk(&toy_bug2_model, CMC_BFS, p2, NULL);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_PASS);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

/* ---- gates ------------------------------------------------------------ */

static int	nondet_counter;		/* the bug: a core reading a global */

static void
nd_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	uint8_t    *s = sv;

	(void) cfg;
	(void) node;
	(void) ev;
	out->n = 0;
	s[0] = (uint8_t) (++nondet_counter & 1);
}

static void
nd_init(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, 8);
}

static void
nd_cfg_default(void *cfg)
{
	(void) cfg;
}

static int
nd_cfg_set(void *cfg, const char *k, const char *v)
{
	(void) cfg;
	(void) k;
	(void) v;
	return -1;
}

static int
nd_nnodes(const void *cfg)
{
	(void) cfg;
	return 1;
}

/* writes "durable" byte 0 on a CLIENT event: violates the durable rule */
static void
dur_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	uint8_t    *s = sv;

	(void) cfg;
	(void) node;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
		s[0] = 1;
}

static void
dur_apply(const void *cfg, int node, void *d, const uint8_t *body, uint16_t len)
{
	(void) cfg;
	(void) node;
	if (len == 1)
		((uint8_t *) d)[0] = body[0];
}

static bool
always_true(const CmcWorld *w, CmcWhy *why)
{
	(void) w;
	(void) why;
	return true;
}

static const CmcProp nd_props[] = {CMC_INVARIANT(True, always_true)};

static const CmcCore nd_core = {
	.name = "nondet", .state_size = 8, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
	.init = nd_init, .step = nd_step,
};
static const CmcModel nd_model = {"nondet", &nd_core, nd_props, 1, 0};

static const CmcCore dur_core = {
	.name = "durable", .state_size = 8, .durable_size = 4, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
	.init = nd_init, .step = dur_step, .apply_persist = dur_apply,
};
static const CmcModel dur_model = {"durable", &dur_core, nd_props, 1, 0};

static void
test_gates(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	CmcResult	res;

	cmc_options_default(&o);
	o.det = true;
	r = mk(&nd_model, CMC_BFS, NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION && strstr(res.why, "determinism gate"));
	cmc_result_free(&res);
	cmc_run_free(r);

	cmc_options_default(&o);
	r = mk(&dur_model, CMC_BFS, NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION && strstr(res.why, "durable rule"));
	cmc_result_free(&res);
	cmc_run_free(r);
}

/* ---- non-omniscience: enabled actions are a function of the
 * environment bytes only.  Scramble every core-state byte of reachable
 * toy worlds and check the enabled set does not change. */

static void
test_not_omniscient(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	size_t		ws;
	uint8_t    *w,
			   *x;
	CmcAction	a1[CMC_MAX_ACTIONS],
				a2[CMC_MAX_ACTIONS];
	int			bad = 0;
	uint64_t	rs = 12345;

	cmc_options_default(&o);
	r = mk(&toy_model, CMC_BFS, NULL, &o);
	ws = cmc_world_size(r);
	w = malloc(ws);
	x = malloc(ws);
	cmc_world_init(r, w);
	for (int step = 0; step < 200; step++)
	{
		int			n1 = cmc_enabled(r, w, a1, CMC_MAX_ACTIONS),
					n2;

		memcpy(x, w, ws);
		for (size_t i = 0; i < 3 * 8; i++)	/* 3 nodes x sizeof(ToyState) */
		{
			rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
			x[i] = (uint8_t) (rs >> 56);
		}
		n2 = cmc_enabled(r, x, a2, CMC_MAX_ACTIONS);
		if (n1 != n2 || memcmp(a1, a2, sizeof(CmcAction) * n1) != 0)
			bad++;
		if (n1 == 0)
			cmc_world_init(r, w);
		else
		{
			rs = rs * 6364136223846793005ULL + 1442695040888963407ULL;
			CHECK(cmc_apply(r, w, a1[(rs >> 33) % (uint64_t) n1]) == 0);
		}
	}
	CHECK(bad == 0);
	free(w);
	free(x);
	cmc_run_free(r);
}

/* ---- PERSIST / FLUSHED / CRASH / DROP paths -------------------------
 * A one-node register: CLIENT persists the request number; FLUSHED copies
 * it into the durable byte and replies.  The bug variant replies before
 * the flush.  Property: every reply's value is durable ("reply only after
 * flush", steering).  With a crash budget the volatile byte is lost and
 * the durable byte survives. */

typedef struct PState
{
	uint8_t		durable;		/* survives CRASH; changes only on FLUSHED */
	uint8_t		pending;		/* volatile */
	uint8_t		pad[6];
} PState;

static int	persist_bug;

static void
p_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	PState	   *s = sv;

	(void) cfg;
	(void) node;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
	{
		CmcOutput   *o = cmc_out_push(out, CMC_OUT_PERSIST);

		o->len = 1;
		o->body[0] = (uint8_t) (ev->arg + 1);
		s->pending = (uint8_t) (ev->arg + 1);
		if (persist_bug)
			cmc_out_push(out, CMC_OUT_REPLY)->arg = ev->arg + 1;
	}
	else if (ev->kind == CMC_EV_FLUSHED && ev->len == 1)
	{
		/* the engine already applied the record (p_apply) */
		s->pending = 0;
		if (!persist_bug)
			cmc_out_push(out, CMC_OUT_REPLY)->arg = ev->body[0];
	}
}

static bool
reply_durable(const CmcWorld *w, CmcWhy *why)
{
	const PState *s = cmc_node(w, 0);

	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			n;
		uint32_t	arg;

		cmc_reply(w, i, &n, &arg);
		if (s->durable < arg)
		{
			cmc_why(why, "reply %u before it was durable (durable=%u)", arg, s->durable);
			return false;
		}
	}
	return true;
}

static bool
reach_crash_kept(const CmcWorld *w, CmcWhy *why)
{
	const PState *s = cmc_node(w, 0);

	(void) why;
	return s->durable == 1 && cmc_nreplies(w) == 1 && s->pending == 0;
}

static const CmcProp p_props[] = {
	CMC_INVARIANT(ReplyDurable, reply_durable),
	CMC_WITNESS_PROP(Reached, reach_crash_kept),
};
static const CmcCore p_core = {
	.name = "persist", .state_size = sizeof(PState), .durable_size = 1, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
	.init = nd_init, .step = p_step, .apply_persist = dur_apply,
};
static const CmcModel p_model = {"persist", &p_core, p_props, 2, 0};

static void
test_persist_crash_drop(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	CmcResult	res;
	size_t		ws;
	uint8_t	   *w;
	CmcAction	a[CMC_MAX_ACTIONS];
	int			na;

	/* correct: passes; states: {idle, persisting, flushed} x crash budget */
	cmc_options_default(&o);
	o.det = true;
	o.crash_budget = 1;
	r = mk(&p_model, CMC_BFS, NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_PASS);
	CHECK(res.nwitness == 1 && res.witness_depth[0] == 3);
	cmc_result_free(&res);

	/* CLIENT, then CRASH: the pending persist and volatile byte are gone,
	 * the durable byte is still 0, and nothing is enabled but nothing */
	ws = cmc_world_size(r);
	w = malloc(ws);
	cmc_world_init(r, w);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)) == 0);
	CHECK(((PState *) w)->pending == 1);
	na = cmc_enabled(r, w, a, CMC_MAX_ACTIONS);
	CHECK(na == 3);				/* FLUSH, CRASH (0 or 1 record reached disk) */
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CRASH, 0, 0, 0, 0)) == 0);
	CHECK(((PState *) w)->pending == 0 && ((PState *) w)->durable == 0);
	CHECK(cmc_enabled(r, w, a, CMC_MAX_ACTIONS) == 0);
	/* an action that is not enabled is refused */
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_FLUSH, 0, 0, 0, 0)) != 0);
	free(w);
	cmc_run_free(r);

	/* bug: reply before flush is caught at depth 2 */
	persist_bug = 1;
	cmc_options_default(&o);
	r = mk(&p_model, CMC_BFS, NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "ReplyDurable"));
	CHECK(res.trace_len == 2);
	cmc_result_free(&res);
	cmc_run_free(r);
	persist_bug = 0;

	/* DROP: the toy has no retry, so one dropped message loses a reply */
	{
		const char *props[] = {"Mutex", "AllReplied", NULL};

		cmc_options_default(&o);
		o.drop_budget = 1;
		r = mk(&toy_model, CMC_BFS, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "AllReplied"));
		/* fifo: CLIENT1, REQ1 granted, DROP GRANT (resets 0-1), both
		 * CONN_RESETs, CLIENT2, REQ2 queued behind a holder that never
		 * releases */
		CHECK(res.trace_len == 8);
		cmc_result_free(&res);
		cmc_run_free(r);
		/* bag: the same without the resets */
		o.net = CMC_NET_BAG;
		r = mk(&toy_model, CMC_BFS, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "AllReplied"));
		CHECK(res.trace_len == 6);
		cmc_result_free(&res);
		cmc_run_free(r);
		/* DUP: a duplicate REQ makes the server queue a client twice; the
		 * toy tolerates it (REL from a non-holder is ignored) */
		cmc_options_default(&o);
		o.dup_budget = 1;
		r = mk(&toy_model, CMC_BFS, props, &o);
		CHECK(r == NULL);		/* a FIFO connection never duplicates */
		o.net = CMC_NET_BAG;
		r = mk(&toy_model, CMC_BFS, props, &o);
		cmc_check(r, &res);
		CHECK(res.verdict != CMC_ERROR);
		CHECK(res.distinct > 34);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

/* ---- docs/research/02 regressions --------------------------------------- */

static CmcRun *
mkc(const CmcModel *m, CmcSearch s, const char *cfg, const char *const *props, CmcOptions *o)
{
	char		err[256];
	CmcRun	   *r;

	o->search = s;
	o->quiet = true;
	r = cmc_run_new(m, cfg, props, o, err, sizeof(err));
	if (!r)
		fprintf(stderr, "cmc_run_new: %s\n", err);
	return r;
}

static CmcVerdict
verdict_of(const CmcModel *m, CmcOptions *o, const char *const *props, char *prop, size_t plen,
		   char *why, size_t wlen, int *depth)
{
	CmcRun	   *r = mkc(m, o->search ? o->search : CMC_BFS, "", props, o);
	CmcResult	res;
	CmcVerdict	v;

	if (!r)
		return CMC_ERROR;
	cmc_check(r, &res);
	v = res.verdict;
	if (getenv("CMC_VERBOSE"))
		fprintf(stderr, "  %-8s %s %s: %s distinct=%llu depth=%d trace=%d\n", m->name,
				v == CMC_PASS ? "PASS" : v == CMC_VIOLATION ? "VIOLATION" : "OTHER",
				res.prop ? res.prop : "", res.why, (unsigned long long) res.distinct,
				res.depth, res.trace_len);
	if (prop)
		snprintf(prop, plen, "%s", res.prop ? res.prop : "");
	if (why)
		snprintf(why, wlen, "%s", res.why);
	if (depth)
		*depth = res.depth;
	cmc_result_free(&res);
	cmc_run_free(r);
	return v;
}

static void
zero8(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, 8);
}

/* E1 (B1): toy-bug2 (a lost wake) plus a server heartbeat timer.  The
 * timer is always armed, so no state is quiescent and CMC_QUIESCENT never
 * runs; bounded liveness (CMC_EVENTUALLY) must still see the lost wake. */
extern void toy_step_v0(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out);
extern void toy_step_v2(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out);

static void
hb_arm(CmcOut *out)
{
	cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = 1;
}

static void
hb_step(void (*inner) (const void *, int, void *, const CmcEvent *, CmcOut *),
		const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	if (node == 0 && ev->kind == CMC_EV_TIMER)
	{
		out->n = 0;
		hb_arm(out);
		return;
	}
	inner(cfg, node, sv, ev, out);
	if (node == 0 && ev->kind == CMC_EV_RECV)
		hb_arm(out);
}

static void
hb0_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	hb_step(toy_step_v0, cfg, node, sv, ev, out);
}

static void
hb2_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	hb_step(toy_step_v2, cfg, node, sv, ev, out);
}

static void
test_b1_heartbeat_lost_wake(void)
{
	CmcCore		c0 = *toy_model.core,
				c2 = *toy_bug2_model.core;
	CmcModel		m0 = toy_model,
				m2 = toy_bug2_model;
	CmcOptions	o;
	char		prop[64];

	c0.step = hb0_step;
	c2.step = hb2_step;
	m0.core = &c0;
	m2.core = &c2;
	/* every property of the toy model: the lost wake must be found */
	cmc_options_default(&o);
	o.det = true;
	CHECK(verdict_of(&m2, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	CHECK(strcmp(prop, "EventuallyAllReplied") == 0);
	cmc_options_default(&o);
	o.search = CMC_SIMULATE;
	o.walks = 1000;
	o.max_depth = 40;
	CHECK(verdict_of(&m2, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	CHECK(strcmp(prop, "EventuallyAllReplied") == 0);
	/* the correct core with the same heartbeat passes (no false alarm) */
	cmc_options_default(&o);
	o.det = true;
	CHECK(verdict_of(&m0, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_PASS);
	/* CMC_QUIESCENT alone: a state whose only action is a timer fire that
	 * returns to the same world counts as quiescent */
	{
		const char *qp[] = {"Mutex", "AllReplied", NULL};

		cmc_options_default(&o);
		CHECK(verdict_of(&m2, &o, qp, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
		CHECK(strcmp(prop, "AllReplied") == 0);
		CHECK(verdict_of(&m0, &o, qp, prop, sizeof(prop), NULL, 0, NULL) == CMC_PASS);
	}
	/* the counterexample (prefix + fair suffix) replays to the same verdict */
	{
		CmcRun	   *r;
		CmcResult	res;
		char		path[512];

		snprintf(path, sizeof(path), "%s/b1.replay",
				 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
		cmc_options_default(&o);
		r = mkc(&m2, CMC_BFS, "", NULL, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION && res.fair_from > 0 &&
			  res.trace_len > res.fair_from);
		CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
		cmc_result_free(&res);
		cmc_run_free(r);
		o.replay_in = path;
		r = mkc(&m2, CMC_REPLAY, "", NULL, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION && res.prop &&
			  strcmp(res.prop, "EventuallyAllReplied") == 0);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

/* E4 (B3): CLIENT persists; FLUSHED makes it durable and replies; RESTART
 * assumes durable means "already replied".  Wrong when the record reached
 * disk but the process died before FLUSHED: a crash must be allowed to
 * keep a pending persist. */
typedef struct E4
{
	uint8_t		durable,
				pad[7];
} E4;

static void
e4_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	E4		   *s = sv;
	CmcOutput   *o;

	(void) cfg;
	(void) node;
	out->n = 0;
	(void) s;
	if (ev->kind == CMC_EV_CLIENT)
	{
		o = cmc_out_push(out, CMC_OUT_PERSIST);
		o->len = 1;
		o->body[0] = 1;
	}
	else if (ev->kind == CMC_EV_FLUSHED)
		cmc_out_push(out, CMC_OUT_REPLY)->arg = 1;	/* durable = 1 by now */
	/* RESTART: durable means "already replied" (the bug) */
}

static bool
e4_qui(const CmcWorld *w, CmcWhy *why)
{
	const E4   *s = cmc_node(w, 0);

	if (s->durable && cmc_nreplies(w) == 0)
	{
		cmc_why(why, "durable write never acknowledged");
		return false;
	}
	return true;
}

static const CmcProp e4_props[] = {CMC_QUIESCENT_PROP(DurableImpliesReplied, e4_qui)};
static const CmcCore e4_core = {
	.name = "e4", .state_size = 8, .durable_size = 1, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
	.init = zero8, .step = e4_step, .apply_persist = dur_apply,
};
static const CmcModel e4_model = {"e4", &e4_core, e4_props, 1, 0};

static void
test_b3_crash_keeps_disk_write(void)
{
	CmcOptions	o;
	char		prop[64];

	cmc_options_default(&o);
	o.det = true;
	o.crash_budget = 1;
	CHECK(verdict_of(&e4_model, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	CHECK(strcmp(prop, "DurableImpliesReplied") == 0);
}

/* E6 (B4): a Paxos-style acceptor that reports its promise from volatile
 * state.  Honest variant: the durable promise is the flushed record (the
 * bug shows as PromiseMonotone).  Dishonest variant: FLUSHED copies the
 * volatile (maybe later, unflushed) ballot into durable.  Both must fail. */
typedef struct E6
{
	uint8_t		dpromised,
				pad0[3],
				v,
				pad[3];
} E6;

static int	e6_from_volatile;

static void
e6_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	E6		   *s = sv;
	CmcOutput   *o;

	(void) cfg;
	out->n = 0;
	if (node != 0)
	{
		if (ev->kind == CMC_EV_CLIENT)
		{
			o = cmc_out_push(out, CMC_OUT_SEND);
			o->dst = 0;
			o->len = 1;
			o->body[0] = (uint8_t[]) {0, 1, 3, 2}[node];
		}
		return;
	}
	if (ev->kind == CMC_EV_RECV && ev->body[0] > s->v)
	{
		s->v = ev->body[0];
		o = cmc_out_push(out, CMC_OUT_PERSIST);
		o->len = 1;
		o->body[0] = s->v;
	}
	else if (ev->kind == CMC_EV_FLUSHED)
	{
		/* honest: e6_apply already set dpromised = record.  Dishonest:
		 * also copy the volatile, maybe unflushed, ballot */
		if (e6_from_volatile)
			s->dpromised = s->v;
		cmc_out_push(out, CMC_OUT_REPLY)->arg = s->v;	/* the bug */
	}
	else if (ev->kind == CMC_EV_RESTART)
		s->v = s->dpromised;
}

static int
e6_clients(const void *c, int n)
{
	(void) c;
	return n != 0;
}

static int
four(const void *c)
{
	(void) c;
	return 4;
}

static bool
e6_inv(const CmcWorld *w, CmcWhy *why)
{
	uint32_t	hi = 0;

	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			n;
		uint32_t	a;

		cmc_reply(w, i, &n, &a);
		if (a < hi)
		{
			cmc_why(why, "promised %u after %u", a, hi);
			return false;
		}
		if (a > hi)
			hi = a;
	}
	return true;
}

static const CmcProp e6_props[] = {CMC_INVARIANT(PromiseMonotone, e6_inv)};
static const CmcCore e6_core = {
	.name = "e6", .state_size = 8, .durable_size = 4, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = four,
	.init = zero8, .step = e6_step, .clients = e6_clients, .apply_persist = dur_apply,
};
static const CmcModel e6_model = {"e6", &e6_core, e6_props, 1, 0};

static void
test_b4_flushed_cannot_write_durable(void)
{
	CmcOptions	o;
	char		prop[64],
				why[512];

	for (e6_from_volatile = 0; e6_from_volatile <= 1; e6_from_volatile++)
	{
		cmc_options_default(&o);
		o.det = true;
		o.crash_budget = 1;
		o.persist_cap = 3;
		CHECK(verdict_of(&e6_model, &o, NULL, prop, sizeof(prop), why, sizeof(why), NULL)
			  == CMC_VIOLATION);
		if (!e6_from_volatile)
		{
			CHECK(strcmp(prop, "PromiseMonotone") == 0);
			o.net = CMC_NET_BAG;		/* the reviewer's setting */
			CHECK(verdict_of(&e6_model, &o, NULL, prop, sizeof(prop), why, sizeof(why), NULL)
				  == CMC_VIOLATION && strcmp(prop, "PromiseMonotone") == 0);
		}
		else
			CHECK(strstr(why, "durable rule") != NULL);
	}
	e6_from_volatile = 0;
}

/* E10 (B5): a client sends once and never retries; the server replies on
 * RECV.  A server crash resets the connection, so the request is lost and
 * the client hangs.  With connections (the default) this must fail. */
static void
e10_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	CmcOutput   *o;

	(void) cfg;
	(void) sv;
	out->n = 0;
	if (node == 1 && ev->kind == CMC_EV_CLIENT)
	{
		o = cmc_out_push(out, CMC_OUT_SEND);
		o->dst = 0;
		o->len = 1;
		o->body[0] = 1;
	}
	if (node == 0 && ev->kind == CMC_EV_RECV)
		cmc_out_push(out, CMC_OUT_REPLY)->arg = 1;
}

static int
e10_clients(const void *c, int n)
{
	(void) c;
	return n == 1;
}

static int
two(const void *c)
{
	(void) c;
	return 2;
}

static bool
e10_qui(const CmcWorld *w, CmcWhy *why)
{
	if (cmc_nreplies(w) != 1)
	{
		cmc_why(why, "client never answered");
		return false;
	}
	return true;
}

static const CmcProp e10_props[] = {CMC_QUIESCENT_PROP(Answered, e10_qui)};
static const CmcCore e10_core = {
	.name = "e10", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = two, .init = zero8, .step = e10_step,
	.clients = e10_clients,
};
static const CmcModel e10_model = {"e10", &e10_core, e10_props, 1, 0};

static void
test_b5_crash_resets_connections(void)
{
	CmcOptions	o;
	char		prop[64];

	cmc_options_default(&o);
	o.crash_budget = 1;
	CHECK(verdict_of(&e10_model, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	CHECK(strcmp(prop, "Answered") == 0);
	/* the bag keeps the request across the crash: the old (unsound for
	 * TCP) behaviour, still available for transports without FIFO */
	o.net = CMC_NET_BAG;
	CHECK(verdict_of(&e10_model, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_PASS);
}

/* E3 (M1): durable_size 600; a CLIENT event writes durable byte 550. */
static void
e3_init(const void *c, int n, void *s)
{
	(void) c;
	(void) n;
	memset(s, 0, 1024);
}

static void
e3_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	uint8_t    *s = sv;

	(void) cfg;
	(void) node;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
		s[550] = 1;
}

static const CmcCore e3_core = {
	.name = "e3", .state_size = 1024, .durable_size = 600, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
	.init = e3_init, .step = e3_step, .apply_persist = dur_apply,
};
static const CmcModel e3_model = {"e3", &e3_core, nd_props, 1, 0};

static void
test_m1_large_durable(void)
{
	CmcOptions	o;
	char		why[512];

	cmc_options_default(&o);
	CHECK(verdict_of(&e3_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION);
	CHECK(strstr(why, "durable rule") != NULL);
}

/* E8 (M3): a core that emits CMC_OUT_MAX + 4 outputs.  Run in a child: on
 * an engine without an overflow check this smashes the stack. */
static void
e8_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) node;
	(void) sv;
	out->n = 0;
	if (ev->kind != CMC_EV_CLIENT)
		return;
	for (int i = 0; i < CMC_OUT_MAX + 4; i++)
		cmc_out_push(out, CMC_OUT_SET_TIMER);
}

static const CmcCore e8_core = {
	.name = "e8", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = e8_step,
};
static const CmcModel e8_model = {"e8", &e8_core, nd_props, 1, 0};

static void
test_m3_output_overflow(void)
{
	pid_t		pid;
	int			st = 0;

	fflush(NULL);
	pid = fork();
	if (pid == 0)
	{
		CmcOptions	o;
		char		why[512];

		cmc_options_default(&o);
		_exit(verdict_of(&e8_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
			  strstr(why, "outputs") ? 0 : 1);
	}
	CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* M4: a replay file recorded with drop=1 replayed under different
 * budgets must be refused, not "completed". */
static void
test_m4_replay_conflict(void)
{
	const char *props[] = {"Mutex", "AllReplied", NULL};
	CmcOptions	o;
	CmcRun	   *r;
	CmcResult	res;
	char		path[512];

	snprintf(path, sizeof(path), "%s/m4.replay",
			 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
	cmc_options_default(&o);
	o.drop_budget = 1;
	r = mkc(&toy_model, CMC_BFS, "", props, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION);
	CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
	cmc_result_free(&res);
	cmc_run_free(r);
	/* same budgets: reproduces */
	o.replay_in = path;
	r = mkc(&toy_model, CMC_REPLAY, "", props, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION);
	cmc_result_free(&res);
	cmc_run_free(r);
	/* a different client budget: refused */
	o.client_budget = 2;
	r = mkc(&toy_model, CMC_REPLAY, "", props, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_ERROR && strstr(res.why, "recorded with"));
	cmc_result_free(&res);
	cmc_run_free(r);
	/* a different model: refused */
	o.client_budget = 1;
	r = mkc(&toy_bug1_model, CMC_REPLAY, "", props, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_ERROR && strstr(res.why, "recorded with"));
	cmc_result_free(&res);
	cmc_run_free(r);
}

/* m1: a complete DFS that never reaches a witness is vacuous, as in BFS */
static void
test_min1_dfs_witness(void)
{
	const char *props[] = {"Mutex", "ReachCS", NULL};
	CmcOptions	o;
	char		prop[64];

	cmc_options_default(&o);
	o.client_budget = 0;
	o.max_depth = 40;
	o.search = CMC_DFS;
	CHECK(verdict_of(&toy_model, &o, props, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	CHECK(strcmp(prop, "ReachCS") == 0);
}

/* m3: a quiescence violation reports the depth of the violating state */
static void
test_min3_quiescent_depth(void)
{
	const char *props[] = {"Mutex", "AllReplied", NULL};
	CmcOptions	o;
	int			depth = 0;

	cmc_options_default(&o);
	CHECK(verdict_of(&toy_bug2_model, &o, props, NULL, 0, NULL, 0, &depth) == CMC_VIOLATION);
	CHECK(depth == 8);
}

/* m4: negative budgets and caps are rejected */
static void
test_min4_negative_bounds(void)
{
	CmcOptions	o;
	char		err[256];
	CmcRun	   *r;
	int		   *fields[] = {&o.drop_budget, &o.dup_budget, &o.crash_budget,
		&o.client_budget, &o.net_cap, &o.persist_cap, &o.reply_cap};

	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
	{
		cmc_options_default(&o);
		*fields[i] = -1;
		r = cmc_run_new(&toy_model, "", NULL, &o, err, sizeof(err));
		CHECK(r == NULL);
		cmc_run_free(r);
	}
}

/* CMC_VERSION is MAJOR.MINOR.PATCH: the release workflows and the flake
 * read it, and the tag must be v followed by it */
static void
test_version(void)
{
	unsigned	maj,
				min,
				pat;
	char		extra;

	CHECK(sscanf(CMC_VERSION, "%u.%u.%u%c", &maj, &min, &pat, &extra) == 3);
}

int
main(void)
{
	test_fingerprint();
	test_toy_pass();
	test_toy_bug1();
	test_toy_bug2();
	test_gates();
	test_not_omniscient();
	test_persist_crash_drop();
	test_b1_heartbeat_lost_wake();
	test_b3_crash_keeps_disk_write();
	test_b4_flushed_cannot_write_durable();
	test_b5_crash_resets_connections();
	test_m1_large_durable();
	test_m3_output_overflow();
	test_m4_replay_conflict();
	test_min1_dfs_witness();
	test_min3_quiescent_depth();
	test_min4_negative_bounds();
	test_version();
	if (failures)
	{
		fprintf(stderr, "test_cmc: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cmc: all engine tests passed\n");
	return 0;
}
