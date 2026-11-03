/*
 * probe/uninit.c: docs/research/02 M2 (E5).  A core step that copies an
 * uninitialised stack byte into its state.  --det runs the step twice on
 * the same stack contents, so the gate sees equal results; MemorySanitizer
 * (make check-mc-msan) must report it.  Built only into the msan probe.
 */
#include "cmc.h"

#include <stdlib.h>

static void __attribute__((noinline))
dirty(void)
{
	volatile uint8_t junk[64];

	for (int i = 0; i < 64; i++)
		junk[i] = (uint8_t) (i * 7 + 3);
}

static void __attribute__((noinline))
copy_garbage(uint8_t *s, int k)
{
	volatile uint8_t buf[64];

	if (k)
		s[0] = buf[5];
}

static void
u_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	(void) cfg;
	(void) node;
	(void) out;
	dirty();
	copy_garbage(sv, ev->kind == CMC_EV_CLIENT);
}

static void
u_init(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, 8);
}

static void
u_cfg_default(void *cfg)
{
	(void) cfg;
}

static int
u_cfg_set(void *cfg, const char *k, const char *v)
{
	(void) cfg;
	(void) k;
	(void) v;
	return -1;
}

static int
u_nnodes(const void *cfg)
{
	(void) cfg;
	return 1;
}

static bool
u_true(const CmcWorld *w, CmcWhy *why)
{
	(void) w;
	(void) why;
	return true;
}

static const CmcProp u_props[] = {CMC_INVARIANT(True, u_true)};
static const CmcCore u_core = {
	.name = "uninit", .state_size = 8, .cfg_size = 4, .cfg_default = u_cfg_default,
	.cfg_set = u_cfg_set, .nnodes = u_nnodes, .init = u_init, .step = u_step,
};
static const CmcModel u_model = {"uninit", &u_core, u_props, 1, 0};

int
main(void)
{
	CmcOptions	o;
	CmcResult	res;
	char		err[256];
	CmcRun	   *r;

	cmc_options_default(&o);
	o.det = true;
	o.quiet = true;
	r = cmc_run_new(&u_model, "", NULL, &o, err, sizeof(err));
	if (!r)
		return 3;
	cmc_check(r, &res);
	printf("uninit probe: verdict %d (no MSan report means the hole is open)\n", res.verdict);
	cmc_result_free(&res);
	cmc_run_free(r);
	return 0;
}
