/*
 * cmc_cli.c: command-line driver: cmc --model-file FILE checks a program
 * as it ships (the process front end, cmc_proc.c); cmc --model NAME runs
 * an in-process CmcCore model (the engine's calibration path, cmc.c).
 *
 *   cmc --model-file replog.model
 *   cmc --model-file replog.model --binary ./replog-debug --simulate --walks 10000
 *   cmc --model hc --tlc-cfg calib/hc/cfg/hc_paper_cp_n3_s1.cfg
 *   cmc --model hc --cfg N=3,MaxEpoch=3,CPMode=TRUE,UnanimityRule=code \
 *           --inv S1_AgreementNonNone --prop S4_EpochMonotone
 *   cmc --model toy --simulate --seed 1 --walks 1000 --depth 30
 *   cmc --model toy-bug1 --trace-out t.replay ; cmc --model toy-bug1 --replay t.replay
 *
 * Output follows TLC's wording so the two can be compared line by line:
 * "N states generated, M distinct states found, K states left on queue."
 * and "The depth of the complete state graph search is D." (init = 1).
 */
#include "cmc.h"
#include "cmc_proc.h"
#include "cmc_spec.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* The model table of the program that called cmc_main(). */
static const CmcModelEntry *models;
static size_t nmodels;

#define MAXP 64

static void
usage(void)
{
	fprintf(stderr,
			"usage: cmc --model-file FILE [--binary PATH]\n"
			"         [--bfs | --simulate [--seed N] [--walks K]] [--depth D]\n"
			"         [--max-states N] [--time S] [--crashes N] [--drops N] [--timeouts N]\n"
			"       cmc --model NAME [--tlc-cfg FILE | --cfg k=v,...]\n"
			"         [--inv NAME[,NAME]] [--prop NAME[,NAME]] [--all-props]\n"
			"         [--bfs | --dfs DEPTH | --simulate [--seed N] [--walks K] [--depth D]\n"
			"          | --replay FILE] [--det] [--max-states N] [--time S]\n"
			"         [--trace-out FILE] [--progress S] [--quiet] [--list]\n"
			"         [--clients N] [--drops N] [--dups N] [--crashes N]\n"
			"         [--net-cap N] [--persist-cap N] [--reply-cap N]\n"
			"         [--net fifo|bag] [--live-steps K]\n"
			"       cmc --version\n"
			"models:");
	for (size_t k = 0; k < nmodels; k++)
		fprintf(stderr, " %s", models[k].name);
	fprintf(stderr, "\n");
	exit(3);
}

static void
add_names(const char **props, int *np, const char *list)
{
	char	   *dup = strdup(list),
			   *save = NULL;

	for (char *t = strtok_r(dup, ",", &save); t; t = strtok_r(NULL, ",", &save))
	{
		if (*np >= MAXP - 1)
			usage();
		props[(*np)++] = strdup(t);
	}
	free(dup);
}

static char *
trim(char *s)
{
	char	   *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = 0;
	return s;
}

/* Reads a TLC .cfg: CONSTANTS k = v, INVARIANTS and PROPERTIES names. */
static void
read_tlc_cfg(const char *path, char *cfg, size_t cfglen, const char **props, int *np)
{
	FILE	   *f = fopen(path, "r");
	char		line[512];
	int			sect = 0;

	if (!f)
	{
		fprintf(stderr, "cmc: cannot open %s\n", path);
		exit(3);
	}
	cfg[0] = 0;
	while (fgets(line, sizeof(line), f))
	{
		char	   *s = trim(line),
				   *eq;

		if (!*s || (s[0] == '\\' && s[1] == '*'))
			continue;
		if (!strcmp(s, "CONSTANTS") || !strcmp(s, "CONSTANT"))
		{
			sect = 1;
			continue;
		}
		if (!strcmp(s, "INVARIANTS") || !strcmp(s, "INVARIANT") ||
			!strcmp(s, "PROPERTIES") || !strcmp(s, "PROPERTY"))
		{
			sect = 2;
			continue;
		}
		if (!strncmp(s, "SPECIFICATION", 13))
		{
			sect = 0;
			continue;
		}
		if (sect == 1 && (eq = strchr(s, '=')))
		{
			char	   *k,
					   *v;
			char		item[256];

			*eq = 0;
			k = trim(s);
			v = trim(eq + 1);
			snprintf(item, sizeof(item), "%s%s=%s", cfg[0] ? "," : "", k, v);
			/* "Writes = {w1, w2}": drop the comma inside the braces */
			for (char *p = item + 1; *p; p++)
				if (*p == ',' && strchr(item, '{') && p > strchr(item, '{'))
					*p = ' ';
			strncat(cfg, item, cfglen - strlen(cfg) - 1);
		}
		else if (sect == 2)
		{
			if (*np >= MAXP - 1)
				usage();
			props[(*np)++] = strdup(s);
		}
	}
	fclose(f);
}

/*
 * docs/research/02 M4: --replay takes the model, cfg and environment from the
 * file's header; flags given on the command line must agree (the engine
 * refuses a conflict).  set[] marks the env flags given explicitly.
 */
static void
replay_defaults(const char *path, const CmcModel **m, char *cfg, size_t cfglen,
				bool cfg_given, CmcOptions *o, const bool *set)
{
	FILE	   *f = fopen(path, "r");
	char		line[700];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
	{
		char	   *s = trim(line);

		if (!strncmp(s, "model ", 6) && !*m)
		{
			for (size_t k = 0; k < nmodels; k++)
				if (!strcmp(models[k].name, s + 6) ||
					!strcmp(models[k].m->name, s + 6))
					*m = models[k].m;
		}
		else if (!strncmp(s, "cfg", 3) && (s[3] == ' ' || !s[3]) && !cfg_given)
			snprintf(cfg, cfglen, "%s", s[3] ? trim(s + 4) : "");
		else if (!strncmp(s, "env ", 4))
		{
			CmcOptions	fo = *o;
			char		net[8] = "";

			if (sscanf(s, "env client=%d drop=%d dup=%d crash=%d net=%d persist=%d"
					   " reply=%d mode=%7s live=%d", &fo.client_budget, &fo.drop_budget,
					   &fo.dup_budget, &fo.crash_budget, &fo.net_cap, &fo.persist_cap,
					   &fo.reply_cap, net, &fo.live_steps) != 9)
				continue;
			fo.net = strcmp(net, "bag") ? CMC_NET_FIFO : CMC_NET_BAG;
#define TAKE(i, fld) do { if (!set[i]) o->fld = fo.fld; } while (0)
			TAKE(0, client_budget);
			TAKE(1, drop_budget);
			TAKE(2, dup_budget);
			TAKE(3, crash_budget);
			TAKE(4, net_cap);
			TAKE(5, persist_cap);
			TAKE(6, reply_cap);
			TAKE(7, net);
			TAKE(8, live_steps);
#undef TAKE
		}
		else if (!strncmp(s, "act ", 4))
			break;
	}
	fclose(f);
}

static const char *
kind_word(const CmcModel *m, const char *prop)
{
	for (int i = 0; i < m->nprops; i++)
		if (!strcmp(m->props[i].name, prop))
			switch (m->props[i].kind)
			{
				case CMC_ACTION_PROP:
					return "Action property";
				case CMC_WITNESS:
					return "Witness";
				case CMC_QUIESCENT:
					return "Quiescence property";
				case CMC_EVENTUALLY:
					return "Liveness property";
				default:
					return "Invariant";
			}
	return "Engine check";
}

/* cmc --model-file FILE: the process front end (cmc_proc.c).  Every
 * option it cannot honour is an error, never silently ignored. */
static int
proc_main(int argc, char **argv)
{
	CmcProcOptions po = {0};
	const char *spec_path = NULL, *bind_path = NULL;
	char *defs[16]; int ndefs = 0;

	po.seed = 1;
	po.walks = 1000;
	po.depth = -1;
	po.crashes = po.drops = po.timeouts = -1;
	for (int i = 1; i < argc; i++)
	{
		const char *a = argv[i];

#define ARG() (i + 1 < argc ? argv[++i] : (usage(), ""))
		if (!strcmp(a, "--model-file"))
			po.model_file = ARG();
		else if (!strcmp(a, "--binary"))
			po.binary = ARG();
		else if (!strcmp(a, "--bfs"))
			po.simulate = 0;
		else if (!strcmp(a, "--simulate"))
			po.simulate = 1;
		else if (!strcmp(a, "--fast"))
			po.fast = 1;
		else if (!strcmp(a, "--seed"))
			po.seed = strtoull(ARG(), NULL, 10);
		else if (!strcmp(a, "--walks"))
			po.walks = strtoull(ARG(), NULL, 10);
		else if (!strcmp(a, "--depth"))
			po.depth = atol(ARG());
		else if (!strcmp(a, "--max-states"))
			po.max_states = strtoull(ARG(), NULL, 10);
		else if (!strcmp(a, "--time"))
			po.time_limit = atof(ARG());
		else if (!strcmp(a, "--crashes"))
			po.crashes = atoi(ARG());
		else if (!strcmp(a, "--drops"))
			po.drops = atoi(ARG());
		else if (!strcmp(a, "--timeouts"))
			po.timeouts = atoi(ARG());
		else if (!strcmp(a, "--refine"))
			{ spec_path = ARG(); po.refine = 1; }
		else if (!strcmp(a, "--bind"))
			bind_path = ARG();
		else if (!strcmp(a, "--define"))
			{ if (ndefs < 16) defs[ndefs++] = (char *) ARG(); else usage(); }
		else
		{
			fprintf(stderr, "cmc: %s is not supported with --model-file\n", a);
			return 3;
		}
#undef ARG
	}
	if (po.refine)
	{
		char err[256];
		if (!bind_path) { fprintf(stderr, "cmc: --refine needs --bind FILE\n"); return 3; }
		if (cmc_refine_load_spec(spec_path, defs, ndefs)) return 3;
		if (cmc_refine_load_bind(bind_path, err, sizeof err)) { fprintf(stderr, "cmc: binding: %s\n", err); return 3; }
	}
	return cmc_proc_main(&po);
}

int
cmc_main(int argc, char **argv, const CmcModelEntry *tab, size_t ntab)
{
	CmcOptions	o;
	const CmcModel *m = NULL;
	const char *props[MAXP];
	int			np = 0;
	char		cfg[512] = "";
	char		err[256];
	bool		all = false,
				list = false,
				cfg_given = false,
				set[9] = {false};
	CmcRun	   *r;
	CmcResult	res;

	models = tab;
	nmodels = ntab;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--model-file") || !strcmp(argv[i], "--refine"))
			return proc_main(argc, argv);
	/* cmc --spec FILE.cml [--define TAG ...]: the abstract-model front
	 * end (cmc_spec.c), model-checking a declarative spec on its own. */
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--spec"))
		{
			const char *path = NULL;
			char *defs[16]; int ndefs = 0;
			for (int j = 1; j < argc; j++)
			{
				if (!strcmp(argv[j], "--spec") && j + 1 < argc) path = argv[++j];
				else if (!strcmp(argv[j], "--define") && j + 1 < argc && ndefs < 16) defs[ndefs++] = argv[++j];
				else { fprintf(stderr, "cmc: %s is not supported with --spec\n", argv[j]); return 3; }
			}
			if (!path) usage();
			return cmc_spec_main(path, defs, ndefs);
		}
	/* cmc --emit-tla FILE.cml [--define TAG ...]: translate to TLA+ */
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--emit-tla"))
		{
			const char *path = NULL;
			char *defs[16]; int ndefs = 0;
			for (int j = 1; j < argc; j++)
			{
				if (!strcmp(argv[j], "--emit-tla") && j + 1 < argc) path = argv[++j];
				else if (!strcmp(argv[j], "--define") && j + 1 < argc && ndefs < 16) defs[ndefs++] = argv[++j];
				else { fprintf(stderr, "cmc: %s is not supported with --emit-tla\n", argv[j]); return 3; }
			}
			if (!path) usage();
			return cmc_emit_tla(path, defs, ndefs);
		}
	cmc_options_default(&o);
	o.progress_s = 60;
	for (int i = 1; i < argc; i++)
	{
		const char *a = argv[i];

#define ARG() (i + 1 < argc ? argv[++i] : (usage(), ""))
		if (!strcmp(a, "--version"))
		{
			printf("cmc (libcmc) %s\n", CMC_VERSION);
			return 0;
		}
		else if (!strcmp(a, "--model"))
		{
			const char *name = ARG();

			for (size_t k = 0; k < nmodels; k++)
				if (!strcmp(models[k].name, name))
					m = models[k].m;
			if (!m)
				usage();
		}
		else if (!strcmp(a, "--cfg"))
		{
			/* appends, so it can override a --tlc-cfg given before it */
			if (cfg[0])
				strncat(cfg, ",", sizeof(cfg) - strlen(cfg) - 1);
			strncat(cfg, ARG(), sizeof(cfg) - strlen(cfg) - 1);
			cfg_given = true;
		}
		else if (!strcmp(a, "--tlc-cfg"))
		{
			read_tlc_cfg(ARG(), cfg, sizeof(cfg), props, &np);
			cfg_given = true;
		}
		else if (!strcmp(a, "--inv") || !strcmp(a, "--prop"))
			add_names(props, &np, ARG());
		else if (!strcmp(a, "--all-props"))
			all = true;
		else if (!strcmp(a, "--bfs"))
			o.search = CMC_BFS;
		else if (!strcmp(a, "--dfs"))
		{
			o.search = CMC_DFS;
			o.max_depth = atoi(ARG());
		}
		else if (!strcmp(a, "--simulate"))
			o.search = CMC_SIMULATE;
		else if (!strcmp(a, "--seed"))
			o.seed = strtoull(ARG(), NULL, 10);
		else if (!strcmp(a, "--walks"))
			o.walks = strtoull(ARG(), NULL, 10);
		else if (!strcmp(a, "--depth"))
			o.max_depth = atoi(ARG());
		else if (!strcmp(a, "--replay"))
		{
			o.search = CMC_REPLAY;
			o.replay_in = ARG();
		}
		else if (!strcmp(a, "--det"))
			o.det = true;
		else if (!strcmp(a, "--max-states"))
			o.max_states = strtoull(ARG(), NULL, 10);
		else if (!strcmp(a, "--time"))
			o.time_limit = atof(ARG());
		else if (!strcmp(a, "--trace-out"))
			o.trace_out = ARG();
		else if (!strcmp(a, "--progress"))
			o.progress_s = atoi(ARG());
		else if (!strcmp(a, "--quiet"))
			o.quiet = true;
		else if (!strcmp(a, "--coverage"))
			o.coverage = true;
		else if (!strcmp(a, "--list"))
			list = true;
		else if (!strcmp(a, "--clients"))
			o.client_budget = atoi(ARG()), set[0] = true;
		else if (!strcmp(a, "--drops"))
			o.drop_budget = atoi(ARG()), set[1] = true;
		else if (!strcmp(a, "--dups"))
			o.dup_budget = atoi(ARG()), set[2] = true;
		else if (!strcmp(a, "--crashes"))
			o.crash_budget = atoi(ARG()), set[3] = true;
		else if (!strcmp(a, "--net-cap"))
			o.net_cap = atoi(ARG()), set[4] = true;
		else if (!strcmp(a, "--persist-cap"))
			o.persist_cap = atoi(ARG()), set[5] = true;
		else if (!strcmp(a, "--reply-cap"))
			o.reply_cap = atoi(ARG()), set[6] = true;
		else if (!strcmp(a, "--net"))
		{
			const char *v = ARG();

			if (!strcmp(v, "fifo"))
				o.net = CMC_NET_FIFO;
			else if (!strcmp(v, "bag"))
				o.net = CMC_NET_BAG;
			else
				usage();
			set[7] = true;
		}
		else if (!strcmp(a, "--live-steps"))
			o.live_steps = atoi(ARG()), set[8] = true;
		else
			usage();
#undef ARG
	}
	if (o.search == CMC_REPLAY)
		replay_defaults(o.replay_in, &m, cfg, sizeof(cfg), cfg_given, &o, set);
	if (!m)
		usage();
	if (list)
	{
		for (int i = 0; i < m->nprops; i++)
			printf("%s%s\n", m->props[i].name, i < m->nalways ? " (always)" : "");
		return 0;
	}
	props[np] = NULL;
	if (!np && !all)
	{
		fprintf(stderr, "cmc: name properties with --inv/--prop/--tlc-cfg, or --all-props\n");
		return 3;
	}
	r = cmc_run_new(m, cfg, all ? NULL : props, &o, err, sizeof(err));
	if (!r)
	{
		fprintf(stderr, "cmc: %s\n", err);
		return 3;
	}
	printf("cmc: model %s, cfg \"%s\", %s, world %zu bytes%s\n", m->name, cfg,
		   o.search == CMC_BFS ? "breadth-first search" :
		   o.search == CMC_DFS ? "depth-first search" :
		   o.search == CMC_SIMULATE ? "random simulation" : "replay",
		   cmc_world_size(r), o.det ? ", determinism gate on" : "");
	fflush(stdout);
	cmc_check(r, &res);

	if (res.verdict == CMC_VIOLATION)
	{
		printf("Error: %s %s is violated.\n", kind_word(m, res.prop), res.prop);
		if (res.why[0])
			printf("  %s\n", res.why);
		if (res.trace_len > 0 && !o.quiet)
		{
			printf("Error: The behavior up to this point is:\n");
			if (res.fair_from)
				printf("  (states 1-%d: the shortest path found; then the fair schedule)\n",
					   res.fair_from);
			cmc_print_trace(r, res.trace, res.trace_len - 1, stdout);
		}
		if (o.trace_out && res.trace_len > 0)
		{
			if (cmc_write_replay(r, o.trace_out, res.trace, res.trace_len - 1) == 0)
				printf("Replay file written to %s (%d actions).\n", o.trace_out,
					   res.trace_len - 1);
			else
				printf("Could not write %s.\n", o.trace_out);
		}
	}
	else if (res.verdict == CMC_PASS)
	{
		if (o.search == CMC_BFS)
		{
			/* optimistic collision probability, as TLC prints it */
			double		n = (double) res.distinct;

			printf("Model checking completed. No error has been found.\n");
			printf("  Estimated probability of a 128-bit fingerprint collision"
				   " (n^2/2^129): %.3g\n", n * n / 6.80564733841877e38);
		}
		else if (o.search == CMC_REPLAY)
			printf("Replay completed: %d states, no property violated.\n", res.trace_len);
		else if (o.search == CMC_DFS)
			printf("Depth-first search completed within depth %d. No error has been found.\n",
				   o.max_depth);
		else
			printf("No error found in %" PRIu64 " walks (not exhaustive).\n", o.walks);
	}
	else
		printf("%s: %s\n", res.verdict == CMC_INCOMPLETE ? "INCOMPLETE (no violation found)"
			   : "ERROR", res.why);
	for (int i = 0; i < res.nwitness; i++)
		printf("Witness %s: %s\n", res.witness_name[i],
			   res.witness_depth[i] ? "reached" : "NOT reached");
	for (int i = 0; i < res.nwitness; i++)
		if (res.witness_depth[i])
			printf("  %s first reached at depth %d\n", res.witness_name[i], res.witness_depth[i]);
	if (o.coverage)
		for (int k = 0; k < 256; k++)
			if (res.cov_gen[k])
			{
				printf("coverage: ");
				cmc_describe_action(r, res.cov_sample[k], stdout);
				printf(" (kind %d): %" PRIu64 ":%" PRIu64 "\n", k, res.cov_new[k], res.cov_gen[k]);
			}
	printf("%" PRIu64 " states generated, %" PRIu64 " distinct states found, %" PRIu64
		   " states left on queue.\n", res.generated, res.distinct, res.left);
	printf("The depth of the complete state graph search is %d.\n", res.depth);
	printf("Finished in %.2fs\n", res.wall_s);
	cmc_result_free(&res);
	cmc_run_free(r);
	for (int i = 0; i < np; i++)
		free((char *) props[i]);
	return res.verdict;
}
