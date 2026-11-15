/*
 * cmc_proc.c: the process front end -- cmc --model-file FILE checks a
 * program as it ships (docs/model-file.md, cmc(7)).
 *
 * Each node is the target binary, run unmodified as its own process
 * under seccomp user notification, with src/cmc_preload.c preloaded for
 * a deterministic heap and a logical clock.  The checker is the network:
 * it intercepts socket/bind/listen/connect/accept/read/write/close and
 * the clock, no real socket ever forms, and a node blocked in recv or
 * accept is a deferred notification the checker answers when the search
 * says so.  When every node is blocked or has exited the world is
 * quiescent: each node's canonical state (the fd-3 hook, and fd 193 for
 * what survives a crash) is read, the invariants are checked, and the
 * enabled actions are the next steps.
 *
 * Search: stateless replay over action sequences.  To explore a
 * sequence, relaunch all nodes, drive to quiescence, apply the
 * sequence's actions (re-driving to quiescence after each), then
 * snapshot.  An action is ACCEPT, DELIVER, EOF, TIMEOUT, CRASH or DROP
 * (enum_actions).  Connections are routed by the port a node dials (node
 * i listens on 127.0.0.1:20000+i), so any topology the nodes set up
 * works; each connection is a FIFO channel, as TCP is.  BFS gives the
 * shortest counterexample; simulation takes seeded random walks.
 *
 * Verdicts are honest (docs/steering/project.md).  The fingerprint holds
 * everything the future depends on: node states, open connections,
 * channels in order, pending connects, replies, budgets left, the
 * clock.  A search stopped by a depth, state or time limit is
 * INCOMPLETE, never a pass.  A model-file key the loader does not
 * implement, a cap the run exceeds, a write longer than one message, a
 * node silent in an unmodelled call, a missing preload, or a replay that
 * diverges is an error, never ignored or truncated.
 *
 * Exit status is cmc(1)'s: 0 no violation and the search was complete;
 * 1 a violation (shortest action sequence printed, by name) or an engine
 * rule broken; 2 incomplete; 3 a usage, model-file or setup error.
 *
 * Linux x86-64 only: elsewhere cmc_proc_main() says so and returns 3.
 */
#define _GNU_SOURCE				/* seccomp, ptrace, process_vm_readv */
#include "cmc_proc.h"
#include "cmc_spec.h"

#if defined(__linux__) && defined(__x86_64__)

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <linux/sched.h>			/* CLONE_THREAD, struct clone_args */
#include <linux/seccomp.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
static int seccomp(unsigned op, unsigned flags, void *a) { return (int) syscall(SYS_seccomp, op, flags, a); }

#define BASE 20000				/* node i listens on 127.0.0.1:BASE+i */
#define MAXNODES 8
#define MAXMSG 128				/* messages in flight, total */
#define MSGLEN 4096			/* hard cap on a message body / write; [target] max_write bounds a model within it */
#define MSGLEN_DEFAULT 64		/* the default per-write limit; a model raises it with [target] max_write */
#define MAXREP 64				/* client replies recorded */
#define MAXCONN 16				/* connections per node */
#define MAXPOLL 16				/* pollfd entries read in one poll() / epoll interest list */
#define MAXEFD 8				/* eventfds per node */
#define MAXEPOLL 4				/* epoll instances per node */
#define LULL_MS 10000			/* a running node silent this long is an error */
#define SNAP 64					/* canonical state bytes per node */
/* The durable canonical-emit stream.  fd 3 is the volatile stream; the
 * durable one must avoid the low fds the dynamic loader uses while
 * handling LD_PRELOAD (dup'ing a pipe onto fd 4 breaks the preload on
 * glibc 2.44), and stay below the 500 threshold that marks a channel. */
#define DURABLE_FD 193
/* actions enabled at one point: per node, per thread at most MAXPOLL
 * wakes or MAXNODES + 1 accepts plus a delivery/EOF and a timeout; then
 * one crash per node and one drop per channel.  With up to MAXTHREAD
 * threads the per-thread term dominates, so the bound scales by it.
 * One restart per node also fits the trailing MAXNODES * (MAXNODES + 1)
 * term alongside the per-node crashes. */
#define MAXACT (MAXNODES * MAXTHREAD * (MAXPOLL + MAXNODES + 4) + MAXNODES * (MAXNODES + 1))
#define MAXINV 8
#define MAXFIELD 16				/* named byte offsets in [fields] */

/* =================================================================== */
/* the model file (docs/model-file.md)                                 */
/* =================================================================== */
typedef struct { char name[32]; char expr[256]; int eventually; int seen; } Inv;
typedef struct { char name[32]; int off; } Field;
static struct
{
	char		binary[4096];	/* [target] binary, resolved against the file's directory */
	int			nodes;
	int			client_write;	/* -1 = no client; else the value it writes to node 0 */
	int			timeouts, crashes, drops, restarts;
	int			max_write;		/* largest single write a node may make (<= MSGLEN) */
	char		args[512];		/* extra argv after the node id, space-separated (e.g. flags) */
	long		depth;			/* -1 = unbounded */
	Inv			inv[MAXINV];
	int			ninv;
	Field		field[MAXFIELD];/* [fields] name = byte offset, for node[i].name */
	int			nfield;
}			M;

static char *trim(char *s){ while(*s&&isspace((unsigned char)*s))s++; char*e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }

static int check_invariant(const char *expr);

/* a whole-string integer in [lo, hi], or the model-file error */
static int
int_value(const char *path, int ln, const char *k, const char *v, long lo, long hi, long *out)
{
	char	   *e;
	long		x = strtol(v, &e, 10);

	if (!*v || *e || x < lo || x > hi)
	{
		fprintf(stderr, "cmc: %s:%d: %s = \"%s\": expected an integer from %ld to %ld\n", path, ln, k, v, lo, hi);
		return -1;
	}
	*out = x;
	return 0;
}

/* Parse the model file.  Every section and key it does not implement is
 * an error: a mistyped `crashs = 1` that was ignored would check the
 * model with no crashes and could pass a protocol that loses data. */
static int
load_model(const char *path)
{
	FILE	   *f = fopen(path, "r");
	char		line[512], sect[32] = "";
	int			ln = 0, seen_nodes = 0;
	long		x;

	if (!f) { fprintf(stderr, "cmc: cannot open model file %s: %s\n", path, strerror(errno)); return -1; }
	memset(&M, 0, sizeof M);
	M.nodes = 1; M.client_write = -1; M.depth = -1; M.max_write = MSGLEN_DEFAULT;
	while (fgets(line, sizeof line, f))
	{
		ln++;
		if (!strchr(line, '\n') && !feof(f)) { fprintf(stderr, "cmc: %s:%d: line too long\n", path, ln); goto bad; }
		char *s = trim(line); if (!*s || *s == '#') continue;
		if (*s == '[')
		{
			char *e = strchr(s, ']');
			if (!e || *trim(e + 1)) { fprintf(stderr, "cmc: %s:%d: malformed section header\n", path, ln); goto bad; }
			*e = 0;
			if (strcmp(s + 1, "target") && strcmp(s + 1, "environment") && strcmp(s + 1, "invariants") && strcmp(s + 1, "fields"))
			{ fprintf(stderr, "cmc: %s:%d: unknown section [%s] (target, environment, fields, invariants)\n", path, ln, s + 1); goto bad; }
			snprintf(sect, sizeof sect, "%s", s + 1);
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) { fprintf(stderr, "cmc: %s:%d: expected key = value\n", path, ln); goto bad; }
		*eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		if (*v == '"')
		{
			v++; char *q = strchr(v, '"');
			if (!q || (*trim(q + 1) && *trim(q + 1) != '#')) { fprintf(stderr, "cmc: %s:%d: unterminated or trailing text after a quoted value\n", path, ln); goto bad; }
			*q = 0;
		}
		else { char *h = strchr(v, '#'); if (h) *h = 0; v = trim(v); }
		if (!*sect) { fprintf(stderr, "cmc: %s:%d: %s outside a section\n", path, ln, k); goto bad; }
		if (!strcmp(sect, "target") && !strcmp(k, "binary"))
		{
			if (!*v) { fprintf(stderr, "cmc: %s:%d: binary is empty\n", path, ln); goto bad; }
			/* relative to the model file's directory */
			const char *sl = strrchr(path, '/');
			if (*v == '/' || !sl) snprintf(M.binary, sizeof M.binary, "%s", v);
			else snprintf(M.binary, sizeof M.binary, "%.*s/%s", (int) (sl - path), path, v);
		}
		else if (!strcmp(sect, "target") && !strcmp(k, "nodes"))
		{ if (int_value(path, ln, k, v, 1, MAXNODES, &x)) goto bad; M.nodes = (int) x; seen_nodes = 1; }
		else if (!strcmp(sect, "target") && !strcmp(k, "max_write"))
		{ if (int_value(path, ln, k, v, 1, MSGLEN, &x)) goto bad; M.max_write = (int) x; }
		else if (!strcmp(sect, "target") && !strcmp(k, "args"))
		{ snprintf(M.args, sizeof M.args, "%s", v); }
		else if (!strcmp(sect, "target") && !strcmp(k, "state"))
		{
			if (strcmp(v, "canon"))
			{ fprintf(stderr, "cmc: %s:%d: state = %s is not supported; the only state is canon (the fd-3 hook)\n", path, ln, v); goto bad; }
		}
		else if (!strcmp(sect, "environment") && !strcmp(k, "client_write"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.client_write = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "timeouts"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.timeouts = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "crashes"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.crashes = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "drops"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.drops = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "restarts"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.restarts = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "depth"))
		{ if (int_value(path, ln, k, v, 0, 1000000, &x)) goto bad; M.depth = x; }
		else if (!strcmp(sect, "fields"))
		{
			/* name = byte offset, so an invariant can say node[i].epoch
			 * instead of node[i][7] and not depend on the hook's layout */
			if (M.nfield >= MAXFIELD) { fprintf(stderr, "cmc: %s:%d: more than %d fields\n", path, ln, MAXFIELD); goto bad; }
			if (strlen(k) >= sizeof M.field[0].name) { fprintf(stderr, "cmc: %s:%d: field name too long\n", path, ln); goto bad; }
			for (int i = 0; i < M.nfield; i++)
				if (!strcmp(M.field[i].name, k)) { fprintf(stderr, "cmc: %s:%d: field %s defined twice\n", path, ln, k); goto bad; }
			if (int_value(path, ln, k, v, 0, 254, &x)) goto bad;
			snprintf(M.field[M.nfield].name, sizeof M.field[0].name, "%s", k);
			M.field[M.nfield].off = (int) x;
			M.nfield++;
		}
		else if (!strcmp(sect, "invariants"))
		{
			if (M.ninv >= MAXINV) { fprintf(stderr, "cmc: %s:%d: more than %d invariants\n", path, ln, MAXINV); goto bad; }
			if (strlen(k) >= sizeof M.inv[0].name || strlen(v) >= sizeof M.inv[0].expr)
			{ fprintf(stderr, "cmc: %s:%d: invariant name or expression too long\n", path, ln); goto bad; }
			for (int i = 0; i < M.ninv; i++)
				if (!strcmp(M.inv[i].name, k)) { fprintf(stderr, "cmc: %s:%d: invariant %s defined twice\n", path, ln, k); goto bad; }
			/* a leading `eventually ` makes it a reachability check: some
			 * reached state must satisfy it, reported at the end of the
			 * search.  Not a fairness-based temporal property -- a bounded
			 * search only tells you some reachable state satisfies it. */
			int evv = 0;
			if (!strncmp(v, "eventually", 10) && (v[10] == ' ' || v[10] == '\t'))
			{ evv = 1; v += 10; while (*v == ' ' || *v == '\t') v++; }
			snprintf(M.inv[M.ninv].name, sizeof M.inv[0].name, "%s", k);
			snprintf(M.inv[M.ninv].expr, sizeof M.inv[0].expr, "%s", v);
			M.inv[M.ninv].eventually = evv;
			M.ninv++;
		}
		else { fprintf(stderr, "cmc: %s:%d: unknown key %s in [%s]\n", path, ln, k, sect); goto bad; }
	}
	fclose(f);
	if (!seen_nodes) { fprintf(stderr, "cmc: %s: [target] nodes is required\n", path); return -1; }
	if (M.ninv == 0) { fprintf(stderr, "cmc: %s: no invariant to check ([invariants] is empty)\n", path); return -1; }
	/* every invariant must parse before the search starts */
	for (int i = 0; i < M.ninv; i++)
		if (check_invariant(M.inv[i].expr))
		{ fprintf(stderr, "cmc: %s: invariant %s: cannot parse \"%s\"\n", path, M.inv[i].name, M.inv[i].expr); return -1; }
	return 0;
bad:
	fclose(f);
	return -1;
}

/* =================================================================== */
/* invariant DSL (from mc.c), generalized to node[i] and a ghost reply  */
/* log `replies` (each entry: .src node, .val)                          */
/* =================================================================== */
typedef struct {
	const unsigned char *node[MAXNODES]; int node_len[MAXNODES]; int nnodes;
	const unsigned char *repl_src; const unsigned char *repl_val; int nrepl;
	char var[32]; long val; int varset;
	/* a second bound var for reply fields (r.src / r.val): when ranging
	 * over replies, `var` binds the index and r.NAME reads the arrays */
	int ranging_replies;
	int checking;			/* parsing only: reply fields read as 0 */
	/* index variables bound by `all I in nodes` / `some I in nodes`: each
	 * names a node index, usable as node[I], node[I][k], node[I].field */
	char ivar[MAXNODES][32]; long ival[MAXNODES]; int niv;
} Ctx;

static void skipws(const char **p){ while(**p&&isspace((unsigned char)**p))(*p)++; }
static int word(const char **p,const char*w){ skipws(p); size_t n=strlen(w); if(!strncmp(*p,w,n)&&!isalnum((unsigned char)(*p)[n])&&(*p)[n]!='_'){*p+=n;return 1;} return 0; }
/* resolve a node-index expression: a literal integer or a bound index
 * variable.  On success advances *p and sets *out; returns 0, else -1. */
static int
node_index(const char **p, Ctx *c, long *out)
{
	skipws(p);
	if (isdigit((unsigned char) **p)) { *out = strtol(*p, (char **) p, 10); return 0; }
	const char *q = *p; char id[32]; int n = 0;
	while ((isalnum((unsigned char) *q) || *q == '_') && n < 31) id[n++] = *q++;
	id[n] = 0;
	for (int i = 0; i < c->niv; i++)
		if (!strcmp(id, c->ivar[i])) { *out = c->ival[i]; *p = q; return 0; }
	return -1;
}
static int eval_expr(const char **p, Ctx *c);
static int get_arith(const char **p, Ctx *c, long *out);
enum { SET_BYTES, SET_REPLIES, SET_NODES };	/* a set a quantifier ranges over */

/* Bind variable `var` over SET (kind/set/slen from get_set) into a copy
 * of `c`, evaluate the boolean body at `*p` once per element, and call
 * visit(r, ctx) for each result r.  Advances *p past the body.  An
 * empty set still parses the body once (checking=1) so a broken body is
 * never hidden.  Returns 0, or -1 on a parse error in the body.  Both
 * the all/some quantifier and `count` go through this, so their binding
 * can never diverge. */
static int
iter_set(const char **p, Ctx *c, const char *var, int kind,
		 const unsigned char *set, int slen, void (*visit)(int r, void *), void *arg)
{
	const char *body = *p;
	Ctx inner = *c;
	int slot = -1;
	if (kind == SET_NODES)
	{
		if (inner.niv >= MAXNODES) return -1;
		slot = inner.niv;
		snprintf(inner.ivar[slot], sizeof inner.ivar[0], "%s", var);
		inner.niv = slot + 1;
	}
	else
	{
		inner.varset = 1; snprintf(inner.var, sizeof inner.var, "%s", var);
		inner.ranging_replies = (kind == SET_REPLIES);
	}
	for (int i = 0; i < slen; i++)
	{
		const char *bp = body;
		if (kind == SET_NODES) inner.ival[slot] = i;
		else inner.val = (kind == SET_REPLIES) ? i : set[i];
		int r = eval_expr(&bp, &inner); if (r < 0) return -1;
		visit(r, arg);
		*p = bp;
	}
	if (slen == 0)
	{
		const char *bp = body;
		if (kind == SET_NODES) inner.ival[slot] = 0;
		else { inner.val = 0; inner.checking = 1; }
		if (eval_expr(&bp, &inner) < 0) return -1;
		*p = bp;
	}
	return 0;
}
struct count_acc { long n; };
static void count_visit(int r, void *a) { if (r) ((struct count_acc *) a)->n++; }
/* all/some accumulator: fold each body result for the quantifier */
struct qc_acc { int isall; int result; };
static void qc_visit(int r, void *a) {
	struct qc_acc *q = a;
	if (q->isall && !r) q->result = 0;
	if (!q->isall && r) q->result = 1;
}

/* a set yields bytes to range over, OR the reply log (special) */
static int
get_set(const char **p, Ctx *c, const unsigned char **buf, int *len, int *kind)
{
	skipws(p);
	if (word(p, "replies")) { *kind = SET_REPLIES; *len = c->nrepl; *buf = NULL; return 0; }
	if (word(p, "nodes")) { *kind = SET_NODES; *len = c->nnodes; *buf = NULL; return 0; }
	if (word(p, "node"))
	{
		skipws(p); if (**p != '[') return -1; (*p)++;
		long i; if (node_index(p, c, &i)) return -1;
		skipws(p); if (**p != ']') return -1; (*p)++;
		if (i < 0 || i >= c->nnodes) return -1;
		*kind = SET_BYTES; *buf = c->node[i]; *len = c->node_len[i]; return 0;
	}
	return -1;
}

/* a term yields a value: NUMBER, bound index/reply var, node[i][k], or
 * node[i].field (a named byte offset from [fields]) */
static int
get_term(const char **p, Ctx *c, long *out)
{
	skipws(p);
	if (isdigit((unsigned char) **p)) { *out = strtol(*p, (char **) p, 10); return 0; }
	/* a parenthesised arithmetic sub-expression (so (count ...) * 2 works) */
	if (**p == '(') { (*p)++; if (get_arith(p, c, out)) return -1; skipws(p); if (**p != ')') return -1; (*p)++; return 0; }
	const char *before = *p;
	char id[32]; int n = 0; while ((isalnum((unsigned char) **p) || **p == '_') && n < 31) id[n++] = *(*p)++; id[n] = 0;
	if (n == 0) return -1;
	/* count V in SET : PRED -- the number of elements of SET for which
	 * PRED holds (a cardinality).  Binds V exactly as the quantifiers do. */
	if (!strcmp(id, "count"))
	{
		skipws(p); char var[32]; int m = 0; while ((isalnum((unsigned char) **p) || **p == '_') && m < 31) var[m++] = *(*p)++; var[m] = 0;
		if (m == 0 || !word(p, "in")) return -1;
		const unsigned char *set; int slen, kind;
		if (get_set(p, c, &set, &slen, &kind)) return -1;
		skipws(p); if (**p != ':') return -1; (*p)++;
		struct count_acc acc = {0};
		if (iter_set(p, c, var, kind, set, slen, count_visit, &acc)) return -1;
		*out = acc.n; return 0;
	}
	/* node[i][k] or node[i].field : a byte of node i's canonical state */
	if (!strcmp(id, "node"))
	{
		skipws(p); if (**p != '[') return -1; (*p)++;
		long i; if (node_index(p, c, &i)) return -1;
		skipws(p); if (**p != ']') return -1; (*p)++;
		long k;
		skipws(p);
		if (**p == '[') { (*p)++; k = strtol(*p, (char **) p, 10); skipws(p); if (**p != ']') return -1; (*p)++; }
		else if (**p == '.')
		{
			(*p)++; char fn[32]; int m = 0; while ((isalnum((unsigned char) **p) || **p == '_') && m < 31) fn[m++] = *(*p)++; fn[m] = 0;
			int fi = -1; for (int j = 0; j < M.nfield; j++) if (!strcmp(M.field[j].name, fn)) fi = j;
			if (fi < 0) return -1;			/* unknown field = parse error */
			k = M.field[fi].off;
		}
		else return -1;
		if (i < 0 || i >= c->nnodes || k < 0 || k >= c->node_len[i]) { *out = -1; return 0; }
		*out = c->node[i][k]; return 0;
	}
	/* r.src / r.val when ranging over replies */
	if (c->ranging_replies && c->varset && !strcmp(id, c->var))
	{
		if (**p == '.') { (*p)++; char fld[8]; int m = 0; while (isalpha((unsigned char) **p) && m < 7) fld[m++] = *(*p)++; fld[m] = 0;
			if (!strcmp(fld, "src")) { *out = c->checking ? 0 : c->repl_src[c->val]; return 0; }
			if (!strcmp(fld, "val")) { *out = c->checking ? 0 : c->repl_val[c->val]; return 0; }
			return -1; }
		*out = c->val; return 0;
	}
	if (c->varset && !strcmp(id, c->var)) { *out = c->val; return 0; }
	/* a bound index variable used as a bare integer (e.g. i == j) */
	for (int j = 0; j < c->niv; j++)
		if (!strcmp(id, c->ivar[j])) { *out = c->ival[j]; return 0; }
	(void) before;
	return -1;
}

/* arithmetic over integer terms, standard precedence: get_arith (+ -)
 * over get_mul (* mod) over get_term.  Kept to bounded integers, which
 * stays inside the SMT-encodable fragment; the explicit-state checker
 * evaluates any product, while the proof bridge (Milestone D) requires
 * a product to have a constant operand (linear arithmetic). */
static int
get_mul(const char **p, Ctx *c, long *out)
{
	long a; if (get_term(p, c, &a)) return -1;
	for (;;)
	{
		skipws(p);
		if (**p == '*') { (*p)++; long b; if (get_term(p, c, &b)) return -1; a *= b; }
		else if (word(p, "mod")) { long b; if (get_term(p, c, &b)) return -1; if (b == 0) return -1; a %= b; }
		else break;
	}
	*out = a; return 0;
}
static int
get_arith(const char **p, Ctx *c, long *out)
{
	long a; if (get_mul(p, c, &a)) return -1;
	for (;;)
	{
		skipws(p);
		if (**p == '+') { (*p)++; long b; if (get_mul(p, c, &b)) return -1; a += b; }
		else if (**p == '-') { (*p)++; long b; if (get_mul(p, c, &b)) return -1; a -= b; }
		else break;
	}
	*out = a; return 0;
}

static int
eval_qc(const char **p, Ctx *c)
{
	skipws(p); const char *save = *p;
	int isall = word(p, "all"), issome = isall ? 0 : word(p, "some");
	if (isall || issome)
	{
		skipws(p); char var[32]; int n = 0; while ((isalnum((unsigned char) **p) || **p == '_') && n < 31) var[n++] = *(*p)++; var[n] = 0;
		if (!word(p, "in")) return -1;
		const unsigned char *set; int slen, kind;
		if (get_set(p, c, &set, &slen, &kind)) return -1;
		skipws(p); if (**p != ':') return -1; (*p)++;
		/* reuse the shared binder so count and the quantifiers cannot
		 * diverge; fold each body result into the all/some accumulator */
		struct qc_acc acc = {isall, isall ? 1 : 0};
		if (iter_set(p, c, var, kind, set, slen, qc_visit, &acc)) return -1;
		return acc.result;
	}
	*p = save;
	long a; if (get_arith(p, c, &a)) return -1;
	skipws(p);
	if (word(p, "in"))
	{
		const unsigned char *set; int slen, kind; if (get_set(p, c, &set, &slen, &kind)) return -1;
		if (kind == SET_REPLIES || kind == SET_NODES) return -1;	/* 'in replies'/'in nodes' not defined */
		for (int i = 0; i < slen; i++) if (set[i] == a) return 1;
		return 0;
	}
	const char *op = *p;
	if (!strncmp(op, "==", 2)) { *p += 2; long b; if (get_arith(p, c, &b)) return -1; return a == b; }
	if (!strncmp(op, "!=", 2)) { *p += 2; long b; if (get_arith(p, c, &b)) return -1; return a != b; }
	if (!strncmp(op, "<=", 2)) { *p += 2; long b; if (get_arith(p, c, &b)) return -1; return a <= b; }
	if (!strncmp(op, ">=", 2)) { *p += 2; long b; if (get_arith(p, c, &b)) return -1; return a >= b; }
	if (*op == '<') { (*p)++; long b; if (get_arith(p, c, &b)) return -1; return a < b; }
	if (*op == '>') { (*p)++; long b; if (get_arith(p, c, &b)) return -1; return a > b; }
	return -1;
}
static int eval_unary(const char **p, Ctx *c){ skipws(p);
	if(word(p,"not")){int r=eval_unary(p,c);return r<0?r:!r;}
	if(**p=='('){
		/* `(` is ambiguous: an arithmetic operand of a comparison, e.g.
		 * (count ... ) * 2 > 3, or a boolean group, e.g. (a == b and c).
		 * Try the comparison path first (eval_qc -> get_arith handles the
		 * inner arithmetic paren); fall back to a boolean group.  A wrong
		 * choice leaves trailing text, which eval_invariant rejects. */
		const char *save=*p; int r=eval_qc(p,c);
		if(r>=0) return r;
		*p=save; (*p)++; r=eval_expr(p,c); if(r<0)return r; skipws(p); if(**p!=')')return -1; (*p)++; return r;
	}
	return eval_qc(p,c); }
static int eval_expr(const char **p, Ctx *c){ int r=eval_unary(p,c); if(r<0)return r;
	for(;;){ skipws(p); if(word(p,"and")){int b=eval_unary(p,c);if(b<0)return b;r=r&&b;}
		else if(word(p,"or")){int b=eval_unary(p,c);if(b<0)return b;r=r||b;} else break; } return r; }

/* evaluate a whole invariant: 1 holds, 0 fails, -1 cannot parse.
 * Text after a complete expression is a parse error, never ignored. */
static int
eval_invariant(const char *expr, Ctx *c)
{
	const char *p = expr;
	int			r = eval_expr(&p, c);

	if (r < 0) return -1;
	skipws(&p);
	return *p ? -1 : r;
}

/* parse an invariant once against empty node states */
static int
check_invariant(const char *expr)
{
	static const unsigned char none[1];
	Ctx			c = {0};

	c.nnodes = M.nodes;
	for (int i = 0; i < M.nodes; i++) { c.node[i] = none; c.node_len[i] = 0; }
	c.repl_src = none; c.repl_val = none; c.nrepl = 0;
	return eval_invariant(expr, &c) < 0;
}

/* =================================================================== */
/* per-node process + the inter-node network                           */
/* =================================================================== */
#define MAXTHREAD 8				/* threads tracked per node (tid slots) */
#define MAXEPITEM 16			/* fds in one epoll instance's interest list */

/* An epoll instance (epoll_create1).  Unlike poll(), whose fd set is
 * passed afresh on every call, an epoll instance holds a PERSISTENT
 * interest list built by epoll_ctl: each entry is a watched fd, the
 * events requested for it, and the opaque user data the loop stored
 * (epoll_event.data).  epoll_wait returns that data verbatim for a ready
 * fd -- it is how an event loop finds the handler for the fd -- so the
 * checker stores it and writes it back unchanged; it is never a search
 * distinction (an ASLR pointer must not leak into the fingerprint). */
typedef struct {
	int epfd;					/* the virtual fd epoll_create1 returned */
	int nit;					/* entries in the interest list */
	int fd[MAXEPITEM]; short events[MAXEPITEM]; uint64_t data[MAXEPITEM];
} Epoll;

/* A per-thread deferred slot.  A node is one process with up to MAXTHREAD
 * threads; each thread has its own blocking point, keyed by its kernel
 * tid (the seccomp notification's .pid field).  A thread is blocked when
 * block_kind is non-zero (it holds a deferred syscall the search answers).
 * The single-threaded case is exactly one slot in use, so the behaviour
 * degenerates to the pre-thread engine. */
typedef struct {
	int inuse;					/* this slot holds a live thread */
	pid_t tid;					/* the thread's kernel tid (notif .pid) */
	uint64_t ctid_addr;			/* CLONE_CHILD_CLEARTID word, woken on exit (pthread_join); 0 for the main thread */
	int block_kind; uint64_t block_id; uint64_t block_bufaddr; int block_want; int block_peer;
	/* BK_POLL: the fds this thread is parked in poll() on, their requested
	 * events, and the address of the caller's pollfd array to write
	 * revents back into when the search makes one ready */
	int pollfd_n; int pollfds[MAXPOLL]; short pollevents[MAXPOLL]; uint64_t block_pollarr;
	/* BK_POLL via epoll_wait: epoll_fd is the epoll instance's epfd (-1 if
	 * this park is an ordinary poll()).  When set, a wake writes a
	 * `struct epoll_event` (with the fd's registered data) back to
	 * block_pollarr, not a pollfd array, and answers with the ready count
	 * capped at poll_maxevents.  polldata[k] is the user data the loop
	 * registered for pollfds[k] (epoll_event.data), returned verbatim. */
	int epoll_fd; int poll_maxevents; uint64_t polldata[MAXPOLL];
	int poll_woke_fd;			/* -1, or the channel fd a poll wake marked ready: its next read delivers (not EAGAIN) */
	uint64_t futex_addr;		/* BK_FUTEX: the futex word this thread waits on */
	int woken;					/* a FUTEX_WAKE marked this BK_FUTEX thread runnable; it is released (its futex answered) only once every other thread of the node is parked, so threads run one at a time and the kernel's atomic futex semantics are never torn by concurrent execution */
} Thread;

typedef struct {
	pid_t pid; int listener; int alive;
	/* association list: which peer each connection fd talks to.
	 * peer -1 = the synthetic client, >=0 = a node id. */
	int cfd[MAXCONN]; int cpeer[MAXCONN]; int cnonblock[MAXCONN]; int ccloexec[MAXCONN]; int nc;
	int ls_nonblock; int ls_cloexec;	/* O_NONBLOCK / FD_CLOEXEC on the listener socket */
	int listener_fd;			/* the fd passed to listen(), or -1; the server's accept fd */
	int pend[MAXCONN]; int npend;	/* connectors queued at this node's listener */
	int efd[MAXEFD]; uint64_t efdval[MAXEFD]; int efdnb[MAXEFD]; int nefd;
	Epoll ep[MAXEPOLL]; int nep;	/* epoll instances: a persistent interest list per epfd */
	Thread th[MAXTHREAD]; int nth;	/* per-thread deferred slots, keyed by tid */
	int th_pending;				/* threads cloned but not yet seen (a birth in flight): the node is not quiescent while any is outstanding */
	uint64_t pending_ctid[MAXTHREAD]; int npending_ctid;	/* CLONE_CHILD_CLEARTID words of births in flight, FIFO; handed to each newborn's slot on first sight */
	int next_fd;
	int timer_fd;				/* -1, or an fd whose first recv is a fired timeout's 'T' */
	const char *binary; int id;
	int fd3[2];					/* volatile canonical-emit pipe */
	int fd4[2];					/* durable canonical-emit pipe (survives a crash) */
	unsigned char canon[SNAP]; int canon_len;
	unsigned char durable[SNAP]; int durable_len;
	int durable_read_off;		/* bytes of the framed durable record a startup read has consumed */
	int crashed;				/* killed by a CRASH action: volatile lost, durable kept */
	int preload_ok;				/* the preload announced itself in this node */
} Node;

static Node N[MAXNODES];
enum { BK_RECV = 1, BK_ACCEPT = 2, BK_POLL = 4, BK_FUTEX = 5 };	/* what a blocked thread waits in (3 marks a running node in the fingerprint) */
#define TIMER_PEER (-3)					/* a synthetic peer: a fired election timer */
#define POLL_LISTENER (-4)				/* A_WAKE target: a BK_POLL node's listener fd */

/* Find the thread slot for tid on node nd, allocating one on first sight
 * (which also clears one pending birth, since a new tid completes a clone).
 * Returns the slot, or NULL if the node already has MAXTHREAD threads. */
static Thread *
th_for(Node *nd, pid_t tid)
{
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && nd->th[i].tid == tid) return &nd->th[i];
	/* a new tid: reuse a freed slot or extend the table */
	int slot = -1;
	for (int i = 0; i < nd->nth; i++)
		if (!nd->th[i].inuse) { slot = i; break; }
	if (slot < 0) { if (nd->nth >= MAXTHREAD) return NULL; slot = nd->nth++; }
	Thread *t = &nd->th[slot];
	memset(t, 0, sizeof *t);
	t->inuse = 1; t->tid = tid; t->poll_woke_fd = -1; t->epoll_fd = -1;
	if (nd->th_pending > 0)
	{
		/* this tid is a newborn whose clone we saw: take its clear-tid
		 * word (FIFO) and clear the pending-birth count */
		nd->th_pending--;
		if (nd->npending_ctid > 0)
		{
			t->ctid_addr = nd->pending_ctid[0];
			for (int i = 1; i < nd->npending_ctid; i++) nd->pending_ctid[i - 1] = nd->pending_ctid[i];
			nd->npending_ctid--;
		}
	}
	return t;
}

/* a thread of nd is genuinely running (between syscalls): block_kind 0 and
 * not a merely-woken futex waiter.  Such a node will produce a
 * notification, so drive_quiescent RECVs it; a node whose only non-parked
 * thread is woken is advanced by release_one_woken, not by a RECV (which
 * would block, nothing being about to notify). */
static int
node_has_running(const Node *nd)
{
	if (!nd->alive) return 0;
	if (nd->th_pending > 0) return 1;	/* a newborn is about to notify */
	if (nd->nth == 0) return 1;			/* the main thread's first call is coming */
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && !nd->th[i].block_kind) return 1;
	return 0;
}

static void kill_all(void);

/* Limits are errors (docs/steering/project.md, rule 4): where the checker
 * would otherwise truncate or guess, it stops.  Exit 1 = an engine rule
 * was broken, 3 = a usage or setup error. */
static void
die(int code, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	kill_all();
	fprintf(stderr, "mc2: ERROR: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
	va_end(ap);
	exit(code);
}

/* record / look up which peer a connection fd talks to */
static void cfd_set(Node *nd, int fd, int peer){ if(nd->nc>=MAXCONN) die(1,"node %d has more than %d connections",nd->id,MAXCONN); nd->cfd[nd->nc]=fd; nd->cpeer[nd->nc]=peer; nd->cnonblock[nd->nc]=0; nd->nc++; }
static int  cfd_peer(Node *nd, int fd){ for(int i=0;i<nd->nc;i++) if(nd->cfd[i]==fd) return nd->cpeer[i]; return -2; }

/* message bag: a pending message src->dst */
typedef struct { int used, src, dst, len, chseq; unsigned char body[MSGLEN]; } Msg;
static Msg bag[MAXMSG];
/* sends so far on each channel (src+1, dst): a message's place in its
 * channel, so delivery is FIFO per connection, as TCP is */
static int chcount[MAXNODES + 1][MAXNODES];
/* reset connections, by unordered node pair (the client, peer -1, cannot
 * be reset: it is not a node with its own reads).  A DROP resets the
 * connection rather than losing one message from the middle of the
 * stream, so a drop is a visible failure both ends see (EOF), not a
 * silent byte loss TCP never produces.  Part of the fingerprint. */
static int conn_reset[MAXNODES][MAXNODES];
static int is_reset(int a, int b){ return a >= 0 && b >= 0 && a < MAXNODES && b < MAXNODES && conn_reset[a][b]; }

/* ghost reply log */
static unsigned char rep_src[MAXREP], rep_val[MAXREP]; static int nrep;

/* environment budgets left on this path; part of the fingerprint */
typedef struct { int timeouts, crashes, drops, restarts; } Budget;
static Budget bud;

/* M5b: the logical clock the checker owns.  It starts at a base and
 * advances by LEASE_TICK only when the checker fires a timeout, so a
 * target's clock_gettime/gettimeofday reads are a pure function of how
 * many timeouts have fired -- stateless replay stays deterministic. */
#define CLK_BASE 1000000000ULL		/* 1e9 ns = 1 s, an arbitrary epoch */
#define CLK_TICK 1000000000ULL		/* each fired timeout advances 1 s */
static unsigned long long logical_ns;

static struct seccomp_notif_sizes g_sz;

/* the preload every node runs under (src/cmc_preload.c), found by
 * find_preload(); it announces itself with a clock_gettime on
 * CMC_PRELOAD_MAGIC, so a node it did not load into is refused */
static char preload_path[4096];
#define CMC_PRELOAD_MAGIC 0x434d43

/* Move n bytes to or from a node's memory.  Each returns how many bytes
 * moved: n on success, fewer when the target passed an address it does
 * not own.  A short read zero-fills the rest of buf, but every caller
 * checks the count: a fixed-size argument the kernel copies whole gets
 * the kernel's EFAULT, and a transfer the model cannot answer exactly
 * ends the check (cr_fail), so a bad pointer is never a success. */
static size_t
cr_write(pid_t pid, uint64_t addr, const void *buf, size_t n)
{ struct iovec l = {(void*)(uintptr_t)buf, n}, r = {(void*)(uintptr_t)addr, n};
  ssize_t k = n ? process_vm_writev(pid, &l, 1, &r, 1, 0) : 0;
  return k > 0 ? (size_t) k : 0; }
static size_t
cr_read(pid_t pid, uint64_t addr, void *buf, size_t n)
{ struct iovec l = {buf, n}, r = {(void*)(uintptr_t)addr, n};
  ssize_t k = n ? process_vm_readv(pid, &l, 1, &r, 1, 0) : 0;
  size_t got = k > 0 ? (size_t) k : 0;
  if (got < n) memset((unsigned char *) buf + got, 0, n - got);
  return got; }

static void die(int code, const char *fmt, ...);
/* end the check: node id passed an address the engine could not read or
 * write for `what`, on a path where the kernel's answer is not modelled */
static void
cr_fail(int id, const char *what)
{
	die(3, "node %d: %s at an address it does not own; EFAULT on this path is not modelled", id, what);
}

static int
is_pie(const char *b)
{ int fd=open(b,O_RDONLY); unsigned char e[18]={0}; if(fd<0)return -1;
  if(read(fd,e,sizeof e)!=(ssize_t)sizeof e){close(fd);return -1;} close(fd);
  if(memcmp(e,"\177ELF",4))return -1;
  return (e[16]|(e[17]<<8))==3; }

static int
install_filter(void)
{
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, AUDIT_ARCH_X86_64, 1, 0),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
#define OWN(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(n),0,1), BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_USER_NOTIF)
		OWN(SYS_socket), OWN(SYS_setsockopt), OWN(SYS_bind), OWN(SYS_listen),
		OWN(SYS_connect), OWN(SYS_accept), OWN(SYS_accept4),
		OWN(SYS_read), OWN(SYS_write), OWN(SYS_recvfrom), OWN(SYS_sendto),
		OWN(SYS_clock_gettime), OWN(SYS_gettimeofday), OWN(SYS_getrandom),
		OWN(SYS_close),
		OWN(SYS_poll), OWN(SYS_ppoll), OWN(SYS_fcntl), OWN(SYS_eventfd2),
		OWN(SYS_epoll_create1), OWN(SYS_epoll_create), OWN(SYS_epoll_ctl),
		OWN(SYS_epoll_wait), OWN(SYS_epoll_pwait),
		OWN(SYS_clone), OWN(SYS_clone3), OWN(SYS_futex), OWN(SYS_exit),
#undef OWN
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog p = {sizeof f/sizeof*f, f};
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	return seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &p);
}

static int
launch(Node *nd)
{
	int sp[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
	if (pipe(nd->fd3) != 0) return -1;
	if (pipe(nd->fd4) != 0) return -1;
	pid_t pid = fork();
	if (pid == 0)
	{
		close(sp[0]);
		setenv("LD_PRELOAD", preload_path, 1);
		dup2(nd->fd3[1], 3); close(nd->fd3[0]); close(nd->fd3[1]);
		dup2(nd->fd4[1], DURABLE_FD); close(nd->fd4[0]); close(nd->fd4[1]);
		/* Tell the node it runs under the checker, and on which fd to emit
		 * its canonical state.  A program that keeps the state hook in its
		 * production build guards every emit on getenv("CMC_STATE_FD"), so
		 * it never writes to a random fd 3 (or fd 193) outside a check.
		 * The value is the volatile fd; the durable fd is CMC_DURABLE_FD. */
		{
			char fdbuf[8];
			snprintf(fdbuf, sizeof fdbuf, "%d", 3); setenv("CMC_STATE_FD", fdbuf, 1);
			snprintf(fdbuf, sizeof fdbuf, "%d", DURABLE_FD); setenv("CMC_DURABLE_FD", fdbuf, 1);
		}
		int lf = install_filter(); if (lf < 0) _exit(2);
		char cb[CMSG_SPACE(sizeof(int))] = {0}; char d = 'L';
		struct iovec iov = {&d, 1}; struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &lf, sizeof(int)); sendmsg(sp[1], &mh, 0);
		char idbuf[8]; snprintf(idbuf, sizeof idbuf, "%d", nd->id);
		/* argv = binary, id, then the space-separated [target] args (so a
		 * target that takes flags needs no wrapper) */
		char *av[64]; int ac = 0;
		av[ac++] = (char *) nd->binary; av[ac++] = idbuf;
		char argbuf[512]; snprintf(argbuf, sizeof argbuf, "%s", M.args);
		for (char *p = strtok(argbuf, " \t"); p && ac < 63; p = strtok(NULL, " \t")) av[ac++] = p;
		av[ac] = NULL;
		execv(nd->binary, av);
		_exit(2);
	}
	close(sp[1]); close(nd->fd3[1]); close(nd->fd4[1]);
	char cb[CMSG_SPACE(sizeof(int))] = {0}; char d;
	struct iovec iov = {&d, 1}; struct msghdr mh = {0};
	mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
	if (recvmsg(sp[0], &mh, 0) < 0) { close(sp[0]); return -1; }
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); nd->listener = -1;
	if (cm) memcpy(&nd->listener, CMSG_DATA(cm), sizeof(int));
	close(sp[0]);
	if (nd->listener < 0) return -1;
	/* make the notify fd non-blocking: with threads, drive_quiescent must
	 * poll the listeners and RECV only a ready notification, never block
	 * one node's RECV while another node's running thread needs service */
	{ int fl = fcntl(nd->listener, F_GETFL); fcntl(nd->listener, F_SETFL, fl | O_NONBLOCK); }
	nd->pid = pid; nd->alive = 1; nd->next_fd = 500; nd->nc = 0;
	nd->npend = 0; nd->crashed = 0; nd->canon_len = 0; nd->durable_len = 0; nd->durable_read_off = 0; nd->preload_ok = 0; nd->timer_fd = -1;
	nd->ls_nonblock = 0; nd->ls_cloexec = 0; nd->listener_fd = -1; nd->nefd = 0; nd->nep = 0;
	nd->nth = 0; nd->th_pending = 0; nd->npending_ctid = 0; memset(nd->th, 0, sizeof nd->th);
	ptrace(PTRACE_SEIZE, pid, 0, 0);
	return 0;
}

/* drain a canonical-emit pipe non-blocking; copy the LAST complete record
 * into out/outlen (records are [count][count bytes]) */
static void
drain_pipe(int fd, unsigned char *out, int *outlen)
{
	unsigned char all[4096]; int tot = 0, r;
	int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	while (tot < (int) sizeof all && (r = (int) read(fd, all + tot, sizeof all - tot)) > 0) tot += r;
	int off = 0, rec = -1, rl = 0;
	while (off < tot) { int cnt = all[off]; if (off + 1 + cnt > tot) break; rec = off + 1; rl = cnt; off += 1 + cnt; }
	if (rec >= 0) { *outlen = rl > SNAP ? SNAP : rl; memcpy(out, all + rec, *outlen); }
}

/* read any canonical-emit records this node wrote; keep the last of each
 * stream (volatile on fd 3, durable on fd 193) */
static void
drain_canon(Node *nd)
{
	drain_pipe(nd->fd3[0], nd->canon, &nd->canon_len);
	drain_pipe(nd->fd4[0], nd->durable, &nd->durable_len);
}

/* bag helpers.  A message joins its channel (src, dst) behind every
 * earlier one, so a channel is FIFO like the TCP connection it models. */
static void
bag_add(int src, int dst, const unsigned char *b, int len)
{
	for (int i = 0; i < MAXMSG; i++)
		if (!bag[i].used)
		{
			bag[i].used = 1; bag[i].src = src; bag[i].dst = dst; bag[i].len = len;
			bag[i].chseq = chcount[src + 1][dst]++;
			memcpy(bag[i].body, b, (size_t) len);
			return;
		}
	die(1, "more than %d messages in flight", MAXMSG);
}

/* the oldest message on channel src -> dst (its bag index), or -1 */
static int
chan_head(int src, int dst)
{
	int best = -1;
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].src == src && bag[i].dst == dst &&
			(best < 0 || bag[i].chseq < bag[best].chseq))
			best = i;
	return best;
}

/*
 * Service one ready notification on node nd without blocking the search.
 * Returns 1 if it did something, 0 if nd had no pending notification.
 * A recv on an empty channel is DEFERRED (the thread's slot set, not
 * answered).
 */
/* an eventfd's slot index on this node, or -1.  eventfds are virtual
 * channels: a write adds to the counter, a read drains it, and a
 * non-zero counter is poll-ready, so an event loop's self-wakeup path
 * is visible to the search rather than passed through to the kernel. */
static int
efd_slot(Node *nd, int fd)
{
	for (int i = 0; i < nd->nefd; i++)
		if (nd->efd[i] == fd) return i;
	return -1;
}

/* the connection-fd slot index, or -1 (cfd_peer returns the peer, not a
 * slot; the nonblock flag needs the slot) */
static int
cfd_slot(Node *nd, int fd)
{
	for (int i = 0; i < nd->nc; i++)
		if (nd->cfd[i] == fd) return i;
	return -1;
}

/* The epoll events the model answers: level-triggered EPOLLIN and
 * EPOLLOUT, plus EPOLLERR and EPOLLHUP, which epoll always reports.
 * EPOLLET and EPOLLONESHOT change when a ready fd is reported again, and
 * EPOLLEXCLUSIVE, EPOLLRDHUP and EPOLLPRI report conditions the model
 * never raises, so each is refused rather than answered as if level
 * triggered. */
static void
ep_events_modelled(const Node *nd, uint32_t ev)
{
	uint32_t	ok = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;

	if (ev & ~ok)
		die(3, "node %d: epoll events %#x are not modelled (only level-triggered EPOLLIN and EPOLLOUT)", nd->id, ev & ~ok);
}

/* the epoll-instance slot index for an epfd on this node, or -1 */
static int
ep_slot(Node *nd, int epfd)
{
	for (int i = 0; i < nd->nep; i++)
		if (nd->ep[i].epfd == epfd) return i;
	return -1;
}

static struct seccomp_notif *g_req;
static struct seccomp_notif_resp *g_resp;

static int futex_wake(Node *nd, uint64_t uaddr, int n);

static int
try_pending_for(Node *nd)
{
	if (!nd->alive) return 0;
	memset(g_req, 0, g_sz.seccomp_notif);
	/* non-blocking RECV: use ioctl; returns -1/ENOENT if none ready.  It
	 * delivers a notification from ANY of this node's threads, so a tid
	 * already parked in a deferred slot does not stop the search from
	 * servicing another tid's call. */
	if (ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_RECV, g_req) != 0)
		return 0;
	long nr = g_req->data.nr; __u64 *a = g_req->data.args;
	/* route the notification to its thread's slot (seccomp_notif.pid is
	 * the calling thread's tid); a first-sight tid gets a fresh slot */
	Thread *th = th_for(nd, g_req->pid);
	if (!th) die(1, "node %d has more than %d threads", nd->id, MAXTHREAD);
	memset(g_resp, 0, g_sz.seccomp_notif_resp); g_resp->id = g_req->id;

	switch (nr)
	{
		case SYS_socket:  g_resp->val = nd->next_fd++; break;
		case SYS_clock_gettime:
			if ((long) a[0] == CMC_PRELOAD_MAGIC) { nd->preload_ok = 1; g_resp->val = 0; break; }
		{
			/* answer with the logical clock (a[1] = struct timespec*) */
			struct { long long sec, nsec; } ts;
			ts.sec = logical_ns / 1000000000ULL; ts.nsec = logical_ns % 1000000000ULL;
			if (cr_write(nd->pid, a[1], &ts, sizeof ts) != sizeof ts) { g_resp->error = -EFAULT; break; }
			g_resp->val = 0; break;
		}
		case SYS_gettimeofday:
		{
			/* gettimeofday(tv, tz): the buffer is the FIRST argument */
			struct { long long sec, usec; } tv;
			tv.sec = logical_ns / 1000000000ULL; tv.usec = (logical_ns % 1000000000ULL) / 1000;
			/* a NULL tv is allowed and writes nothing */
			if (a[0] && cr_write(nd->pid, a[0], &tv, sizeof tv) != sizeof tv) { g_resp->error = -EFAULT; break; }
			g_resp->val = 0; break;
		}
		case SYS_getrandom:
		{
			/* engine-chosen randomness: a fixed byte pattern, so the search
			 * is reproducible (a real nondeterministic source would need its
			 * own budget; deterministic is the honest default here) */
			unsigned char z[256]; size_t n = a[1]; if (n > sizeof z) n = sizeof z;
			memset(z, 0, n);
			size_t got = cr_write(nd->pid, a[0], z, n);
			if (got == 0 && n > 0) { g_resp->error = -EFAULT; break; }
			if (got < n) cr_fail(nd->id, "getrandom fills a buffer");
			g_resp->val = (long) n; break;
		}
		case SYS_setsockopt: case SYS_bind: g_resp->val = 0; break;
		case SYS_listen: nd->listener_fd = (int) a[0]; g_resp->val = 0; break;
		case SYS_close:
		{
			/* a checker-owned fd: forget the connection, so the table and
			 * the fingerprint hold only live ones (shortcut: the peer is
			 * not told; it sees EOF only if this node dies) */
			int fd = (int) a[0];
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
			for (int k = 0; k < nd->nc; k++)
				if (nd->cfd[k] == fd) { nd->nc--; for (int j = k; j < nd->nc; j++) { nd->cfd[j] = nd->cfd[j + 1]; nd->cpeer[j] = nd->cpeer[j + 1]; } break; }
			g_resp->val = 0; break;
		}
		case SYS_connect:
		{
			/* read the dest port to learn the peer id */
			struct { unsigned short fam; unsigned short port; } sa;
			if (cr_read(nd->pid, a[1], &sa, sizeof sa) != sizeof sa) { g_resp->error = -EFAULT; break; }
			int port = (sa.port >> 8) | ((sa.port & 0xff) << 8);
			int peer = port - BASE;
			if (peer < 0 || peer >= M.nodes)
				die(3, "node %d connects to port %d, which is no node's (node i listens on %d+i)", nd->id, port, BASE);
			/* the connection waits at the peer's listener until the search
			 * lets the peer accept it; it never fails (shortcut: no
			 * ECONNREFUSED, upgrade when a target retries connects) */
			if (N[peer].npend >= MAXCONN) die(1, "more than %d connections wait at node %d", MAXCONN, peer);
			N[peer].pend[N[peer].npend++] = nd->id;
			/* the node sends/recvs on the socket() fd (connect's a[0]);
			 * connect() itself returns 0 */
			cfd_set(nd, (int) a[0], peer);
			g_resp->val = 0; break;
		}
		case SYS_accept: case SYS_accept4:
			/* A non-blocking listener with no connection queued returns
			 * EAGAIN at once, so an event loop that drains the backlog
			 * (accept until EAGAIN) makes progress instead of parking on
			 * the second accept.  This is sound for the same reason a
			 * non-blocking read's EAGAIN is: the loop's following poll()
			 * defers and hands control back to the search, which owns when
			 * a connection becomes available. */
			if (nd->ls_nonblock && nd->npend == 0)
			{ g_resp->error = -EAGAIN; g_resp->val = 0; break; }
			/* DEFER: which waiting connection the node accepts, and when,
			 * is a search action, so the topology is whatever the nodes
			 * dial (shortcut: the sockaddr out-parameter is not filled) */
			th->block_kind = BK_ACCEPT; th->block_id = g_req->id; th->block_peer = -2;
			return 1;
		case SYS_write: case SYS_sendto:
		{
			int fd = (int) a[0];
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; } /* not ours (stdout, fd 3, libs) */
			int ews = efd_slot(nd, fd);
			if (ews >= 0)
			{
				/* an 8-byte add to the counter; a non-zero counter makes
				 * the eventfd poll-ready (checked locally in poll) */
				uint64_t add = 0; int len = (int) a[2];
				if (len != 8) die(1, "node %d writes %d bytes to an eventfd (must be 8)", nd->id, len);
				if (cr_read(nd->pid, a[1], &add, 8) != 8) { g_resp->error = -EFAULT; break; }
				nd->efdval[ews] += add; g_resp->val = 8; break;
			}
			int peer = cfd_peer(nd, fd);
			if (peer == -2) die(1, "node %d writes fd %d, which is no connection", nd->id, fd);
			int len = (int) a[2];
			if (len < 0 || len > M.max_write) die(1, "node %d writes %d bytes in one call; the limit is %d (raise [target] max_write, up to %d)", nd->id, len, M.max_write, MSGLEN);
			if (is_reset(nd->id, peer))
			{
				/* writing into a reset connection reaches no one: the bytes
				 * are dropped, not buffered, so a dead channel never carries
				 * a message.  Report the length written (a target that cares
				 * learns the failure on its next read, as EOF). */
				g_resp->val = len; break;
			}
			unsigned char buf[MSGLEN] = {0};
			size_t got = cr_read(nd->pid, a[1], buf, (size_t) len);
			/* nothing readable is the kernel's EFAULT with nothing sent; a
			 * buffer that faults partway would send a prefix, which the
			 * model does not do */
			if (got == 0 && len > 0) { g_resp->error = -EFAULT; break; }
			if (got < (size_t) len) cr_fail(nd->id, "write sends from a buffer that ends");
			if (peer == -1)			/* reply to the client: ghost log */
			{
				if (len >= 2 && buf[0] == 'A')
				{
					if (nrep >= MAXREP) die(1, "more than %d client replies", MAXREP);
					rep_src[nrep] = (unsigned char) nd->id; rep_val[nrep] = buf[1]; nrep++;
				}
			}
			else if (len > 0)		/* inter-node: buffer in the bag */
				bag_add(nd->id, peer, buf, len);
			g_resp->val = len; break;
		}
		case SYS_read: case SYS_recvfrom:
		{
			int fd = (int) a[0];
			if (fd == DURABLE_FD)
			{
				/* A read of the durable descriptor serves the node's surviving
				 * durable record back to it, framed as it was written
				 * ([count][count bytes]): on a fresh launch the record is
				 * empty so the first read returns EOF, and on a RESTART it
				 * returns what the node had made durable before the crash, so
				 * a server that reads its own log on startup recovers.  A
				 * second read returns EOF.  The bytes come from nd->durable
				 * (engine-owned), so this read is a pure function of the world
				 * and replays identically. */
				unsigned char framed[1 + SNAP]; int flen = 0;
				if (nd->durable_len > 0) { framed[0] = (unsigned char) nd->durable_len; memcpy(framed + 1, nd->durable, (size_t) nd->durable_len); flen = 1 + nd->durable_len; }
				int want = (int) a[2];
				int avail = flen - nd->durable_read_off;
				int k = avail < want ? avail : want; if (k < 0) k = 0;
				if (k > 0 && cr_write(nd->pid, a[1], framed + nd->durable_read_off, (size_t) k) != (size_t) k)
					cr_fail(nd->id, "a read of the durable record");
				nd->durable_read_off += k;
				g_resp->val = k; break;
			}
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; } /* loader/lib reads */
			int ers = efd_slot(nd, fd);
			if (ers >= 0)
			{
				/* read drains the counter to 0 and returns its value as 8
				 * bytes; an empty non-blocking eventfd returns EAGAIN */
				int want = (int) a[2];
				if (nd->efdval[ers] == 0)
				{
					if (nd->efdnb[ers]) { g_resp->error = -EAGAIN; g_resp->val = 0; break; }
					die(1, "node %d blocks reading an empty eventfd (no model)", nd->id);
				}
				if (want < 8) die(1, "node %d reads %d bytes from an eventfd (must be 8)", nd->id, want);
				uint64_t v = nd->efdval[ers];
				if (cr_write(nd->pid, a[1], &v, 8) != 8) cr_fail(nd->id, "an eventfd read");
				nd->efdval[ers] = 0; g_resp->val = 8; break;
			}
			int peer = cfd_peer(nd, fd);
			if (peer == -2) die(1, "node %d reads fd %d, which is no connection", nd->id, fd);
			if (is_reset(nd->id, peer))
			{
				/* the connection has been reset (a DROP): every read on it
				 * returns EOF, blocking or not, as TCP does once a peer has
				 * reset.  No search choice remains, so answer immediately. */
				th->poll_woke_fd = -1; g_resp->val = 0; break;
			}
			if (fd == nd->timer_fd)
			{
				/* the first recv on a timer connection returns the fired
				 * timeout's one byte 'T', then the fd is an ordinary
				 * (empty, timer-peer) channel */
				static const unsigned char t = 'T';
				int want = (int) a[2]; int k = want < 1 ? want : 1;
				nd->timer_fd = -1; th->poll_woke_fd = -1;
				if (k > 0 && cr_write(nd->pid, a[1], &t, (size_t) k) != (size_t) k) cr_fail(nd->id, "a read of a timer byte");
				g_resp->val = k; break;
			}
			/* A non-blocking channel read with nothing deliverable returns
			 * EAGAIN immediately: the search has not just woken this fd, so
			 * no message is ready.  This is sound only because the loop's
			 * next poll() defers (BK_POLL), handing control back to the
			 * search; EAGAIN never lets a node bypass delivery. */
			int cs = cfd_slot(nd, fd);
			if (cs >= 0 && nd->cnonblock[cs] && fd != th->poll_woke_fd)
			{ g_resp->error = -EAGAIN; g_resp->val = 0; break; }
			th->poll_woke_fd = -1;	/* the wake is spent; this read defers */
			/* Always DEFER a blocking channel recv: whether it is satisfied
			 * by a message or by a timeout is an environment choice the
			 * search makes, so delivery must never be automatic (M5).  The
			 * node blocks in the kernel until the search delivers to it. */
			th->block_kind = BK_RECV; th->block_id = g_req->id; th->block_bufaddr = a[1];
			th->block_want = (int) a[2]; th->block_peer = peer;
			return 1;				/* do NOT answer */
		}
		case SYS_fcntl:
		{
			/* F_GETFL/F_SETFL carry O_NONBLOCK, which the model acts on;
			 * F_GETFD/F_SETFD carry FD_CLOEXEC, which is inert in the
			 * model (a running node never execs, so close-on-exec cannot
			 * change its future) but is stored and returned so the flag
			 * round-trips.  Any other fcntl on one of our fds has no
			 * complete model, so it is an error, never a silent pass. */
			int fd = (int) a[0]; int cmd = (int) a[1];
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
			int cs = cfd_slot(nd, fd), es = efd_slot(nd, fd);
			int *nbp = cs >= 0 ? &nd->cnonblock[cs] : es >= 0 ? &nd->efdnb[es] : &nd->ls_nonblock;
			int *clp = cs >= 0 ? &nd->ccloexec[cs] : es >= 0 ? NULL : &nd->ls_cloexec;
			if (cmd == F_GETFL) g_resp->val = *nbp ? O_NONBLOCK : 0;
			else if (cmd == F_SETFL) { *nbp = (a[2] & O_NONBLOCK) ? 1 : 0; g_resp->val = 0; }
			else if (cmd == F_GETFD) g_resp->val = (clp && *clp) ? FD_CLOEXEC : 0;
			else if (cmd == F_SETFD) { if (clp) *clp = (a[2] & FD_CLOEXEC) ? 1 : 0; g_resp->val = 0; }
			else die(1, "node %d: fcntl cmd %d on virtual fd %d has no model", nd->id, cmd, fd);
			break;
		}
		case SYS_eventfd2:
		{
			/* a virtual eventfd: next_fd with a counter seeded from the
			 * initval (a[0]).  A read drains the counter to 0, so
			 * EFD_SEMAPHORE (decrement by one) is refused rather than
			 * answered wrongly; EFD_NONBLOCK sets the flag F_SETFL would,
			 * and EFD_CLOEXEC is inert (a node never execs). */
			if (a[1] & EFD_SEMAPHORE) die(3, "node %d: an EFD_SEMAPHORE eventfd is not modelled", nd->id);
			if (a[1] & ~(uint64_t) (EFD_NONBLOCK | EFD_CLOEXEC)) die(3, "node %d: eventfd flags %#llx are not modelled", nd->id, (unsigned long long) a[1]);
			if (nd->nefd >= MAXEFD) die(1, "node %d opens more than %d eventfds", nd->id, MAXEFD);
			int fd = nd->next_fd++;
			nd->efd[nd->nefd] = fd; nd->efdval[nd->nefd] = a[0]; nd->efdnb[nd->nefd] = (a[1] & EFD_NONBLOCK) ? 1 : 0; nd->nefd++;
			g_resp->val = fd; break;
		}
		case SYS_epoll_create1: case SYS_epoll_create:
		{
			/* a virtual epoll instance: next_fd with an empty interest list.
			 * epoll_create's size hint (a[0]) is ignored, as the kernel
			 * ignores it; epoll_create1's flags (EPOLL_CLOEXEC) are inert in
			 * the model, as FD_CLOEXEC is (a node never execs). */
			if (nd->nep >= MAXEPOLL) die(1, "node %d opens more than %d epoll instances", nd->id, MAXEPOLL);
			int fd = nd->next_fd++;
			Epoll *e = &nd->ep[nd->nep++];
			e->epfd = fd; e->nit = 0;
			g_resp->val = fd; break;
		}
		case SYS_epoll_ctl:
		{
			/* maintain an instance's persistent interest list.  ADD/MOD read
			 * the caller's struct epoll_event to capture the requested events
			 * and the opaque user data (epoll_event.data, 8 bytes) epoll_wait
			 * must return verbatim; DEL drops the fd.  An unknown op has no
			 * model, so it is an error, never a silent pass. */
			int epfd = (int) a[0]; int op = (int) a[1]; int fd = (int) a[2];
			int eps = ep_slot(nd, epfd);
			if (eps < 0) die(1, "node %d: epoll_ctl on fd %d, which is no epoll instance", nd->id, epfd);
			Epoll *e = &nd->ep[eps];
			/* struct epoll_event is packed on x86-64: 4-byte events then an
			 * 8-byte data union, 12 bytes with no padding */
			struct __attribute__((packed)) { uint32_t events; uint64_t data; } ev;
			int at = -1;
			for (int i = 0; i < e->nit; i++) if (e->fd[i] == fd) { at = i; break; }
			if (op == EPOLL_CTL_ADD)
			{
				if (at >= 0) { g_resp->error = -EEXIST; g_resp->val = 0; break; }
				if (e->nit >= MAXEPITEM) die(1, "node %d: epoll instance watches more than %d fds", nd->id, MAXEPITEM);
				if (cr_read(nd->pid, a[3], &ev, sizeof ev) != sizeof ev) { g_resp->error = -EFAULT; break; }
				ep_events_modelled(nd, ev.events);
				e->fd[e->nit] = fd; e->events[e->nit] = (short) ev.events; e->data[e->nit] = ev.data; e->nit++;
				g_resp->val = 0; break;
			}
			if (op == EPOLL_CTL_MOD)
			{
				if (at < 0) { g_resp->error = -ENOENT; g_resp->val = 0; break; }
				if (cr_read(nd->pid, a[3], &ev, sizeof ev) != sizeof ev) { g_resp->error = -EFAULT; break; }
				ep_events_modelled(nd, ev.events);
				e->events[at] = (short) ev.events; e->data[at] = ev.data;
				g_resp->val = 0; break;
			}
			if (op == EPOLL_CTL_DEL)
			{
				if (at < 0) { g_resp->error = -ENOENT; g_resp->val = 0; break; }
				e->nit--;
				for (int j = at; j < e->nit; j++) { e->fd[j] = e->fd[j + 1]; e->events[j] = e->events[j + 1]; e->data[j] = e->data[j + 1]; }
				g_resp->val = 0; break;
			}
			die(1, "node %d: epoll_ctl op %d has no model (only ADD/MOD/DEL)", nd->id, op);
			break;
		}
		case SYS_epoll_wait: case SYS_epoll_pwait:
		{
			/* epoll's poll(): a readiness query over the instance's PERSISTENT
			 * interest list.  Local readiness (an eventfd counter, or a
			 * requested EPOLLOUT) is answered here by writing ready
			 * struct epoll_event entries back (with the stored user data); a
			 * channel or listener fd's readiness is a search decision, so if
			 * nothing is locally ready and the call blocks the node DEFERS as
			 * BK_POLL and the search wakes it (deliver/accept/timeout),
			 * exactly as for poll().  The interest list is copied into the
			 * thread's poll slot so poll_wake and enum_actions handle it with
			 * one code path. */
			int epfd = (int) a[0]; uint64_t arr = a[1]; int maxevents = (int) a[2];
			int eps = ep_slot(nd, epfd);
			if (eps < 0) die(1, "node %d: epoll_wait on fd %d, which is no epoll instance", nd->id, epfd);
			if (maxevents <= 0) die(1, "node %d: epoll_wait maxevents is %d (must be > 0)", nd->id, maxevents);
			Epoll *e = &nd->ep[eps];
			if (e->nit > MAXPOLL) die(1, "node %d: epoll instance watches %d fds; the limit is %d", nd->id, e->nit, MAXPOLL);
			/* epoll_wait's timeout is a[3] (ms); epoll_pwait's too (a[4] is the
			 * sigmask).  A 0 timeout returns at once with whatever is ready,
			 * and a negative one blocks.  A positive timeout would return 0
			 * when it expires, which the search never offers, so it is
			 * refused. */
			if ((int) a[3] > 0) die(3, "node %d: epoll_wait with a %d ms timeout is not modelled (only 0 or -1)", nd->id, (int) a[3]);
			int zero_timeout = (a[3] == 0);
			struct __attribute__((packed)) { uint32_t events; uint64_t data; } out[MAXPOLL];
			int ready = 0;
			for (int i = 0; i < e->nit && ready < maxevents; i++)
			{
				int fd = e->fd[i]; int es = efd_slot(nd, fd);
				if (e->events[i] & EPOLLOUT)
				{ out[ready].events = EPOLLOUT; out[ready].data = e->data[i]; ready++; continue; }
				if (es >= 0 && nd->efdval[es] > 0 && (e->events[i] & EPOLLIN))
				{ out[ready].events = EPOLLIN; out[ready].data = e->data[i]; ready++; }
			}
			if (ready > 0 || zero_timeout)
			{
				size_t want = (size_t) ready * sizeof out[0];
				if (ready > 0 && cr_write(nd->pid, arr, out, want) != want)
					cr_fail(nd->id, "epoll_wait returns events");
				g_resp->val = ready; break;
			}
			/* nothing locally ready and the call blocks: a quiescence point,
			 * recorded in the poll slot, but flagged as an epoll_wait so the
			 * wake writes a struct epoll_event (with the stored data) back. */
			th->block_kind = BK_POLL; th->block_id = g_req->id;
			th->block_pollarr = arr; th->pollfd_n = e->nit;
			th->epoll_fd = epfd; th->poll_maxevents = maxevents;
			for (int i = 0; i < e->nit; i++)
			{ th->pollfds[i] = e->fd[i]; th->pollevents[i] = e->events[i]; th->polldata[i] = e->data[i]; }
			return 1;				/* do NOT answer */
		}
		case SYS_poll: case SYS_ppoll:
		{
			/* readiness query.  Local readiness (an eventfd counter, or a
			 * requested POLLOUT) is answered here; a channel or listener
			 * fd's readiness is a search decision, like a deferred recv,
			 * so if nothing is locally ready the node DEFERS as BK_POLL
			 * and the search wakes it (deliver/accept/timeout). */
			uint64_t arr = a[0]; int nfds = (int) a[1];
			if (nfds < 0 || nfds > MAXPOLL) die(1, "node %d polls %d fds; the limit is %d", nd->id, nfds, MAXPOLL);
			struct pollfd pfs[MAXPOLL];
			if (cr_read(nd->pid, arr, pfs, (size_t) nfds * sizeof pfs[0]) != (size_t) nfds * sizeof pfs[0])
			{ g_resp->error = -EFAULT; break; }
			/* poll's timeout is a[2] (ms, negative = block forever); ppoll's
			 * is a struct timespec* at a[2] (NULL = block forever).  A zero
			 * timeout returns at once; a finite positive one would return 0
			 * on expiry, which the search never offers, so it is refused. */
			int zero_timeout;
			if (nr == SYS_poll)
			{
				if ((int) a[2] > 0) die(3, "node %d: poll with a %d ms timeout is not modelled (only 0 or -1)", nd->id, (int) a[2]);
				zero_timeout = (a[2] == 0);
			}
			else if (a[2] == 0) zero_timeout = 0;
			else
			{
				struct { long long sec, nsec; } ts;
				if (cr_read(nd->pid, a[2], &ts, sizeof ts) != sizeof ts) { g_resp->error = -EFAULT; break; }
				if (ts.sec != 0 || ts.nsec != 0) die(3, "node %d: ppoll with a finite timeout is not modelled (only zero or NULL)", nd->id);
				zero_timeout = 1;
			}
			int ready = 0;
			for (int i = 0; i < nfds; i++)
			{
				pfs[i].revents = 0;
				int fd = pfs[i].fd;
				if (fd < 0) continue;
				int es = efd_slot(nd, fd);
				if ((pfs[i].events & POLLOUT)) { pfs[i].revents |= POLLOUT; ready++; continue; }
				if (es >= 0 && nd->efdval[es] > 0 && (pfs[i].events & POLLIN))
				{ pfs[i].revents |= POLLIN; ready++; }
			}
			if (ready > 0 || zero_timeout)
			{
				if (cr_write(nd->pid, arr, pfs, (size_t) nfds * sizeof pfs[0]) != (size_t) nfds * sizeof pfs[0])
				{ g_resp->error = -EFAULT; break; }
				g_resp->val = ready; break;
			}
			/* nothing locally ready and the call blocks: a quiescence
			 * point.  Record the polled fds so the search can mark one
			 * ready and answer the poll. */
			th->block_kind = BK_POLL; th->block_id = g_req->id;
			th->block_pollarr = arr; th->pollfd_n = nfds; th->epoll_fd = -1;
			for (int i = 0; i < nfds; i++) { th->pollfds[i] = pfs[i].fd; th->pollevents[i] = pfs[i].events; }
			return 1;				/* do NOT answer */
		}
		case SYS_clone: case SYS_clone3:
		{
			/* A new thread shares the address space and the seccomp filter,
			 * so its notifications arrive on this same listener.  The real
			 * thread must run (its syscalls are what the search drives), so
			 * the clone is CONTINUE'd; the checker only records that a birth
			 * is in flight (so it does not call the node quiescent while the
			 * newborn is still computing toward its first blocking point)
			 * and captures the CLONE_CHILD_CLEARTID word, which the kernel
			 * zeroes and wakes on the thread's exit (what pthread_join waits
			 * on).  A clone without CLONE_THREAD (a fork) is not a thread of
			 * this node; let it run but do not track it (it would be its own
			 * process, outside the node model -- a target that forks is out
			 * of scope and will trip LULL_MS if that child blocks owned). */
			uint64_t flags, ctid = 0;
			if (nr == SYS_clone3)
			{
				struct clone_args ca; memset(&ca, 0, sizeof ca);
				size_t sz = (size_t) a[1]; if (sz > sizeof ca) sz = sizeof ca;
				/* the kernel itself fails a clone3 whose arguments it cannot
				 * read, so no thread is born: let it, and record nothing */
				if (cr_read(nd->pid, a[0], &ca, sz) != sz) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
				flags = ca.flags; ctid = ca.child_tid;
			}
			else
			{
				/* clone(flags, stack, parent_tid, child_tid, tls) */
				flags = a[0]; ctid = a[3];
			}
			if (flags & CLONE_THREAD)
			{
				if (nd->nth + nd->th_pending >= MAXTHREAD)
					die(1, "node %d creates more than %d threads", nd->id, MAXTHREAD);
				nd->th_pending++;
				/* the newborn's clear-tid word, handed to its slot on first
				 * sight (FIFO); used to wake a pthread_join on its exit */
				if (ctid) nd->pending_ctid[nd->npending_ctid++] = ctid;
			}
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		case SYS_futex:
		{
			/* A futex is how threads of a node hand off and wait on each
			 * other (a mutex, a condvar, pthread_join).  A thread waiting in
			 * futex is QUIESCENT: it cannot progress until another thread
			 * wakes it.  We model FUTEX_WAIT as a deferred slot (BK_FUTEX)
			 * on the futex word, and FUTEX_WAKE as a real event from the
			 * waker's step that wakes the parked waiters -- never an
			 * invented wake. */
			uint64_t uaddr = a[0]; int op = (int) a[1];
			int cmd = op & FUTEX_CMD_MASK;
			if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET)
			{
				/* a timed wait could return ETIMEDOUT, which the search never
				 * offers, so it is refused rather than treated as untimed */
				if (a[3]) die(3, "node %d: a futex wait with a timeout is not modelled", nd->id);
				/* honour the kernel's atomic compare: FUTEX_WAIT sleeps only
				 * if *uaddr == val, else it returns EAGAIN at once.  Reading
				 * *uaddr and matching that keeps us from parking a thread the
				 * kernel would have let run (which could wrongly look
				 * quiescent). */
				uint32_t cur = 0;
				if (cr_read(nd->pid, uaddr, &cur, sizeof cur) != sizeof cur) { g_resp->error = -EFAULT; break; }
				if (cur != (uint32_t) a[2]) { g_resp->error = -EAGAIN; g_resp->val = 0; break; }
				th->block_kind = BK_FUTEX; th->block_id = g_req->id; th->futex_addr = uaddr;
				return 1;			/* do NOT answer: a wake will */
			}
			if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET)
			{
				int woke = futex_wake(nd, uaddr, (int) a[2]);
				g_resp->val = woke; break;
			}
			die(1, "node %d: futex op %d has no model (only WAIT/WAKE)", nd->id, cmd);
			break;
		}
		case SYS_exit:
		{
			/* this thread is exiting: free its slot and do the kernel's
			 * CLONE_CHILD_CLEARTID wake (zero the clear-tid word and wake a
			 * futex waiter on it), so a thread blocked in pthread_join
			 * completes.  CONTINUE lets the real thread die. */
			if (th->ctid_addr)
			{
				uint32_t zero = 0;
				if (cr_write(nd->pid, th->ctid_addr, &zero, sizeof zero) != sizeof zero) cr_fail(nd->id, "a thread exit clears its tid word");
				futex_wake(nd, th->ctid_addr, 1);
			}
			th->inuse = 0; th->block_kind = 0;
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		default: g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
	}
	if (ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_resp) != 0 && errno != ENOENT) {}
	return 1;
}

/* detect that a node has exited (its listener gives ENOTCONN/POLLHUP) */
static void
reap(Node *nd)
{
	if (!nd->alive) return;
	int st; pid_t r = waitpid(nd->pid, &st, WNOHANG);
	/* clear the pid once reaped, so kill_all never signals a reused one */
	if (r == nd->pid) { nd->alive = 0; nd->pid = 0; drain_canon(nd); }
}

/* answer a thread's deferred syscall with return value val, clearing its
 * slot (the thread resumes running) */
static void
answer(Node *nd, Thread *th, long val)
{
	memset(g_resp, 0, g_sz.seccomp_notif_resp); g_resp->id = th->block_id; g_resp->val = val;
	ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_resp);
	th->block_kind = 0;
}

/* wake up to n threads parked in BK_FUTEX on uaddr: answer each woken
 * futex with 0 and return how many woke.  A FUTEX_WAKE from the waker's
 * step; it invents no wakes.  When n would wake a strict subset of
 * several same-address waiters, which subset wakes is a schedule this
 * model does not enumerate, so rather than pick one silently (and risk
 * hiding a bug) the checker errors.  waiters == 1 and n >= waiters (the
 * condvar-signal and broadcast cases, and the loop-plus-one-worker shape
 * the eval asks for) are unambiguous. */
/* Mark up to n threads parked in BK_FUTEX on uaddr as runnable (woken),
 * and return how many.  A FUTEX_WAKE from the waker's step; it invents no
 * wakes.  The woken threads are NOT resumed here: drive_quiescent
 * releases them one at a time only once every other thread of the node is
 * parked, so threads run serially and the kernel's atomic futex and mutex
 * semantics are never torn by two threads running at once.  When n would
 * wake a strict subset of several same-address waiters, which subset
 * wakes is a schedule this model does not enumerate, so rather than pick
 * one silently (and risk hiding a bug) the checker errors.  waiters == 1
 * and n >= waiters (the condvar-signal and broadcast cases, and the
 * loop-plus-one-worker shape the eval asks for) are unambiguous. */
static int
futex_wake(Node *nd, uint64_t uaddr, int n)
{
	int waiters = 0;
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && nd->th[i].block_kind == BK_FUTEX && nd->th[i].futex_addr == uaddr && !nd->th[i].woken) waiters++;
	if (waiters == 0) return 0;
	if (n < waiters && n < 0x7fffffff)
		die(1, "node %d: FUTEX_WAKE of %d would wake a subset of %d waiters on one word; the thread model does not enumerate which (a lock contended by a pool is out of scope)", nd->id, n, waiters);
	int woke = 0;
	for (int i = 0; i < nd->nth && woke < n; i++)
		if (nd->th[i].inuse && nd->th[i].block_kind == BK_FUTEX && nd->th[i].futex_addr == uaddr && !nd->th[i].woken)
		{ nd->th[i].woken = 1; woke++; }
	return woke;
}

/* Release one woken thread on node nd: answer its parked futex with 0 so
 * it resumes and runs to its next owned syscall.  Called by
 * drive_quiescent only when the node has no other running or pending-wake
 * thread, so execution stays serial.  Returns 1 if it released one. */
static int
release_one_woken(Node *nd)
{
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && nd->th[i].woken)
		{
			nd->th[i].woken = 0;
			answer(nd, &nd->th[i], 0);
			return 1;
		}
	return 0;
}

/* answer a thread's deferred recv with up to len bytes (0 = EOF);
 * returns how many it took -- a short read leaves the rest queued */
static int
deliver(Node *nd, Thread *th, const unsigned char *body, int len)
{
	int k = len < th->block_want ? len : th->block_want;
	if (k > 0 && cr_write(nd->pid, th->block_bufaddr, body, (size_t) k) != (size_t) k) cr_fail(nd->id, "a recv buffer");
	answer(nd, th, k);
	return k;
}

/* answer a thread's deferred accept with a connection from peer */
static void
accept_from(Node *nd, Thread *th, int peer)
{
	for (int k = 0; k < nd->npend; k++)
		if (nd->pend[k] == peer) { nd->pend[k] = nd->pend[--nd->npend]; break; }
	int fd = nd->next_fd++;
	cfd_set(nd, fd, peer);
	answer(nd, th, fd);
}

/* CRASH a node (M6): drain its latest emits, kill it, then keep only the
 * DURABLE state (fd 193, what it had fsync'd) and discard the volatile
 * state (fd 3).  A peer reading from a connection to it may then get
 * EOF (the EOF action, once the channel is drained).  This is
 * the engine's crash model (lose volatile, keep durable) on a real
 * process: a protocol that acked a client before the entry was durable
 * on a quorum loses a committed entry here. */
static void
crash_node(Node *nd)
{
	if (!nd->alive) return;
	drain_canon(nd);
	kill(nd->pid, SIGKILL); int st; waitpid(nd->pid, &st, 0);
	nd->pid = 0; nd->alive = 0; nd->crashed = 1; nd->npend = 0;
	nd->nth = 0; nd->th_pending = 0; nd->npending_ctid = 0; memset(nd->th, 0, sizeof nd->th);
	/* volatile is gone; the node's observable state is what was durable */
	memcpy(nd->canon, nd->durable, nd->durable_len); nd->canon_len = nd->durable_len;
	/* close the parent's fds for the dead process (the final drain is done,
	 * the durable bytes are in nd->durable); a RESTART opens fresh ones, so
	 * leaving these open would leak one listener and two pipe ends per
	 * crash across a long search */
	if (nd->listener > 0) close(nd->listener);
	if (nd->fd3[0] > 0) close(nd->fd3[0]);
	if (nd->fd4[0] > 0) close(nd->fd4[0]);
	nd->listener = nd->fd3[0] = nd->fd4[0] = -1;
}

/* RESTART a crashed node (Tier 3 ask 6): relaunch it as a fresh process
 * whose surviving durable record it can read back from descriptor 193.
 * crash_node kept the durable bytes (nd->durable); launch() would zero
 * them, so relaunch saves and restores them, and resets the read offset
 * so the new process's first read of fd 193 returns the surviving record
 * (a server that reads its own log recovers).  The relaunched process is
 * a fresh single-main-thread program: launch() already clears the thread
 * table, the connections and the per-node fds, and it is driven to
 * quiescence by the caller exactly as a fresh launch is.  Returns 0, or
 * -1 if the process would not start. */
static int
relaunch(Node *nd)
{
	if (nd->alive || !nd->crashed) return -1;
	unsigned char saved[SNAP]; int savedlen = nd->durable_len;
	memcpy(saved, nd->durable, (size_t) savedlen);
	if (launch(nd)) return -1;
	memcpy(nd->durable, saved, (size_t) savedlen); nd->durable_len = savedlen;
	nd->durable_read_off = 0;
	return 0;
}

/* run all nodes to quiescence: every live node has every thread parked
 * in a deferred slot (recv/accept/poll/futex) and no thread still being
 * born, or the node exited.  A node with a thread neither parked nor
 * exited is still running between syscalls -- we must wait for its next
 * notification, not treat the lull as quiescence. */
static void
drive_quiescent(void)
{
	for (;;)
	{
		for (int i = 0; i < M.nodes; i++) reap(&N[i]);
		/* service a notification from each node with a running thread (or a
		 * birth in flight).  The RECV ioctl waits for the running thread's
		 * next owned syscall, which drives the node forward.  A node whose
		 * only non-parked thread is merely woken is NOT serviced here (no
		 * thread of it is about to notify; it is advanced by release below),
		 * and a fully quiescent node is skipped. */
		int did = 0;
		for (int i = 0; i < M.nodes; i++)
			if (node_has_running(&N[i]))
				did |= try_pending_for(&N[i]);
		if (did) continue;
		/* release a woken thread (one marked runnable by a FUTEX_WAKE) on
		 * any node that has no thread currently running: this keeps
		 * execution serial (one thread runs to its next syscall at a time),
		 * so the real kernel's atomic futex/mutex semantics are never torn
		 * by two threads of a node running concurrently. */
		int released = 0;
		for (int i = 0; i < M.nodes; i++)
			if (N[i].alive && !node_has_running(&N[i]))
				released |= release_one_woken(&N[i]);
		if (released) continue;
		/* wait for a node that still has a running thread to make a syscall */
		struct pollfd pf[MAXNODES]; int np = 0;
		for (int i = 0; i < M.nodes; i++)
			if (node_has_running(&N[i])) { pf[np].fd = N[i].listener; pf[np].events = POLLIN; np++; }
		if (np == 0) break;			/* every node quiescent or dead */
		int r = poll(pf, np, LULL_MS);
		if (r < 0 && errno == EINTR) continue;
		/* a notify fd that reports POLLHUP has lost its owning process: the
		 * node exited (its last thread ran off the end or called
		 * exit_group).  waitpid(WNOHANG) can momentarily still see a
		 * ptrace-seized tracee as not-yet-reapable, so reap it directly
		 * here rather than spin until WNOHANG catches up. */
		int hup = 0;
		for (int i = 0; i < np; i++)
			if (pf[i].revents & (POLLHUP | POLLERR))
				for (int j = 0; j < M.nodes; j++)
					if (N[j].alive && N[j].listener == pf[i].fd)
					{ int st; waitpid(N[j].pid, &st, 0); N[j].alive = 0; N[j].pid = 0; drain_canon(&N[j]); hup = 1; }
		if (hup) continue;
		if (r == 0)
		{
			/* a running thread made no syscall the checker owns for
			 * LULL_MS: it is busy, or blocked in a call the checker does
			 * not model.  Treating that as quiescence would snapshot a
			 * node mid-step, so it is an error. */
			for (int i = 0; i < M.nodes; i++)
			{
				reap(&N[i]);
				if (node_has_running(&N[i]))
					die(1, "node %d made no checked syscall for %d ms (a thread is busy, or blocked in a call the checker does not model)", i, LULL_MS);
			}
		}
	}
	for (int i = 0; i < M.nodes; i++) if (N[i].alive) drain_canon(&N[i]);
}

/* =================================================================== */
/* stateless-replay search over action sequences                       */
/* =================================================================== */
static uint64_t rotl64(uint64_t x,int r){return (x<<r)|(x>>(64-r));}
static uint64_t fmix64(uint64_t k){k^=k>>33;k*=0xff51afd7ed558ccdULL;k^=k>>33;k*=0xc4ceb9fe1a85ec53ULL;k^=k>>33;return k;}
static void fp128(const uint8_t*p,size_t n,uint64_t fp[2]){
	uint64_t h1=0x9368e53c2f6af274ULL^(n*0x87c37b91114253d5ULL),h2=0x586dcd208f7cd3fdULL+n;
	for(size_t i=0;i<n;i++){h1^=(uint64_t)p[i]<<((i%8)*8);h1=rotl64(h1,27)*5+0x52dce729;h2=rotl64(h2+p[i]+i,23)*0xd6e8feb86659fd93ULL;}
	fp[0]=fmix64(h1);fp[1]=fmix64(h2+n);
}

/* the seen set: an open-addressing table of 128-bit fingerprints that
 * grows at half load, so the search never forgets a state */
typedef struct { uint64_t a, b; } FP;
static FP *seen; static size_t seen_cap, nseen;

static void
seen_put(FP f)
{
	size_t i = f.a & (seen_cap - 1);
	while (seen[i].a | seen[i].b) i = (i + 1) & (seen_cap - 1);
	seen[i] = f;
}

/* 1 if the fingerprint is new (and records it), 0 if seen before */
static int
mark(uint64_t a, uint64_t b)
{
	if (!(a | b)) a = 1;				/* (0,0) marks an empty slot */
	if (2 * (nseen + 1) > seen_cap)
	{
		FP *old = seen; size_t oc = seen_cap;
		seen_cap = oc ? 2 * oc : 4096;
		seen = calloc(seen_cap, sizeof *seen);
		if (!seen) die(1, "out of memory for the seen set at %zu states", nseen);
		for (size_t i = 0; i < oc; i++) if (old[i].a | old[i].b) seen_put(old[i]);
		free(old);
	}
	for (size_t i = a & (seen_cap - 1);; i = (i + 1) & (seen_cap - 1))
	{
		if (!(seen[i].a | seen[i].b)) { seen[i].a = a; seen[i].b = b; nseen++; return 1; }
		if (seen[i].a == a && seen[i].b == b) return 0;
	}
}

/* Encode a node's per-thread blocked state into out (<= cap bytes),
 * canonicalised so the raw kernel tid never leaks in: each live thread
 * contributes a fixed-width record of (block_kind, what-it-waits-on), the
 * records are sorted, and a futex wait address is replaced by its RANK
 * among the distinct futex words the node's threads wait on (sorted by
 * address).  Two runs that assign different tids to the same thread
 * roles, or land condvar words at different ASLR'd addresses, then
 * produce the same encoding -- so dedup keys on thread role/state, not
 * identity.  Returns the number of bytes written. */
#define THREC 20				/* fixed width of one thread's record */
static int
encode_threads(const Node *nd, unsigned char *out, int cap)
{
	/* distinct futex words this node waits on, sorted, for a stable rank */
	uint64_t fw[MAXTHREAD]; int nfw = 0;
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && nd->th[i].block_kind == BK_FUTEX)
		{
			uint64_t a = nd->th[i].futex_addr; int dup = 0;
			for (int j = 0; j < nfw; j++) if (fw[j] == a) { dup = 1; break; }
			if (!dup) fw[nfw++] = a;
		}
	for (int i = 0; i < nfw; i++)		/* insertion sort, nfw <= 8 */
		for (int j = i + 1; j < nfw; j++)
			if (fw[j] < fw[i]) { uint64_t t = fw[i]; fw[i] = fw[j]; fw[j] = t; }

	unsigned char rec[MAXTHREAD][THREC]; int nrec = 0;
	for (int i = 0; i < nd->nth; i++)
	{
		const Thread *th = &nd->th[i];
		if (!th->inuse) continue;
		unsigned char *r = rec[nrec]; memset(r, 0, THREC); int p = 0;
		r[p++] = (unsigned char) (th->block_kind ? th->block_kind : 3);
		if (th->block_kind == BK_RECV || th->block_kind == BK_ACCEPT)
			r[p++] = (unsigned char) (th->block_peer + 2);
		else if (th->block_kind == BK_FUTEX)
		{
			int rank = 0; for (int j = 0; j < nfw; j++) if (fw[j] == th->futex_addr) { rank = j; break; }
			r[p++] = (unsigned char) rank;
		}
		else if (th->block_kind == BK_POLL)
		{
			/* the polled set as semantic classes, as the single-slot code
			 * did: (events, class, which) per fd, so two differently
			 * polling threads stay distinct without raw fd numbers.  A leading
			 * flag marks an epoll_wait park apart from a poll() one, so the
			 * two parking modes never merge in the fingerprint. */
			r[p++] = (unsigned char) (th->epoll_fd >= 0 ? 1 : 0);
			r[p++] = (unsigned char) th->pollfd_n;
			for (int k = 0; k < th->pollfd_n && p + 3 <= THREC; k++)
			{
				int fd = th->pollfds[k];
				int cs = cfd_slot((Node *) nd, fd), es = efd_slot((Node *) nd, fd);
				r[p++] = (unsigned char) th->pollevents[k];
				if (cs >= 0) { r[p++] = 1; r[p++] = (unsigned char) (nd->cpeer[cs] + 2); }
				else if (es >= 0) { r[p++] = 2; r[p++] = (unsigned char) es; }
				else { r[p++] = 3; r[p++] = 0; }
			}
		}
		nrec++;
	}
	/* sort the records so the multiset, not thread order, is the key */
	for (int i = 0; i < nrec; i++)
		for (int j = i + 1; j < nrec; j++)
			if (memcmp(rec[j], rec[i], THREC) < 0)
			{ unsigned char t[THREC]; memcpy(t, rec[i], THREC); memcpy(rec[i], rec[j], THREC); memcpy(rec[j], t, THREC); }
	int n = 0;
	if (1 + nrec * THREC > cap) return -1;
	out[n++] = (unsigned char) nrec;
	for (int i = 0; i < nrec; i++) { memcpy(out + n, rec[i], THREC); n += THREC; }
	return n;
}

/* world fingerprint: every node's state and liveness, the messages in
 * flight (each channel in its order), the reply log, the environment
 * budgets left and the logical clock.  Everything the future depends on
 * goes in; leaving out a budget or the clock merges worlds that differ. */
static void
world_fp(uint64_t fp[2])
{
	unsigned char w[16384]; size_t n = 0;
#define PUT(b) do { if (n >= sizeof w) die(1, "world encoding exceeds %zu bytes", sizeof w); w[n++] = (unsigned char) (b); } while (0)
	for (int i = 0; i < M.nodes; i++)
	{
		/* node liveness: a running node (fully alive), a crashed one, or an
		 * exited one.  The per-thread blocked state follows, canonicalised
		 * by encode_threads so no raw tid leaks into the fingerprint. */
		PUT(N[i].alive ? 3 : (N[i].crashed ? 0xfe : 0xff));
		PUT(N[i].canon_len);
		for (int k = 0; k < N[i].canon_len; k++) PUT(N[i].canon[k]);
		if (N[i].alive)
		{
			unsigned char tb[1 + MAXTHREAD * THREC];
			int tl = encode_threads(&N[i], tb, (int) sizeof tb);
			if (tl < 0) die(1, "thread encoding overflow on node %d", i);
			for (int k = 0; k < tl; k++) PUT(tb[k]);
		}
		else PUT(0);
		/* live connections in the order they were opened (cfd is kept in
		 * that order), by peer, with each one's O_NONBLOCK flag: which
		 * connection goes where, and whether a read on it would return
		 * EAGAIN, is node state.  shortcut: absolute fd numbers are left
		 * out, on the assumption that a target does not branch on a
		 * virtual fd's value, only on its peer and its flags */
		PUT(N[i].alive ? N[i].nc : 0);
		for (int k = 0; N[i].alive && k < N[i].nc; k++) { PUT(N[i].cpeer[k] + 2); PUT(N[i].cnonblock[k]); }
		/* eventfds and their counters: a non-zero counter is poll-ready,
		 * so two states with different counters are different */
		PUT(N[i].alive ? N[i].nefd : 0);
		for (int k = 0; N[i].alive && k < N[i].nefd; k++)
			for (int b = 0; b < 8; b++) PUT(N[i].efdval[k] >> (8 * b));
		PUT(N[i].ls_nonblock);
		/* epoll instances: each one's interest list, canonicalised to
		 * (requested events, fd's semantic class, which peer/eventfd), the
		 * same way a polled set is.  The watched fd's identity is its role,
		 * never a raw fd number, and the user data is deliberately left out:
		 * it is opaque (an ASLR pointer in a real loop), only ever written
		 * back to the node, never a search distinction, so fingerprinting it
		 * would wrongly split identical futures.  Which fds an instance
		 * watches IS the future (it decides what epoll_wait can report), so
		 * it goes in. */
		PUT(N[i].alive ? N[i].nep : 0);
		for (int e = 0; N[i].alive && e < N[i].nep; e++)
		{
			const Epoll *ep = &N[i].ep[e];
			PUT(ep->nit);
			for (int k = 0; k < ep->nit; k++)
			{
				int fd = ep->fd[k];
				int cs = cfd_slot(&N[i], fd), es = efd_slot(&N[i], fd);
				PUT(ep->events[k]);
				if (cs >= 0) { PUT(1); PUT(N[i].cpeer[cs] + 2); }
				else if (es >= 0) { PUT(2); PUT(es); }
				else if (fd == N[i].listener_fd) { PUT(3); PUT(0); }
				else { PUT(4); PUT(0); }
			}
		}
	}
	/* channel by channel, each in the order it is delivered */
	for (int s = -1; s < M.nodes; s++)
		for (int d = 0; d < M.nodes; d++)
			for (int last = -1;;)
			{
				int best = -1;
				for (int i = 0; i < MAXMSG; i++)
					if (bag[i].used && bag[i].src == s && bag[i].dst == d && bag[i].chseq > last &&
						(best < 0 || bag[i].chseq < bag[best].chseq))
						best = i;
				if (best < 0) break;
				last = bag[best].chseq;
				PUT(s + 1); PUT(d); PUT(bag[best].len);
				for (int k = 0; k < bag[best].len; k++) PUT(bag[best].body[k]);
			}
	PUT(0xfe);
	/* reset connections, by node pair: a reset channel reaches EOF
	 * forever, so two worlds that differ only in which pair is reset are
	 * distinct futures */
	for (int a = 0; a < M.nodes; a++)
		for (int b = a + 1; b < M.nodes; b++)
			PUT(conn_reset[a][b]);
	/* connections waiting to be accepted, as a multiset per node */
	for (int d = 0; d < M.nodes; d++)
		for (int s = 0; s < M.nodes; s++)
		{
			int c = 0;
			for (int k = 0; k < N[d].npend; k++) c += N[d].pend[k] == s;
			PUT(c);
		}
	PUT(nrep);
	for (int i = 0; i < nrep; i++) { PUT(rep_src[i]); PUT(rep_val[i]); }
	PUT(bud.timeouts); PUT(bud.crashes); PUT(bud.drops); PUT(bud.restarts);
	for (int k = 0; k < 8; k++) PUT(logical_ns >> (8 * k));
#undef PUT
	fp128(w, n, fp);
}

static void
reset_world(void)
{
	memset(bag, 0, sizeof bag); memset(chcount, 0, sizeof chcount);
	memset(conn_reset, 0, sizeof conn_reset);
	nrep = 0; logical_ns = CLK_BASE;
	bud.timeouts = M.timeouts; bud.crashes = M.crashes; bud.drops = M.drops;
	bud.restarts = M.restarts;
}

/* One step the search can take at a quiescent point.  The list is
 * rebuilt in the same order on every replay, so an index names an
 * action; the shortcut keeps action lists small enough to replay. */
enum { A_ACCEPT, A_DELIVER, A_EOF, A_TIMEOUT, A_CRASH, A_DROP, A_WAKE, A_RESTART };
typedef struct { int kind, node, peer, thr; } Act;

/* Enumerate the actions enabled now:
 *   - ACCEPT: a thread blocked in accept takes a connection waiting at it;
 *   - DELIVER: a thread blocked in recv gets the head of its channel;
 *   - EOF: its peer has crashed and the channel is drained, so recv
 *     returns 0;
 *   - TIMEOUT (budgeted): the recv is satisfied by the timer instead;
 *   - CRASH (budgeted): a live node dies, keeping only its durable state;
 *   - DROP (budgeted): the network loses the head of a channel;
 *   - RESTART (budgeted): a crashed node is relaunched as a fresh process
 *     that can read its surviving durable record back on startup;
 *   - WAKE: a thread parked in poll() is told one polled fd is ready
 *     (POLLIN).  WAKE never consumes a message: the thread then
 *     accept()s or read()s that fd and defers again (BK_ACCEPT/BK_RECV),
 *     which the existing ACCEPT/DELIVER/TIMEOUT actions service, so poll
 *     readiness stays a search decision like a deferred recv (M5).
 * A futex wait (BK_FUTEX) has no environment action: it is woken only by
 * another thread's FUTEX_WAKE, which the search already drives by
 * servicing that other thread.  Each per-thread action carries the
 * thread's slot index (thr), stable across replay because threads are
 * allocated in notification order.  Every environment action is bounded
 * so the search stays finite. */
static int
enum_actions(Act *out)
{
	int c = 0;
#define ADD(k, nd, pr, tr) do { if (c >= MAXACT) die(1, "more than %d actions enabled at one point", MAXACT); \
	out[c].kind = (k); out[c].node = (nd); out[c].peer = (pr); out[c].thr = (tr); c++; } while (0)
	for (int d = 0; d < M.nodes; d++)
	{
		Node *nd = &N[d];
		if (!nd->alive) continue;
		for (int ti = 0; ti < nd->nth; ti++)
		{
			Thread *th = &nd->th[ti];
			if (!th->inuse || !th->block_kind) continue;
			if (th->block_kind == BK_ACCEPT)
			{
				for (int s = -1; s < M.nodes; s++)
				{
					int waiting = 0;
					for (int k = 0; k < nd->npend; k++) waiting |= nd->pend[k] == s;
					if (waiting) ADD(A_ACCEPT, d, s, ti);
				}
				/* a server idle in accept() can have its election timer fire:
				 * the timeout arrives as a synthetic timer connection it
				 * accepts and reads a 'T' from (peer = TIMER_PEER) */
				if (bud.timeouts > 0) ADD(A_TIMEOUT, d, TIMER_PEER, ti);
				continue;
			}
			if (th->block_kind == BK_POLL)
			{
				/* offer a wake for each polled fd the search can make ready:
				 * a polled channel with a pending message (deliver it after),
				 * a polled listener with a connection waiting (accept after),
				 * an eventfd with a non-zero counter.  A budgeted timeout can
				 * wake a polled channel too (the fired-timer 'T' delivery). */
				int saw_chan = 0;
				for (int k = 0; k < th->pollfd_n; k++)
				{
					int fd = th->pollfds[k];
					if (fd < 0) continue;	/* a negative fd is ignored by poll(2) */
					int cs = cfd_slot(nd, fd), es = efd_slot(nd, fd);
					if (cs >= 0)
					{
						int peer = nd->cpeer[cs];
						saw_chan = 1;
						if (chan_head(peer, d) >= 0) ADD(A_WAKE, d, peer, ti);
						else if (peer >= 0 && !N[peer].alive) ADD(A_WAKE, d, peer, ti);
					}
					else if (es >= 0)
					{
						if (nd->efdval[es] > 0) ADD(A_WAKE, d, POLL_LISTENER - 1 - es, ti);
					}
					else if (fd == nd->listener_fd)
					{
						if (nd->npend > 0) ADD(A_WAKE, d, POLL_LISTENER, ti);
					}
					else
					{
						/* a polled fd that is none of this node's connections,
						 * eventfds or its listener is a kernel fd the model does
						 * not track (a self-pipe, a timerfd).  It is never treated
						 * as the listener, so a wake is not offered for it; a loop
						 * that only ever becomes ready through such an fd makes no
						 * progress, which the no-progress check reports. */
					}
				}
				if (saw_chan && bud.timeouts > 0) ADD(A_TIMEOUT, d, TIMER_PEER, ti);
				continue;
			}
			if (th->block_kind == BK_FUTEX) continue;	/* woken by another thread's step */
			int p = th->block_peer;
			if (chan_head(p, d) >= 0) ADD(A_DELIVER, d, p, ti);
			else if (p >= 0 && !N[p].alive) ADD(A_EOF, d, p, ti);	/* peer gone, channel drained */
			if (bud.timeouts > 0) ADD(A_TIMEOUT, d, p, ti);
		}
	}
	if (bud.crashes > 0)
		for (int d = 0; d < M.nodes; d++)
			if (N[d].alive) ADD(A_CRASH, d, -1, -1);
	if (bud.drops > 0)
		for (int s = 0; s < M.nodes; s++)
			for (int d = s + 1; d < M.nodes; d++)
				/* one reset per node pair, offered while the connection
				 * carries a message in either direction and is not already
				 * reset.  A reset discards both channels and gives both ends
				 * EOF, so there is nothing to reset twice. */
				if (!conn_reset[s][d] && (chan_head(s, d) >= 0 || chan_head(d, s) >= 0))
					ADD(A_DROP, d, s, -1);
	if (bud.restarts > 0)
		for (int d = 0; d < M.nodes; d++)
			if (!N[d].alive && N[d].crashed) ADD(A_RESTART, d, -1, -1);
#undef ADD
	return c;
}

/* Wake a thread parked in poll() or epoll_wait(): mark one watched fd
 * ready (POLLIN/EPOLLIN) and answer the call with count 1, consuming no
 * message.  target names the fd: a peer id (a channel), POLL_LISTENER
 * (the listener), or POLL_LISTENER-1-slot (an eventfd slot).  The thread
 * then reads/accepts that fd and defers again, which the search
 * services.  A poll() park has its pollfd array updated in place; an
 * epoll_wait park (epoll_fd >= 0) instead gets a single struct
 * epoll_event written back, carrying the user data the loop registered
 * for the ready fd -- that data is how the loop routes to the fd's
 * handler, so it is returned verbatim. */
static void
poll_wake(Node *nd, Thread *th, int target)
{
	int hit = -1;
	for (int k = 0; k < th->pollfd_n; k++)
	{
		int fd = th->pollfds[k];
		int cs = cfd_slot(nd, fd), es = efd_slot(nd, fd);
		int match = (target == TIMER_PEER) ? (fd == nd->timer_fd)
			: (target >= -1) ? (cs >= 0 && nd->cpeer[cs] == target)
			: (target == POLL_LISTENER) ? (fd == nd->listener_fd)
			: (es >= 0 && es == (POLL_LISTENER - 1 - target));
		if (match && hit < 0) hit = k;
	}
	if (hit < 0) die(1, "poll wake on node %d found no polled fd for target %d", nd->id, target);
	if (th->epoll_fd >= 0)
	{
		/* return one ready struct epoll_event (packed on x86-64) carrying
		 * the fd's stored user data */
		struct __attribute__((packed)) { uint32_t events; uint64_t data; } ev;
		ev.events = (th->pollevents[hit] & POLLIN) ? EPOLLIN : 0;
		ev.data = th->polldata[hit];
		if (cr_write(nd->pid, th->block_pollarr, &ev, sizeof ev) != sizeof ev) cr_fail(nd->id, "epoll_wait returns an event");
	}
	else
	{
		struct pollfd pfs[MAXPOLL];
		for (int k = 0; k < th->pollfd_n; k++)
		{
			pfs[k].fd = th->pollfds[k]; pfs[k].events = th->pollevents[k];
			pfs[k].revents = (k == hit) ? (short) (th->pollevents[k] & POLLIN) : 0;
		}
		size_t want = (size_t) th->pollfd_n * sizeof pfs[0];
		if (cr_write(nd->pid, th->block_pollarr, pfs, want) != want) cr_fail(nd->id, "poll returns revents");
	}
	/* a woken channel's next read must deliver, not return EAGAIN; a timer
	 * wake is serviced by the timer-'T' read path, not a message */
	th->poll_woke_fd = (target >= -1) ? th->pollfds[hit] : -1;
	answer(nd, th, 1);
}

/* Reset the connection between nodes a and b (a DROP): discard every
 * queued message in both directions, mark the pair reset, and tell both
 * ends at once.  A node parked in recv on the other sees EOF; a node
 * parked in poll on a channel to the other is woken so its next read
 * sees EOF.  Modelling a drop as a reset (not a mid-stream byte loss)
 * keeps the process front end faithful to TCP, which never loses a byte
 * from a live stream -- a failure is always visible to both ends. */
static void
reset_conn(int a, int b)
{
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used &&
			((bag[i].src == a && bag[i].dst == b) || (bag[i].src == b && bag[i].dst == a)))
			bag[i].used = 0;
	conn_reset[a][b] = conn_reset[b][a] = 1;
	/* answer either end's threads that are already parked waiting on the
	 * other: a recv sees EOF, a poll on a channel to the other is woken */
	int ends[2] = {a, b}, peers[2] = {b, a};
	for (int e = 0; e < 2; e++)
	{
		Node *nd = &N[ends[e]];
		if (!nd->alive) continue;
		for (int ti = 0; ti < nd->nth; ti++)
		{
			Thread *th = &nd->th[ti];
			if (!th->inuse || !th->block_kind) continue;
			if (th->block_kind == BK_RECV && th->block_peer == peers[e])
				deliver(nd, th, NULL, 0);		/* EOF */
			else if (th->block_kind == BK_POLL)
				for (int k = 0; k < th->pollfd_n; k++)
				{
					int cs = cfd_slot(nd, th->pollfds[k]);
					if (cs >= 0 && nd->cpeer[cs] == peers[e]) { poll_wake(nd, th, peers[e]); break; }
				}
		}
	}
}

/* Break the connections a peer still holds to a node that is restarting.
 * The crashed node's TCP connections are gone; the new process starts
 * with none, so a peer that was connected sees EOF on its stale fd and
 * must re-dial to reach the new process.  For each peer: discard both
 * channels' messages, answer a thread parked recv'ing on the node with
 * EOF, and drop the stale connection from the peer's table (as the
 * peer's close of a reset socket would).  A re-dial already queued at a
 * peer (a pend) is kept, so the restarted process accepts the
 * reconnection.
 *
 * A peer parked in poll() on a connection to the restarting node is the
 * one case the pair-keyed reset model cannot represent faithfully
 * alongside a fresh re-dial on the same pair, so rather than guess (and
 * risk hiding a bug) the checker errors -- an honest boundary, like the
 * futex-subset case.  The request/reply recovery ask 6 targets does not
 * reach it. */
static void
break_peer_links(int node)
{
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && (bag[i].src == node || bag[i].dst == node))
			bag[i].used = 0;
	for (int p = 0; p < M.nodes; p++)
	{
		Node *nd = &N[p];
		if (p == node || !nd->alive) continue;
		for (int ti = 0; ti < nd->nth; ti++)
		{
			Thread *th = &nd->th[ti];
			if (!th->inuse || !th->block_kind) continue;
			if (th->block_kind == BK_RECV && th->block_peer == node)
				deliver(nd, th, NULL, 0);		/* EOF on the broken connection */
			else if (th->block_kind == BK_POLL)
				for (int k = 0; k < th->pollfd_n; k++)
				{
					int cs = cfd_slot(nd, th->pollfds[k]);
					if (cs >= 0 && nd->cpeer[cs] == node)
						die(1, "node %d polls a connection to node %d as it restarts; the reset model does not represent a surviving poll across a restart", p, node);
				}
		}
		/* drop the peer's stale connections to the restarting node (the
		 * broken socket; a new one is a fresh re-dial) */
		for (int k = 0; k < nd->nc; )
			if (nd->cpeer[k] == node)
			{
				nd->nc--;
				for (int j = k; j < nd->nc; j++) { nd->cfd[j] = nd->cfd[j + 1]; nd->cpeer[j] = nd->cpeer[j + 1]; nd->cnonblock[j] = nd->cnonblock[j + 1]; nd->ccloexec[j] = nd->ccloexec[j + 1]; }
			}
			else k++;
		/* drop the dead process's outbound dials still queued at this peer's
		 * listener; a re-dial from the new process is a fresh connect that
		 * arrives after the relaunch */
		for (int k = 0; k < nd->npend; )
			if (nd->pend[k] == node) nd->pend[k] = nd->pend[--nd->npend];
			else k++;
	}
}

static void
apply(const Act *a)
{
	Node *nd = &N[a->node];
	Thread *th = (a->thr >= 0 && a->thr < nd->nth) ? &nd->th[a->thr] : NULL;
	switch (a->kind)
	{
		case A_ACCEPT: accept_from(nd, th, a->peer); break;
		case A_DELIVER:
		{
			/* one write per read: a read never joins two queued writes
			 * (shortcut: real TCP may coalesce; upgrade if a target
			 * relies on it) */
			int h = chan_head(a->peer, a->node);
			int k = deliver(nd, th, bag[h].body, bag[h].len);
			if (k < bag[h].len) { memmove(bag[h].body, bag[h].body + k, (size_t) (bag[h].len - k)); bag[h].len -= k; }
			else bag[h].used = 0;
			break;
		}
		case A_EOF: deliver(nd, th, NULL, 0); break;
		case A_TIMEOUT:
		{
			static const unsigned char t = 'T';
			bud.timeouts--;
			logical_ns += CLK_TICK;			/* time advances only here */
			if (th->block_kind == BK_ACCEPT)
			{
				/* answer the accept() with a timer connection; the node's
				 * next recv on it returns 'T' (the recv handler) */
				int fd = nd->next_fd++;
				cfd_set(nd, fd, TIMER_PEER);
				nd->timer_fd = fd;
				answer(nd, th, fd);
			}
			else if (th->block_kind == BK_POLL)
			{
				/* a fired timer wakes the poll on the first polled channel
				 * fd; that fd's next read returns the timer's 'T' */
				for (int k = 0; k < th->pollfd_n; k++)
				{
					int cs = cfd_slot(nd, th->pollfds[k]);
					if (cs >= 0) { nd->timer_fd = th->pollfds[k]; break; }
				}
				poll_wake(nd, th, TIMER_PEER);
			}
			else
				deliver(nd, th, &t, 1);
			break;
		}
		case A_WAKE: poll_wake(nd, th, a->peer); break;
		case A_CRASH: bud.crashes--; crash_node(nd); break;
		case A_DROP: bud.drops--; reset_conn(a->node, a->peer); break;
		case A_RESTART:
			bud.restarts--;
			break_peer_links(a->node);	/* the old process's connections are gone */
			if (relaunch(nd)) die(3, "node %d would not restart", a->node);
			break;
	}
	drive_quiescent();
}

/* C2 (--refine-steps): when g_step_refine is set, run_seq performs the
 * action-level refinement check at the initial state and after every
 * replayed action (a genuine transition chain).  step_refine_here is
 * defined with the other refinement helpers below; it is forward-
 * declared here so run_seq can call it. */
static int g_step_refine;
static int g_step_rc = -1;
static const char *g_step_name;
static int step_refine_here(const char **name);

/* Replay `seq` from a fresh launch.  Returns 0, or -1 if a node would not
 * start.  On return the world is at the quiescent point after the last
 * action, and acts/nact list what is enabled there. */
static int
run_seq(const int *seq, int seqlen, Act *acts, int *nact)
{
	reset_world();
	for (int i = 0; i < M.nodes; i++) { N[i].id = i; if (launch(&N[i])) return -1; }
	if (M.client_write >= 0)
	{
		/* the client dials node 0 and sends WRITE(v) on that connection */
		unsigned char w[2] = {'W', (unsigned char) M.client_write};
		N[0].pend[N[0].npend++] = -1;
		bag_add(-1, 0, w, 2);
	}
	drive_quiescent();
	/* without the preload a node reads the real clock and the real heap,
	 * and replay is no longer deterministic: refuse rather than guess */
	for (int i = 0; i < M.nodes; i++)
		if (!N[i].preload_ok)
			die(3, "node %d did not load the preload %s (set CMC_PRELOAD to its path)", i, preload_path);
	if (g_step_refine) { cmc_refine_reset(); g_step_rc = step_refine_here(&g_step_name); if (g_step_rc != -1) return 0; }
	for (int s = 0; s < seqlen; s++)
	{
		Act a[MAXACT]; int na = enum_actions(a);
		if (seq[s] < 0 || seq[s] >= na)
			die(1, "replay diverged at step %d: action %d of %d (is the target deterministic?)", s, seq[s], na);
		apply(&a[seq[s]]);
		if (g_step_refine) { g_step_rc = step_refine_here(&g_step_name); if (g_step_rc != -1) return 0; }
	}
	*nact = enum_actions(acts);
	return 0;
}

static void
kill_all(void)
{
	for (int i = 0; i < M.nodes; i++)
		if (N[i].pid > 0) { kill(N[i].pid, SIGKILL); int st; waitpid(N[i].pid, &st, 0); N[i].pid = 0; }
	/* close this run's fds: a long search would otherwise exhaust them */
	for (int i = 0; i < M.nodes; i++)
	{
		if (N[i].listener > 0) close(N[i].listener);
		if (N[i].fd3[0] > 0) close(N[i].fd3[0]);
		if (N[i].fd4[0] > 0) close(N[i].fd4[0]);
		N[i].listener = N[i].fd3[0] = N[i].fd4[0] = -1;
	}
}

/* refinement (Milestone C): expose the live nodes' observables to the
 * abstract model, and check the implementation's state refines it. */
static long ob_node_byte(void *cx, int i, int k){ (void)cx; if(i<0||i>=M.nodes||k<0||k>=N[i].canon_len) return -1; return N[i].canon[k]; }
static int  ob_node_len(void *cx, int i){ (void)cx; return (i>=0&&i<M.nodes)?N[i].canon_len:0; }
static long ob_repl_src(void *cx, int r){ (void)cx; return rep_src[r]; }
static long ob_repl_val(void *cx, int r){ (void)cx; return rep_val[r]; }
static int
refine_check_state(int steps, const char **name, char *err, size_t errcap)
{
	CmcObs o = {0};
	o.nnodes = M.nodes; o.nrepl = nrep;
	o.node_byte = ob_node_byte; o.node_len = ob_node_len;
	o.repl_src = ob_repl_src; o.repl_val = ob_repl_val;
	return steps ? cmc_refine_step(&o, name, err, errcap)
	             : cmc_refine_check(&o, name, err, errcap);
}

/* C2 step check used inside run_seq's replay.  Stores any error message
 * in a module buffer (g_step_err) so the caller need not thread it.
 * Returns the cmc_refine_step code (-1 fine, >=0 invariant, -3 step, -2
 * error). */
static char g_step_err[256];
static int
step_refine_here(const char **name)
{
	return refine_check_state(1, name, g_step_err, sizeof g_step_err);
}

static int
check_invariants(int *which)
{
	Ctx c = {0}; c.nnodes = M.nodes;
	for (int i = 0; i < M.nodes; i++) { c.node[i] = N[i].canon; c.node_len[i] = N[i].canon_len; }
	c.repl_src = rep_src; c.repl_val = rep_val; c.nrepl = nrep;
	for (int k = 0; k < M.ninv; k++) {
		int r = eval_invariant(M.inv[k].expr, &c);
		if (r < 0) { fprintf(stderr, "mc2: invariant %s parse error\n", M.inv[k].name); *which = -2; return 0; }
		if (M.inv[k].eventually) { if (r) M.inv[k].seen = 1; continue; }
		if (!r) { *which = k; return 0; }
	}
	return 1;
}

/* after an exhaustive search: an `eventually` invariant that no reached
 * state satisfied is a violation.  Returns the first such invariant, or
 * -1 if all held (or there were none). */
static int
unmet_eventually(void)
{
	for (int k = 0; k < M.ninv; k++)
		if (M.inv[k].eventually && !M.inv[k].seen) return k;
	return -1;
}

static const char *
act_name(const Act *a, char *b, size_t n)
{
	static const char *k[] = {"accept", "deliver", "eof", "timeout", "crash", "drop", "wake", "restart"};
	if (a->kind == A_CRASH) snprintf(b, n, "crash n%d", a->node);
	else if (a->kind == A_RESTART) snprintf(b, n, "restart n%d", a->node);
	else if (a->kind == A_TIMEOUT) snprintf(b, n, "timeout n%d", a->node);
	else if (a->kind == A_DROP) snprintf(b, n, "reset n%d<->n%d", a->peer, a->node);
	else if (a->kind == A_WAKE && a->peer == POLL_LISTENER) snprintf(b, n, "wake n%d listener", a->node);
	else if (a->kind == A_WAKE && a->peer < POLL_LISTENER) snprintf(b, n, "wake n%d eventfd%d", a->node, POLL_LISTENER - 1 - a->peer);
	else if (a->peer < 0) snprintf(b, n, "%s client->n%d", k[a->kind], a->node);
	else snprintf(b, n, "%s n%d->n%d", k[a->kind], a->peer, a->node);
	return b;
}

/* report a violation: the action sequence, by name, and the world there */
static void
report(int which, const int *seq, int len)
{
	char nm[48];
	printf("Error: Invariant %s is violated.\n", M.inv[which].name);
	printf("Error: The behavior up to this point is (%d actions):\n", len);
	Act a[MAXACT]; int na;
	run_seq(NULL, 0, a, &na);
	for (int i = 0; i < len; i++)
	{
		if (seq[i] >= na) die(1, "replay of the counterexample diverged at step %d", i);
		printf("    %d. %s\n", i + 1, act_name(&a[seq[i]], nm, sizeof nm));
		apply(&a[seq[i]]);
		na = enum_actions(a);
	}
	printf("  node states:");
	for (int i = 0; i < M.nodes; i++)
	{
		printf(" node[%d]=", i);
		if (!N[i].canon_len) printf("()");
		for (int k = 0; k < N[i].canon_len; k++) printf("%s%d", k ? "," : "", N[i].canon[k]);
	}
	printf("\n  replies:"); for (int i = 0; i < nrep; i++) printf(" (n%d,%d)", rep_src[i], rep_val[i]);
	printf("%s\n", nrep ? "" : " none");
	kill_all();
}

/* report a refinement counterexample: the action trace and the state,
 * the same way report() does, but the heading is printed by the caller. */
static void
report_refine(const int *seq, int len)
{
	char nm[48];
	printf("Error: The behavior up to this point is (%d actions):\n", len);
	Act a[MAXACT]; int na;
	run_seq(NULL, 0, a, &na);
	for (int i = 0; i < len; i++)
	{
		if (seq[i] >= na) die(1, "replay of the counterexample diverged at step %d", i);
		printf("    %d. %s\n", i + 1, act_name(&a[seq[i]], nm, sizeof nm));
		apply(&a[seq[i]]);
		na = enum_actions(a);
	}
	printf("  node states:");
	for (int i = 0; i < M.nodes; i++)
	{
		printf(" node[%d]=", i);
		if (!N[i].canon_len) printf("()");
		for (int k = 0; k < N[i].canon_len; k++) printf("%s%d", k ? "," : "", N[i].canon[k]);
	}
	printf("\n");
	kill_all();
}

/* the BFS frontier: action sequences, appended in one growing pool */
static int *qpool; static size_t qcap, qused;
typedef struct { size_t off; int len; uint64_t pfp0, pfp1; } QEnt;
static QEnt *Q; static size_t Qcap, qh, qt;

static void
q_push(const int *seq, int len, int next, uint64_t pfp0, uint64_t pfp1)
{
	if (qt == Qcap) { Qcap = Qcap ? 2 * Qcap : 4096; Q = realloc(Q, Qcap * sizeof *Q); if (!Q) die(1, "out of memory for the frontier"); }
	if (qused + (size_t) len + 1 > qcap)
	{
		while (qused + (size_t) len + 1 > qcap) qcap = qcap ? 2 * qcap : 65536;
		qpool = realloc(qpool, qcap * sizeof *qpool); if (!qpool) die(1, "out of memory for the frontier");
	}
	Q[qt].off = qused; Q[qt].len = len + 1; Q[qt].pfp0 = pfp0; Q[qt].pfp1 = pfp1; qt++;
	if (len) memcpy(qpool + qused, seq, (size_t) len * sizeof *seq);
	qpool[qused + (size_t) len] = next;
	qused += (size_t) len + 1;
}

/* Locate the preload: $CMC_PRELOAD, else cmc-preload.so beside the
 * running program (a build tree), else ../lib/libcmc/ from it (an
 * installed bin/cmc).  Found from the program, not from a path compiled
 * in, so an install to any prefix finds its own copy. */
static int
find_preload(void)
{
	static const char *const rel[] = {"/cmc-preload.so", "/../lib/libcmc/cmc-preload.so"};
	const char *env = getenv("CMC_PRELOAD");
	char		exe[4096];
	ssize_t		n;

	if (env && *env)
	{
		snprintf(preload_path, sizeof preload_path, "%s", env);
		if (access(preload_path, R_OK) == 0) return 0;
		fprintf(stderr, "cmc: CMC_PRELOAD=%s: the preload is not readable: %s\n", env, strerror(errno));
		return -1;
	}
	n = readlink("/proc/self/exe", exe, sizeof exe - 64);
	if (n > 0)
	{
		exe[n] = 0;
		char *sl = strrchr(exe, '/');
		if (sl)
			for (size_t k = 0; k < sizeof rel / sizeof *rel; k++)
			{
				strcpy(sl, rel[k]);		/* fits: 64 bytes were kept free */
				snprintf(preload_path, sizeof preload_path, "%s", exe);
				if (access(preload_path, R_OK) == 0) return 0;
			}
	}
	fprintf(stderr, "cmc: cannot find cmc-preload.so beside this program or in ../lib/libcmc; set CMC_PRELOAD to its path\n");
	return -1;
}

/* the root: the empty sequence */
static void
q_root(void)
{
	Q = malloc(4096 * sizeof *Q); Qcap = 4096;
	if (!Q) die(1, "out of memory for the frontier");
	Q[0].off = 0; Q[0].len = 0; Q[0].pfp0 = 0; Q[0].pfp1 = 0; qt = 1;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double) t.tv_sec + t.tv_nsec / 1e9; }

/* splitmix64: the walk's seeded choices */
static uint64_t rng;
static uint64_t next_rand(void) { uint64_t z = (rng += 0x9e3779b97f4a7c15ULL); z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL; z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; return z ^ (z >> 31); }

/* The process front end: cmc --model-file FILE, from cmc_main(). */
int
cmc_proc_main(const CmcProcOptions *po)
{
	if (load_model(po->model_file)) return 3;
	if (po->binary)
		snprintf(M.binary, sizeof M.binary, "%s", po->binary);
	if (!*M.binary)
	{ fprintf(stderr, "cmc: %s: no binary to check ([target] binary, or --binary)\n", po->model_file); return 3; }
	if (po->crashes >= 0) M.crashes = po->crashes;
	if (po->drops >= 0) M.drops = po->drops;
	if (po->timeouts >= 0) M.timeouts = po->timeouts;
	long depth = po->depth >= 0 ? po->depth : M.depth;
	int pie = is_pie(M.binary);
	if (pie < 0) { fprintf(stderr, "cmc: %s is not an ELF executable\n", M.binary); return 3; }
	if (pie) { fprintf(stderr, "cmc: %s must be a non-PIE ELF (build with -no-pie)\n", M.binary); return 3; }
	if (find_preload()) return 3;
	for (int i = 0; i < MAXNODES; i++) { N[i].binary = M.binary; N[i].listener = N[i].fd3[0] = N[i].fd4[0] = -1; }
	if (seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &g_sz))
	{ fprintf(stderr, "cmc: seccomp user notification is not available: %s\n", strerror(errno)); return 3; }
	g_req = calloc(1, g_sz.seccomp_notif); g_resp = calloc(1, g_sz.seccomp_notif_resp);
	if (!g_req || !g_resp) die(1, "out of memory");
	double t0 = now_s();

	printf("cmc: model file %s, binary %s, %d node%s, %s\n", po->model_file, M.binary, M.nodes,
		   M.nodes == 1 ? "" : "s", po->simulate ? "random simulation" : "breadth-first search");
	printf("  environment: client_write %d, timeouts %d, crashes %d, drops %d, restarts %d",
		   M.client_write, M.timeouts, M.crashes, M.drops, M.restarts);
	if (depth >= 0) printf(", depth %ld", depth);
	printf("\n");
	fflush(stdout);

	if (po->simulate)
	{
		/* seeded random walks: the same seed takes the same walks.  Not
		 * exhaustive, so a clean finish is "no violation found", exit 2. */
		if (depth < 0) depth = 100;
		rng = po->seed;
		uint64_t states = 0, w;
		int *seq = malloc((size_t) (depth + 1) * sizeof *seq);
		if (!seq) die(1, "out of memory");
		for (w = 0; w < po->walks; w++)
		{
			Act a[MAXACT]; int na, len = 0;
			if (run_seq(NULL, 0, a, &na)) die(3, "a node would not start");
			if (po->refine_steps) cmc_refine_reset();
			for (;;)
			{
				int which = -1; states++;
				if (!check_invariants(&which))
				{
					if (which == -2) { kill_all(); return 3; }
					kill_all();
					printf("Walk %" PRIu64 " (seed %" PRIu64 "):\n", w + 1, po->seed);
					report(which, seq, len);
					free(seq);
					return 1;
				}
				if (po->refine)
				{
					const char *inm = NULL; char rerr[256];
					int rr = refine_check_state(po->refine_steps, &inm, rerr, sizeof rerr);
					if (rr == -2) { fprintf(stderr, "cmc: refinement: %s\n", rerr); kill_all(); free(seq); return 3; }
					if (rr != -1)
					{
						kill_all();
						printf("Walk %" PRIu64 " (seed %" PRIu64 "):\n", w + 1, po->seed);
						if (rr == -3)
							printf("Error: the implementation does not refine the spec: a step has %s.\n", inm);
						else
							printf("Error: the implementation does not refine the spec: abstract invariant %s is violated.\n", inm);
						report_refine(seq, len);
						free(seq); return 1;
					}
				}
				if (na == 0 || len >= depth) break;
				int pick = (int) (next_rand() % (uint64_t) na);
				seq[len++] = pick;
				apply(&a[pick]);
				na = enum_actions(a);
			}
			kill_all();
			if (po->time_limit > 0 && now_s() - t0 > po->time_limit) { w++; break; }
		}
		free(seq);
		printf("INCOMPLETE (no violation found): %" PRIu64 " walks of up to %ld actions, %" PRIu64 " states checked, seed %" PRIu64 " (not exhaustive), %.1f s\n",
			   w, depth, states, po->seed, now_s() - t0);
		return 2;
	}

	/* breadth-first search: shortest counterexample, every distinct world
	 * expanded once.  Each frontier entry is replayed from a fresh launch
	 * (stateless), so a world costs one replay of its whole prefix. */
	size_t distinct = 0, cut = 0, replays = 0; int maxlen = 0;
	size_t progressed = 0, stutter_leaves = 0;	/* no-progress (self-loop) detection */
	const char *why = NULL;
	g_step_refine = po->refine_steps;	/* C2 checks run inside run_seq's replay */
	q_root();
	while (qh < qt)
	{
		QEnt e = Q[qh++];
		int *seq = malloc(((size_t) e.len + 1) * sizeof *seq);
		if (!seq) die(1, "out of memory");
		if (e.len) memcpy(seq, qpool + e.off, (size_t) e.len * sizeof *seq);
		Act a[MAXACT]; int na = 0;
		if (run_seq(seq, e.len, a, &na)) die(3, "a node would not start");
		replays++;

		if (g_step_refine && g_step_rc != -1)
		{
			/* C2: run_seq found a step along this path that does not
			 * refine (invariant, forbidden step, or an error) */
			if (g_step_rc == -2) { fprintf(stderr, "cmc: refinement: %s\n", g_step_err); kill_all(); free(seq); return 3; }
			kill_all();
			if (g_step_rc == -3)
				printf("Error: the implementation does not refine the spec: a step has %s.\n", g_step_name);
			else
				printf("Error: the implementation does not refine the spec: abstract invariant %s is violated.\n", g_step_name);
			report_refine(seq, e.len);
			free(seq); return 1;
		}

		int which = -1; int ok = check_invariants(&which);
		if (which == -2) { kill_all(); free(seq); return 3; }
		if (ok && po->refine && !po->refine_steps)
		{
			/* C1: does this reached state refine the abstract model? */
			const char *inm = NULL; char rerr[256];
			int rr = refine_check_state(0, &inm, rerr, sizeof rerr);
			if (rr == -2) { fprintf(stderr, "cmc: refinement: %s\n", rerr); kill_all(); free(seq); return 3; }
			if (rr >= 0)
			{
				printf("Error: the implementation does not refine the spec: abstract invariant %s is violated.\n", inm);
				report_refine(seq, e.len);
				free(seq); return 1;
			}
		}
		uint64_t fp[2]; world_fp(fp);
		int isnew = mark(fp[0], fp[1]);
		if (isnew) { distinct++; if (e.len > maxlen) maxlen = e.len; }
		/* did this action make progress?  A non-root entry whose reached
		 * world equals its parent's is a stutter: the action was enabled
		 * but changed nothing the fingerprint sees, a self-loop the
		 * program cannot leave by that step.  If every reachable path is a
		 * stutter, the program never advanced past its initial state and a
		 * safety-only pass is vacuous (reported below). */
		if (e.len > 0)
		{
			if (fp[0] == e.pfp0 && fp[1] == e.pfp1) stutter_leaves++;
			else progressed++;
		}
		kill_all();
		if (!ok) { report(which, seq, e.len); free(seq); return 1; }
		if (isnew && na > 0)
		{
			if (depth >= 0 && e.len >= depth) cut++;
			else for (int i = 0; i < na; i++) q_push(seq, e.len, i, fp[0], fp[1]);
		}
		free(seq);
		if (po->max_states && distinct >= po->max_states) { why = "max states reached"; break; }
		if (po->time_limit > 0 && now_s() - t0 > po->time_limit) { why = "time limit reached"; break; }
	}
	double dt = now_s() - t0;
	if (!why && cut) why = "depth bound reached";
	if (why)
	{
		printf("INCOMPLETE (no violation found): %s; %zu distinct states, %zu left on queue, %zu cut at depth %ld, %.1f s\n",
			   why, distinct, qt - qh, cut, depth, dt);
		return 2;
	}
	/* the search was exhaustive: an `eventually` property that no state
	 * satisfied is a real violation (a bounded cut above would make this
	 * unsound, so it is only checked here) */
	int ev = unmet_eventually();
	if (ev >= 0)
	{
		printf("Error: Invariant %s is violated (eventually: no reachable state satisfies it).\n", M.inv[ev].name);
		printf("%zu states generated, %zu distinct states found; the whole state graph was searched.\n", replays, distinct);
		return 1;
	}
	/* No-progress guard (davros eval ask 4): the search was exhaustive and
	 * every invariant held, but if no action ever changed the world (every
	 * transition was a stutter, or none was enabled) the program never
	 * advanced past its initial state, so a safety-only pass is vacuous.
	 * Report INCOMPLETE rather than a clean pass.  A model with an
	 * `eventually` property is exempt: reaching it was checked above, and
	 * is the progress the guard asks for. */
	int has_eventually = 0;
	for (int k = 0; k < M.ninv; k++) has_eventually |= M.inv[k].eventually;
	if (progressed == 0 && !has_eventually)
	{
		/* a root with no enabled action is the same vacuous pass: no
		 * environment step was ever possible, so nothing was explored */
		if (stutter_leaves > 0)
			printf("INCOMPLETE (no violation found): the program made no progress -- every enabled action left the world unchanged (a self-loop at the initial state).  %zu distinct states, %zu self-loops, %.1f s.\n",
				   distinct, stutter_leaves, dt);
		else
			printf("INCOMPLETE (no violation found): the program made no progress -- no action was ever enabled, so only the initial state was checked.  %.1f s.\n", dt);
		printf("  Pair the safety invariant with an `eventually` property that the program must reach, so a run that never advances fails instead of passing.\n");
		return 2;
	}
	/* TLC's wording; depth counts the initial state as 1 */
	printf("Model checking completed. No error has been found.\n");
	printf("%zu states generated, %zu distinct states found, 0 states left on queue.\n", replays, distinct);
	printf("The depth of the complete state graph search is %d.\n", maxlen + 1);
	printf("  %d invariant%s held; %.1f s, %.0f replays/s.\n", M.ninv, M.ninv == 1 ? "" : "s",
		   dt, dt > 0 ? replays / dt : 0.0);
	return 0;
}

#else							/* not Linux x86-64 */

#include <stdio.h>

int
cmc_proc_main(const CmcProcOptions *po)
{
	fprintf(stderr, "cmc: --model-file %s: the process front end needs Linux x86-64 (seccomp user notification)\n",
			po->model_file);
	return 3;
}

#endif
