/*
 * test/spec/refine_driver.c: drive cmc_refine_step() over a sequence of
 * observed abstract values, with no process front end, so the spec gate
 * can pin C2 (step refinement) verdicts on small hand-made behaviours.
 *
 *   refine_driver SPEC.cml BIND V0 V1 ...
 *
 * Each Vi is the value node 0 reports at byte 0 at step i.  Exit 0 if
 * every observation is accepted, 1 if one is rejected (an invariant or a
 * step that matches no abstract action), 3 on a load or evaluation
 * error.  The first rejection is printed.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cmc_spec.h"

static long observed;

static long
node_byte(void *ctx, int node, int byte)
{
	(void) ctx;
	(void) node;
	(void) byte;
	return observed;
}

static int
node_len(void *ctx, int node)
{
	(void) ctx;
	(void) node;
	return 1;
}

int
main(int argc, char **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: refine_driver SPEC BIND V0 [V1 ...]\n");
		return 3;
	}
	char err[256] = {0};

	if (cmc_refine_load_spec(argv[1], NULL, 0))
		return 3;
	if (cmc_refine_load_bind(argv[2], err, sizeof err))
	{
		fprintf(stderr, "binding: %s\n", err);
		return 3;
	}
	CmcObs obs = {0};

	obs.nnodes = 1;
	obs.node_byte = node_byte;
	obs.node_len = node_len;
	cmc_refine_reset();
	for (int i = 3; i < argc; i++)
	{
		const char *name = NULL;

		observed = strtol(argv[i], NULL, 10);
		int rc = cmc_refine_step(&obs, &name, err, sizeof err);

		if (rc == -2)
		{
			fprintf(stderr, "refinement error at step %d: %s\n", i - 3, err);
			return 3;
		}
		if (rc != -1)
		{
			printf("rejected at step %d (value %ld): %s\n", i - 3, observed, name ? name : "?");
			return 1;
		}
	}
	printf("accepted %d observations\n", argc - 3);
	return 0;
}
