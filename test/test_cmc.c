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

/* one worker in memory against several (and a queue budget): same result */
static void same_search(const CmcModel *m, const char *const *props, const CmcOptions *base,
						int workers, uint64_t queue_mem);
static void same_search_cfg(const CmcModel *m, const char *cfg, const char *const *props,
							const CmcOptions *base, int workers, uint64_t queue_mem);

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

static uint64_t last_distinct;	/* of the last verdict_of() run */

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
	last_distinct = res.distinct;
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

static int
zero_nodes(const void *c)
{
	(void) c;
	return 0;
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

/* crashable(): nodes in e10_crash_mask may crash.  With the server
 * excluded, a crash can no longer lose the request. */
static unsigned e10_crash_mask;

static int
e10_crashable(const void *c, int n)
{
	(void) c;
	return (e10_crash_mask >> n) & 1;
}

static const CmcCore e10s_core = {
	.name = "e10s", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = two, .init = zero8, .step = e10_step,
	.clients = e10_clients, .crashable = e10_crashable,
};
static const CmcModel e10s_model = {"e10s", &e10s_core, e10_props, 1, 0};

static void
test_crashable(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	uint8_t    *w;
	CmcAction	a[CMC_MAX_ACTIONS];
	int			na;
	char		prop[64];

	cmc_options_default(&o);
	o.crash_budget = 1;
	e10_crash_mask = 1;
	CHECK(verdict_of(&e10s_model, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	e10_crash_mask = 0;
	CHECK(verdict_of(&e10s_model, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_PASS);
	e10_crash_mask = 2;
	r = mkc(&e10s_model, CMC_BFS, "", NULL, &o);
	w = calloc(1, cmc_world_size(r));
	cmc_world_init(r, w);
	na = cmc_enabled(r, w, a, CMC_MAX_ACTIONS);
	for (int i = 0; i < na; i++)
		CHECK(CMC_ACT_KIND(a[i]) != CMC_A_CRASH || CMC_ACT_A(a[i]) == 1);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CRASH, 0, 0, 0, 0)) != 0);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CRASH, 1, 0, 0, 0)) == 0);
	free(w);
	cmc_run_free(r);
}

/* Timers and wakes with an argument.  On CLIENT the node arms timer 1
 * twice, with arg2 9 and 7, and asks for wake 2 with arg2 5.  Timer
 * (1, 9) re-arms itself and changes nothing else; timer (1, 7) and the
 * wake record the argument they came back with; the wake cancels (1, 9). */
typedef struct TA
{
	uint8_t		f7,
				wk,
				pad[6];
} TA;

static void
ta_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	TA		   *s = sv;
	CmcOutput  *o;

	(void) cfg;
	(void) node;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
	{
		o = cmc_out_push(out, CMC_OUT_SET_TIMER);
		o->arg = 1;
		o->arg2 = 9;
		o = cmc_out_push(out, CMC_OUT_SET_TIMER);
		o->arg = 1;
		o->arg2 = 7;
		o = cmc_out_push(out, CMC_OUT_WAKE);
		o->arg = 2;
		o->arg2 = 5;
	}
	else if (ev->kind == CMC_EV_TIMER && ev->arg2 == 9)
	{
		o = cmc_out_push(out, CMC_OUT_SET_TIMER);
		o->arg = 1;
		o->arg2 = 9;
	}
	else if (ev->kind == CMC_EV_TIMER)
		s->f7 = (uint8_t) ev->arg2;
	else if (ev->kind == CMC_EV_WAKE)
	{
		s->wk = (uint8_t) ev->arg2;
		o = cmc_out_push(out, CMC_OUT_CANCEL_TIMER);
		o->arg = 1;
		o->arg2 = 9;
	}
}

static bool
ta_both(const CmcWorld *w, CmcWhy *why)
{
	const TA   *s = cmc_node(w, 0);

	(void) why;
	return s->f7 == 7 && s->wk == 5;
}

static const CmcProp ta_props[] = {CMC_INVARIANT(True, always_true),
CMC_WITNESS_PROP(BothBack, ta_both)};
static const CmcCore ta_core = {
	.name = "ta", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = ta_step,
};
static const CmcModel ta_model = {"ta", &ta_core, ta_props, 2, 0};

static void
test_timer_args(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	uint8_t    *w,
			   *w2;
	CmcAction	a[CMC_MAX_ACTIONS];
	char		why[512];

	cmc_options_default(&o);
	o.det = true;
	r = mkc(&ta_model, CMC_BFS, "", NULL, &o);
	w = calloc(1, cmc_world_size(r));
	w2 = calloc(1, cmc_world_size(r));
	cmc_world_init(r, w);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)) == 0);
	/* equal deadlines, different arguments: two events, sorted by arg2 */
	CHECK(cmc_enabled(r, w, a, CMC_MAX_ACTIONS) == 3);
	CHECK(a[0] == CMC_ACT(CMC_A_FIRE, 0, 1, 0, 7) && a[1] == CMC_ACT(CMC_A_FIRE, 0, 1, 0, 9));
	CHECK(a[2] == CMC_ACT(CMC_A_WAKE, 0, 2, 0, 5));
	/* re-arming (1, 9) after (1, 7) gives the same bytes: canonical */
	memcpy(w2, w, cmc_world_size(r));
	CHECK(cmc_apply(r, w2, a[1]) == 0 && memcmp(w, w2, cmc_world_size(r)) == 0);
	CHECK(cmc_apply(r, w2, CMC_ACT(CMC_A_FIRE, 0, 1, 1, 7)) != 0);
	CHECK(cmc_apply(r, w2, CMC_ACT(CMC_A_FIRE, 0, 1, 0, 8)) != 0);
	CHECK(cmc_apply(r, w2, CMC_ACT(CMC_A_WAKE, 0, 2, 0, 4)) != 0);
	CHECK(cmc_apply(r, w2, a[2]) == 0 && ((TA *) w2)->wk == 5);
	CHECK(cmc_enabled(r, w2, a, CMC_MAX_ACTIONS) == 1);	/* (1, 9) cancelled */
	CHECK(cmc_apply(r, w2, a[0]) == 0 && ((TA *) w2)->f7 == 7);
	free(w);
	free(w2);
	cmc_run_free(r);
	CHECK(verdict_of(&ta_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	/* the caps are errors, never truncation */
	o.timer_cap = 1;
	CHECK(verdict_of(&ta_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "timer_cap 1 exceeded"));
	o.timer_cap = 2;
	o.wake_cap = 0;
	CHECK(verdict_of(&ta_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "wake_cap 0 exceeded"));
	o.timer_cap = 256;
	CHECK(verdict_of(&ta_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_ERROR);
}

/* A lease whose safety rests on bounded clock drift, ported from
 * test/proc/leasedrift.c.  Node 0 reads its clock (t_send), asks node 1
 * for a lease of LD_L, and on the grant serves reads until valid_until =
 * t_send + L(1 - rho) on its own clock.  Node 1 grants, then waits L and
 * one more unit on its own clock before the lease is free again.  With
 * ld_bug the holder uses L, no margin: on equal clocks it still stops a
 * unit before the granter's wait ends, so only a slow holder clock shows
 * the bug. */
#define LD_L 1000
#define LD_RHO 100				/* per thousand: the drift the margin covers */

typedef struct LD
{
	uint64_t	t_send,
				valid_until,
				first;			/* ld_renew: node 0's first request */
	uint8_t		serving,		/* node 0 */
				again,			/* node 1: the lease is free again */
				granted,		/* node 1: a grant not yet free (ld_renew) */
				pending,		/* node 1: a request waits for it */
				vet,			/* node 0: first is LD_VET old */
				pad[3];
} LD;

/*
 * ld_renew: node 0 asks again whenever its lease ends, forever, and node
 * 1 grants a request that arrives while a grant is out once that one is
 * free.  The clocks then grow without bound, and only --time-bound makes
 * the space finite.  Node 0 also records when it first asked, and once
 * that is LD_VET old on its clock becomes a veteran: a time it compares
 * against a constant larger than the lease, and keeps forever.
 */
#define LD_VET 3000

static int	ld_bug,
			ld_renew,
			ld_vet_off;			/* no veteran: first is never compared */

static void
ld_timer(CmcOut *out, uint64_t dur)
{
	CmcOutput  *o = cmc_out_push(out, CMC_OUT_SET_TIMER);

	o->dur = dur;
}

static void
ld_send(CmcOut *out, int dst, uint8_t b)
{
	CmcOutput  *o = cmc_out_push(out, CMC_OUT_SEND);

	o->dst = (uint8_t) dst;
	o->len = 1;
	o->body[0] = b;
}

static void
ld_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	LD		   *s = sv;

	(void) cfg;
	out->n = 0;
	if (node == 0 && ld_renew && !ld_vet_off && s->first && !s->vet &&
		ev->now - s->first >= LD_VET)
		s->vet = 1;
	if (node == 0 && (ev->kind == CMC_EV_CLIENT || (ld_renew && ev->kind == CMC_EV_TIMER)))
	{
		if (ld_renew && !s->first)
			s->first = ev->now;
		if (ld_renew)
			s->serving = 0;
		s->t_send = ev->now;
		ld_send(out, 1, 'Q');
	}
	else if (node == 0 && ev->kind == CMC_EV_RECV)
	{
		s->valid_until = s->t_send + (uint64_t) LD_L * (ld_bug ? 1000 : 1000 - LD_RHO) / 1000;
		/* a grant that arrives after the lease ended is never served;
		 * ld_renew asks again */
		if (ev->now >= s->valid_until)
		{
			if (ld_renew)
			{
				s->t_send = ev->now;
				ld_send(out, 1, 'Q');
			}
			return;
		}
		s->serving = 1;
		ld_timer(out, s->valid_until - ev->now);
	}
	else if (node == 0 && ev->kind == CMC_EV_TIMER)
		s->serving = 0;
	else if (node == 1 && ev->kind == CMC_EV_RECV && s->granted)
		s->pending = 1;
	else if (node == 1 && (ev->kind == CMC_EV_RECV || (ev->kind == CMC_EV_TIMER && s->pending)))
	{
		ld_send(out, 0, 'G');
		ld_timer(out, LD_L + 1);
		s->granted = (uint8_t) ld_renew;
		s->pending = 0;
		if (ld_renew)
			s->again = 0;
	}
	else if (node == 1 && ev->kind == CMC_EV_TIMER)
		s->again = 1, s->granted = 0;
}

static void
ld_init(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, sizeof(LD));
}

static int
ld_clients(const void *c, int n)
{
	(void) c;
	return n == 0;
}

static bool
ld_one_holder(const CmcWorld *w, CmcWhy *why)
{
	const LD   *h = cmc_node(w, 0),
			   *g = cmc_node(w, 1);

	if (h->serving && g->again)
	{
		cmc_why(why, "node 0 serves at clock %llu, node 1 freed the lease at %llu",
				(unsigned long long) cmc_clock(w, 0), (unsigned long long) cmc_clock(w, 1));
		return false;
	}
	return true;
}

/* the holder's clock never passes valid_until while it serves: its
 * deadline fires first */
static bool
ld_on_clock(const CmcWorld *w, CmcWhy *why)
{
	const LD   *h = cmc_node(w, 0);

	if (h->serving && cmc_clock(w, 0) > h->valid_until)
	{
		cmc_why(why, "serving at clock %llu", (unsigned long long) cmc_clock(w, 0));
		return false;
	}
	return true;
}

static bool
ld_freed(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return cmc_now(w) > LD_L;
}

static const CmcProp ld_props[] = {CMC_INVARIANT(OneHolder, ld_one_holder),
	CMC_INVARIANT(ServesOnOwnClock, ld_on_clock), CMC_WITNESS_PROP(Freed, ld_freed)};
static const CmcCore ld_core = {
	.name = "lease", .state_size = sizeof(LD), .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = two, .init = ld_init,
	.step = ld_step, .clients = ld_clients,
};
static const CmcModel ld_model = {"lease", &ld_core, ld_props, 3, 0};

/* node 0's times; ld_tf_skip leaves one undeclared (1 + its index) */
static int	ld_tf_skip;

static int
ld_time_fields(const void *cfg, int node, size_t *off, int max)
{
	size_t		all[3] = {offsetof(LD, t_send), offsetof(LD, valid_until), offsetof(LD, first)};
	int			k = 0;

	(void) cfg;
	for (int i = 0; i < 3 && node == 0 && k < max; i++)
		if (ld_tf_skip != i + 1)
			off[k++] = all[i];
	return k;
}

static bool
ld_veteran(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return ((const LD *) cmc_node(w, 0))->vet;
}

/* the renewing lease with its times declared; Freed reads cmc_now(),
 * which a time-bound world does not keep, so Veteran stands in for it */
static const CmcProp ldr_props[] = {CMC_INVARIANT(OneHolder, ld_one_holder),
	CMC_INVARIANT(ServesOnOwnClock, ld_on_clock), CMC_WITNESS_PROP(Veteran, ld_veteran)};
static const CmcCore ldr_core = {
	.name = "lease-renew", .state_size = sizeof(LD), .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = two, .init = ld_init,
	.step = ld_step, .clients = ld_clients, .time_fields = ld_time_fields,
};
static const CmcModel ldr_model = {"lease-renew", &ldr_core, ldr_props, 3, 0};

static void
test_lease_drift(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	CmcResult	res;
	char		prop[64],
				why[512];
	int			depth;

	/* L(1 - rho) holds under drift 50, the bug holds on equal clocks */
	for (ld_bug = 0; ld_bug <= 1; ld_bug++)
	{
		cmc_options_default(&o);
		o.det = true;
		o.drift = ld_bug ? 0 : 50;
		CHECK(verdict_of(&ld_model, &o, NULL, prop, sizeof(prop), why, sizeof(why), NULL)
			  == CMC_PASS);
	}
	/* no margin: caught with node 0's clock slow when node 1's timer fires */
	ld_bug = 1;
	o.drift = 50;
	r = mkc(&ld_model, CMC_BFS, "", NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "OneHolder"));
	CHECK(res.trace_len >= 2 &&
		  res.trace[res.trace_len - 2] == CMC_ACT(CMC_A_FIRE, 1, 0, 1, 0));
	cmc_result_free(&res);
	cmc_run_free(r);
	/* margin beaten: drift 200 */
	ld_bug = 0;
	o.drift = 200;
	CHECK(verdict_of(&ld_model, &o, NULL, prop, sizeof(prop), NULL, 0, &depth)
		  == CMC_VIOLATION && !strcmp(prop, "OneHolder"));
	/* a horizon short of the lease: no pass, INCOMPLETE */
	o.drift = 50;
	o.max_time = 500;
	CHECK(verdict_of(&ld_model, &o, NULL, prop, sizeof(prop), why, sizeof(why), NULL)
		  == CMC_INCOMPLETE && strstr(why, "max_time"));
	o.max_time = 5000;
	CHECK(verdict_of(&ld_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	o.drift = 501;
	CHECK(verdict_of(&ld_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_ERROR);
	/* DFS and simulation stop at the horizon too, never passing */
	o.drift = 0;
	o.max_time = 500;
	o.search = CMC_DFS;
	CHECK(verdict_of(&ld_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_INCOMPLETE);
	o.search = CMC_SIMULATE;
	o.walks = 20;
	CHECK(verdict_of(&ld_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_INCOMPLETE);
}

/*
 * --time-bound on the renewing lease: without a horizon the clocks grow
 * forever, and with the bound the search ends, with the verdicts of the
 * lease above.  --time-check catches a time field left undeclared and a
 * bound below the veteran age the core compares against.
 */
static void
test_time_bound(void)
{
	CmcOptions	o;
	char		prop[64],
				why[512];
	int			depth;
	uint64_t	n1,
				n2;

	ld_renew = 1;
	cmc_options_default(&o);
	o.max_states = 200000;
	o.drift = 50;
	/* a core with time fields must name its bound */
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_ERROR);
	/* the lease core without time fields never ends */
	CHECK(verdict_of(&ld_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_INCOMPLETE &&
		  strstr(why, "max_states"));
	o.time_bound = LD_VET;
	o.time_check = true;
	o.det = true;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	/* a larger bound than the core needs is sound, only bigger (at drift
	 * 0: under drift each round multiplies the ages first can have) */
	o.drift = 0;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	n1 = last_distinct;
	/* workers fingerprint the canonical form too, and run the time check
	 * in their own buffers: the same search, passing and caught */
	same_search(&ldr_model, NULL, &o, 4, 0);
	ld_bug = 1;
	o.drift = 50;
	same_search(&ldr_model, NULL, &o, 4, 0);
	ld_bug = 0;
	ld_tf_skip = 1;
	same_search(&ldr_model, NULL, &o, 4, 0);
	ld_tf_skip = 0;
	o.drift = 0;
	o.time_bound = 2 * LD_VET;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	n2 = last_distinct;
	CHECK(n1 > 0 && n2 > n1);
	o.drift = 50;
	/* no margin, slow holder: caught; drift 200 beats the margin */
	o.time_bound = LD_VET;
	ld_bug = 1;
	CHECK(verdict_of(&ldr_model, &o, NULL, prop, sizeof(prop), NULL, 0, NULL)
		  == CMC_VIOLATION && !strcmp(prop, "OneHolder"));
	o.drift = 0;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	ld_bug = 0;
	o.drift = 200;
	CHECK(verdict_of(&ldr_model, &o, NULL, prop, sizeof(prop), NULL, 0, &depth)
		  == CMC_VIOLATION && !strcmp(prop, "OneHolder"));
	/* the time check: each field left undeclared, and a bound below the
	 * veteran age */
	o.drift = 50;
	for (ld_tf_skip = 1; ld_tf_skip <= 3; ld_tf_skip++)
		CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, why, sizeof(why), NULL)
			  == CMC_VIOLATION && strstr(why, "time check: node 0"));
	ld_tf_skip = 0;
	o.time_bound = LD_VET - 2;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, why, sizeof(why), NULL)
		  == CMC_VIOLATION && strstr(why, "time check: node 0"));
	/* a bound below the age at which the grant computes valid_until
	 * from t_send: arithmetic on a saturated time */
	o.time_bound = 50;
	o.drift = 0;
	ld_vet_off = 1;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, why, sizeof(why), NULL)
		  == CMC_VIOLATION && strstr(why, "time check: node 0 step on RECV"));
	ld_vet_off = 0;
	/* DFS and simulation canonicalize the same way: DFS reaches BFS's
	 * states, though its paths never end, so it cannot pass */
	o.time_bound = LD_VET;
	o.search = CMC_DFS;
	o.max_depth = 60;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_INCOMPLETE &&
		  last_distinct == n1);
	o.search = CMC_SIMULATE;
	o.walks = 50;
	CHECK(verdict_of(&ldr_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	ld_renew = 0;
}

/* time fields as tf_off[0..tf_n - 1]; a state of 32 bytes, 8 durable,
 * whose init writes tf_init to byte 8 */
static size_t tf_off[3];
static int	tf_n;
static uint64_t tf_init;

static int
tf_fields(const void *cfg, int node, size_t *off, int max)
{
	(void) cfg;
	(void) node;
	for (int i = 0; i < tf_n && i < max; i++)
		off[i] = tf_off[i];
	return tf_n;
}

static void
tf_initf(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, 32);
	memcpy((uint8_t *) sv + 8, &tf_init, 8);
}

static void
tf_apply(const void *cfg, int node, void *d, const uint8_t *body, uint16_t len)
{
	(void) cfg;
	(void) node;
	(void) d;
	(void) body;
	(void) len;
}

static const CmcModel cap_model;

/* stores a time far in the future on CLIENT */
static void
tf_far_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	uint64_t	far = UINT64_MAX >> 1;

	(void) cfg;
	(void) node;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
		memcpy((uint8_t *) sv + 8, &far, 8);
}

static void
tf_hist(const CmcWorld *pre, CmcAction a, const CmcWorld *post, uint8_t *ghost)
{
	(void) pre;
	(void) a;
	(void) post;
	(void) ghost;
}

/* time-bound refusals: bad declarations, bad option mixes, times that
 * leave the clock's range */
static void
test_time_bound_refusals(void)
{
	CmcCore		c = {.name = "tf", .state_size = 32, .durable_size = 8, .cfg_size = 4,
		.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
		.init = tf_initf, .step = tf_far_step, .apply_persist = tf_apply,
	.time_fields = tf_fields};
	CmcModel	m = {"tf", &c, nd_props, 1, 0};
	CmcOptions	o;
	char		err[256];
	CmcRun	   *r;
	struct
	{
		int			n;
		size_t		a,
					b;
		uint64_t	bound;
		bool		check;
		uint64_t	max_time;
		const char *msg;
	}			bad[] = {
		{1, 8, 0, 0, false, 0, "needs time_bound"},
		{-1, 8, 0, 9, false, 0, "outside 0..CMC_MAX_TIME_FIELDS"},
		{CMC_MAX_TIME_FIELDS + 1, 8, 0, 9, false, 0, "outside 0..CMC_MAX_TIME_FIELDS"},
		{1, 0, 0, 9, false, 0, "outside the volatile part"},
		{1, 25, 0, 9, false, 0, "outside the volatile part"},
		{1, 40, 0, 9, false, 0, "outside the volatile part"},
		{2, 8, 12, 9, false, 0, "overlap"},
		{0, 8, 0, 0, true, 0, "time_check needs time_bound"},
		{0, 8, 0, (uint64_t) 1 << 39, false, 0, "below 2^39"},
		{0, 8, 0, 9, false, 100, "max_time has no meaning"},
	};

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
	{
		cmc_options_default(&o);
		o.quiet = true;
		tf_n = bad[i].n;
		tf_off[0] = bad[i].a;
		tf_off[1] = bad[i].b;
		o.time_bound = bad[i].bound;
		o.time_check = bad[i].check;
		o.max_time = bad[i].max_time;
		r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
		CHECK(!r && strstr(err, bad[i].msg));
		if (r || !strstr(err, bad[i].msg))
			fprintf(stderr, "  row %zu: %s\n", i, err);
		cmc_run_free(r);
	}
	/* an initial time past the clock's range */
	cmc_options_default(&o);
	o.quiet = true;
	o.time_bound = 9;
	tf_n = 1;
	tf_off[0] = 8;
	tf_init = UINT64_MAX >> 1;
	r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
	CHECK(!r && strstr(err, "initial world: node 0: a time field past"));
	/* the same time stored by a step is an engine violation */
	tf_init = 0;
	CHECK(verdict_of(&m, &o, NULL, NULL, 0, err, sizeof(err), NULL) == CMC_VIOLATION &&
		  strstr(err, "node 0: a time field past"));
	tf_n = 0;
	/* a direct-mode run takes no time options */
	o.time_bound = 5;
	r = cmc_run_new(&cap_model, "", NULL, &o, err, sizeof(err));
	CHECK(!r && strstr(err, "need actor mode"));
	/* nor does a model with a history variable, whose bytes the engine
	 * cannot move with the clocks */
	m.history_size = 8;
	m.history = tf_hist;
	r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
	CHECK(!r && strstr(err, "history variable cannot be kept under time_bound"));
}

/* Deadline order on one node.  On CLIENT node 0 arms timer 1 for 30,
 * timer 2 for 10 and timer 3 untimed; each fire records its clock. */
typedef struct TO
{
	uint64_t	at[4];
} TO;

static int	to_dur_max;

static void
to_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	TO		   *s = sv;
	CmcOutput  *o;

	(void) cfg;
	(void) node;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
	{
		for (int t = 1; t <= 3; t++)
		{
			o = cmc_out_push(out, CMC_OUT_SET_TIMER);
			o->arg = (uint32_t) t;
			o->dur = t == 1 ? 30 : t == 2 ? 10 : 0;
		}
	}
	else if (ev->kind == CMC_EV_TIMER)
	{
		s->at[ev->arg] = ev->now + 1;
		/* a deadline past the clock's range, once the clock is not 0 */
		if (to_dur_max && ev->now)
			cmc_out_push(out, CMC_OUT_SET_TIMER)->dur = UINT64_MAX;
	}
}

static void
to_init(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, sizeof(TO));
}

/* timer 1 never fires before timer 2, nor before its own deadline */
static bool
to_order(const CmcWorld *w, CmcWhy *why)
{
	const TO   *s = cmc_node(w, 0);

	(void) why;
	return (!s->at[1] || (s->at[2] && s->at[1] == 31)) && (!s->at[2] || s->at[2] == 11);
}

static bool
to_late(const CmcWorld *w, CmcWhy *why)
{
	const TO   *s = cmc_node(w, 0);

	(void) why;
	return s->at[3] == 31;		/* the untimed timer fires after both */
}

/* the fair schedule fires every timer, the timed ones in deadline order */
static bool
to_all(const CmcWorld *w, CmcWhy *why)
{
	const TO   *s = cmc_node(w, 0);

	(void) why;
	return cmc_requests(w, 0) == 0 || (s->at[1] && s->at[2] && s->at[3]);
}

static const CmcProp to_props[] = {CMC_INVARIANT(Order, to_order),
	CMC_WITNESS_PROP(UntimedLast, to_late), CMC_EVENTUALLY_PROP(AllFired, to_all)};
static const CmcCore to_core = {
	.name = "torder", .state_size = sizeof(TO), .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
	.init = to_init, .step = to_step,
};
static const CmcModel to_model = {"torder", &to_core, to_props, 3, 0};

/* two nodes; node 0 arms the longest timer the engine takes */
static void
big_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) sv;
	out->n = 0;
	if (node == 0 && ev->kind == CMC_EV_CLIENT)
		cmc_out_push(out, CMC_OUT_SET_TIMER)->dur = UINT64_MAX >> 2;
}

static const CmcCore big_core = {
	.name = "big", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = two, .init = zero8, .step = big_step,
	.clients = ld_clients,
};
static const CmcModel big_model = {"big", &big_core, nd_props, 1, 0};

static const CmcModel cap_model;

static void
test_deadline_order(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	uint8_t    *w;
	char		why[512];
	CmcWorld	W;

	cmc_options_default(&o);
	o.det = true;
	o.drift = 100;				/* one node: no other clock to drift */
	CHECK(verdict_of(&to_model, &o, NULL, NULL, 0, NULL, 0, NULL) == CMC_PASS);
	r = mkc(&to_model, CMC_BFS, "", NULL, &o);
	w = calloc(1, cmc_world_size(r));
	cmc_world_init(r, w);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)) == 0);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_FIRE, 0, 1, 0, 0)) != 0);	/* not first */
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_FIRE, 0, 2, 1, 0)) != 0);	/* no node 0 drift */
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_FIRE, 0, 2, 0, 0)) == 0);
	W.run = r;
	W.bytes = w;
	CHECK(cmc_now(&W) == 10 && cmc_clock(&W, 0) == 10 && cmc_clock(&W, 1) == 0);
	free(w);
	cmc_run_free(r);
	/* a calibration core has no clocks: 0, and no drift or horizon */
	cmc_options_default(&o);
	r = mkc(&cap_model, CMC_BFS, "", NULL, &o);
	w = calloc(1, cmc_world_size(r) + 8);
	W.run = r;
	W.bytes = w;
	CHECK(cmc_now(&W) == 0 && cmc_clock(&W, 0) == 0);
	free(w);
	cmc_run_free(r);
	o.max_time = 10;
	CHECK(mkc(&cap_model, CMC_BFS, "", NULL, &o) == NULL);
	cmc_options_default(&o);
	to_dur_max = 1;
	CHECK(verdict_of(&to_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "deadline past the clock's range"));
	to_dur_max = 0;
	/* the longest deadline, with another clock fast at drift 500, would
	 * carry that clock past the range: an error, not a wrap */
	cmc_options_default(&o);
	o.drift = 500;
	CHECK(verdict_of(&big_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "clock past its range"));
}

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
		exit(verdict_of(&e8_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
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
	/* a search the engine does not know would run nothing and pass */
	for (int i = 0; i < 2; i++)
	{
		cmc_options_default(&o);
		o.search = (CmcSearch) (i ? 99 : 0);
		r = cmc_run_new(&toy_model, "", NULL, &o, err, sizeof(err));
		CHECK(r == NULL && strstr(err, "search") != NULL);
		cmc_run_free(r);
	}
	cmc_options_default(&o);
	o.search = CMC_REPLAY;
	r = cmc_run_new(&toy_model, "", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "replay_in") != NULL);
	cmc_run_free(r);
	/* a simulation of no walks looks at nothing, so it cannot pass */
	cmc_options_default(&o);
	o.search = CMC_SIMULATE;
	o.walks = 0;
	r = cmc_run_new(&toy_model, "", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "walk") != NULL);
	cmc_run_free(r);
}

/* ---- soundness plan, phase 2 (docs/plans/soundness-remediation.md) ---- */

/* Every client request has a reply: false while one is outstanding. */
static bool
replied_all(const CmcWorld *w, CmcWhy *why)
{
	if (cmc_nreplies(w) != cmc_requests(w, 0))
	{
		cmc_why(why, "%d request(s), %d reply(ies)", cmc_requests(w, 0), cmc_nreplies(w));
		return false;
	}
	return true;
}

static const CmcProp replied_qui[] = {CMC_QUIESCENT_PROP(Replied, replied_all)};
static const CmcProp replied_ev[] = {CMC_EVENTUALLY_PROP(EventuallyReplied, replied_all)};

/* 2.1: CLIENT makes the node send the request to itself; RECV replies. */
static void
self_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) sv;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
		cmc_out_push(out, CMC_OUT_SEND)->dst = (uint8_t) node;
	else if (ev->kind == CMC_EV_RECV)
		cmc_out_push(out, CMC_OUT_REPLY)->arg = 1;
}

static const CmcCore self_core = {
	.name = "self", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = self_step,
};
static const CmcModel self_model = {"self", &self_core, replied_qui, 1, 0};

/* 2.1: what a node sent itself never left the process, so a crash loses
 * it in either network (the bag keeps only the messages between nodes,
 * test_b5): the restarted process must not receive it. */
static void
test_p21_crash_clears_self_channel(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	uint8_t    *w;
	CmcAction	a[CMC_MAX_ACTIONS];
	char		prop[64];

	cmc_options_default(&o);
	o.crash_budget = 1;
	for (int net = 0; net < 2; net++)
	{
		o.net = net ? CMC_NET_BAG : CMC_NET_FIFO;
		CHECK(verdict_of(&self_model, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) ==
			  CMC_VIOLATION);
		CHECK(strcmp(prop, "Replied") == 0);
	}
	o.net = CMC_NET_FIFO;
	r = mkc(&self_model, CMC_BFS, "", NULL, &o);
	w = malloc(cmc_world_size(r));
	cmc_world_init(r, w);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)) == 0);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CRASH, 0, 0, 0, 0)) == 0);
	CHECK(cmc_enabled(r, w, a, CMC_MAX_ACTIONS) == 0);
	free(w);
	cmc_run_free(r);
}

/* 2.2: a core that never answers.  CLIENT leads to a quiescent state with
 * the request outstanding, the last state of a walk of depth 2. */
static void
mute_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) node;
	(void) sv;
	(void) ev;
	out->n = 0;
}

static const CmcCore mute_core = {
	.name = "mute", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = mute_step,
};

/* 2.2: a walk checks its last state too, for quiescence and eventuality */
static void
test_p22_simulate_checks_last_state(void)
{
	CmcModel	mq = {"mute", &mute_core, replied_qui, 1, 0},
				me = {"mute", &mute_core, replied_ev, 1, 0};
	CmcOptions	o;
	char		prop[64];

	cmc_options_default(&o);
	o.search = CMC_SIMULATE;
	o.walks = 1;
	o.max_depth = 2;
	CHECK(verdict_of(&mq, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	CHECK(strcmp(prop, "Replied") == 0);
	CHECK(verdict_of(&me, &o, NULL, prop, sizeof(prop), NULL, 0, NULL) == CMC_VIOLATION);
	CHECK(strcmp(prop, "EventuallyReplied") == 0);
}

/* a BFS that stops at its witnesses has not searched the rest, so it
 * cannot pass: toy-bug1's Mutex violation lies past ReachCS */
static void
test_stop_on_witness(void)
{
	const char *props[] = {"Mutex", "ReachCS", NULL};
	CmcOptions	o;
	char		why[512];

	cmc_options_default(&o);
	o.stop_on_witness = true;
	CHECK(verdict_of(&toy_bug1_model, &o, props, NULL, 0, why, sizeof(why), NULL) ==
		  CMC_INCOMPLETE && strstr(why, "witness") != NULL);
	/* a witness of the initial state stops the search before it starts */
	{
		static const CmcProp p0[] = {CMC_WITNESS_PROP(Always, always_true)};
		CmcModel	m = {"w0", &mute_core, p0, 1, 0};
		int			depth = -1;

		CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), &depth) ==
			  CMC_INCOMPLETE && depth == 1);
	}
}

/* 2.3: more properties of one kind than the engine holds are refused when
 * the run is set up, whether listed in the model or named twice.  Run in a
 * child: on an engine without the check this writes past the arrays. */
static void
test_p23_property_cap(void)
{
	pid_t		pid;
	int			st = 0;

	fflush(NULL);
	pid = fork();
	if (pid == 0)
	{
		CmcProp		ps[65];
		CmcModel	m = {"many", &mute_core, ps, 64, 0};
		CmcOptions	o;
		char		err[256];
		const char *names[66];
		CmcRun	   *r;
		int			ok = 1;

		for (int i = 0; i < 65; i++)
			ps[i] = (CmcProp) CMC_INVARIANT(True, always_true);
		cmc_options_default(&o);
		r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
		ok &= r != NULL;
		cmc_run_free(r);
		m.nprops = 65;
		r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
		ok &= r == NULL && strstr(err, "properties") != NULL;
		cmc_run_free(r);
		m.nprops = 1;
		for (int i = 0; i < 65; i++)
			names[i] = "True";
		names[65] = NULL;
		r = cmc_run_new(&m, "", names, &o, err, sizeof(err));
		ok &= r == NULL && strstr(err, "properties") != NULL;
		cmc_run_free(r);
		/* a property of no known kind would never be checked */
		ps[0].kind = (CmcPropKind) 0;
		r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
		ok &= r == NULL && strstr(err, "kind") != NULL;
		cmc_run_free(r);
		exit(ok ? 0 : 1);
	}
	CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* 2.4: a direct core with one action from its initial state, after which
 * its action list overflows (cmc_enabled() returns -1) */
static int
cap_actions(const void *cfg, const void *sv, CmcAction *a, int max)
{
	(void) cfg;
	if (((const uint8_t *) sv)[0] != 0 || max < 1)
		return -1;
	a[0] = 1;
	return 1;
}

static void
cap_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) node;
	(void) ev;
	out->n = 0;
	((uint8_t *) sv)[0] = 1;
}

static const CmcCore cap_core = {
	.name = "cap", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = cap_step,
	.direct_actions = cap_actions,
};
static const CmcModel cap_model = {"cap", &cap_core, nd_props, 1, 0};

/* 2.4: the replay of an engine error at the last state gives BFS's verdict */
static void
test_p24_replay_enabled_error(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	CmcResult	res;
	char		path[512],
				why[sizeof(res.why)];
	int			len;

	snprintf(path, sizeof(path), "%s/p24.replay",
			 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
	cmc_options_default(&o);
	r = mkc(&cap_model, CMC_BFS, "", NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "cmc:engine"));
	CHECK(res.trace_len == 2);
	CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
	snprintf(why, sizeof(why), "%s", res.why);
	len = res.trace_len;
	cmc_result_free(&res);
	cmc_run_free(r);
	o.replay_in = path;
	r = mkc(&cap_model, CMC_REPLAY, "", NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "cmc:engine"));
	CHECK(strcmp(res.why, why) == 0 && res.trace_len == len);
	cmc_result_free(&res);
	cmc_run_free(r);
}

/* 2.5: CLIENT arms timer 0 and starts a message the node sends itself
 * forever; the timer ends the wait.  The goal holds once it fires. */
static void
pingpong_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	uint8_t    *s = sv;

	(void) cfg;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
	{
		s[0] = 1;
		cmc_out_push(out, CMC_OUT_SEND)->dst = (uint8_t) node;
		cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = 0;
	}
	else if (ev->kind == CMC_EV_RECV)
		cmc_out_push(out, CMC_OUT_SEND)->dst = (uint8_t) node;
	else if (ev->kind == CMC_EV_TIMER)
		s[0] = 2;
}

static bool
not_waiting(const CmcWorld *w, CmcWhy *why)
{
	if (((const uint8_t *) cmc_node(w, 0))[0] == 1)
	{
		cmc_why(why, "timer 0 never fired");
		return false;
	}
	return true;
}

static const CmcProp pingpong_props[] = {CMC_EVENTUALLY_PROP(TimerDone, not_waiting)};
static const CmcCore pingpong_core = {
	.name = "pingpong", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = pingpong_step,
};
static const CmcModel pingpong_model = {"pingpong", &pingpong_core, pingpong_props, 1, 0};

/* 2.5: node 0's CLIENT starts the same endless self-message and sends a
 * request to node 1, which replies on RECV */
static void
relay_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) sv;
	out->n = 0;
	if (node == 1 && ev->kind == CMC_EV_RECV)
		cmc_out_push(out, CMC_OUT_REPLY)->arg = 1;
	else if (node == 0 && (ev->kind == CMC_EV_CLIENT || ev->kind == CMC_EV_RECV))
		cmc_out_push(out, CMC_OUT_SEND)->dst = 0;
	if (node == 0 && ev->kind == CMC_EV_CLIENT)
		cmc_out_push(out, CMC_OUT_SEND)->dst = 1;
}

static int
first_only(const void *c, int n)
{
	(void) c;
	return n == 0;
}

static const CmcCore relay_core = {
	.name = "relay", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = two, .init = zero8, .step = relay_step,
	.clients = first_only,
};
static const CmcModel relay_model = {"relay", &relay_core, replied_ev, 1, 0};

/* 2.5: endless deliveries cannot postpone an armed timer, or a message on
 * another channel, forever */
static void
test_p25_fair_schedule_fires_timers(void)
{
	CmcOptions	o;
	char		why[512];

	cmc_options_default(&o);
	o.det = true;
	CHECK(verdict_of(&pingpong_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_PASS);
	for (int net = 0; net < 2; net++)
	{
		cmc_options_default(&o);
		o.net = net ? CMC_NET_BAG : CMC_NET_FIFO;
		CHECK(verdict_of(&relay_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_PASS);
	}
}

/* ---- the engine's rules on what a core emits -------------------------
 * One configurable core: on CLIENT it emits the outputs bad_kind picks,
 * each of which breaks one engine rule.  Each must end the check with an
 * engine violation that names the rule, never pass or crash. */

static int	bad_kind;

static void
bad_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	CmcOutput  *o;

	(void) cfg;
	(void) node;
	(void) sv;
	out->n = 0;
	if (ev->kind != CMC_EV_CLIENT)
		return;
	switch (bad_kind)
	{
		case 0:					/* a body longer than a message holds */
			cmc_out_push(out, CMC_OUT_SEND)->len = CMC_MSG_MAX + 1;
			break;
		case 1:					/* a send to a node that does not exist */
			cmc_out_push(out, CMC_OUT_SEND)->dst = 5;
			break;
		case 2:					/* a timer id past the last */
			cmc_out_push(out, CMC_OUT_CANCEL_TIMER)->arg = 256;
			break;
		case 3:					/* a waiter id past the last */
			cmc_out_push(out, CMC_OUT_WAKE)->arg = 256;
			break;
		case 4:					/* more persists than --persist-cap */
			for (int i = 0; i < 3; i++)
				cmc_out_push(out, CMC_OUT_PERSIST)->len = 1;
			break;
		case 5:					/* more replies than --reply-cap */
			for (int i = 0; i < 9; i++)
				cmc_out_push(out, CMC_OUT_REPLY);
			break;
		case 6:					/* more messages than --net-cap */
			for (int i = 0; i < 9; i++)
			{
				o = cmc_out_push(out, CMC_OUT_SEND);
				o->len = 1;
				o->body[0] = (uint8_t) i;
			}
			break;
		case 7:					/* an output kind there is none of */
			cmc_out_push(out, 99);
			break;
		case 8:					/* an output count out of range */
			out->n = -1;
			break;
		case 9:					/* past the array, with no overflow counted */
			out->n = CMC_OUT_MAX + 1;
			break;
		case 10:				/* a duration on a wake */
			cmc_out_push(out, CMC_OUT_WAKE)->dur = 5;
			break;
	}
}

static const CmcCore bad_core = {
	.name = "bad", .state_size = 8, .durable_size = 1, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = nd_nnodes,
	.init = zero8, .step = bad_step, .apply_persist = dur_apply,
};
static const CmcModel bad_model = {"bad", &bad_core, nd_props, 1, 0};

static void
test_output_rules(void)
{
	static const char *const want[] = {"output body too long", "SEND to bad node 5",
		"bad timer id 256", "bad waiter id 256", "persist_cap 2 exceeded",
		"reply_cap 8 exceeded", "net_cap 8 exceeded", "unknown output kind 99",
	"core emitted -1 outputs", "core emitted 17 outputs", "a duration on an output"};
	char		path[512];

	snprintf(path, sizeof(path), "%s/bad.replay",
			 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
	for (bad_kind = 0; bad_kind < 11; bad_kind++)
		for (int s = 0; s < 4; s++)
		{
			CmcOptions	o;
			CmcRun	   *r;
			CmcResult	res;

			/* BFS, DFS and simulation each report it, and BFS's trace
			 * replays to it */
			cmc_options_default(&o);
			o.net = CMC_NET_BAG;
			o.walks = 1;
			o.replay_in = path;
			r = mkc(&bad_model, s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : s == 2 ? CMC_SIMULATE :
					CMC_REPLAY, "", NULL, &o);
			cmc_check(r, &res);
			CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "cmc:engine"));
			CHECK(strstr(res.why, want[bad_kind]) != NULL && res.trace_len == 2);
			if (s == 0)
				CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
			cmc_result_free(&res);
			cmc_run_free(r);
		}
	bad_kind = 0;
}

/* ---- cmc_apply refuses an action that is not enabled ----------------- */

/* a node that sets timer 1 and waiter 2 on CLIENT, persists and sends to
 * itself, and replies on WAKE: it reaches every action kind */
static void
all_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	CmcOutput  *o;

	(void) cfg;
	(void) sv;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
	{
		cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = 1;
		cmc_out_push(out, CMC_OUT_WAKE)->arg = 2;
		o = cmc_out_push(out, CMC_OUT_PERSIST);
		o->len = 1;
		o->body[0] = 7;
		o = cmc_out_push(out, CMC_OUT_SEND);
		o->dst = (uint8_t) (1 - node);
		o->len = 1;
	}
	else if (ev->kind == CMC_EV_WAKE)
		cmc_out_push(out, CMC_OUT_REPLY)->arg = ev->arg;
}

static bool
woke(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return cmc_nreplies(w) > 0;
}

static void
all_describe(const void *cfg, int node, const void *sv, FILE *f)
{
	(void) cfg;
	fprintf(f, "n%d=%u", node, ((const uint8_t *) sv)[0]);
}

static const CmcProp all_props[] = {CMC_WITNESS_PROP(Woke, woke)};
static const CmcCore all_core = {
	.name = "all", .state_size = 8, .durable_size = 1, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = two,
	.init = zero8, .step = all_step, .apply_persist = dur_apply, .describe = all_describe,
};
static const CmcModel all_model = {"all", &all_core, all_props, 1, 0};

static void
test_apply_refuses(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	CmcResult	res;
	uint8_t    *w;
	CmcAction	a[CMC_MAX_ACTIONS];
	int			na;
	char	   *text = NULL;
	size_t		tlen = 0;
	FILE	   *f;
	static const CmcAction never[] = {
		CMC_ACT(CMC_A_DELIVER, 0, 1, 0, 9),		/* no message 9 */
		CMC_ACT(CMC_A_DUP, 0, 1, 0, 0),			/* no dup budget */
		CMC_ACT(CMC_A_DROP, 0, 1, 0, 0),		/* no drop budget */
		CMC_ACT(CMC_A_FIRE, 9, 1, 0, 0),		/* no node 9 */
		CMC_ACT(CMC_A_FIRE, 0, 3, 0, 0),		/* timer 3 not armed */
		CMC_ACT(CMC_A_FIRE, 0, 20, 0, 0),		/* no timer 20 */
		CMC_ACT(CMC_A_FLUSH, 1, 0, 0, 0),		/* nothing pending at node 1 */
		CMC_ACT(CMC_A_WAKE, 0, 3, 0, 0),		/* waiter 3 not set */
		CMC_ACT(CMC_A_WAKE, 0, 9, 0, 0),		/* no waiter 9 */
		CMC_ACT(CMC_A_WAKE, 9, 2, 0, 0),		/* no node 9 */
		CMC_ACT(CMC_A_RESET, 0, 1, 0, 0),		/* no reset pending */
		CMC_ACT(CMC_A_RESET, 0, 9, 0, 0),		/* no peer 9 */
		CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0),		/* client budget spent */
		CMC_ACT(CMC_A_CLIENT, 9, 0, 0, 0),		/* no node 9 */
		CMC_ACT(CMC_A_CRASH, 0, 0, 0, 0),		/* no crash budget */
		CMC_ACT(0x7f, 0, 0, 0, 0),				/* no such kind */
	};

	cmc_options_default(&o);
	o.net = CMC_NET_BAG;
	r = mk(&all_model, CMC_BFS, NULL, &o);
	w = malloc(cmc_world_size(r));
	cmc_world_init(r, w);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)) == 0);
	na = cmc_enabled(r, w, a, CMC_MAX_ACTIONS);
	/* DELIVER 0->1, FIRE 0/1, FLUSH 0, WAKE 0/2, CLIENT 1 */
	CHECK(na == 5);
	CHECK(cmc_enabled(r, w, a, 2) == -1);	/* the caller's array is too small */
	/* each is refused, and leaves the world as it was */
	{
		uint8_t    *w0 = malloc(cmc_world_size(r));

		memcpy(w0, w, cmc_world_size(r));
		for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++)
		{
			CHECK(cmc_apply(r, w, never[i]) != 0);
			CHECK(memcmp(w, w0, cmc_world_size(r)) == 0);
			memcpy(w, w0, cmc_world_size(r));
		}
		free(w0);
	}
	/* each action kind's description, in a trace */
	f = open_memstream(&text, &tlen);
	for (int i = 0; i < na; i++)
		cmc_describe_action(r, a[i], f);
	cmc_describe_action(r, CMC_ACT(CMC_A_DUP, 1, 0, 0, 0), f);
	cmc_describe_action(r, CMC_ACT(CMC_A_DROP, 1, 0, 0, 0), f);
	cmc_describe_action(r, CMC_ACT(CMC_A_CRASH, 0, 2, 0, 0), f);
	cmc_describe_action(r, CMC_ACT(CMC_A_RESET, 0, 1, 0, 0), f);
	cmc_describe_action(r, CMC_ACT(0x7f, 0, 0, 0, 0), f);
	cmc_print_trace(r, (CmcAction[]) {CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0),
		CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)}, 2, f);
	fclose(f);
	CHECK(strstr(text, "WAKE node 0 waiter 2") && strstr(text, "FLUSH node 0") &&
		  strstr(text, "DUP 0->1 #0") && strstr(text, "DROP 0->1 #0") &&
		  !strstr(text, "DROP 0->1 #0 (resets") &&
		  strstr(text, "CRASH node 0 (2 pending persists reached disk)") &&
		  strstr(text, "CONN_RESET node 0 peer 1") && strstr(text, "action 7f00000000000000") &&
		  strstr(text, "node 0: n0=0") && strstr(text, "(apply failed: action"));
	free(text);
	free(w);
	cmc_run_free(r);

	/* the wake and its reply are reached, in every search */
	for (int s = 0; s < 3; s++)
	{
		cmc_options_default(&o);
		o.net = CMC_NET_BAG;
		o.walks = 50;
		r = mk(&all_model, s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : CMC_SIMULATE, NULL, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_PASS && res.nwitness == 1 && res.witness_depth[0] >= 3);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

/* ---- the engine's own checks of a core ------------------------------
 * Cores that break a rule the engine enforces, each with the verdict the
 * rule gives.  A rule that let any of them through would let a core's
 * bug read as a pass. */

static int	gate_calls;

/* a step whose outputs differ on a repeat: first one, then two sends */
static void
gate_out_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) node;
	(void) sv;
	out->n = 0;
	if (ev->kind != CMC_EV_CLIENT)
		return;
	cmc_out_push(out, CMC_OUT_SEND)->dst = 0;
	if (gate_calls++ & 1)
		cmc_out_push(out, CMC_OUT_SEND)->dst = 0;
}

/* a step whose one output differs on a repeat in field gate_field: its
 * kind, destination, length, argument, body or duration */
static int	gate_field;

static void
gate_body_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	CmcOutput  *o;
	int			odd = gate_calls++ & 1;

	(void) cfg;
	(void) node;
	(void) sv;
	out->n = 0;
	if (ev->kind != CMC_EV_CLIENT)
		return;
	o = cmc_out_push(out, gate_field == 0 && odd ? CMC_OUT_REPLY : CMC_OUT_SEND);
	o->len = 1;
	if (gate_field == 1)
		o->dst = (uint8_t) odd;
	else if (gate_field == 2)
		o->len = (uint16_t) (1 + odd);
	else if (gate_field == 3)
		o->arg = (uint32_t) odd;
	else if (gate_field == 4)
		o->body[0] = (uint8_t) odd;
	else if (gate_field == 5)
		o->dur = (uint64_t) odd;
}

/* an apply_persist that changes volatile byte 4 */
static void
vol_apply(const void *cfg, int node, void *d, const uint8_t *body, uint16_t len)
{
	(void) cfg;
	(void) node;
	(void) body;
	(void) len;
	((uint8_t *) d)[4] = 1;
}

/* an apply_persist that writes a different durable byte each time */
static void
nondet_apply(const void *cfg, int node, void *d, const uint8_t *body, uint16_t len)
{
	(void) cfg;
	(void) node;
	(void) body;
	(void) len;
	((uint8_t *) d)[0] = (uint8_t) (gate_calls++ & 1 ? 2 : 1);
}

/* CLIENT persists one byte */
static void
persist1_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) node;
	(void) sv;
	out->n = 0;
	if (ev->kind == CMC_EV_CLIENT)
		cmc_out_push(out, CMC_OUT_PERSIST)->len = 1;
}

/* a direct-mode core whose action list differs on a repeat */
static int
nondet_actions(const void *cfg, const void *sv, CmcAction *a, int max)
{
	(void) cfg;
	(void) sv;
	if (max < 2)
		return -1;
	a[0] = 1;
	a[1] = (CmcAction) (gate_calls++ & 1) + 2;
	return 2;
}

static void
direct_describe(const void *cfg, CmcAction a, FILE *f)
{
	(void) cfg;
	fprintf(f, "step %u", (unsigned) a);
}

static bool
never_ok(const CmcWorld *w, CmcWhy *why)
{
	(void) w;
	(void) why;
	return false;
}

static void
test_core_rules(void)
{
	static const CmcProp qui[] = {CMC_QUIESCENT_PROP(Q, always_true)};
	static const CmcProp ev[] = {CMC_EVENTUALLY_PROP(E, always_true)};
	static const CmcProp wit[] = {CMC_WITNESS_PROP(W, never_ok)};
	CmcCore		c;
	CmcModel	m;
	CmcOptions	o;
	char		why[512],
				err[256];
	CmcRun	   *r;

	/* the determinism gate sees outputs that differ, in count or body */
	c = mute_core;
	m = (CmcModel) {"gate", &c, nd_props, 1, 0};
	cmc_options_default(&o);
	o.det = true;
	o.net = CMC_NET_BAG;
	c.step = gate_out_step;
	CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "different outputs on a repeat"));
	c.step = gate_body_step;
	c.nnodes = two;
	for (gate_field = 0; gate_field < 6; gate_field++)
		CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
			  strstr(why, "different outputs on a repeat"));
	/* under the gate, a body too long is refused without being compared
	 * past the end of its buffer */
	c.step = bad_step;
	c.nnodes = nd_nnodes;
	bad_kind = 0;
	CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "different outputs on a repeat"));
	/* apply_persist may write only the durable bytes, and the same each time */
	c = mute_core;
	c.durable_size = 1;
	c.step = persist1_step;
	c.apply_persist = vol_apply;
	CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "apply_persist changed volatile state"));
	c.apply_persist = nondet_apply;
	CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "apply_persist differs on a repeat"));
	/* a crash that keeps the pending record applies it through the core */
	o.det = false;
	o.crash_budget = 1;
	c.apply_persist = vol_apply;
	CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "apply_persist changed volatile state"));
	/* a direct core's action list must be a function of the state */
	c = cap_core;
	c.direct_actions = nondet_actions;
	c.describe_action = direct_describe;
	m = (CmcModel) {"dgate", &c, nd_props, 1, 0};
	cmc_options_default(&o);
	o.det = true;
	for (int s = 0; s < 3; s++)
	{
		o.search = s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : CMC_SIMULATE;
		CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
			  strstr(why, "action list differs on a repeat"));
	}
	/* a direct core has no quiescence of timers, and no fair schedule */
	c = cap_core;
	m = (CmcModel) {"dq", &c, qui, 1, 0};
	cmc_options_default(&o);
	CHECK(verdict_of(&m, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "too many enabled actions"));
	m.props = ev;
	r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "actor mode"));
	cmc_run_free(r);
	/* a direct core's trace names its actions through describe_action */
	{
		char	   *text = NULL;
		size_t		tlen = 0;
		FILE	   *f = open_memstream(&text, &tlen);

		c.describe_action = direct_describe;
		m.props = wit;
		r = mk(&m, CMC_BFS, NULL, NULL);
		cmc_describe_action(r, 5, f);
		c.describe_action = NULL;
		cmc_describe_action(r, 6, f);
		fclose(f);
		CHECK(strstr(text, "step 5") && strstr(text, "action 0000000000000006"));
		free(text);
		cmc_run_free(r);
	}
	/* a core the engine cannot hold, or with a durable part it cannot write */
	c = mute_core;
	m = (CmcModel) {"setup", &c, nd_props, 1, 0};
	cmc_options_default(&o);
	c.cfg_size = 300;
	r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "cfg too large"));
	c = mute_core;
	c.nnodes = four;
	c.durable_size = 9;
	r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "durable_size exceeds state_size"));
	c.durable_size = 1;
	r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "needs apply_persist"));
	c.durable_size = 0;
	/* a settings string of key=value items, each one the core accepts */
	r = cmc_run_new(&m, "clients", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "want key=value"));
	r = cmc_run_new(&m, "clients=3", NULL, &o, err, sizeof(err));
	CHECK(r == NULL && strstr(err, "bad cfg clients=3 for core mute"));
	r = cmc_run_new(&m, NULL, NULL, &o, err, sizeof(err));
	CHECK(r != NULL);
	cmc_run_free(r);
	/* every bound out of range, one at a time */
	for (int i = 0; i < 13; i++)
	{
		cmc_options_default(&o);
		switch (i)
		{
			case 0: o.max_depth = 0; break;
			case 1: o.live_steps = 0; break;
			case 2: o.time_limit = -1; break;
			case 3: o.progress_s = -1; break;
			case 4: o.net = (CmcNet) 7; break;
			case 5: o.net_cap = 256; break;
			case 6: o.reply_cap = 256; break;
			case 7: o.persist_cap = 256; break;
			case 8: o.client_budget = 256; break;
			case 9: o.drop_budget = 256; break;
			case 10: o.dup_budget = 256; o.net = CMC_NET_BAG; break;
			case 11: o.crash_budget = 256; break;
			case 12: o.out = NULL; break;
		}
		r = cmc_run_new(&m, "", NULL, &o, err, sizeof(err));
		CHECK(i == 12 ? r != NULL : r == NULL);
		cmc_run_free(r);
	}
	/* a node count the engine cannot hold */
	c.nnodes = nd_nnodes;
	{
		static const CmcCore none = {
			.name = "none", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
			.cfg_set = nd_cfg_set, .nnodes = zero_nodes, .init = zero8, .step = mute_step,
		};
		CmcModel	mn = {"none", &none, nd_props, 1, 0};

		r = cmc_run_new(&mn, "", NULL, &o, err, sizeof(err));
		CHECK(r == NULL && strstr(err, "node count 0 out of range"));
	}
}

/* ---- every search on one model --------------------------------------
 * The toy and its bugs through BFS, DFS, simulation and replay, with
 * each kind of property, so the searches agree on what they report. */

static void
test_searches_agree(void)
{
	static const char *const inv[] = {"Mutex", NULL};
	static const char *const act[] = {"PhaseMonotone", NULL};
	static const char *const qui[] = {"AllReplied", NULL};
	CmcOptions	o;
	char		prop[64],
				why[512];

	for (int s = 0; s < 3; s++)
	{
		cmc_options_default(&o);
		o.search = s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : CMC_SIMULATE;
		o.max_depth = 40;
		o.walks = 2000;
		/* an action property, a quiescence property, an invariant */
		CHECK(verdict_of(&toy_bug3_model, &o, act, prop, sizeof(prop), NULL, 0, NULL) ==
			  CMC_VIOLATION && !strcmp(prop, "PhaseMonotone"));
		CHECK(verdict_of(&toy_bug2_model, &o, qui, prop, sizeof(prop), NULL, 0, NULL) ==
			  CMC_VIOLATION && !strcmp(prop, "AllReplied"));
		CHECK(verdict_of(&toy_bug1_model, &o, inv, prop, sizeof(prop), NULL, 0, NULL) ==
			  CMC_VIOLATION && !strcmp(prop, "Mutex"));
		/* the correct toy holds every property, in a FIFO and in a bag */
		CHECK(verdict_of(&toy_model, &o, NULL, NULL, 0, why, sizeof(why), NULL) == CMC_PASS);
		o.net = CMC_NET_BAG;
		o.drop_budget = 1;
		o.dup_budget = 1;
		CHECK(verdict_of(&toy_model, &o, inv, NULL, 0, why, sizeof(why), NULL) == CMC_PASS);
	}
	/* DFS: a depth bound that cuts the graph is incomplete, and a state
	 * reached again more shallowly is searched again below */
	cmc_options_default(&o);
	o.search = CMC_DFS;
	o.max_depth = 5;
	CHECK(verdict_of(&toy_model, &o, inv, NULL, 0, why, sizeof(why), NULL) == CMC_INCOMPLETE &&
		  strstr(why, "depth bound 5"));
	o.max_depth = 40;
	o.max_states = 10;
	CHECK(verdict_of(&toy_model, &o, inv, NULL, 0, why, sizeof(why), NULL) == CMC_INCOMPLETE &&
		  strstr(why, "max_states"));
	/* a bug at depth 7 is found by DFS under any bound that reaches it */
	o.max_states = 0;
	o.max_depth = 7;
	CHECK(verdict_of(&toy_bug1_model, &o, inv, prop, sizeof(prop), NULL, 0, NULL) ==
		  CMC_VIOLATION);
	/* the replay of each search's trace reports what the search did */
	for (int s = 0; s < 3; s++)
	{
		char		path[512];
		CmcRun	   *r;
		CmcResult	res;

		snprintf(path, sizeof(path), "%s/agree.replay",
				 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
		cmc_options_default(&o);
		o.max_depth = 40;
		o.walks = 2000;
		r = mkc(&toy_bug3_model, s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : CMC_SIMULATE, "", act, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION && res.trace_len > 1);
		CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
		cmc_result_free(&res);
		cmc_run_free(r);
		o.replay_in = path;
		r = mkc(&toy_bug3_model, CMC_REPLAY, "", act, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "PhaseMonotone"));
		cmc_result_free(&res);
		cmc_run_free(r);
		/* and of the quiescent state of a lost wake */
		r = mkc(&toy_bug2_model, s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : CMC_SIMULATE, "", qui, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION);
		CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
		cmc_result_free(&res);
		cmc_run_free(r);
		r = mkc(&toy_bug2_model, CMC_REPLAY, "", qui, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION && res.prop && !strcmp(res.prop, "AllReplied"));
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

/* a replay file that does not fit the run is refused, never run */
static void
test_replay_refusals(void)
{
	static const char *const inv[] = {"Mutex", NULL};
	static const char *const bad[] = {
		"not a replay\n",
		"# cmc replay v2\nmodel toy_model\nenv client=1 drop=0 dup=0 crash=0 net=8 persist=2"
		" reply=8 mode=fifo live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\n",
		"# cmc replay v2\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8 persist=2 reply=8"
		" mode=fifo live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=2 reply=8 mode=tree live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg clients=9\nenv client=1 drop=0 dup=0 crash=0"
		" net=8 persist=2 reply=8 mode=fifo live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=1 crash=0 net=8"
		" persist=2 reply=8 mode=fifo live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=7"
		" persist=2 reply=8 mode=fifo live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=3 reply=8 mode=fifo live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=2 reply=7 mode=fifo live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=2 reply=8 mode=bag live=1000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=2 reply=8 mode=fifo live=999\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=2 reply=8 mode=fifo live=1000\nact 0701000000000000\nact 0701000000000000\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=2 reply=8 mode=fifo live=1000\ncaps timer=15 wake=8\n",
		"# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0 net=8"
		" persist=2 reply=8 mode=fifo live=1000\ncaps timer=16\n",
	};
	static const char *const want[] = {"not a cmc replay", "lacks its model", "lacks its model",
		"lacks its model", "bad env line", "recorded with cfg", "different environment",
		"different environment", "different environment", "different environment",
		"different environment", "different environment", "replay step 2",
	"different environment", "bad caps line"};
	char		path[512];

	snprintf(path, sizeof(path), "%s/refuse.replay",
			 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
	{
		FILE	   *f = fopen(path, "w");
		CmcOptions	o;
		CmcRun	   *r;
		CmcResult	res;
		/* live steps differ only for a run with an eventually property */
		const char *const ev[] = {"Mutex", "EventuallyAllReplied", NULL};

		fputs(bad[i], f);
		fclose(f);
		cmc_options_default(&o);
		o.replay_in = path;
		r = mkc(&toy_model, CMC_REPLAY, "", i == 11 ? ev : inv, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_ERROR && strstr(res.why, want[i]) != NULL);
		if (res.verdict != CMC_ERROR || !strstr(res.why, want[i]))
			fprintf(stderr, "  replay %zu: %s\n", i, res.why);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

/* ---- the fair schedule of an eventually property --------------------
 * fair_core's CLIENT event at node 0 starts what fair_mode asks for; the
 * goal is that every request has its reply.  Each mode drives check_ev()
 * down one way it can end. */

static int	fair_mode;

static void
fair_step_fn(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	uint8_t    *s = (uint8_t *) sv + 4;	/* past the durable byte */
	CmcOutput  *o;

	(void) cfg;
	out->n = 0;
	switch (fair_mode)
	{
		case 0:					/* a wake, then a persist, then the reply */
			if (ev->kind == CMC_EV_CLIENT)
				cmc_out_push(out, CMC_OUT_WAKE)->arg = 3;
			else if (ev->kind == CMC_EV_WAKE)
				cmc_out_push(out, CMC_OUT_PERSIST)->len = 1;
			else if (ev->kind == CMC_EV_FLUSHED)
				cmc_out_push(out, CMC_OUT_REPLY);
			break;
		case 1:					/* a timer that re-arms itself and never replies */
			if (ev->kind == CMC_EV_CLIENT || ev->kind == CMC_EV_TIMER)
			{
				s[0] = (uint8_t) (s[0] + 1) & 3;
				cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = 2;
			}
			break;
		case 2:					/* two nodes forever sending each other one byte */
			if (ev->kind == CMC_EV_CLIENT || ev->kind == CMC_EV_RECV)
			{
				o = cmc_out_push(out, CMC_OUT_SEND);
				o->dst = (uint8_t) (1 - node);
				o->len = 1;
				o->body[0] = (uint8_t) (s[0]++ & 7);
			}
			break;
		case 3:					/* the reply only after a long count of wakes */
			if (ev->kind == CMC_EV_CLIENT || (ev->kind == CMC_EV_WAKE && s[0] < 20))
			{
				s[0]++;
				cmc_out_push(out, CMC_OUT_WAKE)->arg = 0;
			}
			else if (ev->kind == CMC_EV_WAKE)
				cmc_out_push(out, CMC_OUT_REPLY);
			break;
		case 4:					/* a wake whose step breaks the durable rule */
			if (ev->kind == CMC_EV_CLIENT)
				cmc_out_push(out, CMC_OUT_WAKE)->arg = 0;
			else if (ev->kind == CMC_EV_WAKE)
				((uint8_t *) sv)[0] = 1;
			break;
		case 5:					/* a timer whose fire breaks the durable rule */
			if (ev->kind == CMC_EV_CLIENT)
				cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = 0;
			else if (ev->kind == CMC_EV_TIMER)
				((uint8_t *) sv)[0] = 1;
			break;
	}
}

static const CmcProp fair_props[] = {CMC_EVENTUALLY_PROP(Replied, replied_all)};
static const CmcCore fair_core = {
	.name = "fair", .state_size = 8, .durable_size = 1, .cfg_size = 4,
	.cfg_default = nd_cfg_default, .cfg_set = nd_cfg_set, .nnodes = two, .init = zero8,
	.step = fair_step_fn, .clients = first_only, .apply_persist = dur_apply,
};
static const CmcModel fair_model = {"fair", &fair_core, fair_props, 1, 0};

static void
test_fair_schedule(void)
{
	static const char *const want[] = {NULL, "the fair schedule cycles",
		"the fair schedule cycles", "not reached within 10 fair steps",
	"durable rule", "durable rule"};

	for (fair_mode = 0; fair_mode < 6; fair_mode++)
		for (int s = 0; s < (want[fair_mode] ? 4 : 3); s++)
		{
			CmcOptions	o;
			CmcRun	   *r;
			CmcResult	res;
			char		path[512];

			snprintf(path, sizeof(path), "%s/fair.replay",
					 getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".");
			cmc_options_default(&o);
			o.live_steps = fair_mode == 3 ? 10 : 1000;
			o.max_depth = 30;
			o.walks = 20;
			o.replay_in = path;
			r = mkc(&fair_model, s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : s == 2 ? CMC_SIMULATE :
					CMC_REPLAY, "", NULL, &o);
			cmc_check(r, &res);
			if (getenv("CMC_VERBOSE"))
				fprintf(stderr, "  fair mode %d search %d: verdict %d %s: %s\n", fair_mode, s,
						res.verdict, res.prop ? res.prop : "", res.why);
			if (!want[fair_mode])
				CHECK(res.verdict == CMC_PASS);
			else
			{
				CHECK(res.verdict == CMC_VIOLATION && strstr(res.why, want[fair_mode]) != NULL);
				/* an engine error in the fair run is the engine's, not Replied's */
				CHECK(!!strcmp(res.prop, "cmc:engine") == (fair_mode < 4));
			}
			if (s == 0 && res.trace_len > 1)
				CHECK(cmc_write_replay(r, path, res.trace, res.trace_len - 1) == 0);
			cmc_result_free(&res);
			cmc_run_free(r);
		}
	fair_mode = 0;
}

/* ---- limits on a large search -----------------------------------------
 * A direct core counting four bytes up to 31 each: 2^20 states, more
 * than the engine's first table and queue chunk hold, and enough that
 * the time checks come due. */

static int
wide_actions(const void *cfg, const void *sv, CmcAction *a, int max)
{
	const uint8_t *s = sv;
	int			n = 0;

	(void) cfg;
	for (int i = 0; i < 4 && n < max; i++)
		if (s[i] < 31)
			a[n++] = (CmcAction) i;
	return n;
}

static void
wide_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) node;
	out->n = 0;
	((uint8_t *) sv)[ev->action]++;
}

static const CmcCore wide_core = {
	.name = "wide", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = wide_step,
	.direct_actions = wide_actions,
};
static const CmcModel wide_model = {"wide", &wide_core, nd_props, 1, 0};

static void
test_search_limits(void)
{
	CmcOptions	o;
	CmcRun	   *r;
	CmcResult	res;

	/* the whole space, exactly, through a table and queue that grow */
	cmc_options_default(&o);
	o.time_limit = 600;
	r = mk(&wide_model, CMC_BFS, NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_PASS && res.distinct == 1u << 20 && res.depth == 125);
	cmc_result_free(&res);
	cmc_run_free(r);
	/* a time limit that has passed by the first check is INCOMPLETE, in
	 * each search that has one; so is DFS's state limit */
	for (int s = 0; s < 3; s++)
	{
		cmc_options_default(&o);
		o.time_limit = 1e-9;
		o.max_depth = 200;
		o.walks = 100000;
		r = mk(&wide_model, s == 0 ? CMC_BFS : s == 1 ? CMC_DFS : CMC_SIMULATE, NULL, &o);
		cmc_check(r, &res);
		CHECK(res.verdict == CMC_INCOMPLETE && strstr(res.why, "time limit") != NULL);
		cmc_result_free(&res);
		cmc_run_free(r);
	}
}

/* ---- parallel BFS and the queue spill ---------------------------------
 * A search with four workers, or with a queue that spills to files, must
 * give what one worker in memory gives: the verdict, the property, the
 * counts, the depth, the witness depths, the coverage and the very same
 * counterexample (the first in BFS order). */

static void
same_search(const CmcModel *m, const char *const *props, const CmcOptions *base,
			int workers, uint64_t queue_mem)
{
	same_search_cfg(m, "", props, base, workers, queue_mem);
}

static void
same_search_cfg(const CmcModel *m, const char *cfg, const char *const *props,
				const CmcOptions *base, int workers, uint64_t queue_mem)
{
	CmcOptions	o1 = *base,
				o2 = *base;
	CmcRun	   *r1,
			   *r2;
	CmcResult	a,
				b;

	o2.workers = workers;
	o2.queue_mem = queue_mem;
	r1 = mkc(m, CMC_BFS, cfg, props, &o1);
	r2 = mkc(m, CMC_BFS, cfg, props, &o2);
	cmc_check(r1, &a);
	cmc_check(r2, &b);
	if (getenv("CMC_VERBOSE"))
		fprintf(stderr, "  %s workers %d: verdict %d %s distinct=%llu trace=%d\n", m->name,
				workers, b.verdict, b.prop ? b.prop : "", (unsigned long long) b.distinct,
				b.trace_len);
	CHECK(a.verdict == b.verdict && a.distinct == b.distinct && a.generated == b.generated);
	CHECK(a.left == b.left && a.depth == b.depth && a.fair_from == b.fair_from);
	CHECK((a.prop == NULL) == (b.prop == NULL) && (!a.prop || !strcmp(a.prop, b.prop)));
	CHECK(!strcmp(a.why, b.why) && a.trace_len == b.trace_len);
	if (a.trace_len == b.trace_len && a.trace_len > 1)
		CHECK(!memcmp(a.trace, b.trace, sizeof(CmcAction) * (size_t) (a.trace_len - 1)));
	CHECK(a.nwitness == b.nwitness &&
		  !memcmp(a.witness_depth, b.witness_depth, sizeof(a.witness_depth)));
	CHECK(!memcmp(a.cov_gen, b.cov_gen, sizeof(a.cov_gen)) &&
		  !memcmp(a.cov_new, b.cov_new, sizeof(a.cov_new)));
	cmc_result_free(&a);
	cmc_result_free(&b);
	cmc_run_free(r1);
	cmc_run_free(r2);
}

/* A spill file that cannot be created ends the search with exit 3 (in a
 * child), never a pass on the states that fit. */
static bool
spill_refused(void)
{
	pid_t		pid;
	int			st = 0;

	fflush(NULL);
	pid = fork();
	if (pid == 0)
	{
		CmcOptions	o;
		CmcRun	   *r;
		CmcResult	res;

		if (!freopen("/dev/null", "w", stderr))
			_exit(99);
		cmc_options_default(&o);
		o.queue_mem = 4096;
		r = mkc(&wide_model, CMC_BFS, "", NULL, &o);
		cmc_check(r, &res);
		_exit(res.verdict);
	}
	waitpid(pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 3;
}

static void
test_workers_agree(void)
{
	static const char *const inv[] = {"Mutex", NULL};
	static const char *const act[] = {"PhaseMonotone", NULL};
	static const char *const qui[] = {"AllReplied", NULL};
	CmcOptions	o;
	char		err[256];

	cmc_options_default(&o);
	/* every property kind of the toy, passing and broken */
	same_search(&toy_model, NULL, &o, 4, 0);
	same_search(&toy_bug1_model, inv, &o, 4, 0);
	same_search(&toy_bug2_model, qui, &o, 4, 0);
	same_search(&toy_bug3_model, act, &o, 4, 0);
	o.net = CMC_NET_BAG;
	o.drop_budget = o.dup_budget = 1;
	same_search(&toy_model, NULL, &o, 3, 0);
	/* fair-schedule failures and engine errors in the fair run */
	for (fair_mode = 0; fair_mode < 6; fair_mode++)
	{
		cmc_options_default(&o);
		o.live_steps = fair_mode == 3 ? 10 : 1000;
		same_search(&fair_model, NULL, &o, 4, 0);
	}
	fair_mode = 0;
	/* a step the engine refuses, and an output cap */
	cmc_options_default(&o);
	same_search(&cap_model, NULL, &o, 4, 0);
	/* many batches and levels, a state limit, a stop at the witnesses, and
	 * a queue that spills, a chunk of 256 states at a time, under
	 * $CMC_BUILD; then a spill directory that is not there */
	{
		char	   *keep = getenv("CMC_SCRATCH") ? strdup(getenv("CMC_SCRATCH")) : NULL;

		setenv("CMC_SCRATCH", getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".", 1);
		same_search(&wide_model, NULL, &o, 4, 4096);
		setenv("CMC_SCRATCH", "/nonexistent/cmc-spill", 1);
		CHECK(spill_refused());
		if (keep)
			setenv("CMC_SCRATCH", keep, 1);
		else
			unsetenv("CMC_SCRATCH");
		free(keep);
	}
	o.max_states = 70000;
	same_search(&wide_model, NULL, &o, 4, 0);
	cmc_options_default(&o);
	o.stop_on_witness = true;
	same_search(&toy_model, NULL, &o, 4, 0);
	/* clocks: drift's FIRE variants, the lease bug they catch, a horizon
	 * that cuts the search (INCOMPLETE), deadline order, and clock range
	 * errors */
	for (ld_bug = 0; ld_bug <= 1; ld_bug++)
		for (int d = 0; d < 3; d++)
		{
			cmc_options_default(&o);
			o.drift = d == 0 ? 0 : d == 1 ? 50 : 200;
			same_search(&ld_model, NULL, &o, 4, 0);
			o.max_time = 500;
			same_search(&ld_model, NULL, &o, 4, 0);
		}
	ld_bug = 0;
	cmc_options_default(&o);
	o.drift = 100;
	same_search(&to_model, NULL, &o, 4, 0);
	cmc_options_default(&o);
	o.drift = 500;
	same_search(&big_model, NULL, &o, 4, 0);
	/* the options a search cannot honour are refused */
	cmc_options_default(&o);
	o.search = CMC_DFS;
	o.workers = 2;
	CHECK(cmc_run_new(&toy_model, "", NULL, &o, err, sizeof(err)) == NULL &&
		  strstr(err, "breadth-first"));
	o.workers = 0;
	o.queue_mem = 1;
	CHECK(cmc_run_new(&toy_model, "", NULL, &o, err, sizeof(err)) == NULL &&
		  strstr(err, "breadth-first"));
	o.search = CMC_BFS;
	o.queue_mem = 0;
	o.workers = 257;
	CHECK(cmc_run_new(&toy_model, "", NULL, &o, err, sizeof(err)) == NULL &&
		  strstr(err, "workers"));
}

/* ---- the command line (cmc_main) ------------------------------------ */

/* toy-bug1 with its first property (Mutex) always checked, under a
 * table name that differs from its own; filled in by test_cli_runs() */
static CmcModel always_model;

/* an invariant that fails without saying why */
static bool
never_true(const CmcWorld *w, CmcWhy *why)
{
	(void) w;
	(void) why;
	return false;
}

static const CmcProp mute_props[] = {CMC_INVARIANT(Never, never_true)};
static const CmcModel mute_model = {"mute", &mute_core, mute_props, 1, 0};

static const CmcModelEntry cli_models[] = {
	{"toy", &toy_model},
	{"toy-bug1", &toy_bug1_model},
	{"toy-bug2", &toy_bug2_model},
	{"toy-bug3", &toy_bug3_model},
	{"always", &always_model},
	{"cap", &cap_model},
	{"mute", &mute_model},
	{"lease", &ld_model},
	{"lease-renew", &ldr_model},
};

/* p (512 bytes) = the path of a file under $CMC_BUILD */
static const char *
cli_path(char *p, const char *name)
{
	snprintf(p, 512, "%s/%s", getenv("CMC_BUILD") ? getenv("CMC_BUILD") : ".", name);
	return p;
}

/* write text to the file at path */
static void
cli_write(const char *path, const char *text)
{
	FILE	   *f = fopen(path, "w");

	CHECK(f != NULL);
	if (f)
	{
		fputs(text, f);
		fclose(f);
	}
}

/*
 * Runs cmc_main() on the NULL-terminated argv in a child, with stdout and
 * stderr in one file, and checks that it exits with want and printed
 * needle.  The child returns through exit(), not _exit(), so stdio is
 * flushed and a --coverage build counts it.  Returns whether both held.
 */
static int
cli_expect(int want, const char *needle, const char *absent, const char *const *argv)
{
	char		out[512],
				buf[8192] = "";
	int			argc = 0,
				st = 0,
				ok;
	pid_t		pid;
	FILE	   *f;

	cli_path(out, "cli.out");
	while (argv[argc])
		argc++;
	fflush(NULL);
	pid = fork();
	if (pid == 0)
	{
		if (!freopen(out, "w", stdout) || dup2(fileno(stdout), 2) < 0)
			_exit(99);
		exit(cmc_main(argc, (char **) argv, cli_models,
					  sizeof(cli_models) / sizeof(cli_models[0])));
	}
	CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
	if ((f = fopen(out, "r")))
	{
		buf[fread(buf, 1, sizeof(buf) - 1, f)] = 0;
		fclose(f);
	}
	ok = WIFEXITED(st) && WEXITSTATUS(st) == want && strstr(buf, needle) != NULL &&
		(!absent || !strstr(buf, absent));
	if (!ok)
	{
		fprintf(stderr, "FAIL cli:");
		for (int i = 1; i < argc; i++)
			fprintf(stderr, " %s", argv[i]);
		fprintf(stderr, "\n  exit %d, wanted %d and \"%s\"%s%s; output:\n%s\n",
				WIFEXITED(st) ? WEXITSTATUS(st) : -1, want, needle,
				absent ? " without " : "", absent ? absent : "", buf);
		failures++;
	}
	return ok;
}

#define CLI(want, needle, ...) \
	cli_expect((want), (needle), NULL, (const char *const[]) {"cmc", __VA_ARGS__, NULL})
/* the same, and the output must not contain absent */
#define CLI_NOT(want, needle, absent, ...) \
	cli_expect((want), (needle), (absent), (const char *const[]) {"cmc", __VA_ARGS__, NULL})

/* input the user mistyped is an error, never a silent 0 or a dropped
 * setting that checks less than was asked */
static void
test_cli_input(void)
{
	char		cfg[512],
				none[512],
				longcfg[700] = "";

	cli_path(cfg, "cli.cfg");
	/* the model-file rows stop at the option, before the file is read */
	cli_path(none, "none.model");

	/* --crashes x read as 0 would pass the planted bug that needs a crash */
	CLI(3, "--crashes needs a number from 0 to 255, not 'x'",
		"--model", "toy", "--all-props", "--crashes", "x");
	CLI(3, "--clients needs a number from 0 to 255, not '1x'",
		"--model", "toy", "--all-props", "--clients", "1x");
	CLI(3, "--net-cap needs a number from 0 to 255, not '256'",
		"--model", "toy", "--all-props", "--net-cap", "256");
	CLI(3, "--walks needs a number from 1", "--model", "toy", "--all-props",
		"--simulate", "--walks", "abc");
	CLI(3, "--seed needs a number", "--model", "toy", "--all-props",
		"--simulate", "--seed", "-1");
	CLI(3, "--time needs a number of seconds", "--model", "toy", "--all-props",
		"--time", "soon");
	CLI(3, "--time needs a number of seconds, not '5s'", "--model", "toy", "--all-props",
		"--time", "5s");
	CLI(3, "--time needs a number of seconds, not 'inf'", "--model", "toy", "--all-props",
		"--time", "inf");
	CLI(3, "--time needs a number of seconds, not '-1'", "--model", "toy", "--all-props",
		"--time", "-1");
	CLI(3, "--seed needs a number from 0 to 18446744073709551615, not '18446744073709551616'",
		"--model", "toy", "--all-props", "--simulate", "--seed", "18446744073709551616");
	CLI(3, "--dfs needs a number from 1", "--model", "toy", "--all-props", "--dfs", "0");
	/* zero walks is a simulation that looks at nothing */
	CLI(3, "--walks needs a number from 1", "--model", "toy", "--all-props",
		"--simulate", "--walks", "0");
	CLI(3, "--depth needs a number from 1", "--model-file", none,
		"--depth", "0");
	CLI(3, "--walks needs a number from 1", "--model-file", none,
		"--simulate", "--walks", "0");
	CLI(3, "--drops needs a number from 0 to 255, not 'two'",
		"--model-file", none, "--drops", "two");
	/* the search resources: numbers in range, BFS only, and not --spec */
	CLI(3, "--workers needs a number from 1 to 256, not '0'", "--model", "toy",
		"--all-props", "--workers", "0");
	CLI(3, "--queue-mem needs a number from 1", "--model", "toy", "--all-props",
		"--queue-mem", "0");
	CLI(3, "more than one worker needs breadth-first search", "--model", "toy",
		"--all-props", "--dfs", "20", "--workers", "2");
	CLI(3, "--workers is not supported with --spec", "--spec", "examples/spec/paxos.cml",
		"--workers", "2");
	CLI(1, "27 distinct states", "--model", "toy-bug1", "--inv", "Mutex", "--workers", "4");
	/* --refine belongs to the process front end, which needs a model file */
	CLI(3, "--refine needs --model-file", "--refine", "x.cml", "--bind", "x.bind");
	/* TLC's one-line forms: CONSTANT k = v and INVARIANT name */
	cli_write(cfg, "CONSTANT clients = 2\nINVARIANT PhaseMonotone\n");
	CLI(1, "PhaseMonotone is violated", "--model", "toy-bug3", "--tlc-cfg", cfg);
	cli_write(cfg, "CONSTANTS\n  clients = 2\nINVARIANT Mutex\nSYMMETRY Perms\n");
	CLI(3, "cannot read line 4: SYMMETRY Perms", "--model", "toy", "--tlc-cfg", cfg);
	/* settings past the buffer would be cut, not checked */
	while (strlen(longcfg) < 600)
		strcat(longcfg, "clients=2,");
	strcat(longcfg, "clients=1");
	CLI(3, "the core settings are too long", "--model", "toy-bug1", "--inv", "Mutex",
		"--cfg", longcfg);
	/* a mistyped tag would check the bound spec without its planted bug */
	CLI(3, "names no [BUGG] action", "--model-file", none,
		"--refine", "examples/spec/paxos.cml", "--bind", "examples/spec/paxos.bind",
		"--define", "BUGG");
}

/* the options and the output of an in-process check, in each search */
static void
test_cli_runs(void)
{
	char		rp[512],
				cfg[512];

	cli_path(rp, "cli.replay");
	cli_path(cfg, "cli.cfg");
	always_model = toy_bug1_model;
	always_model.name = "always_toy";
	always_model.nalways = 1;
	CLI(0, "cmc (libcmc) " CMC_VERSION, "--version");
	CLI(3, "models: toy toy-bug1", "--help");
	CLI(3, "usage: cmc --model-file", "--model", "nosuch", "--all-props");
	CLI(3, "usage:", "--model");
	CLI(3, "usage:", "--model", "toy", "--all-props", "--net", "tree");
	CLI(3, "name properties with", "--model", "toy");
	CLI(3, "model toy_model has no property Nope", "--model", "toy", "--inv", "Nope");
	CLI(3, "a FIFO connection never duplicates", "--model", "toy", "--all-props", "--dups", "1");
	CLI(3, "usage:", "--all-props");
	/* an option at the end of the line with no value */
	for (int i = 0; i < 28; i++)
	{
		static const char *const opt[] = {"--cfg", "--tlc-cfg", "--inv", "--dfs", "--seed",
			"--walks", "--depth", "--replay", "--max-states", "--time", "--trace-out",
			"--progress", "--clients", "--drops", "--dups", "--crashes", "--net-cap",
			"--persist-cap", "--reply-cap", "--net", "--live-steps", "--prop", "--model",
		"--timer-cap", "--wake-cap", "--drift", "--max-time", "--bfs"};

		/* --bfs takes no value, so the line is merely short of a model */
		CLI(3, "usage:", "--all-props", opt[i]);
	}
	CLI(3, "usage:", "--model", "toy", "--inv", "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,"
		"w,x,y,z,A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,0,1,2,3,4,5,6,7,8,9,_,.");
	CLI(0, "34 distinct states found", "--model", "toy", "--all-props", "--net", "bag");
	CLI(0, "Mutex (always)\nPhaseMonotone\n", "--model", "always", "--list");
	CLI(0, "EventuallyAllReplied\n", "--model", "toy", "--list");
	/* every search, each with the line that says how it ended */
	CLI(0, "34 distinct states found", "--model", "toy", "--all-props", "--bfs", "--det",
		"--clients", "1", "--drops", "0", "--crashes", "0", "--net-cap", "8",
		"--persist-cap", "2", "--reply-cap", "8", "--net", "fifo", "--live-steps", "1000",
		"--timer-cap", "16", "--wake-cap", "8", "--max-states", "0", "--time", "60", "--progress", "0", "--quiet");
	CLI(0, "ReachCS first reached at depth 4", "--model", "toy", "--all-props");
	CLI(0, "Depth-first search completed within depth 40", "--model", "toy",
		"--all-props", "--dfs", "40");
	CLI(0, "No error found in 10 walks (not exhaustive)", "--model", "toy", "--all-props",
		"--simulate", "--seed", "3", "--walks", "10", "--depth", "50");
	CLI(0, "coverage: CLIENT node 1 (kind 7)", "--model", "toy", "--inv", "Mutex",
		"--coverage");
	CLI(2, "INCOMPLETE (no violation found): max_states reached", "--model", "toy",
		"--inv", "Mutex", "--max-states", "5");
	CLI(1, "Witness ReachCS: NOT reached", "--model", "toy", "--inv", "Mutex,ReachCS",
		"--clients", "0");
	CLI(1, "Error: Witness ReachCS is violated.", "--model", "toy", "--prop", "ReachCS",
		"--clients", "0");
	/* a counterexample: its trace, unless --quiet, and its replay file */
	CLI(1, "Error: The behavior up to this point is:", "--model", "toy-bug1", "--inv", "Mutex");
	CLI_NOT(1, "Invariant Mutex is violated", "State 1:", "--model", "toy-bug1", "--inv",
			"Mutex", "--quiet");
	CLI(1, "Action property PhaseMonotone is violated", "--model", "toy-bug3", "--prop",
		"PhaseMonotone");
	CLI(1, "Quiescence property AllReplied is violated", "--model", "toy-bug2", "--inv",
		"AllReplied");
	CLI(1, "(states 1-3: the shortest path found; then the fair schedule)", "--model",
		"toy-bug2", "--inv", "EventuallyAllReplied");
	CLI(1, "Engine check cmc:engine is violated", "--model", "cap", "--all-props");
	/* a violation with no reason, and so no line for it */
	CLI_NOT(1, "Error: Invariant Never is violated.\nError: The behavior", "\n  \n",
			"--model", "mute", "--all-props");
	/* a vacuous witness has no trace, so no replay file */
	CLI_NOT(1, "Witness ReachCS is violated", "Replay file", "--model", "toy", "--prop",
			"ReachCS", "--clients", "0", "--trace-out", rp);
	/* one at the initial state replays from no action */
	CLI(1, "Replay file written to", "--model", "mute", "--all-props", "--trace-out", rp);
	CLI(1, "Invariant Never is violated", "--replay", rp, "--all-props");
	CLI(1, "Could not write /nonexistent/x.replay", "--model", "toy-bug1", "--inv", "Mutex",
		"--trace-out", "/nonexistent/x.replay");
	CLI(1, "Replay file written to", "--model", "toy-bug1", "--inv", "Mutex", "--crashes", "0",
		"--trace-out", rp);
	/* --replay takes the model, cfg and budgets from the file */
	CLI(1, "Invariant Mutex is violated", "--replay", rp, "--inv", "Mutex");
	CLI(1, "Invariant Mutex is violated", "--replay", rp, "--model", "toy-bug1", "--inv",
		"Mutex", "--cfg", "clients=2");
	/* the file's cfg */
	cli_write(rp, "# cmc replay v2\nmodel toy_model\ncfg clients=1\nenv client=1 drop=0"
			  " dup=0 crash=0 net=8 persist=2 reply=8 mode=fifo live=1000\n");
	CLI(0, "cfg \"clients=1\"", "--replay", rp, "--inv", "Mutex");
	CLI(3, "recorded with cfg \"clients=1\"", "--replay", rp, "--inv", "Mutex", "--cfg",
		"clients=2");
	cli_write(rp, "# cmc replay v2\nmodel toy_model\ncfgx\n");
	CLI(3, "lacks its model, cfg or env line", "--replay", rp, "--inv", "Mutex");
	CLI(1, "Replay file written to", "--model", "toy-bug1", "--inv", "Mutex", "--crashes", "0",
		"--trace-out", rp);
	/* flags given agree with the file, so each one replaces the file's */
	CLI(1, "Invariant Mutex is violated", "--replay", rp, "--inv", "Mutex", "--clients", "1",
		"--drops", "0", "--dups", "0", "--net-cap", "8", "--persist-cap", "2", "--reply-cap",
		"8", "--net", "fifo", "--live-steps", "1000", "--timer-cap", "16", "--wake-cap", "8");
	CLI(3, "recorded with different environment bounds", "--replay", rp, "--inv", "Mutex",
		"--crashes", "1");
	CLI(3, "recorded with different environment bounds", "--replay", rp, "--inv", "Mutex",
		"--wake-cap", "3");
	CLI(3, "recorded with model toy_bug1_model, not toy_model", "--replay", rp,
		"--model", "toy", "--inv", "Mutex");
	CLI(3, "cannot open /nonexistent/x.replay", "--replay", "/nonexistent/x.replay",
		"--model", "toy", "--inv", "Mutex");
	CLI(3, "usage:", "--replay", "/nonexistent/x.replay", "--inv", "Mutex");
	/* time: a drift trace names the slow clock and replays under the
	 * file's drift; a horizon is INCOMPLETE, never a pass */
	ld_bug = 1;
	CLI(1, "FIRE node 1 timer 0 (node 0's clock slow)", "--model", "lease", "--inv",
		"OneHolder", "--drift", "50", "--det", "--trace-out", rp);
	CLI(1, "time: now=1001 n0=951 n1=1001", "--replay", rp, "--inv", "OneHolder");
	CLI(3, "recorded with different environment bounds", "--replay", rp, "--inv",
		"OneHolder", "--drift", "40");
	ld_bug = 0;
	CLI(2, "INCOMPLETE (no violation found): max_time 500 reached", "--model", "lease",
		"--all-props", "--drift", "50", "--max-time", "500");
	CLI(0, "No error has been found", "--model", "lease", "--inv", "OneHolder", "--max-time",
		"5000");
	CLI(3, "--drift needs a number from 0 to 500", "--model", "lease", "--all-props",
		"--drift", "501");
	CLI(3, "--max-time needs a number from 1", "--model", "lease", "--all-props",
		"--max-time", "0");
	/* --time-bound: the renewing lease ends, its trace prints clocks in
	 * the canonical frame, and the replay needs the same bound */
	ld_renew = 1;
	CLI(0, "No error has been found", "--model", "lease-renew", "--all-props", "--drift",
		"50", "--time-bound", "3000", "--time-check");
	CLI(3, "needs time_bound", "--model", "lease-renew", "--all-props");
	CLI(3, "--time-bound needs a number from 1", "--model", "lease-renew", "--all-props",
		"--time-bound", "0");
	ld_bug = 1;
	CLI(1, "time: now=0 n0=3002 n1=3002", "--model", "lease-renew", "--inv", "OneHolder",
		"--drift", "50", "--time-bound", "3000", "--trace-out", rp);
	CLI(1, "Invariant OneHolder is violated", "--replay", rp, "--inv", "OneHolder");
	CLI(3, "time-bound=3000", "--replay", rp, "--inv", "OneHolder", "--time-bound", "3001");
	ld_bug = 0;
	ld_renew = 0;
	cli_write(rp, "# cmc replay v2\nmodel lease\ncfg\nenv client=1 drop=0 dup=0 crash=0"
			  " net=8 persist=2 reply=8 mode=fifo live=1000\ntime x\n");
	CLI(3, "bad time line", "--replay", rp, "--inv", "OneHolder");
	/* the file's caps, and a garbled caps line, which the driver leaves
	 * to the engine to refuse */
	cli_write(rp, "# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0"
			  " net=8 persist=2 reply=8 mode=fifo live=1000\ncaps timer=3 wake=2\n");
	CLI(0, "Replay completed", "--replay", rp, "--inv", "Mutex");
	cli_write(rp, "# cmc replay v2\nmodel toy_model\ncfg\nenv client=1 drop=0 dup=0 crash=0"
			  " net=8 persist=2 reply=8 mode=fifo live=1000\ncaps x\n");
	CLI(3, "bad caps line", "--replay", rp, "--inv", "Mutex");
	cli_write(rp, "# cmc replay v2\nmodel always_toy\ncfg\nenv garbled\n");
	CLI(3, "bad env line in replay file", "--replay", rp, "--inv", "ReachCS");
	/* the file names the model by its own name, not its table name */
	cli_write(rp, "# cmc replay v2\nmodel always_toy\ncfg\nenv client=1 drop=0 dup=0 crash=0"
			  " net=8 persist=2 reply=8 mode=bag live=1000\nact 0701000000000000\n");
	CLI(0, "Replay completed: 2 states, no property violated.", "--replay", rp,
		"--inv", "ReachCS");
	/* a TLC .cfg, with --cfg after it overriding its constants */
	cli_write(cfg, "\\* a comment\nCONSTANTS\n  clients = 2\n\nSPECIFICATION Spec\n"
			  "INVARIANTS\n  Mutex \\* trailing comment\nPROPERTIES\n  ReachCS\n");
	CLI(1, "Invariant Mutex is violated", "--model", "toy-bug1", "--tlc-cfg", cfg);
	CLI(0, "cfg \"clients=2,clients=1\"", "--model", "toy-bug1", "--tlc-cfg", cfg,
		"--cfg", "clients=1");
	CLI(3, "cannot open /nonexistent/x.cfg", "--model", "toy", "--tlc-cfg",
		"/nonexistent/x.cfg");
	cli_write(cfg, "CONSTANT Writes = {w1, w2} Mode = \"a b\"\nINVARIANT Mutex\n");
	CLI(3, "bad cfg Writes={w1  w2} for core toy", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "CONSTANTS\n  clients 2\n");
	CLI(3, "cannot read line 2: clients 2", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "CONSTANT clients = {1\n");
	CLI(3, "cannot read line 1", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "INVARIANT Mutex, ReachCS\n");
	CLI(3, "cannot read line 1: Mutex, ReachCS", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "SPECIFICATION Spec Other\n");
	CLI(3, "cannot read line 1: Spec Other", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "clients = 2\n");
	CLI(3, "cannot read line 1: clients = 2", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "CONSTANT clients =\n");
	CLI(3, "cannot read line 1", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "CONSTANT = 2\n");
	CLI(3, "cannot read line 1", "--model", "toy", "--tlc-cfg", cfg);
	cli_write(cfg, "INVARIANT Mutex-2\n");
	CLI(3, "cannot read line 1: Mutex-2", "--model", "toy", "--tlc-cfg", cfg);
	/* a file written on Windows, with nothing but a section heading */
	cli_write(cfg, "\tINVARIANTS \r\n  Mutex\r\nSPECIFICATION\r\n  Spec\r\n");
	CLI(0, "cfg \"\"", "--model", "toy", "--tlc-cfg", cfg);
	/* a line longer than the reader holds is refused, not read in two */
	{
		char		lng[700];

		memset(lng, 'x', 600);
		strcpy(lng + 600, "\n");
		cli_write(cfg, lng);
		CLI(3, "the line is too long", "--model", "toy", "--tlc-cfg", cfg);
	}
}

/* the front ends' own option checks (the model file is never read) */
static void
test_cli_front_ends(void)
{
	char		none[512];

	cli_path(none, "none.model");
	CLI(3, "--bogus is not supported with --model-file", "--model-file", none, "--bogus");
	CLI(3, "--refine-steps needs --refine", "--model-file", none, "--refine-steps");
	CLI(3, "--refine needs --bind", "--model-file", none, "--refine", "x.cml");
	CLI(3, "cannot open x.cml", "--model-file", none, "--refine", "x.cml", "--bind", "x.bind");
	CLI(3, "binding: cannot open /nonexistent/x.bind", "--model-file", none, "--refine",
		"examples/spec/paxos.cml", "--bind", "/nonexistent/x.bind");
	CLI(3, "usage:", "--model-file", none, "--binary");
	CLI(3, "cannot open model file", "--model-file", none, "--binary", "x", "--bfs",
		"--simulate", "--seed", "1", "--walks", "2", "--max-states", "3", "--time", "4",
		"--crashes", "1", "--timeouts", "1");
	CLI(3, "--model is not supported with --spec", "--spec", "x.cml", "--model", "toy");
	CLI(3, "usage:", "--spec");
	CLI(3, "usage:", "--emit-tla", "x.cml", "--define");
	CLI(3, "cannot open x.cml", "--spec", "x.cml", "--define", "BUG");
	CLI(3, "cannot open x.cml", "--emit-tla", "x.cml");
	CLI(3, "--spec is not supported with --emit-tla", "--emit-tla", "x.cml", "--spec", "y");
	/* more --define tags than the front ends hold */
	{
		const char *argv[40] = {"cmc", "--spec", "x.cml"};
		int			n = 3;

		for (int i = 0; i < 17; i++)
		{
			argv[n++] = "--define";
			argv[n++] = "T";
		}
		argv[n] = NULL;
		cli_expect(3, "usage:", NULL, argv);
		argv[1] = "--model-file";
		argv[2] = none;
		cli_expect(3, "usage:", NULL, argv);
	}
	/* a binding the spec cannot use */
	{
		char		bind[512];

		cli_write(cli_path(bind, "cli.bind"), "[map]\n");
		CLI(3, "binding: binding has no [map]", "--model-file", none, "--refine",
			"examples/spec/paxos.cml", "--bind", bind);
	}
}

/* CMC_VERSION is MAJOR.MINOR.PATCH: the release workflows and the flake
 * read it, and the tag must be v followed by it */
/* ---- the request log and linearizability (2.4) ------------------------ */

/*
 * A register over two nodes: node 0 takes writes (request r writes r + 1),
 * node 1 takes reads and answers its copy.  Node 0 sends the value to
 * node 1; the correct core answers the write only when node 1 acks it,
 * lin_stale_step answers at once, so a read invoked after the write's
 * answer can still return the old value.
 */
static void
lin_step_common(int node, uint8_t *s, const CmcEvent *ev, CmcOut *out, bool stale)
{
	CmcOutput  *o;

	if (node == 0 && ev->kind == CMC_EV_CLIENT)
	{
		s[0] = (uint8_t) (ev->arg + 1);
		s[1] = (uint8_t) ev->arg;
		o = cmc_out_push(out, CMC_OUT_SEND);
		o->dst = 1;
		o->len = 1;
		o->body[0] = s[0];
		if (stale)
		{
			o = cmc_out_push(out, CMC_OUT_REPLY);
			o->arg = s[0];
			o->req = ev->arg + 1;
		}
	}
	else if (node == 1 && ev->kind == CMC_EV_RECV)
	{
		s[0] = ev->body[0];
		o = cmc_out_push(out, CMC_OUT_SEND);
		o->dst = 0;
		o->len = 1;
		o->body[0] = s[0];
	}
	else if (node == 0 && ev->kind == CMC_EV_RECV && !stale && ev->body[0] == s[0])
	{
		o = cmc_out_push(out, CMC_OUT_REPLY);
		o->arg = s[0];
		o->req = (uint32_t) s[1] + 1;
	}
	else if (node == 1 && ev->kind == CMC_EV_CLIENT)
	{
		o = cmc_out_push(out, CMC_OUT_REPLY);
		o->arg = s[0];
		o->req = ev->arg + 1;
	}
}

static void
lin_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	lin_step_common(node, sv, ev, out, false);
}

static void
lin_stale_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	lin_step_common(node, sv, ev, out, true);
}

/*
 * Linearizable reads of a register whose writes only grow: a read that
 * was invoked after a write was answered returns at least that write.
 */
static bool
lin_reads(const CmcWorld *w, CmcWhy *why)
{
	int			nr = cmc_nrequests(w);

	for (int i = 0; i < nr; i++)
		for (int j = 0; j < nr; j++)
		{
			int			ni,
						nj,
						ri,
						rj;
			bool		ai,
						aj;
			uint32_t	vi,
						vj;

			cmc_request(w, i, &ni, &ri, &ai, &vi);
			cmc_request(w, j, &nj, &rj, &aj, &vj);
			if (ni == 0 && nj == 1 && aj && cmc_precedes(w, i, j) && vj < vi)
			{
				cmc_why(why, "write %d answered %u, then read %d returned %u", ri, vi, rj, vj);
				return false;
			}
		}
	return true;
}

static const CmcProp lin_props[] = {CMC_INVARIANT(LinReads, lin_reads)};
static const CmcCore lin_core = {
	.name = "lin", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = two, .init = zero8, .step = lin_step,
};
static const CmcCore lin_stale_core = {
	.name = "lin_stale", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = nd_cfg_set, .nnodes = two, .init = zero8, .step = lin_stale_step,
};
static const CmcModel lin_model = {"lin", &lin_core, lin_props, 1, 0, .requests = true};
static const CmcModel lin_stale_model = {
	"lin_stale", &lin_stale_core, lin_props, 1, 0, .requests = true,
};

/*
 * Reply rules of the request log, by cfg mode: 1 names a request never
 * invoked, 2 answers one request twice, 3 replies from node 1, which
 * takes no clients.
 */
static int
rq_cfg_set(void *cfg, const char *k, const char *v)
{
	if (strcmp(k, "mode") || v[0] < '1' || v[0] > '3' || v[1])
		return -1;
	((uint8_t *) cfg)[0] = (uint8_t) (v[0] - '0');
	return 0;
}

static void
rq_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	int			mode = ((const uint8_t *) cfg)[0];

	(void) sv;
	if (ev->kind == CMC_EV_CLIENT)
	{
		cmc_out_push(out, CMC_OUT_REPLY)->req = mode == 1 ? 5 : ev->arg + 1;
		if (mode == 2)
			cmc_out_push(out, CMC_OUT_REPLY)->req = ev->arg + 1;
		if (mode == 3)
			cmc_out_push(out, CMC_OUT_SEND)->dst = 1;
	}
	else if (node == 1 && ev->kind == CMC_EV_RECV)
		cmc_out_push(out, CMC_OUT_REPLY)->req = 1;
}

static const CmcCore rq_core = {
	.name = "rq", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = rq_cfg_set, .nnodes = two, .init = zero8, .step = rq_step,
	.clients = first_only,
};
static const CmcModel rq_model = {"rq", &rq_core, nd_props, 1, 0, .requests = true};
static const CmcModel rq_nolog_model = {"rq_nolog", &rq_core, nd_props, 1, 0};
static const CmcModel lin_nolog_model = {"lin_nolog", &lin_core, lin_props, 1, 0};

/* the verdict and reason of model m under cfg, BFS, default bounds */
static CmcVerdict
cfg_verdict(const CmcModel *m, const char *cfg, CmcOptions *o, char *why, size_t wlen,
			uint64_t *distinct)
{
	CmcRun	   *r = mkc(m, CMC_BFS, cfg, NULL, o);
	CmcResult	res;
	CmcVerdict	v;

	if (!r)
		return CMC_ERROR;
	cmc_check(r, &res);
	v = res.verdict;
	snprintf(why, wlen, "%s", res.why);
	if (distinct)
		*distinct = res.distinct;
	cmc_result_free(&res);
	cmc_run_free(r);
	return v;
}

/* ---- history variables (2.4) ------------------------------------------ */

/*
 * One node: a CLIENT chooses (s[0] = 1) and arms timer 0.  In the buggy
 * core the timer un-chooses.  A state invariant cannot tell "never
 * chosen" from "chosen, then lost"; the history byte can.
 */
static void
hist_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	uint8_t    *s = sv;

	(void) node;
	if (ev->kind == CMC_EV_CLIENT)
	{
		s[0] = 1;
		cmc_out_push(out, CMC_OUT_SET_TIMER)->arg = 0;
	}
	else if (ev->kind == CMC_EV_TIMER && ((const uint8_t *) cfg)[0])
		s[0] = 0;
}

static int
hist_cfg_set(void *cfg, const char *k, const char *v)
{
	if (strcmp(k, "bug") || strcmp(v, "1"))
		return -1;
	((uint8_t *) cfg)[0] = 1;
	return 0;
}

static void
hist_update(const CmcWorld *pre, CmcAction a, const CmcWorld *post, uint8_t *ghost)
{
	(void) a;
	ghost[0] = (uint8_t) (cmc_history(pre)[0] | ((const uint8_t *) cmc_node(post, 0))[0]);
}

static int	hist_calls;

static void
hist_nondet(const CmcWorld *pre, CmcAction a, const CmcWorld *post, uint8_t *ghost)
{
	(void) pre;
	(void) a;
	(void) post;
	ghost[0] = (uint8_t) (++hist_calls & 1);
}

static bool
stays_chosen(const CmcWorld *w, CmcWhy *why)
{
	if (cmc_history(w)[0] && !((const uint8_t *) cmc_node(w, 0))[0])
	{
		cmc_why(why, "chosen once, not chosen now");
		return false;
	}
	return true;
}

static const CmcProp hist_props[] = {CMC_INVARIANT(StaysChosen, stays_chosen)};
static const CmcCore hist_core = {
	.name = "hist", .state_size = 8, .cfg_size = 4, .cfg_default = nd_cfg_default,
	.cfg_set = hist_cfg_set, .nnodes = nd_nnodes, .init = zero8, .step = hist_step,
};
static const CmcModel hist_model = {
	"hist", &hist_core, hist_props, 1, 0, .history_size = 1, .history = hist_update,
};
static const CmcModel hist_nondet_model = {
	"hist_nondet", &hist_core, nd_props, 1, 0, .history_size = 1, .history = hist_nondet,
};

/* a call that must end the process with status 3 (a model bug) */
static void
expect_exit3(const CmcModel *m)
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
		o.client_budget = 1;
		if (!freopen("/dev/null", "w", stderr))
			exit(1);
		cfg_verdict(m, "", &o, why, sizeof(why), NULL);
		exit(0);
	}
	CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
}

static void
test_request_log(void)
{
	CmcOptions	o;
	char		why[512],
				err[256];
	uint64_t	nd = 0;
	CmcRun	   *r;
	CmcResult	res;
	char	   *text = NULL;
	size_t		tlen = 0;
	FILE	   *f;

	/* a correct register passes; a stale read after an answered write is caught */
	cmc_options_default(&o);
	o.client_budget = 2;
	CHECK(cfg_verdict(&lin_model, "", &o, why, sizeof(why), &nd) == CMC_PASS && nd > 1);
	cmc_options_default(&o);
	o.client_budget = 2;
	CHECK(cfg_verdict(&lin_stale_model, "", &o, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "then read"));
	/* the log is the same with four workers, trace included */
	same_search_cfg(&lin_stale_model, "", NULL, &o, 4, 0);
	same_search_cfg(&lin_model, "", NULL, &o, 4, 0);
	/* the trace shows the log: answers and the precedence */
	cmc_options_default(&o);
	o.client_budget = 1;
	r = mkc(&lin_stale_model, CMC_BFS, "", NULL, &o);
	cmc_check(r, &res);
	CHECK(res.verdict == CMC_VIOLATION && res.trace_len == 3);
	f = open_memstream(&text, &tlen);
	cmc_print_trace(r, res.trace, res.trace_len - 1, f);
	fclose(f);
	CHECK(strstr(text, "requests: [0] n0#0 =1 [1] n1#0 =0 after[0]") != NULL);
	free(text);
	cmc_result_free(&res);
	cmc_run_free(r);

	/* reply rules: each is an engine error, never a silent drop */
	cmc_options_default(&o);
	CHECK(cfg_verdict(&rq_model, "mode=1", &o, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "never invoked"));
	CHECK(cfg_verdict(&rq_model, "mode=2", &o, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "twice"));
	CHECK(cfg_verdict(&rq_model, "mode=3", &o, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "takes no clients"));
	CHECK(cfg_verdict(&rq_nolog_model, "mode=2", &o, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "keeps no request log"));
	/* a property reading a log the model does not keep stops the run */
	expect_exit3(&lin_nolog_model);

	/* the log must fit --req-cap: 2 nodes x 5 clients > 8 */
	cmc_options_default(&o);
	o.client_budget = 5;
	o.quiet = true;
	CHECK(!cmc_run_new(&lin_model, "", NULL, &o, err, sizeof(err)) &&
		  strstr(err, "needs 10 slots"));
	o.req_cap = 10;
	r = cmc_run_new(&lin_model, "", NULL, &o, err, sizeof(err));
	CHECK(r != NULL);
	cmc_run_free(r);
	o.req_cap = 65;
	CHECK(!cmc_run_new(&lin_model, "", NULL, &o, err, sizeof(err)) &&
		  strstr(err, "--req-cap must be"));
	cmc_options_default(&o);
	o.quiet = true;
	CHECK(!cmc_run_new(&(CmcModel) {"cal", &cap_core, nd_props, 1, 0, .requests = true},
					   "", NULL, &o, err, sizeof(err)) && strstr(err, "actor mode"));
	CLI(3, "--req-cap needs a number", "--model", "toy", "--all-props", "--req-cap", "65");
}

static void
test_history(void)
{
	CmcOptions	o;
	char		why[512],
				err[256];
	uint64_t	n0 = 0,
				n1 = 0;
	CmcRun	   *r;
	uint8_t    *w;

	cmc_options_default(&o);
	CHECK(cfg_verdict(&hist_model, "", &o, why, sizeof(why), &n0) == CMC_PASS);
	CHECK(cfg_verdict(&hist_model, "bug=1", &o, why, sizeof(why), &n1) == CMC_VIOLATION &&
		  strstr(why, "chosen once"));
	/* the ghost is part of the state: chosen-then-lost differs from never chosen */
	CHECK(n0 == 3);
	/* history() runs in every worker, with the same result */
	same_search_cfg(&hist_model, "", NULL, &o, 4, 0);
	same_search_cfg(&hist_model, "bug=1", NULL, &o, 4, 0);
	cmc_options_default(&o);
	o.search = CMC_SIMULATE;
	o.walks = 20;
	r = mkc(&hist_model, CMC_SIMULATE, "bug=1", NULL, &o);
	{
		CmcResult	res;

		cmc_check(r, &res);
		CHECK(res.verdict == CMC_VIOLATION);
		cmc_result_free(&res);
	}
	cmc_run_free(r);
	/* history() runs twice under --det and must agree */
	cmc_options_default(&o);
	o.det = true;
	CHECK(cfg_verdict(&hist_nondet_model, "", &o, why, sizeof(why), NULL) == CMC_VIOLATION &&
		  strstr(why, "history()"));
	/* zero at init; NULL for a model with none */
	cmc_options_default(&o);
	r = mkc(&hist_model, CMC_BFS, "", NULL, &o);
	w = calloc(1, cmc_world_size(r));
	cmc_world_init(r, w);
	CHECK(cmc_history(&(CmcWorld) {r, w})[0] == 0);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)) == 0 &&
		  cmc_history(&(CmcWorld) {r, w})[0] == 1);
	CHECK(cmc_apply(r, w, CMC_ACT(CMC_A_CLIENT, 0, 0, 0, 0)) != 0);
	free(w);
	cmc_run_free(r);
	r = mkc(&nd_model, CMC_BFS, "", NULL, &o);
	CHECK(cmc_history(&(CmcWorld) {r, NULL}) == NULL);
	cmc_run_free(r);
	/* a size with no hook (or a hook with no size) is refused */
	CHECK(!cmc_run_new(&(CmcModel) {"h", &hist_core, nd_props, 1, 0, .history_size = 1},
					   "", NULL, &o, err, sizeof(err)) && strstr(err, "go together"));
}

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
	test_crashable();
	test_timer_args();
	test_lease_drift();
	test_time_bound();
	test_time_bound_refusals();
	test_deadline_order();
	test_m1_large_durable();
	test_m3_output_overflow();
	test_m4_replay_conflict();
	test_min1_dfs_witness();
	test_min3_quiescent_depth();
	test_min4_negative_bounds();
	test_p21_crash_clears_self_channel();
	test_p22_simulate_checks_last_state();
	test_p23_property_cap();
	test_stop_on_witness();
	test_p24_replay_enabled_error();
	test_p25_fair_schedule_fires_timers();
	test_output_rules();
	test_core_rules();
	test_searches_agree();
	test_replay_refusals();
	test_fair_schedule();
	test_search_limits();
	test_workers_agree();
	test_apply_refuses();
	test_cli_input();
	test_cli_runs();
	test_cli_front_ends();
	test_request_log();
	test_history();
	test_version();
	if (failures)
	{
		fprintf(stderr, "test_cmc: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_cmc: all engine tests passed\n");
	return 0;
}
