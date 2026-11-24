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
#include "cmc_tla.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The model table of the program that called cmc_main(). */
static const CmcModelEntry *models;
static size_t nmodels;

#define MAXP 64

static _Noreturn void
usage(void)
{
	fprintf(stderr,
			"usage: cmc --model-file FILE [--binary PATH]\n"
			"         [--bfs | --simulate [--seed N] [--walks K]] [--depth D]\n"
			"         [--max-states N] [--time S] [--crashes N] [--drops N] [--timeouts N]\n"
			"         [--trace-out FILE [--trace-format json]]\n"
			"       cmc --spec FILE [--define TAG] [--coverage] [--trace-out FILE [--trace-format itf|json]]\n"
			"       cmc --spec FILE --check-inductive [--define TAG]\n"
			"       cmc --model NAME [--tlc-cfg FILE | --cfg k=v,...]\n"
			"         [--inv NAME[,NAME]] [--prop NAME[,NAME]] [--all-props]\n"
			"         [--bfs | --dfs DEPTH | --simulate [--seed N] [--walks K] [--depth D]\n"
			"          | --replay FILE] [--det] [--max-states N] [--time S]\n"
			"         [--trace-out FILE] [--progress S] [--quiet] [--list]\n"
			"         [--clients N] [--drops N] [--dups N] [--crashes N]\n"
			"         [--net-cap N] [--persist-cap N] [--reply-cap N]\n"
			"         [--net fifo|bag] [--live-steps K] [--workers N] [--queue-mem MB]\n"
			"       cmc --spec | --emit-tla | --emit-cfg FILE.cml\n"
			"         [--define TAG ...] [--no-deadlock]\n"
			"       cmc --spec | --emit-cml FILE.tla [--config FILE.cfg] [--no-deadlock]\n"
			"       cmc --version\n"
			"models:");
	for (size_t k = 0; k < nmodels; k++)
		fprintf(stderr, " %s", models[k].name);
	fprintf(stderr, "\n");
	exit(3);
}

/* argv[*i + 1], the value of option argv[*i], or the usage message when
 * the command line ends first */
static const char *
next_arg(int argc, char **argv, int *i)
{
	if (*i + 1 >= argc)
		usage();
	return argv[++*i];
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

/*
 * The value of option opt: a whole decimal number from lo to hi, or exit
 * 3.  atoi() would read a mistyped "--crashes x" as 0 and check less than
 * was asked, which reads as a pass.
 */
static unsigned long long
num_arg(const char *opt, const char *v, unsigned long long lo, unsigned long long hi)
{
	char	   *e;
	unsigned long long x;

	errno = 0;
	x = strtoull(v, &e, 10);
	if (!isdigit((unsigned char) v[0]) || *e || errno == ERANGE || x < lo || x > hi)
	{
		fprintf(stderr, "cmc: %s needs a number from %llu to %llu, not '%s'\n", opt, lo, hi, v);
		exit(3);
	}
	return x;
}

/* The value of --time: a number of seconds, at least 0, or exit 3 */
static double
seconds_arg(const char *v)
{
	char	   *e;
	double		x = strtod(v, &e);

	if (e == v || *e || !isfinite(x) || x < 0)
	{
		fprintf(stderr, "cmc: --time needs a number of seconds, not '%s'\n", v);
		exit(3);
	}
	return x;
}

/* Appends the setting kv to the comma-separated core settings in
 * cfg[cap], or exits 3: a setting cut off at the end of the buffer
 * would check a different model. */
static void
cfg_append(char *cfg, size_t cap, const char *kv)
{
	size_t		len = strlen(cfg);

	if (len + (len ? 1 : 0) + strlen(kv) >= cap)
	{
		fprintf(stderr, "cmc: the core settings are too long (limit %zu bytes)\n", cap - 1);
		exit(3);
	}
	if (len)
		cfg[len++] = ',';
	strcpy(cfg + len, kv);
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

/* Exits 3 for line ln of TLC .cfg path: a line cmc does not understand
 * asks for a check cmc would not do, so it is never skipped. */
static void
tlc_cfg_refuse(const char *path, int ln, const char *s)
{
	fprintf(stderr, "cmc: %s: cannot read line %d: %s (cmc reads CONSTANTS,"
			" INVARIANTS, PROPERTIES and SPECIFICATION)\n", path, ln, s);
	exit(3);
}

/*
 * Appends the TLC constants on s, "k = v" one or more times, to cfg.  A
 * value is a word, a "string" or a {set}; commas in it become spaces, as
 * a comma separates core settings.  Returns 0, or -1 when s is not that.
 */
static int
tlc_constants(char *s, char *cfg, size_t cfglen)
{
	while (*s)
	{
		char		kv[512];	/* as long as a line, so it always fits */
		char	   *k = s,
				   *v;
		int			kl;

		while (isalnum((unsigned char) *s) || *s == '_')
			s++;
		kl = (int) (s - k);
		s += strspn(s, " \t");
		if (kl == 0 || *s != '=')
			return -1;
		s += 1 + strspn(s + 1, " \t");
		v = s;
		if (*s == '{' || *s == '"')
		{
			s = strchr(s + 1, *s == '{' ? '}' : '"');
			if (!s)
				return -1;
			s++;
		}
		else
			s += strcspn(s, " \t");
		if (s == v)
			return -1;
		snprintf(kv, sizeof(kv), "%.*s=%.*s", kl, k, (int) (s - v), v);
		for (char *p = kv + kl; *p; p++)
			if (*p == ',')
				*p = ' ';
		cfg_append(cfg, cfglen, kv);
		s += strspn(s, " \t");
	}
	return 0;
}

/*
 * Reads a TLC .cfg: CONSTANTS become core settings, INVARIANTS and
 * PROPERTIES the properties to check, and SPECIFICATION is the model's
 * own.  A section keyword may head its items or share their line
 * ("INVARIANT TypeOK").  Any other keyword or line is an error.
 */
static void
read_tlc_cfg(const char *path, char *cfg, size_t cfglen, const char **props, int *np)
{
	static const char *const sections[] = {"CONSTANT", "CONSTANTS", "INVARIANT", "INVARIANTS",
	"PROPERTY", "PROPERTIES", "SPECIFICATION"};
	/* TLC's other keywords, which cmc does not implement */
	static const char *const others[] = {"INIT", "NEXT", "SYMMETRY", "VIEW", "CONSTRAINT",
		"CONSTRAINTS", "ACTION_CONSTRAINT", "ACTION_CONSTRAINTS", "CHECK_DEADLOCK",
	"POSTCONDITION", "ALIAS"};
	FILE	   *f = fopen(path, "r");
	char		line[512];
	int			sect = -1,
				ln = 0;

	if (!f)
	{
		fprintf(stderr, "cmc: cannot open %s\n", path);
		exit(3);
	}
	cfg[0] = 0;
	while (fgets(line, sizeof(line), f))
	{
		char	   *s;
		size_t		kw;

		ln++;
		if (!strchr(line, '\n') && !feof(f))
			tlc_cfg_refuse(path, ln, "the line is too long");
		if ((s = strstr(line, "\\*")))
			*s = 0;				/* a \* comment runs to the end of the line */
		s = trim(line);
		if (!*s)
			continue;
		/* a section keyword: CONSTANT(S) 0-1, INVARIANT(S) and PROPERTY(IES)
		 * 2-5, SPECIFICATION 6 */
		kw = strcspn(s, " \t");
		for (size_t k = 0; k < sizeof(others) / sizeof(others[0]); k++)
			if (strlen(others[k]) == kw && !strncmp(s, others[k], kw))
				tlc_cfg_refuse(path, ln, s);
		for (int k = 0; k < 7; k++)
			if (strlen(sections[k]) == kw && !strncmp(s, sections[k], kw))
			{
				sect = k < 2 ? 0 : k < 6 ? 1 : 2;
				s = trim(s + kw);
			}
		if (sect == 0 && tlc_constants(s, cfg, cfglen) != 0)
			tlc_cfg_refuse(path, ln, s);
		else if (sect == 1)
		{
			for (char *t = s; *t; t += strspn(t, " \t"))
			{
				size_t		n = strcspn(t, " \t");

				for (size_t c = 0; c < n; c++)
					if (!isalnum((unsigned char) t[c]) && t[c] != '_')
						tlc_cfg_refuse(path, ln, s);
				if (*np >= MAXP - 1)
					usage();
				props[(*np)++] = strndup(t, n);
				t += n;
			}
		}
		else if (sect == 2 && *s)
		{
			/* the one name of the specification, which is the model's */
			if (s[strcspn(s, " \t")])
				tlc_cfg_refuse(path, ln, s);
			sect = -1;
		}
		else if (sect == -1)
			tlc_cfg_refuse(path, ln, s);
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

/* --trace-format names the format of the --trace-out file, so one
 * without the other is an error, and so is a format that is neither
 * itf nor json.  *fmt defaults to dflt.  Returns 0, or 3 with a message. */
static int
trace_check(const char **fmt, const char *out, const char *dflt)
{
	if (*fmt && !out)
	{
		fprintf(stderr, "cmc: --trace-format needs --trace-out FILE\n");
		return 3;
	}
	if (!*fmt)
		*fmt = dflt;
	if (strcmp(*fmt, "itf") && strcmp(*fmt, "json"))
	{
		fprintf(stderr, "cmc: --trace-format %s: the formats are itf and json\n", *fmt);
		return 3;
	}
	return 0;
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

#define ARG() next_arg(argc, argv, &i)
		if (!strcmp(a, "--model-file"))
			po.model_file = ARG();
		else if (!strcmp(a, "--binary"))
			po.binary = ARG();
		else if (!strcmp(a, "--bfs"))
			po.simulate = 0;
		else if (!strcmp(a, "--simulate"))
			po.simulate = 1;
		else if (!strcmp(a, "--seed"))
			po.seed = num_arg(a, ARG(), 0, UINT64_MAX);
		else if (!strcmp(a, "--walks"))
			po.walks = num_arg(a, ARG(), 1, UINT64_MAX);
		else if (!strcmp(a, "--depth"))
			po.depth = (long) num_arg(a, ARG(), 1, 1000000);
		else if (!strcmp(a, "--max-states"))
			po.max_states = num_arg(a, ARG(), 0, SIZE_MAX);
		else if (!strcmp(a, "--time"))
			po.time_limit = seconds_arg(ARG());
		else if (!strcmp(a, "--crashes"))
			po.crashes = (int) num_arg(a, ARG(), 0, 255);
		else if (!strcmp(a, "--drops"))
			po.drops = (int) num_arg(a, ARG(), 0, 255);
		else if (!strcmp(a, "--timeouts"))
			po.timeouts = (int) num_arg(a, ARG(), 0, 255);
		else if (!strcmp(a, "--refine"))
			{ spec_path = ARG(); po.refine = 1; }
		else if (!strcmp(a, "--refine-steps"))
			{ po.refine_steps = 1; }
		else if (!strcmp(a, "--bind"))
			bind_path = ARG();
		else if (!strcmp(a, "--define"))
			{ if (ndefs < 16) defs[ndefs++] = (char *) ARG(); else usage(); }
		else if (!strcmp(a, "--trace-out"))
			po.trace_out = ARG();
		else if (!strcmp(a, "--trace-format"))
			po.trace_fmt = ARG();
		else
		{
			fprintf(stderr, "cmc: %s is not supported with --model-file\n", a);
			return 3;
		}
#undef ARG
	}
	if (!po.model_file) { fprintf(stderr, "cmc: --refine needs --model-file FILE\n"); return 3; }
	if (trace_check(&po.trace_fmt, po.trace_out, "json")) return 3;
	/* a world is node records and bytes, not TLA+ values */
	if (!strcmp(po.trace_fmt, "itf")) { fprintf(stderr, "cmc: --trace-format itf is for --spec; --model-file writes json\n"); return 3; }
	if (po.refine_steps && !po.refine) { fprintf(stderr, "cmc: --refine-steps needs --refine SPEC\n"); return 3; }
	if (po.refine)
	{
		char err[256];
		if (!bind_path) { fprintf(stderr, "cmc: --refine needs --bind FILE\n"); return 3; }
		if (cmc_refine_load_spec(spec_path, defs, ndefs)) return 3;
		if (cmc_refine_load_bind(bind_path, err, sizeof err)) { fprintf(stderr, "cmc: binding: %s\n", err); return 3; }
	}
	return cmc_proc_main(&po);
}

/* --check-inductive was given (spec_main() sets it, spec_run() reads it) */
static bool g_ind;

static int tla_main(const char *path, const char *cfg, const char *mode, char **defs,
					int ndefs, int nodl, const char *trace_out, const char *trace_fmt);
static int spec_run(const char *path, const char *mode, char **defs, int ndefs, int nodl,
					const char *trace_out, const char *trace_fmt);

/* cmc --spec FILE, cmc --emit-tla FILE, cmc --emit-cfg FILE or cmc
 * --emit-cml FILE.tla (mode), with --config FILE.cfg for a TLA+ module,
 * with --define TAG ... and --no-deadlock: every other option is an
 * error, and so is an option with no value */
static int
spec_main(int argc, char **argv, const char *mode)
{
	const char *path = NULL,
			   *cfg = NULL,
			   *trace_out = NULL,
			   *trace_fmt = NULL;
	char	   *defs[16];
	int			ndefs = 0;
	int			nodl = 0;
	bool		spec = !strcmp(mode, "--spec");

	g_ind = false;
	for (int j = 1; j < argc; j++)
	{
		bool		def = !strcmp(argv[j], "--define"),
					conf = !strcmp(argv[j], "--config"),
					out = spec && !strcmp(argv[j], "--trace-out"),
					fmt = spec && !strcmp(argv[j], "--trace-format");

		if (!strcmp(argv[j], "--no-deadlock"))
		{
			nodl = 1;
			continue;
		}
		if (spec && !strcmp(argv[j], "--check-inductive"))
		{
			g_ind = true;
			continue;
		}
		if (spec && !strcmp(argv[j], "--coverage"))
		{
			cmc_spec_set_coverage(1);
			continue;
		}
		if (!def && !conf && !out && !fmt && strcmp(argv[j], mode))
		{
			fprintf(stderr, "cmc: %s is not supported with %s\n", argv[j], mode);
			return 3;
		}
		if (j + 1 == argc || (def && ndefs == 16))
			usage();
		if (def)
			defs[ndefs++] = argv[++j];
		else if (conf)
			cfg = argv[++j];
		else if (out)
			trace_out = argv[++j];
		else if (fmt)
			trace_fmt = argv[++j];
		else
			path = argv[++j];
	}
	if (!path)
		usage();
	return tla_main(path, cfg, mode, defs, ndefs, nodl, trace_out, trace_fmt);
}

/* run mode on the model at path: a .cml file, or a TLA+ module (named
 * *.tla, or read with --config) translated to .cml first (cmc_tla.c) */
static int
tla_main(const char *path, const char *cfg, const char *mode, char **defs,
		 int ndefs, int nodl, const char *trace_out, const char *trace_fmt)
{
	size_t		n = strlen(path);
	bool		tla = cfg || (n > 4 && !strcmp(path + n - 4, ".tla"));
	char	   *text = NULL,
				cfgbuf[4096],
				err[512];
	int			rc;

	if (!tla && !strcmp(mode, "--emit-cml"))
	{
		fprintf(stderr, "cmc: --emit-cml reads a TLA+ module (FILE.tla or --config FILE)\n");
		return 3;
	}
	if (tla)
	{
		if (!cfg)
		{
			/* TLC's default: the .cfg beside the module */
			snprintf(cfgbuf, sizeof cfgbuf, "%.*s.cfg", (int) (n - 4), path);
			cfg = cfgbuf;
		}
		if (ndefs)
		{
			fprintf(stderr, "cmc: --define is for .cml models; a TLA+ module's constants are set in its .cfg\n");
			return 3;
		}
		text = cmc_tla_translate(path, cfg, err, sizeof err);
		if (!text)
		{
			fprintf(stderr, "cmc: %s\n", err);
			return 3;
		}
		if (!strcmp(mode, "--emit-cml"))
		{
			fputs(text, stdout);
			free(text);
			return 0;
		}
		cmc_spec_set_text(path, text);
	}
	rc = spec_run(path, mode, defs, ndefs, nodl, trace_out, trace_fmt);
	cmc_spec_set_text(NULL, NULL);
	free(text);
	return rc;
}

static int
spec_run(const char *path, const char *mode, char **defs, int ndefs, int nodl,
		 const char *trace_out, const char *trace_fmt)
{
	bool		spec = !strcmp(mode, "--spec");

	if (!spec)
	{
		if (trace_out || trace_fmt) { fprintf(stderr, "cmc: --trace-out is for --spec\n"); return 3; }
		if (!strcmp(mode, "--emit-cfg"))
			return cmc_emit_cfg(path, defs, ndefs, nodl);
		return cmc_emit_tla(path, defs, ndefs, nodl);
	}
	if (trace_check(&trace_fmt, trace_out, "itf"))
		return 3;
	if (g_ind)
	{
		/* an induction check has no behaviour to write, and no deadlock */
		if (trace_out || nodl) { fprintf(stderr, "cmc: --check-inductive takes neither --trace-out nor --no-deadlock\n"); return 3; }
		return cmc_spec_inductive(path, defs, ndefs);
	}
	return cmc_spec_main(path, defs, ndefs, nodl, trace_out, trace_fmt);
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
	 * end (cmc_spec.c), model-checking a declarative spec on its own;
	 * cmc --emit-tla FILE.cml [--define TAG ...]: translate it to TLA+,
	 * and cmc --emit-cfg FILE.cml: write the TLC .cfg that goes with it */
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--spec") || !strcmp(argv[i], "--emit-tla") ||
			!strcmp(argv[i], "--emit-cfg") || !strcmp(argv[i], "--emit-cml"))
			return spec_main(argc, argv, argv[i]);
	cmc_options_default(&o);
	o.progress_s = 60;
	for (int i = 1; i < argc; i++)
	{
		const char *a = argv[i];

#define ARG() next_arg(argc, argv, &i)
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
			cfg_append(cfg, sizeof(cfg), ARG());
			cfg_given = true;
		}
		else if (!strcmp(a, "--tlc-cfg"))
		{
			char		tcfg[sizeof(cfg)];

			/* appends, as --cfg does, so a later --cfg overrides it */
			read_tlc_cfg(ARG(), tcfg, sizeof(tcfg), props, &np);
			if (tcfg[0])
				cfg_append(cfg, sizeof(cfg), tcfg);
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
			o.max_depth = (int) num_arg(a, ARG(), 1, INT_MAX);
		}
		else if (!strcmp(a, "--simulate"))
			o.search = CMC_SIMULATE;
		else if (!strcmp(a, "--seed"))
			o.seed = num_arg(a, ARG(), 0, UINT64_MAX);
		else if (!strcmp(a, "--walks"))
			o.walks = num_arg(a, ARG(), 1, UINT64_MAX);
		else if (!strcmp(a, "--depth"))
			o.max_depth = (int) num_arg(a, ARG(), 1, INT_MAX);
		else if (!strcmp(a, "--replay"))
		{
			o.search = CMC_REPLAY;
			o.replay_in = ARG();
		}
		else if (!strcmp(a, "--det"))
			o.det = true;
		else if (!strcmp(a, "--max-states"))
			o.max_states = num_arg(a, ARG(), 0, UINT64_MAX);
		else if (!strcmp(a, "--time"))
			o.time_limit = seconds_arg(ARG());
		else if (!strcmp(a, "--trace-out"))
			o.trace_out = ARG();
		else if (!strcmp(a, "--progress"))
			o.progress_s = (int) num_arg(a, ARG(), 0, INT_MAX);
		else if (!strcmp(a, "--quiet"))
			o.quiet = true;
		else if (!strcmp(a, "--coverage"))
			o.coverage = true;
		else if (!strcmp(a, "--list"))
			list = true;
		else if (!strcmp(a, "--clients"))
			o.client_budget = (int) num_arg(a, ARG(), 0, 255), set[0] = true;
		else if (!strcmp(a, "--drops"))
			o.drop_budget = (int) num_arg(a, ARG(), 0, 255), set[1] = true;
		else if (!strcmp(a, "--dups"))
			o.dup_budget = (int) num_arg(a, ARG(), 0, 255), set[2] = true;
		else if (!strcmp(a, "--crashes"))
			o.crash_budget = (int) num_arg(a, ARG(), 0, 255), set[3] = true;
		else if (!strcmp(a, "--net-cap"))
			o.net_cap = (int) num_arg(a, ARG(), 0, 255), set[4] = true;
		else if (!strcmp(a, "--persist-cap"))
			o.persist_cap = (int) num_arg(a, ARG(), 0, 255), set[5] = true;
		else if (!strcmp(a, "--reply-cap"))
			o.reply_cap = (int) num_arg(a, ARG(), 0, 255), set[6] = true;
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
			o.live_steps = (int) num_arg(a, ARG(), 1, INT_MAX), set[8] = true;
		else if (!strcmp(a, "--workers"))
			o.workers = (int) num_arg(a, ARG(), 1, 256);
		else if (!strcmp(a, "--queue-mem"))
			o.queue_mem = num_arg(a, ARG(), 1, UINT64_MAX >> 20) << 20;
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
