/*
 * probe/static_state.c: docs/research/02 B2 (E2).  An epoch filter whose
 * high-water mark lives in a function-local static, outside the world
 * bytes.  The state is not fingerprinted, so dedup merges worlds that
 * differ.  The purity gate must reject this object.
 */
#include "cmc.h"

void		probe_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out);

void
probe_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	static uint8_t hw;			/* the bug: state outside the world */
	uint8_t    *s = sv;

	(void) cfg;
	(void) out;
	if (ev->kind == CMC_EV_RECV && node == 2 && ev->body[0] >= hw)
	{
		hw = ev->body[0];
		s[0]++;
	}
}
