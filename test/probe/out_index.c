/*
 * probe/out_index.c: docs/research/02 M3 (E8).  A core that writes outputs by
 * indexing the output array, past CMC_OUT_MAX.  The source gate must
 * reject it: cores emit through cmc_out_push().
 */
#include "cmc.h"

#include <string.h>

void		probe_flood(CmcOut *out);

void
probe_flood(CmcOut *out)
{
	for (int i = 0; i < CMC_OUT_MAX + 4; i++)
	{
		CmcOutput   *o = &out->o[out->n++];

		memset(o, 0, sizeof(*o));
		o->kind = CMC_OUT_SET_TIMER;
	}
}
