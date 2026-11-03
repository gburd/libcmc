/*
 * toy_model.c: configuration, descriptions and properties of the toy
 * lock-server core (toy_core.c).  Built once per CMC_TOY_BUG variant so
 * each variant is its own model (toy, toy-bug1, ...).
 */
#include "toy.h"

#include <string.h>

#ifndef TOY_MODEL_NAME
#define TOY_MODEL_NAME toy_model
#endif
#define STR2(x) #x
#define STR(x) STR2(x)

static void
toy_cfg_default(void *cfg)
{
	((ToyCfg *) cfg)->clients = 2;
}

static int
toy_cfg_set(void *cfg, const char *k, const char *v)
{
	if (strcmp(k, "clients") == 0 && (v[0] == '1' || v[0] == '2') && !v[1])
	{
		((ToyCfg *) cfg)->clients = v[0] - '0';
		return 0;
	}
	return -1;
}

static int
toy_nnodes(const void *cfg)
{
	return 1 + ((const ToyCfg *) cfg)->clients;
}

static int
toy_clients(const void *cfg, int node)
{
	(void) cfg;
	return node == 0 ? 0 : 1;
}

static void
toy_describe(const void *cfg, int node, const void *sv, FILE *f)
{
	const ToyState *s = sv;

	(void) cfg;
	if (node == 0)
		fprintf(f, "server holder=%u queue=[%u,%u] (n=%u)", s->holder, s->q[0], s->q[1], s->nq);
	else
		fprintf(f, "client phase=%u", s->phase);
}

static void
toy_describe_msg(const void *cfg, const uint8_t *b, FILE *f)
{
	(void) cfg;
	fprintf(f, "%s", b[0] == M_REQ ? "REQ" : b[0] == M_GRANT ? "GRANT" : "REL");
}

static const CmcCore toy_core = {
	.name = "toy",
	.state_size = sizeof(ToyState),
	.cfg_size = sizeof(ToyCfg),
	.cfg_default = toy_cfg_default,
	.cfg_set = toy_cfg_set,
	.nnodes = toy_nnodes,
	.init = toy_init,
	.step = toy_step,
	.describe = toy_describe,
	.describe_msg = toy_describe_msg,
	.clients = toy_clients,
};

/* ---- properties (read core state; never used to enable actions) ----- */

static bool
mutex(const CmcWorld *w, CmcWhy *why)
{
	int			in = 0;

	for (int n = 1; n < cmc_nnodes(w); n++)
		if (((const ToyState *) cmc_node(w, n))->phase == 2)
			in++;
	if (in > 1)
	{
		cmc_why(why, "%d clients in the critical section", in);
		return false;
	}
	return true;
}

/* bounded liveness: when nothing can happen, every client was replied to */
static bool
all_replied(const CmcWorld *w, CmcWhy *why)
{
	if (cmc_nreplies(w) != cmc_nnodes(w) - 1)
	{
		cmc_why(why, "quiescent with %d of %d clients replied",
			   cmc_nreplies(w), cmc_nnodes(w) - 1);
		return false;
	}
	return true;
}

/* bounded liveness: the fair schedule answers every request issued */
static bool
eventually_all_replied(const CmcWorld *w, CmcWhy *why)
{
	int			req = 0;

	for (int n = 0; n < cmc_nnodes(w); n++)
		req += cmc_requests(w, n);
	if (cmc_nreplies(w) != req)
	{
		cmc_why(why, "%d of %d requests replied", cmc_nreplies(w), req);
		return false;
	}
	return true;
}

static bool
reach_cs(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	for (int n = 1; n < cmc_nnodes(w); n++)
		if (((const ToyState *) cmc_node(w, n))->phase == 2)
			return true;
	return false;
}

static bool
phase_monotone(const CmcWorld *pre, CmcAction a, const CmcWorld *post, CmcWhy *why)
{
	(void) a;
	for (int n = 1; n < cmc_nnodes(pre); n++)
		if (((const ToyState *) cmc_node(post, n))->phase <
			((const ToyState *) cmc_node(pre, n))->phase)
		{
			cmc_why(why, "client %d phase went down", n);
			return false;
		}
	return true;
}

static const CmcProp toy_props[] = {
	CMC_INVARIANT(Mutex, mutex),
	CMC_ACTION_PROPERTY(PhaseMonotone, phase_monotone),
	CMC_QUIESCENT_PROP(AllReplied, all_replied),
	CMC_WITNESS_PROP(ReachCS, reach_cs),
	CMC_EVENTUALLY_PROP(EventuallyAllReplied, eventually_all_replied),
};

const CmcModel TOY_MODEL_NAME = {
	.name = STR(TOY_MODEL_NAME),
	.core = &toy_core,
	.props = toy_props,
	.nprops = sizeof(toy_props) / sizeof(toy_props[0]),
};
