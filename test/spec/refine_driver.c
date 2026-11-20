/*
 * test/spec/refine_driver.c: drive cmc_refine_step() over a sequence of
 * observed abstract values, with no process front end, so the spec gate
 * can pin C2 (step refinement) verdicts on small hand-made behaviours.
 *
 *   refine_driver [-1] SPEC.cml BIND V0 V1 ...
 *
 * Each Vi is the value node 0 reports at byte 0 at step i.  Node 1
 * reports Vi + 1 at every byte, and the reply log holds one reply (from
 * node 1, value Vi) once Vi is not 0.  Exit 0 if every observation is
 * accepted, 1 if one is rejected (an invariant or a step that matches no
 * abstract action), 3 on a load or evaluation error.  The first
 * rejection is printed.  With -1, each observation is checked at the
 * state level only (cmc_refine_check, the C1 check), not as a step.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmc_spec.h"

static long observed;

static long
node_byte(void *ctx, int node, int byte)
{
	(void) ctx;
	(void) byte;
	return node == 0 ? observed : observed + 1;
}

static long
repl_src(void *ctx, int r)
{
	(void) ctx;
	(void) r;
	return 1;
}

static long
repl_val(void *ctx, int r)
{
	(void) ctx;
	(void) r;
	return observed;
}

int
main(int argc, char **argv)
{
	int			states = argc > 1 && !strcmp(argv[1], "-1");

	argv += states;
	argc -= states;
	if (argc < 4)
	{
		fprintf(stderr, "usage: refine_driver [-1] SPEC BIND V0 [V1 ...]\n");
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

	obs.nnodes = 2;
	obs.node_byte = node_byte;
	obs.repl_src = repl_src;
	obs.repl_val = repl_val;
	cmc_refine_reset();
	for (int i = 3; i < argc; i++)
	{
		const char *name = NULL;

		observed = strtol(argv[i], NULL, 10);
		obs.nrepl = observed != 0;
		int rc = states ? cmc_refine_check(&obs, &name, err, sizeof err) :
			cmc_refine_step(&obs, &name, err, sizeof err);

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
