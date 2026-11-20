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
 * snapshot.  An action is ACCEPT, DELIVER, EOF, TIMEOUT, EXPIRE, CRASH,
 * DROP and the rest (enum_actions).  Connections are routed by the port a
 * node dials (node i listens on 127.0.0.1:20000+i), so any topology the
 * nodes set up works; each connection is a FIFO channel, as TCP is.  BFS
 * gives the shortest counterexample; simulation takes seeded random walks.
 *
 * Verdicts are honest (docs/steering/project.md).  The fingerprint holds
 * everything the future depends on: node states, open connections,
 * channels in order, pending connects, replies, budgets left, every
 * node clock and deadline.  A search stopped by a depth, state or time
 * limit is INCOMPLETE, never a pass.  A model-file key the loader does not
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

#include <asm/termbits.h>			/* TCGETS2, for stdio's isatty() (kernel_fd_call) */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <linux/sched.h>			/* CLONE_THREAD, struct clone_args */
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>				/* user_regs_struct, for trace_stop() */
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cmc_fs.h"

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
static int seccomp(unsigned op, unsigned flags, void *a) { return (int) syscall(SYS_seccomp, op, flags, a); }
/* file system calls newer than some C libraries' headers (x86-64 numbers) */
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452
#endif
#ifndef SYS_setxattrat
#define SYS_setxattrat 463
#endif
#ifndef SYS_removexattrat
#define SYS_removexattrat 466
#endif
#ifndef SYS_getxattrat
#define SYS_getxattrat 464
#endif
#ifndef SYS_listxattrat
#define SYS_listxattrat 465
#endif
#ifndef SYS_file_setattr
#define SYS_file_setattr 469
#endif

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
#define LULL_SLICE_MS 20		/* how often a silent node is checked for a signal stop */
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
 * term alongside the per-node crashes.  A crash of a node with modelled
 * files is up to FS_MAXCRASH actions. */
#define MAXACT (MAXNODES * MAXTHREAD * (MAXPOLL + MAXNODES + 4) + MAXNODES * (MAXNODES + 1) + MAXNODES * 2 * MAXNODES + MAXNODES * FS_MAXCRASH)
#define MAXINV 8
#define FS_BYTES_DEFAULT 1024	/* a node's file bytes, all files and every kept version */
#define FS_BYTES_MAX 4096
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
	int			timeouts, crashes, drops, restarts, short_writes, partitions;
	int			drift;			/* per thousand: how far two node clocks may diverge per fired deadline */
	int			max_write;		/* largest single write a node may make (<= MSGLEN) */
	char		args[512];		/* extra argv after the node id, space-separated (e.g. flags) */
	long		depth;			/* -1 = unbounded */
	Inv			inv[MAXINV];
	int			ninv;
	Field		field[MAXFIELD];/* [fields] name = byte offset, for node[i].name */
	int			nfield;
	/* [files]: the directory whose files the checker models (empty: none),
	 * the EIO and ENOSPC budgets, and each node's byte budget */
	char		fsdir[256];
	int			eio, enospc;
	long		fsbytes;
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
	M.nodes = 1; M.client_write = -1; M.depth = -1; M.max_write = MSGLEN_DEFAULT; M.fsbytes = FS_BYTES_DEFAULT;
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
			if (strcmp(s + 1, "target") && strcmp(s + 1, "environment") && strcmp(s + 1, "invariants") && strcmp(s + 1, "fields") && strcmp(s + 1, "files"))
			{ fprintf(stderr, "cmc: %s:%d: unknown section [%s] (target, environment, fields, files, invariants)\n", path, ln, s + 1); goto bad; }
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
		else if (!strcmp(sect, "environment") && !strcmp(k, "drift"))
		{ if (int_value(path, ln, k, v, 0, 500, &x)) goto bad; M.drift = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "crashes"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.crashes = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "drops"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.drops = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "short_writes"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.short_writes = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "partitions"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.partitions = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "restarts"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.restarts = (int) x; }
		else if (!strcmp(sect, "environment") && !strcmp(k, "depth"))
		{ if (int_value(path, ln, k, v, 0, 1000000, &x)) goto bad; M.depth = x; }
		else if (!strcmp(sect, "files") && !strcmp(k, "dir"))
		{
			/* an absolute directory, kept without a trailing slash */
			size_t dl = strlen(v);
			while (dl > 1 && v[dl - 1] == '/') v[--dl] = 0;
			if (*v != '/' || dl < 2 || dl >= sizeof M.fsdir || strstr(v, "/.") || strstr(v, "//"))
			{ fprintf(stderr, "cmc: %s:%d: dir must be an absolute directory other than / (no . or .. parts)\n", path, ln); goto bad; }
			snprintf(M.fsdir, sizeof M.fsdir, "%s", v);
		}
		else if (!strcmp(sect, "files") && !strcmp(k, "eio"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.eio = (int) x; }
		else if (!strcmp(sect, "files") && !strcmp(k, "enospc"))
		{ if (int_value(path, ln, k, v, 0, 255, &x)) goto bad; M.enospc = (int) x; }
		else if (!strcmp(sect, "files") && !strcmp(k, "bytes"))
		{ if (int_value(path, ln, k, v, 1, FS_BYTES_MAX, &x)) goto bad; M.fsbytes = x; }
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
	if (!M.fsdir[0] && (M.eio || M.enospc))
	{ fprintf(stderr, "cmc: %s: [files] eio and enospc need a dir\n", path); return -1; }
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
/* A chain of `and`s or of `or`s.  Mixing the two at one level is a parse
 * error: they would associate left to right with equal precedence, so
 * `a or b and c` meant (a or b) and c, which a reader who expects `and`
 * to bind tighter would take for a different invariant. */
static int
eval_expr(const char **p, Ctx *c)
{
	int			r = eval_unary(p, c), conn = 0;

	if (r < 0) return r;
	for (;;)
	{
		skipws(p);
		int			is_and = word(p, "and"), is_or = is_and ? 0 : word(p, "or");

		if (!is_and && !is_or) break;
		if (conn && conn != (is_and ? 1 : 2)) return -1;
		conn = is_and ? 1 : 2;
		int			b = eval_unary(p, c);

		if (b < 0) return b;
		r = is_and ? (r && b) : (r || b);
	}
	return r;
}

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
#define MAXRIOV 16				/* iovec entries of a readv or recvmsg */

/* An epoll instance (epoll_create1).  Unlike poll(), whose fd set is
 * passed afresh on every call, an epoll instance holds a PERSISTENT
 * interest list built by epoll_ctl: each entry is a watched fd, the
 * events requested for it, and the opaque user data the loop stored
 * (epoll_event.data).  epoll_wait returns that data verbatim for a ready
 * fd -- it is how an event loop finds the handler for the fd -- so the
 * checker stores it and writes it back unchanged; it is never a search
 * distinction (an ASLR pointer must not leak into the fingerprint). */
/* owned fds per node (numbered from 500, never reused within a process) */
#define MAXOWNED 256
/* fdflag bits */
#define FD_NB 1					/* O_NONBLOCK */
#define FD_CX 2					/* FD_CLOEXEC */
#define FD_PLAIN 4				/* a socket() not yet connected or listening */
/* sockopt bits: the boolean options setsockopt accepts and records */
#define SO_B_KEEPALIVE 1
#define SO_B_REUSEADDR 2
#define SO_B_REUSEPORT 4
#define SO_B_NODELAY 8

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
	int block_conn, block_end;	/* BK_RECV: the connection and this node's end of it (-1: a timer fd) */
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
	/* a readv or recvmsg: niov entries of the iovec array the bytes are
	 * scattered over (0 for a read into one buffer), and recvmsg's
	 * msghdr, whose msg_namelen and msg_flags the answer sets (0 else) */
	int vecread, niov; uint64_t iovb[MAXRIOV]; uint64_t iovl[MAXRIOV]; uint64_t block_msg;
	uint64_t futex_addr;		/* BK_FUTEX: the futex word this thread waits on */
	/* a finite timeout of the call this thread is parked in (a kind
	 * timed_kind() accepts): has_dl, and the deadline dl on its node's
	 * clock */
	int has_dl; unsigned long long dl;
	/* the thread's latest notification: the call, its arguments, the
	 * address of its syscall instruction and, in a node with more than one
	 * thread, its stack pointer there.  A parked thread makes no other
	 * call, so they name where it is parked. */
	long nr; uint64_t args[6]; uint64_t ip, sp;
	/* BK_DONE: the answer to call nr, already carried out and held until
	 * the search runs the thread (A_RUN).  data is every byte the call
	 * wrote into the thread's memory (a recv's message, poll's revents),
	 * which the world encoding cannot otherwise see once the call is
	 * done; held_len counts them from the call's notification. */
	long long held_val; int held_err; unsigned held_flags;
	int held_len; unsigned char held_data[MSGLEN];
	int cloning;				/* call nr is a clone that makes a thread: once it runs, birth() takes the newborn */
	uint64_t clone_ctid;		/* that clone's CLONE_CHILD_CLEARTID word, for the newborn's slot */
} Thread;

typedef struct {
	pid_t pid; int listener; int alive;
	/* association list: which peer each connection fd talks to.
	 * peer -1 = the synthetic client, >=0 = a node id. */
	int cfd[MAXCONN]; int cpeer[MAXCONN]; int cnonblock[MAXCONN]; int ccloexec[MAXCONN]; int nc;
	int cconn[MAXCONN]; int cend[MAXCONN];	/* each fd's connection (-1: a timer fd) and which end this node holds */
	int ls_nonblock; int ls_cloexec;	/* O_NONBLOCK / FD_CLOEXEC on the listener socket */
	/* the file flags of each owned fd (index fd - 500) that is neither a
	 * connection nor the listener: a socket not yet dialled or listening
	 * (FD_PLAIN), an eventfd's FD_CLOEXEC, an epoll instance's flags */
	unsigned char fdflag[MAXOWNED];
	/* the socket options of each owned socket fd (index fd - 500), SO_*
	 * bits below, as getsockopt reads them back; none changes the model */
	unsigned char sockopt[MAXOWNED];
	/* dup: for an owned fd that is a second number for a connection,
	 * the fd it duplicates (which holds the connection slot); 0 else */
	int dupof[MAXOWNED];
	int listener_fd;			/* the fd passed to listen(), or -1; the server's accept fd */
	int pend[MAXCONN]; int npend;	/* connections waiting at this node's listener, oldest first */
	int efd[MAXEFD]; uint64_t efdval[MAXEFD]; int efdnb[MAXEFD]; int nefd;
	Epoll ep[MAXEPOLL]; int nep;	/* epoll instances: a persistent interest list per epfd */
	Thread th[MAXTHREAD]; int nth;	/* per-thread deferred slots, keyed by tid */
	int next_fd;
	int timer_fd;				/* -1, or an fd whose first recv is a fired timeout's 'T' */
	const char *binary; int id;
	int fd3[2];					/* volatile canonical-emit pipe */
	int fd4[2];					/* durable canonical-emit pipe (survives a crash) */
	int hook_w[2];				/* the checker's write ends of fd3 and fd4 (serve_hook) */
	unsigned char canon[SNAP]; int canon_len;
	unsigned char durable[SNAP]; int durable_len;
	int durable_read_off;		/* bytes of the framed durable record a startup read has consumed */
	int crashed;				/* killed by a CRASH action: volatile lost, durable kept */
	int preload_ok;				/* the preload announced itself in this node */
	int threaded;				/* this process has run more than one thread (world_threaded) */
	int race_ok;				/* the race runtime announced itself in this process */
	int execd;					/* the launch's own exec of the binary has run */
	/* the node's modelled files ([files] dir), which outlive its process;
	 * and, while its thread is held after a write, truncation or fsync of
	 * one, what failing that call instead would undo (undo_kind 'w' for a
	 * write or truncation, 's' for an fsync; 0 for none) */
	Fs fs;
	FsUndo undo; int undo_kind;
} Node;

static Node N[MAXNODES];
/* A data race the race runtime reported in this replay (node, the two
 * program counters, the address), like a fault. */
static int g_race_node = -1;
static uint64_t g_race_pc[2], g_race_addr;
/* Over the whole search: a node ran more than one thread without the
 * race runtime (its id + 1), an atomic weaker than seq_cst ran, and a
 * try or timed acquisition (or a semaphore wait that left another
 * waiter room) ran with more than one thread.  Any one means the orders
 * searched need not cover every execution, so the search cannot end in
 * a pass (cmc(7), THREADS). */
static int g_unchecked_node, g_weak_seen, g_try_seen;
/* What a parked thread waits in (3 marks a running thread in the
 * fingerprint).  BK_DONE holds an answer the search has yet to send.  A
 * clone leaves two threads in ptrace stops, each released by a RUN:
 * BK_NEWBORN is the new thread before its first instruction, and
 * BK_CLONED the parent at the clone's return.  BK_CONNWAIT is a call,
 * not yet carried out, whose answer depends on whether a non-blocking
 * connect's handshake has completed (see connwait()).  BK_SHORTW is a
 * write, not yet carried out, that the search may cut short (see
 * shortwait()).  BK_SLEEP is a pure wait on the node's clock (see
 * sleep_call()).  BK_SYNWAIT is a blocking connect, not yet carried
 * out, across a cut link (see SYN_TIMEOUT). */
enum { BK_RECV = 1, BK_ACCEPT = 2, BK_POLL = 4, BK_FUTEX = 5, BK_DONE = 6, BK_NEWBORN = 7, BK_CLONED = 8, BK_CONNWAIT = 9, BK_SHORTW = 10, BK_SLEEP = 11, BK_SYNWAIT = 12 };
/* Linux's blocking connect gives up after tcp_syn_retries (6 by
 * default) retransmissions of its SYN, from a 1 s timeout doubling each
 * time: 1 + 2 + ... + 64 = 127 s, then ETIMEDOUT. */
#define SYN_TIMEOUT (127ULL * 1000000000ULL)
#define TIMER_PEER (-3)					/* a synthetic peer: a fired election timer */
#define POLL_LISTENER (-4)				/* A_WAKE target: a BK_POLL node's listener fd */

/* Can a thread parked in kind carry a deadline (Thread.has_dl)? */
static int
timed_kind(int kind)
{
	return kind == BK_POLL || kind == BK_SLEEP || kind == BK_FUTEX || kind == BK_SYNWAIT;
}

/* Find the thread slot for tid on node nd, allocating one on first sight
 * (the main thread's first call, or a birth).  Returns the slot, or NULL
 * if the node already has MAXTHREAD threads. */
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
	return t;
}

/* a thread of nd is running (between syscalls): block_kind 0.  Such a
 * node will produce a notification, so drive_node() RECVs it. */
static int
node_has_running(const Node *nd)
{
	if (!nd->alive) return 0;
	if (nd->nth == 0) return 1;			/* the main thread's first call is coming */
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && !nd->th[i].block_kind) return 1;
	return 0;
}

/* Does nd run more than one thread?  Only then is the order of its
 * threads' calls a search choice; a node with one thread is answered
 * inline, as if threads did not exist.  A newborn counts from its birth,
 * which the clone completes before its parent runs on. */
static int
node_multi(const Node *nd)
{
	int n = 0;
	for (int i = 0; i < nd->nth; i++) n += nd->th[i].inuse;
	return n > 1;
}

static void kill_all(void);
static void die(int code, const char *fmt, ...);

/* The thread whose call is being answered, or NULL.  Every byte the
 * checker writes into a node's memory while it is set is kept in the
 * thread's held_data: once the call is carried out those bytes are the
 * call's result, and a held result must be part of the world.  A call
 * that answers another thread (a close whose RST ends a parked recv)
 * retargets it for that answer with cap_begin and restores it after. */
static Thread *g_cap;

static Thread *
cap_begin(Thread *th)
{
	Thread	   *old = g_cap;

	g_cap = th;
	if (th) th->held_len = 0;
	return old;
}

static void
capture(const void *buf, size_t n)
{
	if (!g_cap) return;
	if ((size_t) g_cap->held_len + n > sizeof g_cap->held_data)
		die(1, "a held answer writes more than %zu bytes into a thread", sizeof g_cap->held_data);
	memcpy(g_cap->held_data + g_cap->held_len, buf, n);
	g_cap->held_len += (int) n;
}

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

/* A TCP connection.  End 0 is the dialler (a node id, or -1 for the
 * synthetic client), end 1 the acceptor.  Each direction is its own FIFO
 * queue of messages in the bag, so two connections between the same two
 * nodes never share bytes.  A connection lives until both ends are
 * closed: a node closed its descriptor, or the node died.
 *
 * Closing an end with nothing unread queues a FIN toward the other end,
 * which reads it as EOF once delivered.  Closing it with bytes unread is
 * an RST, as Linux sends.  What a reset leaves at a live end follows
 * Linux, as measured on loopback:
 *   - bytes already queued toward the end stay readable, in order, then
 *     the reset is reported once: ECONNRESET, or EPIPE if the end had
 *     already been sent a FIN (Linux's CLOSE_WAIT);
 *   - a read reports ECONNRESET, never EPIPE (after a FIN it reads 0);
 *   - once reported, a read returns 0 and a write fails with EPIPE and
 *     the SIGPIPE the writer's disposition decides;
 *   - a write to a peer that closed cleanly succeeds once (the peer's
 *     stack answers it with an RST, leaving EPIPE).
 * A DROP is a network failure: the bytes in flight are lost both ways,
 * then both ends see the reset.  A connect() that was refused leaves the
 * socket unconnected (reads ENOTCONN, writes EPIPE) until it is dialled
 * again. */
#define MAXCONNS 64
typedef struct {
	int used;
	int node[2];				/* each end's node (end 0 may be -1, the client) */
	int closed[2];				/* the end's descriptor is gone */
	int gone[2];				/* closed because its node died (crash or exit) */
	int accepted;				/* end 1 exists (the acceptor took it) */
	int reset;					/* an RST ended it */
	int err[2];					/* errno still to report at this end (ECONNRESET or EPIPE), or 0 */
	int refused;				/* the dial was refused: end 0 is unconnected */
	int eof[2];					/* this end has read the FIN */
	int seq[2];					/* sends so far toward each end, for FIFO order */
	/* A non-blocking connect.  connecting: the handshake is still out
	 * (SYN_SENT); a search action (A_CONNECT) completes it, into the
	 * peer's backlog or refused.  nbdial: the dialler's socket is still
	 * in Linux's SS_CONNECTING, so its next connect() reports the outcome
	 * (0, or the pending error or ECONNABORTED). */
	int connecting, nbdial;
	int lport;					/* the dialler's local port, for getsockname and getpeername */
	int shut_wr[2], shut_rd[2];	/* shutdown() of this end's sending, receiving side */
	int full[2];				/* this end's send buffer is full (a SHORT write): writes EAGAIN, no POLLOUT */
} Conn;
static Conn conns[MAXCONNS];

/* record which connection end a node's fd is; the peer is kept for
 * naming and for the fd's class in the world encoding */
/* What poll reports for POLLOUT on fd, as Linux does: a connection and
 * an eventfd are writable (the model has no full send buffer); a plain
 * socket, never connected, reports POLLOUT with POLLHUP; the listener
 * and an epoll instance are never writable.  fd_flags() and cfd_slot()
 * are defined below. */
static unsigned writable(Node *nd, int fd);

/* is tid one of node nd's threads? */
static int
tid_of_node(const Node *nd, pid_t tid)
{
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && nd->th[i].tid == tid) return 1;
	return tid == nd->pid;
}

/* the fdflag entry of owned fd fd, ending the check past MAXOWNED */
static unsigned char *
fd_flags(Node *nd, int fd)
{
	if (fd < 500 || fd - 500 >= MAXOWNED) die(1, "node %d opens more than %d checker-owned fds", nd->id, MAXOWNED);
	return &nd->fdflag[fd - 500];
}

static int cfd_slot(Node *nd, int fd);
static int efd_slot(Node *nd, int fd);

static unsigned
writable(Node *nd, int fd)
{
	if (fd < 500 || fd >= nd->next_fd) return 0;
	if (cfd_slot(nd, fd) >= 0 || efd_slot(nd, fd) >= 0) return POLLOUT;
	if (fd == nd->listener_fd) return 0;
	return (*fd_flags(nd, fd) & FD_PLAIN) ? POLLOUT | POLLHUP : 0;
}


/* What kind of checker-owned fd fd is, for a socket call that Linux
 * answers by its kind: a connection, the listener, an eventfd, an epoll
 * instance, a socket not yet dialled or listening, or nothing open.
 * fd is 500 or above. */
enum { K_NONE, K_CONN, K_LISTEN, K_EFD, K_EPOLL, K_PLAIN };
static int ep_slot(Node *nd, int fd);

static int
fd_kind(Node *nd, int fd)
{
	if (fd >= nd->next_fd) return K_NONE;
	if (cfd_slot(nd, fd) >= 0) return K_CONN;
	if (fd == nd->listener_fd) return K_LISTEN;
	if (efd_slot(nd, fd) >= 0) return K_EFD;
	if (ep_slot(nd, fd) >= 0) return K_EPOLL;
	if (fd - 500 < MAXOWNED && (nd->fdflag[fd - 500] & FD_PLAIN)) return K_PLAIN;
	return K_NONE;
}

/* The local port of a dial from node nd's socket fd, as getsockname
 * reports it.  Linux picks an ephemeral port; the model names one per
 * node and fd so it never collides, and a program that depends on its
 * value is outside the model. */
static int
local_port(const Node *nd, int fd)
{
	return 40000 + nd->id * MAXOWNED + (fd - 500);
}

/* What poll reports at once for owned fd fd asked for events, with no
 * search choice: POLLNVAL for nothing open, the writability above, an
 * eventfd's count, and a dial's settled outcome.  A connection still
 * handshaking (a non-blocking connect) reports nothing until the search
 * completes it (A_CONNECT).  A refused dial reports as Linux does, as
 * measured on loopback: POLLHUP always, POLLERR while the error is
 * unread, and POLLIN and POLLOUT while the socket is still the one the
 * non-blocking connect left (nbdial) or the error is unread; after a
 * blocking refusal only POLLOUT.  Whether a connection is readable
 * otherwise is the search's (WAKE), not this. */
static short
local_ready(Node *nd, int fd, short events)
{
	if (fd < 0) return 0;
	if (fd >= 500 && fd_kind(nd, fd) == K_NONE) return POLLNVAL;
	int cs = cfd_slot(nd, fd), es = efd_slot(nd, fd);
	if (cs >= 0 && nd->cconn[cs] >= 0)
	{
		const Conn *cn = &conns[nd->cconn[cs]];
		if (cn->connecting) return 0;
		if (cn->full[nd->cend[cs]] && !cn->reset && !cn->err[nd->cend[cs]]) events &= ~POLLOUT;
		if (!cn->refused && !cn->reset && cn->shut_rd[nd->cend[cs]])
			return (short) ((events & (POLLIN | POLLOUT)) | (cn->shut_wr[nd->cend[cs]] ? POLLHUP : 0));
		if (cn->refused)
		{
			short r = POLLHUP | (cn->err[0] ? POLLERR : 0);
			if (cn->err[0] || cn->nbdial) r |= events & (POLLIN | POLLOUT);
			else r |= events & POLLOUT;
			return r;
		}
	}
	unsigned out_ev = writable(nd, fd);
	if ((events & POLLOUT) && out_ev) return (short) out_ev;
	if (es >= 0 && nd->efdval[es] > 0 && (events & POLLIN)) return POLLIN;
	return 0;
}

/* Linux's errno for socket call nr on owned fd fd of the wrong kind, or
 * 0 when the call applies to it: ENOTSOCK on an eventfd, an epoll
 * instance or a modelled file, EBADF on nothing open, and the state
 * errors of a socket
 * (connect on a connected or listening socket is EISCONN; listen on a
 * connection, accept on anything but the listener, and bind on a
 * connection are EINVAL).  read, write, recvfrom and sendto on a plain
 * socket are answered by the callers (ENOTCONN, EPIPE). */
static int
wrong_kind(Node *nd, long nr, int fd)
{
	int k = fd_kind(nd, fd);

	if (k == K_EFD || k == K_EPOLL || (k == K_NONE && fs_isfd(&nd->fs, fd))) return ENOTSOCK;
	if (k == K_NONE) return EBADF;
	switch (nr)
	{
		case SYS_connect: return k == K_LISTEN ? EISCONN : 0;
		case SYS_listen: return k == K_CONN ? EINVAL : 0;
		case SYS_bind: return k == K_CONN ? EINVAL : 0;
		case SYS_accept: case SYS_accept4: return k == K_LISTEN ? 0 : EINVAL;
		default: return 0;
	}
}

static void cfd_set(Node *nd, int fd, int peer, int conn, int end){ if(nd->nc>=MAXCONN) die(1,"node %d has more than %d connections",nd->id,MAXCONN); nd->cfd[nd->nc]=fd; nd->cpeer[nd->nc]=peer; nd->cconn[nd->nc]=conn; nd->cend[nd->nc]=end; nd->cnonblock[nd->nc]=0; nd->ccloexec[nd->nc]=0; nd->nc++; }
/* forget a node's fd slot k, keeping the rest in open order */
static void
cfd_del(Node *nd, int k)
{
	nd->nc--;
	for (int j = k; j < nd->nc; j++)
	{
		nd->cfd[j] = nd->cfd[j + 1]; nd->cpeer[j] = nd->cpeer[j + 1]; nd->cconn[j] = nd->cconn[j + 1];
		nd->cend[j] = nd->cend[j + 1]; nd->cnonblock[j] = nd->cnonblock[j + 1]; nd->ccloexec[j] = nd->ccloexec[j + 1];
	}
}

/* message bag: bytes queued on connection conn toward its end `to`; a
 * zero-length eof entry is the FIN that ends the stream */
typedef struct { int used, conn, to, len, seq, eof; unsigned char body[MSGLEN]; } Msg;
static Msg bag[MAXMSG];

/* ghost reply log */
static unsigned char rep_src[MAXREP], rep_val[MAXREP]; static int nrep;

/* environment budgets left on this path; part of the fingerprint */
typedef struct { int timeouts, crashes, drops, restarts, short_writes, partitions, eio, enospc; } Budget;
static Budget bud;
/* The network's cut links: cut[a][b] holds back every segment from node
 * a to node b (bytes, FINs and handshakes wait, as TCP retransmits
 * them) until the link heals.  Part of the world. */
static unsigned char g_cut[MAXNODES][MAXNODES];
/* may a segment travel from node a to node b now?  The client (-1)
 * and a node's connection to itself are never cut */
static int link_up(int a, int b) { return a < 0 || b < 0 || !g_cut[a][b]; }
/* may a segment reach end `to` of connection conn: the link from the
 * other end's node to this one is up */
#define CONN_UP(conn, to) link_up(conns[conn].node[1 - (to)], conns[conn].node[to])
/* An RST from node a to node b is about to be reported at b at once.
 * The model has an RST arrive with no delay; across a cut link it
 * would wait, and holding it back is not modelled, so the check ends
 * rather than answering as if the link were up. */
static void die(int code, const char *fmt, ...);
static void
rst_path(int a, int b)
{
	if (!link_up(a, b))
		die(3, "node %d resets a connection to node %d across a cut link; an RST held back by a partition is not modelled", a, b);
}

/* The logical clocks the checker owns, one per node (a node's clock
 * outlives a crash of its process, as a machine's does).  Every clock
 * starts at a base.  A budgeted TIMEOUT advances them all by CLK_TICK;
 * a node's own deadline firing (A_EXPIRE) advances its clock to the
 * deadline and the others by about as much, within the model's drift
 * (fire_deadline()).  A target's clock_gettime/gettimeofday reads are a
 * pure function of the actions taken, so stateless replay stays
 * deterministic. */
#define CLK_BASE 1000000000ULL		/* 1e9 ns = 1 s, an arbitrary epoch */
#define CLK_TICK 1000000000ULL		/* each fired TIMEOUT advances 1 s */
static unsigned long long g_clk[MAXNODES];

static struct seccomp_notif_sizes g_sz;

/* the preload every node runs under (src/cmc_preload.c), found by
 * find_preload(); it announces itself with a clock_gettime on
 * CMC_PRELOAD_MAGIC, so a node it did not load into is refused */
static char preload_path[4096];
#define CMC_PRELOAD_MAGIC 0x434d43
/* the first argument of the preload's synchronisation marker, a
 * sched_yield made before each lock, semaphore wait, barrier and once
 * (cmc_preload.c has the same) */
#define CMC_SYNC_MAGIC 0x434d4353
/* the race runtime's markers (cmc_race.c has the same): sched_yield with
 * CMC_RACE_MAGIC, an operation and three words */
#define CMC_RACE_MAGIC 0x434d4352
enum { RACE_HELLO = 1, RACE_ATOMIC = 2, RACE_WEAK = 3, RACE_REPORT = 4, RACE_FULL = 5, RACE_TRY = 6 };

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
  if (k > 0) capture(buf, (size_t) k);
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

/* Read the struct timespec at addr in node nd into *ns.  Returns 1, 0
 * when it is not a valid duration (EINVAL on Linux), or -1 when it
 * cannot be read (EFAULT). */
static int
read_timespec(const Node *nd, uint64_t addr, unsigned long long *ns)
{
	struct { long long sec, nsec; } ts;
	if (cr_read(nd->pid, addr, &ts, sizeof ts) != sizeof ts) return -1;
	if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000LL) return 0;
	/* a deadline past a century is as good as never; keep the sum finite */
	if (ts.sec > 3153600000LL) ts.sec = 3153600000LL;
	*ns = (unsigned long long) ts.sec * 1000000000ULL + (unsigned long long) ts.nsec;
	return 1;
}

/* Give thread th, about to park, a deadline tmo_ns from now on its node's
 * clock when finite is set, else none.  Firing it is A_EXPIRE. */
static void
set_deadline(const Node *nd, Thread *th, int finite, unsigned long long tmo_ns)
{
	th->has_dl = finite;
	th->dl = finite ? g_clk[nd->id] + tmo_ns : 0;
}

static int
is_pie(const char *b)
{ int fd=open(b,O_RDONLY); unsigned char e[18]={0}; if(fd<0)return -1;	/* GCOVR_EXCL_LINE: the binary was found and opened a moment ago */
  if(read(fd,e,sizeof e)!=(ssize_t)sizeof e){close(fd);return -1;} close(fd);	/* GCOVR_EXCL_LINE: the binary was found and opened a moment ago */
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
#define FSX(n) OWN(M.fsdir[0] ? (unsigned) (n) : 0xffffffffU)
		OWN(SYS_socket), OWN(SYS_setsockopt), OWN(SYS_bind), OWN(SYS_listen),
		OWN(SYS_connect), OWN(SYS_accept), OWN(SYS_accept4),
		OWN(SYS_read), OWN(SYS_write), OWN(SYS_recvfrom), OWN(SYS_sendto),
		OWN(SYS_clock_gettime), OWN(SYS_gettimeofday), OWN(SYS_getrandom),
		OWN(SYS_close),
		OWN(SYS_poll), OWN(SYS_ppoll), OWN(SYS_fcntl), OWN(SYS_eventfd2),
		OWN(SYS_epoll_create1), OWN(SYS_epoll_create), OWN(SYS_epoll_ctl),
		OWN(SYS_epoll_wait), OWN(SYS_epoll_pwait),
		OWN(SYS_nanosleep), OWN(SYS_clock_nanosleep), OWN(SYS_select), OWN(SYS_pselect6),
		OWN(SYS_setitimer), OWN(SYS_alarm), OWN(SYS_timer_create), OWN(SYS_timerfd_create),
		OWN(SYS_clone), OWN(SYS_clone3), OWN(SYS_futex), OWN(SYS_exit),
		OWN(SYS_sched_yield),
		OWN(SYS_fork), OWN(SYS_vfork),
		/* calls the model does not answer, intercepted only so that one on
		 * a checker-owned fd is refused rather than sent to the kernel,
		 * which has no such socket (see refuse_on_owned).  sendmsg is
		 * matched below on its fd as well: the child itself sends the
		 * notify fd to the parent with sendmsg on a low fd once this
		 * filter is in, which nobody could answer, so below fd 500 the
		 * second filter (install_trace_filter) stops it for the tracer. */
		OWN(SYS_readv), OWN(SYS_writev), OWN(SYS_preadv), OWN(SYS_pwritev),
		OWN(SYS_pread64), OWN(SYS_pwrite64), OWN(SYS_recvmsg),
		OWN(SYS_sendmmsg), OWN(SYS_recvmmsg), OWN(SYS_shutdown), OWN(SYS_dup),
		OWN(SYS_dup2), OWN(SYS_dup3), OWN(SYS_getsockopt), OWN(SYS_getsockname),
		OWN(SYS_getpeername), OWN(SYS_ioctl), OWN(SYS_splice), OWN(SYS_sendfile),
		/* the file system, which is read-only to a node (fs_call): every
		 * open, so one that can change a file is refused; the calls that
		 * change the namespace or a file's metadata; and the calls on an
		 * open file that the model answers for its own fds.  Each OWN is
		 * its own two-instruction test with jumps of 0 and 1, so the list
		 * can grow without reaching classic BPF's 8-bit jump offsets. */
		OWN(SYS_open), OWN(SYS_openat), OWN(SYS_openat2), OWN(SYS_creat),
		OWN(SYS_unlink), OWN(SYS_unlinkat), OWN(SYS_rename), OWN(SYS_renameat),
		OWN(SYS_renameat2), OWN(SYS_mkdir), OWN(SYS_mkdirat), OWN(SYS_rmdir),
		OWN(SYS_link), OWN(SYS_linkat), OWN(SYS_symlink), OWN(SYS_symlinkat),
		OWN(SYS_truncate), OWN(SYS_mknod), OWN(SYS_mknodat),
		OWN(SYS_chmod), OWN(SYS_fchmod), OWN(SYS_fchmodat), OWN(SYS_fchmodat2),
		OWN(SYS_chown), OWN(SYS_fchown), OWN(SYS_lchown), OWN(SYS_fchownat),
		OWN(SYS_utime), OWN(SYS_utimes), OWN(SYS_futimesat), OWN(SYS_utimensat),
		OWN(SYS_setxattr), OWN(SYS_lsetxattr), OWN(SYS_fsetxattr), OWN(SYS_setxattrat),
		OWN(SYS_removexattr), OWN(SYS_lremovexattr), OWN(SYS_fremovexattr), OWN(SYS_removexattrat),
		OWN(SYS_file_setattr), OWN(SYS_open_by_handle_at), OWN(SYS_io_uring_setup),
		/* channels between processes the model does not have (see
		 * outside_call) */
		OWN(SYS_process_vm_readv), OWN(SYS_process_vm_writev),
		OWN(SYS_shmget), OWN(SYS_shmat), OWN(SYS_msgget), OWN(SYS_semget), OWN(SYS_mq_open),
		OWN(SYS_kill), OWN(SYS_tkill), OWN(SYS_tgkill), OWN(SYS_rt_sigqueueinfo),
		OWN(SYS_rt_tgsigqueueinfo), OWN(SYS_pidfd_open), OWN(SYS_pidfd_send_signal),
		OWN(SYS_lseek), OWN(SYS_fsync), OWN(SYS_fdatasync), OWN(SYS_ftruncate),
		OWN(SYS_fallocate), OWN(SYS_flock), OWN(SYS_sync_file_range),
		/* with a modelled directory, the calls that read a file's
		 * metadata or name, which the checker answers for its files
		 * (mfs_call); without one they go to the kernel unseen, as
		 * before (FSX matches no call then) */
		FSX(SYS_stat), FSX(SYS_lstat), FSX(SYS_fstat), FSX(SYS_newfstatat), FSX(SYS_statx),
		FSX(SYS_access), FSX(SYS_faccessat), FSX(SYS_faccessat2), FSX(SYS_readlink),
		FSX(SYS_readlinkat), FSX(SYS_chdir), FSX(SYS_fchdir), FSX(SYS_getdents64),
		FSX(SYS_getdents), FSX(SYS_getxattr), FSX(SYS_lgetxattr), FSX(SYS_fgetxattr),
		FSX(SYS_listxattr), FSX(SYS_llistxattr), FSX(SYS_flistxattr), FSX(SYS_getxattrat),
		FSX(SYS_listxattrat), FSX(SYS_statfs), FSX(SYS_fstatfs),
#undef FSX
#undef OWN
		/* mmap: notify only when its fd (args[4]) is a checker-owned one,
		 * 500 up to INT_MAX (an anonymous map passes -1) */
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_mmap, 0, 5),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[4])),
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K, 500, 0, 1),
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K, 0x80000000U, 0, 1),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
		/* sendmsg: notify only when its fd (the low word of args[0]) is a
		 * checker-owned one, 500 or above; install_trace_filter() catches
		 * the rest once the listener has been sent */
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_sendmsg, 0, 3),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[0])),
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K, 500, 0, 1),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog p = {sizeof f/sizeof*f, f};
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	return seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &p);
}

/* The second filter, installed by the launch child after it has sent the
 * listener: a sendmsg on an fd below 500, which the first filter lets
 * through, stops the thread for its tracer (PTRACE_EVENT_SECCOMP), so
 * the checker sees it (trace_stop()).  Filters stack and the kernel
 * takes the most severe answer, and USER_NOTIF ranks above TRACE, so a
 * sendmsg the first filter notifies is still notified.  The arch check
 * is the first filter's: its KILL_PROCESS outranks anything here. */
static int
install_trace_filter(void)
{
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, nr)),
		/* execve and execveat stop too: the launch's own exec of the node
		 * is let through, and a later one is refused (trace_stop) */
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_execve, 4, 0),	/* to RET_TRACE */
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_execveat, 3, 0),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, SYS_sendmsg, 0, 3),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS, offsetof(struct seccomp_data, args[0])),
		BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K, 500, 1, 0),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRACE),
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog p = {sizeof f/sizeof*f, f};
	return seccomp(SECCOMP_SET_MODE_FILTER, 0, &p);
}

/* Start node nd's process.  The parent seizes the child with ptrace
 * before the child installs any filter, and the child waits for a byte
 * on the socketpair until it has.  The child installs both filters and
 * then sends the listener with a sendmsg below fd 500, which the second
 * filter stops: that stop shows the trace filter is in and reaches the
 * checker, and the checker resumes the call.  The socketpair is
 * close-on-exec, so neither end leaks into the node. */
static int
launch(Node *nd)
{
	int sp[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sp) != 0) return -1;	/* GCOVR_EXCL_LINE: a socketpair of the checker's own */
	if (pipe(nd->fd3) != 0) return -1;	/* GCOVR_EXCL_LINE: a pipe of the checker's own */
	if (pipe(nd->fd4) != 0) return -1;	/* GCOVR_EXCL_LINE: a pipe of the checker's own */
	pid_t pid = fork();
	if (pid == 0)
	{
		close(sp[0]);
		char go;
		if (read(sp[1], &go, 1) != 1) _exit(2);	/* GCOVR_EXCL_LINE: the parent always sends the go byte */
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
		/* Start the node as a service starts: every signal at its default
		 * disposition and none blocked.  Ignored signals and the mask
		 * survive exec, so without this a node would inherit whatever the
		 * checker's own parent set (CI runners ignore SIGPIPE), and the
		 * same binary would behave differently from one host to another. */
		{
			sigset_t none; sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			for (int sig = 1; sig < NSIG; sig++)
				if (sig != SIGKILL && sig != SIGSTOP) signal(sig, SIG_DFL);
		}
		/* No address randomisation: a thread's stack, its pthread_join
		 * word and every mmap land at the same address on every replay,
		 * so the world encoding can name a futex word or an epoll token by
		 * its address and still dedup across replays. */
		if (personality(ADDR_NO_RANDOMIZE) == -1)	/* GCOVR_EXCL_LINE: ADDR_NO_RANDOMIZE is always allowed */
		{
			static const char msg[] = "cmc: cannot disable address randomisation for a node\n";
			(void) !write(2, msg, sizeof msg - 1);
			_exit(2);
		}
		char idbuf[8]; snprintf(idbuf, sizeof idbuf, "%d", nd->id);
		/* argv = binary, id, then the space-separated [target] args (so a
		 * target that takes flags needs no wrapper) */
		char *av[64]; int ac = 0;
		av[ac++] = (char *) nd->binary; av[ac++] = idbuf;
		char argbuf[512]; snprintf(argbuf, sizeof argbuf, "%s", M.args);
		for (char *p = strtok(argbuf, " \t"); p && ac < 63; p = strtok(NULL, " \t")) av[ac++] = p;
		av[ac] = NULL;
#ifdef CMC_COVERAGE
		/* a coverage build: write this child's counts now, while it may
		 * still write files; past the filter an open for writing is refused,
		 * and exec discards the counters */
		{
			extern void __gcov_dump(void);

			__gcov_dump();
		}
#endif
		int lf = install_filter(); if (lf < 0) _exit(2);	/* GCOVR_EXCL_START: the node's side of the filter, past the last count */
		/* From here until the listener is sent no call may reach the first
		 * filter's USER_NOTIF: nobody holds the listener to answer it. */
		if (install_trace_filter() != 0) _exit(2);
		char cb[CMSG_SPACE(sizeof(int))] = {0}; char d = 'L';
		struct iovec iov = {&d, 1}; struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &lf, sizeof(int));
		if (sendmsg(sp[1], &mh, 0) != 1) _exit(2);
		execv(nd->binary, av);
		_exit(2);	/* GCOVR_EXCL_STOP */
	}
	/* the checker keeps the write ends: it writes a node's state-hook
	 * records into them itself (serve_hook) */
	close(sp[1]); nd->hook_w[0] = nd->fd3[1]; nd->hook_w[1] = nd->fd4[1];
	/* TRACECLONE: a thread the node creates starts in a ptrace stop,
	 * so the search, not the kernel, decides when it first runs.
	 * TRACESECCOMP: a sendmsg on a kernel fd stops (trace_stop). */
	if (ptrace(PTRACE_SEIZE, pid, 0, PTRACE_O_TRACECLONE | PTRACE_O_TRACESECCOMP) != 0)	/* GCOVR_EXCL_LINE: the parent may always seize its own child */
	{
		fprintf(stderr, "cmc: cannot ptrace node %d: %s\n", nd->id, strerror(errno));
		kill(pid, SIGKILL); waitpid(pid, NULL, __WALL); close(sp[0]);
		return -1;
	}
	if (write(sp[0], "g", 1) != 1)	/* GCOVR_EXCL_LINE: a write to the checker's own socketpair */
	{
		kill(pid, SIGKILL); waitpid(pid, NULL, __WALL); close(sp[0]);
		return -1;
	}
	{
		int st; pid_t r;
		do r = waitpid(pid, &st, __WALL);
		while (r < 0 && errno == EINTR);	/* GCOVR_EXCL_LINE: a wait interrupted by a signal; the checker takes none */
		if (r != pid || !WIFSTOPPED(st) || st >> 8 != (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8)))
		{
			fprintf(stderr, "cmc: node %d did not stop at its sendmsg trace filter (status %#x)\n", nd->id, st);
			if (r != pid || WIFSTOPPED(st)) { kill(pid, SIGKILL); waitpid(pid, NULL, __WALL); }	/* GCOVR_EXCL_LINE: the node's status is the one just waited for */
			close(sp[0]);
			return -1;
		}
		if (ptrace(PTRACE_CONT, pid, 0, 0) != 0)	/* GCOVR_EXCL_LINE: the child is in the stop just waited for */
		{
			fprintf(stderr, "cmc: cannot resume node %d: %s\n", nd->id, strerror(errno));
			kill(pid, SIGKILL); waitpid(pid, NULL, __WALL); close(sp[0]);
			return -1;
		}
	}
	char cb[CMSG_SPACE(sizeof(int))] = {0}; char d;
	struct iovec iov = {&d, 1}; struct msghdr mh = {0};
	mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
	if (recvmsg(sp[0], &mh, 0) < 0) { close(sp[0]); return -1; }	/* GCOVR_EXCL_LINE: the child sent the listener before this */
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); nd->listener = -1;	/* GCOVR_EXCL_LINE: the child always sends one fd */
	if (cm) memcpy(&nd->listener, CMSG_DATA(cm), sizeof(int));	/* GCOVR_EXCL_LINE: the child always sends one fd */
	close(sp[0]);
	if (nd->listener < 0) return -1;	/* GCOVR_EXCL_LINE: the child always sends one fd */
	/* close-on-exec, so a node launched later does not inherit this one's
	 * hook.  Not O_NONBLOCK: the status flags are shared with the node's
	 * own fd 3, and it would see them; serve_hook polls instead. */
	for (int k = 0; k < 2; k++) fcntl(nd->hook_w[k], F_SETFD, FD_CLOEXEC);
	/* make the notify fd non-blocking: drive_node() polls the listener
	 * and RECVs only a ready notification, so a lull reaches its check */
	{ int fl = fcntl(nd->listener, F_GETFL); fcntl(nd->listener, F_SETFL, fl | O_NONBLOCK); }
	nd->pid = pid; nd->alive = 1; nd->next_fd = 500; nd->nc = 0;
	nd->npend = 0; nd->crashed = 0; nd->canon_len = 0; nd->durable_len = 0; nd->durable_read_off = 0; nd->preload_ok = 0; nd->timer_fd = -1;
	nd->ls_nonblock = 0; nd->ls_cloexec = 0; nd->listener_fd = -1; nd->nefd = 0; nd->nep = 0;
	memset(nd->fdflag, 0, sizeof nd->fdflag); memset(nd->sockopt, 0, sizeof nd->sockopt); memset(nd->dupof, 0, sizeof nd->dupof); nd->execd = 0;
	nd->nth = 0; memset(nd->th, 0, sizeof nd->th); nd->threaded = 0; nd->race_ok = 0;
	return 0;
}

/* Drain a canonical-emit pipe non-blocking and copy the LAST complete
 * record into out/outlen (records are [count][count bytes]).  The record
 * is the node's state, so it must be exact: one longer than SNAP bytes
 * cannot be stored whole, and bytes left after the last complete record
 * are a record the node has not finished writing.  Either ends the check
 * (exit 3) rather than truncating or dropping state.  Records are read
 * through a carry buffer, so a node that writes many records between two
 * quiescent points is drained whole. */
static void
drain_pipe(const Node *nd, int fd, unsigned char *out, int *outlen, const char *which)
{
	unsigned char all[4096 + 1 + 255]; int tot = 0, r;
	int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	for (;;)
	{
		r = (int) read(fd, all + tot, sizeof all - (size_t) tot);
		if (r <= 0) break;
		tot += r;
		/* consume every complete record; keep a trailing partial one */
		int off = 0;
		while (off < tot && off + 1 + all[off] <= tot)
		{
			int cnt = all[off];
			if (cnt > SNAP) die(3, "node %d: a %s state record of %d bytes is longer than the %d the checker stores", nd->id, which, cnt, SNAP);
			memcpy(out, all + off + 1, (size_t) cnt); *outlen = cnt;
			off += 1 + cnt;
		}
		memmove(all, all + off, (size_t) (tot - off)); tot -= off;
	}
	if (tot > 0) die(3, "node %d: a partial %s state record (%d bytes) was left unfinished", nd->id, which, tot);
}

/* read any canonical-emit records this node wrote; keep the last of each
 * stream (volatile on fd 3, durable on fd 193) */
static void
drain_canon(Node *nd)
{
	drain_pipe(nd, nd->fd3[0], nd->canon, &nd->canon_len, "volatile");
	drain_pipe(nd, nd->fd4[0], nd->durable, &nd->durable_len, "durable");
}

/* bag helpers.  A message joins its connection's direction behind
 * every earlier one, so each direction is FIFO, as TCP is. */
static void
bag_add(int conn, int to, const unsigned char *b, int len, int eof)
{
	for (int i = 0; i < MAXMSG; i++)
		if (!bag[i].used)
		{
			bag[i].used = 1; bag[i].conn = conn; bag[i].to = to; bag[i].len = len; bag[i].eof = eof;
			bag[i].seq = conns[conn].seq[to]++;
			if (len > 0) memcpy(bag[i].body, b, (size_t) len);
			return;
		}
	die(1, "more than %d messages in flight", MAXMSG);
}

/* the oldest message on connection conn toward end `to` (its bag index),
 * or -1; the next one after sequence number `after` when after >= 0 */
static int
conn_next(int conn, int to, int after)
{
	int best = -1;
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].conn == conn && bag[i].to == to && bag[i].seq > after &&
			(best < 0 || bag[i].seq < bag[best].seq))
			best = i;
	return best;
}
static int conn_head(int conn, int to) { return conn < 0 ? -1 : conn_next(conn, to, -1); }

/* does connection conn hold unread bytes (not just a FIN) toward `to`? */
static int
conn_has_data(int conn, int to)
{
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].conn == conn && bag[i].to == to && !bag[i].eof) return 1;
	return 0;
}

/* discard every message queued on connection conn, both directions */
static void
conn_discard(int conn)
{
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].conn == conn) bag[i].used = 0;
}

/* discard the messages queued on connection conn toward end `to` */
static void
conn_discard_to(int conn, int to)
{
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].conn == conn && bag[i].to == to) bag[i].used = 0;
}

/* has end e been sent the peer's FIN (queued, or already read)?  Linux
 * then holds the end in CLOSE_WAIT, where a reset reports EPIPE */
static int
fin_toward(int conn, int e)
{
	if (conns[conn].eof[e]) return 1;
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].conn == conn && bag[i].to == e && bag[i].eof) return 1;
	return 0;
}

/* the errno a reset leaves at end e of connection conn */
static int
reset_errno(int conn, int e)
{
	return fin_toward(conn, e) ? EPIPE : ECONNRESET;
}

/* a new connection from node a (or -1, the client) to node b's listener */
static int
conn_new(int a, int b)
{
	for (int c = 0; c < MAXCONNS; c++)
		if (!conns[c].used)
		{
			memset(&conns[c], 0, sizeof conns[c]);
			conns[c].used = 1; conns[c].node[0] = a; conns[c].node[1] = b;
			return c;
		}
	die(1, "more than %d connections open at once", MAXCONNS);
	return -1;
}

/* free connection conn once neither end has a descriptor */
static void
conn_maybe_free(int conn)
{
	if (conns[conn].closed[0] && conns[conn].closed[1]) { conn_discard(conn); conns[conn].used = 0; }
}

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
	if (fd >= 500 && fd - 500 < MAXOWNED && nd->dupof[fd - 500]) fd = nd->dupof[fd - 500];
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

/* A call the model does not answer, on fd a[0].  If the fd is one the
 * checker owns (a virtual socket, eventfd or epoll instance, numbered from
 * 500), the kernel has no such file, and answering EBADF or acting on a
 * real fd of that number would both be wrong, so the check ends.  On any
 * other fd the call goes to the kernel as usual.  dup2 and dup3 also
 * refuse an owned target fd. */
static void
refuse_on_owned(const Node *nd, long nr, const __u64 *a)
{
	int fd = (int) a[0];
	/* the call's other fd: dup2's and dup3's target, splice's output,
	 * sendfile's input */
	int other = nr == SYS_dup2 || nr == SYS_dup3 ? (int) a[1] : nr == SYS_splice ? (int) a[2] :
		nr == SYS_sendfile ? (int) a[1] : -1;
	if (!(fd >= 500 && fd < nd->next_fd) && other >= 500 && other < nd->next_fd) fd = other;
	if (fd >= 500 && fd < nd->next_fd)
		die(3, "node %d: system call %ld on checker-owned fd %d is not modelled", nd->id, nr, fd);
}

/* The file system is read-only to a node.  Its durable state goes
 * through fd 193, which the world holds and a crash keeps; a file a node
 * wrote would survive into the next replay and past a crash, outside the
 * world, so a replay could diverge or a crash test pass on bytes the
 * model never accounted for.
 *
 * The path argument of a call that opens or names a file is read from
 * the node's memory.  A path the checker cannot read whole (an unmapped
 * pointer, or longer than PATH_MAX) is refused too, rather than guessed:
 * the kernel would fail it, or open something. */
#define FS_PATHMAX 4096
static const char *
fs_path(const Node *nd, uint64_t addr, char *buf)
{
	size_t got = 0;
	/* a page at a time: process_vm_readv stops at the first unmapped
	 * page, so a path near the end of a mapping still reads */
	while (got < FS_PATHMAX)
	{
		size_t chunk = 4096 - (size_t) ((addr + got) & 4095);
		if (chunk > FS_PATHMAX - got) chunk = FS_PATHMAX - got;
		size_t k = cr_read(nd->pid, addr + got, buf + got, chunk);
		if (memchr(buf + got, 0, k)) return buf;
		if (k < chunk) break;
		got += k;
	}
	die(3, "node %d: a file system call passes a path the checker cannot read", nd->id);
	return NULL;
}

/* An open that cannot change a file: read-only access, and none of the
 * flags that create, truncate or append to one.  O_TMPFILE contains
 * O_DIRECTORY, so it is tested whole. */
static int
open_readonly(uint64_t flags)
{
	if ((flags & O_ACCMODE) != O_RDONLY) return 0;
	if (flags & (O_CREAT | O_TRUNC | O_APPEND)) return 0;
	return (flags & O_TMPFILE) != O_TMPFILE;
}

/* Answer a file system call (fs_call is 1 for it), or end the check.
 *   open, openat, openat2, creat: a read-only open goes to the kernel;
 *     any other, except one of /dev/null, is refused, naming the path.
 *   the calls that change the namespace or metadata: refused, naming
 *     the path when there is one; open_by_handle_at like an open; and
 *     io_uring_setup, whose operations bypass seccomp.
 *   lseek, fsync, fdatasync, ftruncate, fallocate, flock and
 *     sync_file_range on a checker-owned fd: Linux's answer for a
 *     socket, measured on Linux 7.3 for each (lseek ESPIPE; fsync,
 *     fdatasync and ftruncate EINVAL; sync_file_range ESPIPE), or a
 *     refusal where the answer depends on more than the fd being a
 *     socket (fallocate, flock).  fsync and fdatasync of fd 193
 *     succeed: the record is durable when written.  On a kernel fd
 *     lseek, fsync, fdatasync and sync_file_range go to the kernel
 *     (kernel_fd_call() refuses them first in a node with more than one
 *     thread).  ftruncate and fallocate are refused there, since the
 *     only file a node can have open for writing is one it inherited;
 *     so is flock, which works on a read-only fd and leaves a lock other
 *     nodes see, kernel state the world does not hold.
 * Returns 1 if nr is one of these (g_resp holds the answer), else 0. */
static int
fs_call(const Node *nd, long nr, const __u64 *a)
{
	char path[FS_PATHMAX];
	uint64_t flags;
	int fd = (int) a[0];

	switch (nr)
	{
		case SYS_open: flags = a[1]; fs_path(nd, a[0], path); break;
		case SYS_openat: flags = a[2]; fs_path(nd, a[1], path); break;
		case SYS_creat: flags = O_CREAT | O_WRONLY | O_TRUNC; fs_path(nd, a[0], path); break;
		case SYS_openat2:
		{
			struct { uint64_t flags, mode, resolve; } how;
			if (a[3] < sizeof how || cr_read(nd->pid, a[2], &how, sizeof how) != sizeof how)
				die(3, "node %d: openat2 passes an open_how the checker cannot read", nd->id);
			flags = how.flags; fs_path(nd, a[1], path);
			break;
		}
		case SYS_unlink: case SYS_rmdir: case SYS_mkdir: case SYS_truncate: case SYS_mknod:
		case SYS_chmod: case SYS_chown: case SYS_lchown: case SYS_utime: case SYS_utimes:
		case SYS_rename: case SYS_link: case SYS_symlink:
		case SYS_setxattr: case SYS_lsetxattr: case SYS_removexattr: case SYS_lremovexattr:
			die(3, "node %d: system call %ld changes the file system at %s; the file system is read-only to a node (durable state goes through fd %d)",
				nd->id, nr, fs_path(nd, nr == SYS_symlink ? a[1] : a[0], path), DURABLE_FD);
			return 1;
		case SYS_unlinkat: case SYS_mkdirat: case SYS_mknodat: case SYS_fchmodat: case SYS_fchmodat2:
		case SYS_fchownat: case SYS_futimesat: case SYS_renameat: case SYS_renameat2: case SYS_linkat:
		case SYS_symlinkat: case SYS_setxattrat: case SYS_removexattrat:
			die(3, "node %d: system call %ld changes the file system at %s; the file system is read-only to a node (durable state goes through fd %d)",
				nd->id, nr, fs_path(nd, nr == SYS_symlinkat ? a[2] : a[1], path), DURABLE_FD);
			return 1;
		case SYS_utimensat:
			/* a NULL path is futimens(), on the fd itself */
			if (!a[1]) die(3, "node %d: futimens on fd %d changes the file system; the file system is read-only to a node", nd->id, fd);
			die(3, "node %d: system call %ld changes the file system at %s; the file system is read-only to a node (durable state goes through fd %d)",
				nd->id, nr, fs_path(nd, a[1], path), DURABLE_FD);
			return 1;
		case SYS_fchmod: case SYS_fchown: case SYS_fsetxattr: case SYS_fremovexattr:
			die(3, "node %d: system call %ld on fd %d changes the file system; the file system is read-only to a node", nd->id, nr, fd);
			return 1;
		case SYS_file_setattr:
			die(3, "node %d: file_setattr changes the file system at %s; the file system is read-only to a node", nd->id, fs_path(nd, a[1], path));
			return 1;
		case SYS_open_by_handle_at:
			if (!open_readonly(a[2]))
				die(3, "node %d: opens a file by handle for writing (flags %#llo); the file system is read-only to a node", nd->id, (unsigned long long) a[2]);
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
			return 1;
		case SYS_io_uring_setup:
			/* io_uring's operations reach the kernel without a system call
			 * each, so seccomp never sees them: a socket, a file write, an
			 * open would all go unchecked */
			die(3, "node %d: io_uring is not modelled (its operations bypass the checker)", nd->id);
			return 1;
		case SYS_lseek: case SYS_fsync: case SYS_fdatasync: case SYS_ftruncate:
		case SYS_fallocate: case SYS_flock: case SYS_sync_file_range:
			if (fd == DURABLE_FD && (nr == SYS_fsync || nr == SYS_fdatasync)) { g_resp->val = 0; return 1; }
			if (fd >= 500 && fd < nd->next_fd)
			{
				if (nr == SYS_lseek) { g_resp->error = (int) a[2] > 4 ? -EINVAL : -ESPIPE; return 1; }
				if (nr == SYS_fsync || nr == SYS_fdatasync || nr == SYS_ftruncate) { g_resp->error = -EINVAL; return 1; }
				if (nr == SYS_sync_file_range) { g_resp->error = (a[3] & ~7ULL) ? -EINVAL : -ESPIPE; return 1; }
				die(3, "node %d: system call %ld on checker-owned fd %d is not modelled", nd->id, nr, fd);
			}
			if (nr == SYS_ftruncate || nr == SYS_fallocate)
				die(3, "node %d: system call %ld on fd %d changes a file; the file system is read-only to a node", nd->id, nr, fd);
			if (nr == SYS_flock)
				die(3, "node %d: flock on fd %d is not modelled (a lock is kernel state other nodes see)", nd->id, fd);
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
			return 1;
		default:
			return 0;
	}
	/* an open */
	if (open_readonly(flags) || !strcmp(path, "/dev/null"))
	{
		g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
		return 1;
	}
	die(3, "node %d: opens %s for writing (flags %#llo); the file system is read-only to a node (durable state goes through fd %d)",
		nd->id, path, (unsigned long long) flags, DURABLE_FD);
	return 1;
}

/* Is path under the modelled directory ([files] dir)?  If so, rel gets
 * it relative to that directory ("" for the directory itself) with its
 * "." parts and repeated slashes removed, and the answer is 1.  A path
 * outside it is 0, and keeps the read-only file system.  path is
 * absolute.  A ".." part is refused, wherever it is, and so is a name
 * longer than the model's component limit. */
static int
mfs_path(const Node *nd, const char *path, char *rel)
{
	char norm[2 * FS_PATHMAX];
	size_t dl = strlen(M.fsdir), n = 0;

	if (!M.fsdir[0]) return 0;
	/* path without its empty and "." parts, as "/a/b" */
	for (const char *p = path; *p;)
	{
		while (*p == '/') p++;
		const char *e = p;
		while (*e && *e != '/') e++;
		size_t len = (size_t) (e - p);
		if (len == 2 && p[0] == '.' && p[1] == '.')
			die(3, "node %d: path %s has a .. part, which is not modelled while a [files] dir is", nd->id, path);
		if (len > 0 && !(len == 1 && *p == '.'))
		{
			norm[n++] = '/';
			memcpy(norm + n, p, len); n += len;
		}
		p = e;
	}
	norm[n] = 0;
	if (strncmp(norm, M.fsdir, dl) || (norm[dl] && norm[dl] != '/')) return 0;
	const char *r = norm[dl] ? norm + dl + 1 : norm + dl;
	for (const char *p = r; *p;)
	{
		const char *e = strchr(p, '/');
		size_t len = e ? (size_t) (e - p) : strlen(p);
		if (len >= FS_NAME)
			die(3, "node %d: path %s has a name longer than %d bytes, which is not modelled", nd->id, path, FS_NAME - 1);
		p += len + (e != NULL);
	}
	strcpy(rel, r);
	return 1;
}

/* Set when a call on the modelled files is one the search may crash
 * the node after, or fail (see mfs_hold()); try_pending_for() holds its
 * answer then. */
static int g_fs_hold;

/* Answer g_resp with r, a modelled call's result: a count, or a negative
 * errno.  A model limit or an operation the model does not have ends the
 * check, and so does a node whose files outgrow [files] bytes. */
static void
mfs_answer(const Node *nd, long nr, long r)
{
	if (r == FS_LIMIT)
		die(3, "node %d: system call %ld on a modelled file passes a limit of the file model (%d files, %d entries a directory, %d unsynced operations a file, %d bytes a file)",
			nd->id, nr, FS_MAXINO, FS_MAXENT, FS_MAXOP, FS_FILEMAX);
	if (r == FS_NOTMOD)
		die(3, "node %d: system call %ld on a modelled file is not modelled (a rename between directories, or of a directory)", nd->id, nr);
	if (fs_bytes(&nd->fs) > M.fsbytes)
		die(3, "node %d: its modelled files hold %ld bytes, past the model's [files] bytes = %ld", nd->id, fs_bytes(&nd->fs), M.fsbytes);
	if (r < 0) g_resp->error = (int) r;
	else g_resp->val = r;
}

/* Write a stat for modelled inode ino (size, kind) to addr, as the
 * kernel's struct stat (x86-64), or statx's when sx is set. */
static int
mfs_put_stat(const Node *nd, uint64_t addr, int ino, long size, int isdir, int sx)
{
	unsigned mode = isdir ? S_IFDIR | 0755 : S_IFREG | 0644;
	if (sx)
	{
		struct statx x; memset(&x, 0, sizeof x);
		x.stx_mask = STATX_BASIC_STATS; x.stx_blksize = 4096; x.stx_nlink = 1;
		x.stx_mode = (uint16_t) mode; x.stx_ino = (uint64_t) ino + 1;
		x.stx_size = (uint64_t) size; x.stx_blocks = (uint64_t) (size + 511) / 512;
		return cr_write(nd->pid, addr, &x, sizeof x) == sizeof x ? 0 : -EFAULT;
	}
	struct stat st; memset(&st, 0, sizeof st);
	st.st_mode = mode; st.st_ino = (ino_t) ino + 1; st.st_nlink = 1; st.st_blksize = 4096;
	st.st_size = size; st.st_blocks = (size + 511) / 512;
	return cr_write(nd->pid, addr, &st, sizeof st) == sizeof st ? 0 : -EFAULT;
}

/* The modelled open flags for Linux open flags fl, or -1 for a flag the
 * model does not have: O_DIRECT, O_TMPFILE, O_PATH, O_SYNC, O_DSYNC, a
 * directory opened other than O_RDONLY, or an access mode of 3. */
static int
mfs_flags(uint64_t fl)
{
	int acc = (int) (fl & O_ACCMODE), f = 0;
	uint64_t known = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_DIRECTORY | O_CLOEXEC |
		O_NOCTTY | O_LARGEFILE | O_NOFOLLOW | O_NOATIME;

	if (fl & ~known) return -1;
	if (acc == 3) return -1;
	if ((fl & O_DIRECTORY) && acc != O_RDONLY) return -1;
	if (acc != O_WRONLY) f |= FS_O_READ;
	if (acc != O_RDONLY) f |= FS_O_WRITE;
	if (fl & O_CREAT) f |= FS_O_CREAT;
	if (fl & O_EXCL) f |= FS_O_EXCL;
	if (fl & O_TRUNC) f |= FS_O_TRUNC;
	if (fl & O_APPEND) f |= FS_O_APPEND;
	if (fl & O_DIRECTORY) f |= FS_O_DIRECTORY;
	return f;
}

/* An open of a modelled path rel with Linux flags fl: the fd is the
 * node's next checker-owned number, so it can never meet a kernel fd. */
static void
mfs_open(Node *nd, long nr, const char *rel, uint64_t fl)
{
	int f = mfs_flags(fl);

	if (f < 0)
		die(3, "node %d: opens modelled file %s/%s with flags %#llo, which are not modelled (O_DIRECT, O_SYNC, O_TMPFILE and O_PATH are not)",
			nd->id, M.fsdir, rel, (unsigned long long) fl);
	int fd = nd->next_fd;
	(void) fd_flags(nd, fd);
	long r = fs_open(&nd->fs, rel, f, fd);
	if (r >= 0)
	{
		nd->next_fd++;
		*fd_flags(nd, fd) = (fl & O_CLOEXEC) ? FD_CX : 0;
		g_fs_hold = (f & (FS_O_CREAT | FS_O_TRUNC)) != 0;	/* the open may have changed a directory or a file */
	}
	mfs_answer(nd, nr, r);
}

/* The path argument of a path call nr (its index in a), the dirfd it is
 * relative to (AT_FDCWD for the calls without one), and for a rename or
 * a link the second path's index and dirfd; -1 when nr takes no path. */
static int
mfs_path_arg(long nr, const __u64 *a, int *dirfd, int *second, int *dirfd2)
{
	*dirfd = *dirfd2 = AT_FDCWD; *second = -1;
	switch (nr)
	{
		case SYS_open: case SYS_creat: case SYS_mkdir: case SYS_unlink: case SYS_rmdir:
		case SYS_stat: case SYS_lstat: case SYS_access: case SYS_readlink: case SYS_chdir:
		case SYS_getxattr: case SYS_lgetxattr: case SYS_listxattr: case SYS_llistxattr:
		case SYS_statfs: case SYS_truncate: case SYS_mknod: case SYS_chmod: case SYS_chown:
		case SYS_lchown: case SYS_utime: case SYS_utimes: case SYS_setxattr: case SYS_lsetxattr:
		case SYS_removexattr: case SYS_lremovexattr: case SYS_link:
			if (nr == SYS_link) *second = 1;
			return 0;
		case SYS_rename:
			*second = 1;
			return 0;
		case SYS_symlink:
			return 1;
		case SYS_openat: case SYS_openat2: case SYS_mkdirat: case SYS_unlinkat: case SYS_newfstatat:
		case SYS_statx: case SYS_faccessat: case SYS_faccessat2: case SYS_readlinkat:
		case SYS_getxattrat: case SYS_listxattrat: case SYS_mknodat: case SYS_fchmodat:
		case SYS_fchmodat2: case SYS_fchownat: case SYS_futimesat: case SYS_utimensat:
		case SYS_setxattrat: case SYS_removexattrat: case SYS_file_setattr:
			*dirfd = (int) a[0];
			return 1;
		case SYS_renameat: case SYS_renameat2: case SYS_linkat:
			*dirfd = (int) a[0]; *second = 3; *dirfd2 = (int) a[2];
			return 1;
		case SYS_symlinkat:
			*dirfd = (int) a[1];
			return 2;
		default:
			return -1;
	}
}

/* Is nr one of the calls the filter sends only while the model has a
 * directory (FSX)?  Outside that directory they go on to the kernel. */
static int
mfs_extra(long nr)
{
	switch (nr)
	{
		case SYS_stat: case SYS_lstat: case SYS_fstat: case SYS_newfstatat: case SYS_statx:
		case SYS_access: case SYS_faccessat: case SYS_faccessat2: case SYS_readlink:
		case SYS_readlinkat: case SYS_chdir: case SYS_fchdir: case SYS_getdents64:
		case SYS_getdents: case SYS_getxattr: case SYS_lgetxattr: case SYS_fgetxattr:
		case SYS_listxattr: case SYS_llistxattr: case SYS_flistxattr: case SYS_getxattrat:
		case SYS_listxattrat: case SYS_statfs: case SYS_fstatfs: case SYS_mmap:
			return 1;
		default:
			return 0;
	}
}

/* A stat-like call nr on modelled path rel (fd-based when rel is NULL):
 * stat, lstat, newfstatat and statx write the metadata; access and
 * faccessat say whether it exists; readlink says it is no link. */
static void
mfs_stat_call(Node *nd, long nr, const __u64 *a, int fd, const char *rel)
{
	long size; int isdir;
	int ino = fs_stat(&nd->fs, fd, rel, &size, &isdir);

	if (ino < 0) { mfs_answer(nd, nr, ino); return; }
	switch (nr)
	{
		case SYS_stat: case SYS_lstat: case SYS_fstat:
			mfs_answer(nd, nr, mfs_put_stat(nd, a[1], ino, size, isdir, 0)); return;
		case SYS_newfstatat:
			mfs_answer(nd, nr, mfs_put_stat(nd, a[2], ino, size, isdir, 0)); return;
		case SYS_statx:
			mfs_answer(nd, nr, mfs_put_stat(nd, a[4], ino, size, isdir, 1)); return;
		case SYS_readlink: case SYS_readlinkat:
			mfs_answer(nd, nr, -EINVAL); return;
		default:
			mfs_answer(nd, nr, 0); return;
	}
}

/* Carry out path call nr on modelled path rel (and rel2, a rename's
 * target).  Returns 1. */
static int
mfs_path_do(Node *nd, long nr, const __u64 *a, const char *rel, const char *rel2)
{
	switch (nr)
	{
		case SYS_open: mfs_open(nd, nr, rel, a[1]); return 1;
		case SYS_openat: mfs_open(nd, nr, rel, a[2]); return 1;
		case SYS_creat: mfs_open(nd, nr, rel, O_CREAT | O_WRONLY | O_TRUNC); return 1;
		case SYS_mkdir: case SYS_mkdirat:
			g_fs_hold = 1; mfs_answer(nd, nr, fs_mkdir(&nd->fs, rel)); return 1;
		case SYS_unlink:
			g_fs_hold = 1; mfs_answer(nd, nr, fs_unlink(&nd->fs, rel)); return 1;
		case SYS_unlinkat:
			if (a[2] & ~(uint64_t) AT_REMOVEDIR) { mfs_answer(nd, nr, -EINVAL); return 1; }
			if (a[2]) break;
			g_fs_hold = 1; mfs_answer(nd, nr, fs_unlink(&nd->fs, rel)); return 1;
		case SYS_rename: case SYS_renameat:
			g_fs_hold = 1; mfs_answer(nd, nr, fs_rename(&nd->fs, rel, rel2)); return 1;
		case SYS_renameat2:
			if (a[4]) break;
			g_fs_hold = 1; mfs_answer(nd, nr, fs_rename(&nd->fs, rel, rel2)); return 1;
		case SYS_stat: case SYS_lstat: case SYS_newfstatat: case SYS_statx:
		case SYS_access: case SYS_faccessat: case SYS_faccessat2:
		case SYS_readlink: case SYS_readlinkat:
			mfs_stat_call(nd, nr, a, -1, rel); return 1;
		default:
			break;
	}
	die(3, "node %d: system call %ld on modelled path %s/%s is not modelled (links, xattrs, rmdir, metadata changes, chdir and statfs are not)",
		nd->id, nr, M.fsdir, rel);
	return 1;
}

/* The flags word of an *at call that can take AT_EMPTY_PATH, or 0. */
static uint64_t
at_flags(long nr, const __u64 *a)
{
	if (nr == SYS_newfstatat) return a[3];
	if (nr == SYS_statx || nr == SYS_faccessat2) return a[nr == SYS_statx ? 2 : 3];
	return 0;
}

/* Read path argument k of call nr into buf and resolve it against the
 * model: 1 with rel set when it is a modelled path, 0 when it is not.
 * A relative path is joined to the node's working directory, or to the
 * directory a kernel dirfd names, as the kernel would resolve it (the
 * working directory is never a modelled one: chdir there is refused).
 * A path relative to a modelled directory fd is not modelled. */
static int
mfs_arg(Node *nd, const __u64 *a, int k, int dirfd, char *buf, char *rel)
{
	char link[64], base[FS_PATHMAX], full[2 * FS_PATHMAX];

	fs_path(nd, a[k], buf);
	if (buf[0] == '/') return mfs_path(nd, buf, rel);
	if (dirfd != AT_FDCWD && fs_isfd(&nd->fs, dirfd))
		die(3, "node %d: path %s relative to modelled directory fd %d is not modelled; use an absolute path", nd->id, buf, dirfd);
	if (dirfd == AT_FDCWD) snprintf(link, sizeof link, "/proc/%d/cwd", (int) nd->pid);
	else snprintf(link, sizeof link, "/proc/%d/fd/%d", (int) nd->pid, dirfd);
	ssize_t n = readlink(link, base, sizeof base - 1);
	if (n <= 0 || base[0] != '/') return 0;	/* no such kernel fd, or no directory: the kernel fails it */
	base[n] = 0;
	snprintf(full, sizeof full, "%s/%s", base, buf);
	return mfs_path(nd, full, rel);
}

/* A path call on the modelled directory.  Returns 1 when it answered nr
 * (g_resp holds the answer), 0 when nr names no modelled path, and the
 * caller treats it as before. */
static int
mfs_path_call(Node *nd, long nr, const __u64 *a)
{
	char buf[FS_PATHMAX], rel[FS_PATHMAX], buf2[FS_PATHMAX], rel2[FS_PATHMAX];
	int dirfd, second, dirfd2;
	int k = mfs_path_arg(nr, a, &dirfd, &second, &dirfd2);

	if (k < 0 || !M.fsdir[0]) return 0;
	/* fstat as glibc makes it: an empty path and AT_EMPTY_PATH, on dirfd */
	if (dirfd != AT_FDCWD && (at_flags(nr, a) & AT_EMPTY_PATH) && fs_path(nd, a[k], buf)[0] == 0)
	{
		if (!fs_isfd(&nd->fs, dirfd)) return 0;
		mfs_stat_call(nd, nr, a, dirfd, NULL);
		return 1;
	}
	int in1 = mfs_arg(nd, a, k, dirfd, buf, rel);
	int in2 = second >= 0 && mfs_arg(nd, a, second, dirfd2, buf2, rel2);
	if (in1 != in2 && second >= 0)
		die(3, "node %d: system call %ld links or renames between a modelled and an unmodelled path (%s, %s)", nd->id, nr, buf, buf2);
	if (!in1) return 0;
	return mfs_path_do(nd, nr, a, rel, rel2);
}

/* Copy n bytes (at most FS_FILEMAX) between the node and buf: in from
 * the node for a write, out to it for a read.  -EFAULT for an address
 * the node does not own, as the kernel answers. */
static long
mfs_io(Node *nd, long nr, const __u64 *a, long off)
{
	static unsigned char buf[FS_FILEMAX];
	long n = (long) a[2], r;
	int fd = (int) a[0];
	int wr = nr == SYS_write || nr == SYS_pwrite64;

	if (n < 0) return -EINVAL;
	if (n > FS_FILEMAX) n = FS_FILEMAX;
	if (wr)
	{
		size_t got = cr_read(nd->pid, a[1], buf, (size_t) n);
		if (got < (size_t) n) return -EFAULT;
		r = fs_write(&nd->fs, fd, buf, n, off, &nd->undo);
		if (r >= 0) { nd->undo_kind = 'w'; g_fs_hold = 1; }
		return r;
	}
	r = fs_read(&nd->fs, fd, buf, n, off);
	if (r > 0 && cr_write(nd->pid, a[1], buf, (size_t) r) != (size_t) r) return -EFAULT;
	return r;
}

/* A call on a modelled file's fd.  Returns 1 when fd is one (g_resp
 * holds the answer), else 0. */
static int
mfs_fd_call(Node *nd, long nr, const __u64 *a)
{
	int fd = (int) (nr == SYS_mmap ? a[4] : a[0]);
	int d1, d2, k2;
	long r = 0;

	/* a call with a path is mfs_path_call()'s, even on a modelled dirfd */
	if (mfs_path_arg(nr, a, &d1, &k2, &d2) >= 0) return 0;
	if (!fs_isfd(&nd->fs, fd)) return 0;
	switch (nr)
	{
		case SYS_read: case SYS_write: r = mfs_io(nd, nr, a, -1); break;
		case SYS_pread64: case SYS_pwrite64:
			r = (long) a[3] < 0 ? -EINVAL : mfs_io(nd, nr, a, (long) a[3]); break;
		case SYS_lseek: r = fs_lseek(&nd->fs, fd, (long) a[1], (int) a[2]); break;
		case SYS_ftruncate:
			r = fs_ftruncate(&nd->fs, fd, (long) a[1], &nd->undo);
			if (r == 0) { nd->undo_kind = 'w'; g_fs_hold = 1; }
			break;
		case SYS_fsync: case SYS_fdatasync:
			r = fs_fsync(&nd->fs, fd, &nd->undo);
			if (r == 0) { nd->undo_kind = 's'; g_fs_hold = 1; }
			break;
		case SYS_fstat:
			mfs_stat_call(nd, nr, a, fd, NULL); return 1;
		case SYS_close:
			r = fs_close(&nd->fs, fd);
			if (r == 0) *fd_flags(nd, fd) = 0;
			break;
		case SYS_fcntl:
			/* FD_CLOEXEC is inert (a node does not exec), as for a socket */
			if ((int) a[1] == F_GETFD) r = (*fd_flags(nd, fd) & FD_CX) ? FD_CLOEXEC : 0;
			else if ((int) a[1] == F_SETFD) { *fd_flags(nd, fd) = (a[2] & FD_CLOEXEC) ? FD_CX : 0; r = 0; }
			else die(3, "node %d: fcntl %d on modelled file fd %d is not modelled", nd->id, (int) a[1], fd);
			break;
		case SYS_mmap:
			die(3, "node %d: maps modelled file fd %d into memory, which is not modelled (its writes would bypass the checker)", nd->id, fd);
			return 1;
		default:
			die(3, "node %d: system call %ld on modelled file fd %d is not modelled", nd->id, nr, fd);
			return 1;
	}
	mfs_answer(nd, nr, r);
	return 1;
}

/* The modelled file system's part of serve(): a call on a modelled fd
 * or path is answered here (1), anything else is left to serve() (0),
 * except that a metadata call outside the model (mfs_extra()) goes on to
 * the kernel, as it would without a [files] section.  The files are
 * modelled for a node with one thread; in one with more, two threads'
 * order on a file would be a schedule the search does not enumerate, so
 * that is refused. */
static int
mfs_call(Node *nd, long nr, const __u64 *a)
{
	int done;

	g_fs_hold = 0; nd->undo_kind = 0;
	if (!M.fsdir[0]) return 0;
	done = mfs_fd_call(nd, nr, a) || mfs_path_call(nd, nr, a);
	if (done && node_multi(nd))
		die(3, "node %d: a node with more than one thread uses its modelled files (system call %ld), which is not modelled", nd->id, nr);
	if (!done && mfs_extra(nr)) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; return 1; }
	return done;
}

/* A node that forks becomes two processes the model cannot tell apart,
 * sharing one listener and its connections, and the child survives the
 * check.  End the check before the fork happens (the call is never
 * answered, so no child is created). */
static void
refuse_fork(const Node *nd)
{
	die(3, "node %d forks a second process; a node is one process (threads are modelled, fork is not)", nd->id);
}
/* answer()'s own response buffer: it may run while a handler is still
 * filling g_resp for its own notification (a close() whose RST answers
 * the peer's parked recv), and must not overwrite it */
static struct seccomp_notif_resp *g_ans;

static void answer(Node *nd, Thread *th, long val);
static void poll_wake(Node *nd, Thread *th, int target, int at);

/* An RST reached end e of connection conn.  A thread parked in recv on
 * it, with nothing queued before the reset, returns now: ECONNRESET, or
 * 0 when the end was in CLOSE_WAIT (the EPIPE is left for a write).  A
 * recv with bytes still queued stays parked, since those are delivered
 * first.  A thread parked in poll on it is woken, as Linux wakes both. */
static void
rst_notify(int conn, int e)
{
	Conn *cn = &conns[conn];
	if (cn->closed[e] || cn->node[e] < 0) return;
	Node *nd = &N[cn->node[e]];
	if (!nd->alive) return;
	for (int ti = 0; ti < nd->nth; ti++)
	{
		Thread *th = &nd->th[ti];
		if (!th->inuse || !th->block_kind) continue;
		if (th->block_kind == BK_RECV && th->block_conn == conn && th->block_end == e)
		{
			if (conn_head(conn, e) >= 0) continue;
			if (cn->err[e] == ECONNRESET) { cn->err[e] = 0; answer(nd, th, -ECONNRESET); }
			else answer(nd, th, 0);
		}
		else if (th->block_kind == BK_POLL)
			for (int k = 0; k < th->pollfd_n; k++)
			{
				int cs = cfd_slot(nd, th->pollfds[k]);
				if (cs >= 0 && nd->cconn[cs] == conn) { poll_wake(nd, th, nd->cpeer[cs], k); break; }
			}
	}
}

/* Close end e of connection conn.  dead says the end's host crashed:
 * nothing is sent, so the peer learns nothing until it writes (the
 * search's EOF action models a peer that notices).  A live close sends a
 * FIN, queued behind the bytes already sent, or an RST when this end
 * still had bytes unread.  The RST discards those unread bytes, keeps
 * what this end had already sent (it reaches the peer before the RST),
 * and leaves the peer the reset's errno. */
static void
end_close(int conn, int e, int dead)
{
	Conn *cn = &conns[conn];
	if (cn->closed[e]) return;
	cn->closed[e] = 1; cn->gone[e] = dead;
	if (dead) conn_discard_to(conn, e);
	else if (!cn->reset && !cn->closed[1 - e])
	{
		if (conn_has_data(conn, e))
		{
			conn_discard_to(conn, e);
			rst_path(cn->node[e], cn->node[1 - e]);
			cn->reset = 1; cn->err[1 - e] = reset_errno(conn, 1 - e);
			rst_notify(conn, 1 - e);
		}
		else if (!cn->shut_wr[e]) bag_add(conn, 1 - e, NULL, 0, 1);
	}
	conn_maybe_free(conn);
}

/* one hex signal mask ("SigBlk:" and the like) from a thread's
 * /proc status; die if it cannot be read */
static uint64_t
sigmask_of(const Node *nd, pid_t tid, const char *key)
{
	char path[64], line[128]; size_t kl = strlen(key);
	snprintf(path, sizeof path, "/proc/%d/task/%d/status", (int) nd->pid, (int) tid);
	FILE *f = fopen(path, "r");
	if (!f) die(1, "node %d: cannot read %s: %s", nd->id, path, strerror(errno));	/* GCOVR_EXCL_LINE: a live node's own /proc status */
	uint64_t m = 0; int found = 0;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, key, kl)) { m = strtoull(line + kl, NULL, 16); found = 1; break; }
	fclose(f);
	if (!found) die(1, "node %d: no %s in %s", nd->id, key, path);	/* GCOVR_EXCL_LINE: every /proc status has the signal masks */
	return m;
}

/* What Linux does with the SIGPIPE a write into a reset connection
 * raises, decided by the writing thread's disposition.  An ignored
 * signal is discarded and the write returns EPIPE.  The default action
 * ends the process, so the node exits here, killed with its write
 * unanswered (the signal itself cannot be sent: a node is
 * ptrace-seized, and a signal would park it in a signal-delivery stop).
 * A blocked signal would stay pending until unblocked, and a handler
 * would run and restart the write; neither is modelled, so either is
 * refused.  Returns 1 if the node died. */
static void node_gone(Node *nd);
static void drain_canon(Node *nd);
static void kill_node(Node *nd);
static int
sigpipe_kills(Node *nd, Thread *th)
{
	uint64_t bit = (uint64_t) 1 << (SIGPIPE - 1);
	if (sigmask_of(nd, th->tid, "SigIgn:") & bit) return 0;
	if (sigmask_of(nd, th->tid, "SigBlk:") & bit)
		die(3, "node %d writes into a reset connection with SIGPIPE blocked; a pending SIGPIPE is not modelled", nd->id);
	if (sigmask_of(nd, th->tid, "SigCgt:") & bit)
		die(3, "node %d writes into a reset connection with a SIGPIPE handler installed; a handler is not modelled", nd->id);
	kill_node(nd);
	nd->alive = 0; drain_canon(nd); node_gone(nd);
	return 1;
}

static int futex_wake(Node *nd, uint64_t uaddr, int n);

/* Is call nr a visible operation: one whose effect another thread of the
 * node can see other than through memory?  Those are the futex calls,
 * every call on a checker-owned fd (a socket, eventfd or epoll instance,
 * numbered from 500), a write to the state hook (fd 3) or the durable
 * record (fd 193) and a read of the durable record, and the calls that
 * create a checker-owned fd.  The checker carries out each of them
 * itself.  In a node with more than one thread each visible operation
 * ends the thread's step, and the search picks which thread runs next.
 * A call on any other fd, the kernel's, is refused in such a node (see
 * kernel_fd_call), except a write to stdout or stderr.  Everything else
 * (the clock, getrandom, mmap, writes to fd 1 and 2) is answered at once:
 * the clock moves only on a TIMEOUT action, when every thread is parked,
 * and nothing another thread does through memory between two calls is
 * explored.  clone and exit are visible too, but are not held here.  A
 * clone stops both threads at its return (see birth()), which is the
 * same choice point.  An exit's effect, the
 * clear-tid wake a pthread_join waits for, is carried out when the thread
 * reaches it, and the thread then has nothing left to run. */
static int
visible(long nr, const __u64 *a)
{
	switch (nr)
	{
		case SYS_futex: case SYS_socket: case SYS_eventfd2:
		case SYS_epoll_create1: case SYS_epoll_create: case SYS_epoll_ctl:
		case SYS_epoll_wait: case SYS_epoll_pwait: case SYS_poll: case SYS_ppoll:
		case SYS_accept: case SYS_accept4:
		case SYS_sched_yield:
			return 1;
		case SYS_setsockopt: case SYS_bind: case SYS_listen: case SYS_connect:
		case SYS_read: case SYS_write: case SYS_recvfrom: case SYS_sendto:
		case SYS_close: case SYS_fcntl:
		case SYS_readv: case SYS_writev: case SYS_recvmsg: case SYS_sendmsg:
		case SYS_shutdown: case SYS_getsockopt: case SYS_getsockname: case SYS_getpeername:
		case SYS_dup: case SYS_dup2: case SYS_dup3:
		{
			int fd = (int) a[0];
			return fd >= 500 || fd == 3 || fd == DURABLE_FD;
		}
		default:
			return 0;
	}
}

/* Is call nr of a node with one thread a point where a crash may fall?
 * A machine can lose power between any two system calls, and what other
 * machines or the disk see is decided per call, so while a crash is in
 * the budget each call with an effect outside the process ends the step:
 * a write or send on a checker-owned fd (bytes reach a peer or the
 * client), a close of one (a FIN or an RST), a connect (the connection
 * waits in the peer's backlog), and a durable write (fd 193).  Its
 * answer is held (BK_DONE), and the search may RUN the node on or CRASH
 * it first (midstep_node()).  Calls with no outside effect
 * stay inside the step, and so does a write to the volatile state hook,
 * which a crash discards.  A node with more
 * than one thread holds every visible call already (visible()). */
static int
crash_point(const Node *nd, long nr, const __u64 *a)
{
	if (bud.crashes <= 0 || node_multi(nd)) return 0;
	int fd = (int) a[0];
	switch (nr)
	{
		case SYS_write: case SYS_writev: return fd >= 500 || fd == DURABLE_FD;
		case SYS_sendto: case SYS_close: case SYS_sendmsg:
		case SYS_shutdown: case SYS_connect:
			return fd >= 500;
		default: return 0;
	}
}

/* Is call nr, in a node with more than one thread, a call on a kernel fd
 * the model does not answer?  Two threads that share a pipe or a file
 * interleave there on Linux, and its contents and offset are kernel
 * state the world encoding does not hold, so a check that sent such a
 * call to the kernel would search orders it cannot tell apart (and merge
 * them).  Every intercepted call whose first argument is an fd below the
 * checker's 500 counts, except the ones the checker answers itself (a
 * write to the state hook, a read of the durable record) and a write to
 * stdout or stderr, which no thread reads back.  A negative fd is the
 * kernel's EBADF, which shares nothing.  A sendmsg below fd 500 is not
 * notified but stopped by the second filter, and trace_stop() refuses it
 * the same way.  lseek, fsync, fdatasync, ftruncate, fallocate, flock and
 * sync_file_range count too: two threads that share a file share its
 * offset.
 *
 * The C library itself makes two such calls from a worker: the first
 * output on a stdio stream asks whether it is a terminal (ioctl TCGETS or
 * TCGETS2 on the stream's fd, to choose line buffering), and a failed
 * assert() writes its message to stderr with writev().  Both are let
 * through: the first only reads the terminal's settings, the same in
 * every schedule, and the second writes stderr. */
static int
kernel_fd_call(long nr, const __u64 *a)
{
	int fd = (int) a[0];

	switch (nr)
	{
		case SYS_write: case SYS_writev:
			if (fd == 1 || fd == 2) return 0;
			if (fd == 3 || fd == DURABLE_FD) return 0;
			break;
		case SYS_ioctl:
			if (fd >= 0 && fd <= 2 && (a[1] == TCGETS || a[1] == TCGETS2)) return 0;
			break;
		case SYS_read:
			if (fd == DURABLE_FD) return 0;
			break;
		case SYS_recvfrom: case SYS_sendto: case SYS_close: case SYS_fcntl:
		case SYS_readv: case SYS_preadv: case SYS_pwritev:
		case SYS_pread64: case SYS_pwrite64: case SYS_recvmsg:
		case SYS_sendmmsg: case SYS_recvmmsg: case SYS_shutdown: case SYS_dup:
		case SYS_dup2: case SYS_dup3: case SYS_getsockopt: case SYS_getsockname:
		case SYS_getpeername: case SYS_splice: case SYS_sendfile:
		case SYS_setsockopt: case SYS_bind: case SYS_listen: case SYS_connect:
		case SYS_accept: case SYS_accept4:
			break;
		case SYS_lseek: case SYS_fsync: case SYS_fdatasync: case SYS_ftruncate:
		case SYS_fallocate: case SYS_flock: case SYS_sync_file_range:
			/* the checker answers fsync and fdatasync of the durable record */
			if (fd == DURABLE_FD && (nr == SYS_fsync || nr == SYS_fdatasync)) return 0;
			break;
		default:
			return 0;
	}
	return fd >= 0 && fd < 500;
}

/* A write to the state hook: fd 3, or DURABLE_FD for the durable record.
 * The checker carries it out itself: it reads the bytes from the
 * thread's memory and writes them into the node's hook pipe, then
 * answers with the length.  So a hook write is a call the checker
 * answers like any other visible operation: its effect happens when the
 * thread reaches it, and in a node with more than one thread its answer
 * is held until the search runs the thread.  drain_canon() reads the
 * pipe as before, so the record checks (too long, left unfinished)
 * apply unchanged.  A write of at most PIPE_BUF bytes into a pipe with
 * room for them is whole, which poll's POLLOUT promises. */
static void hook_put(Node *nd, int fd, const unsigned char *buf, size_t len);
static void
serve_hook(Node *nd, const __u64 *a)
{
	unsigned char buf[MSGLEN];
	size_t		len = (size_t) a[2];

	if (len > sizeof buf)
		die(3, "node %d writes %zu bytes to its state hook in one call; the limit is %zu", nd->id, len, sizeof buf);
	size_t		got = cr_read(nd->pid, a[1], buf, len);
	if (got == 0 && len > 0) { g_resp->error = -EFAULT; return; }
	if (got < len) cr_fail(nd->id, "a state-hook write sends from a buffer that ends");
	hook_put(nd, (int) a[0], buf, len);
}

/* The len bytes at buf, written to state hook fd (3 or DURABLE_FD) of
 * node nd as one record write; the answer is the length. */
static void
hook_put(Node *nd, int fd, const unsigned char *buf, size_t len)
{
	int			w = fd == 3 ? nd->hook_w[0] : nd->hook_w[1];
	struct pollfd pf = {w, POLLOUT, 0};
	if (len > 0 && (poll(&pf, 1, 0) != 1 || !(pf.revents & POLLOUT)))
		die(1, "node %d writes more state records between two quiescent points than the checker's pipe holds", nd->id);
	if (len > 0 && write(w, buf, len) != (ssize_t) len)
		die(1, "node %d: cannot pass a state record to the checker: %s", nd->id, strerror(errno));
	g_resp->val = (long long) len;
}

static int serve(Node *nd, Thread *th);
static void put_read(Node *nd, Thread *th, const unsigned char *body, int k);
static int carry_out(Node *nd, Thread *th);
static int connwait(const Node *nd, long nr, const __u64 *a);
static int shortwait(Node *nd, long nr, const __u64 *a);
static int read_iov(Node *nd, uint64_t addr, long cnt, int *total);
/* -1, or the count a SHORT action makes the write being carried out
 * return (0 for EAGAIN) */
static int g_short = -1;
static void birth(Node *nd, Thread *th);
static void reap_tid(pid_t tid);

/* Thread th's stack pointer at the system call it is stopped in, from
 * /proc/PID/task/TID/syscall ("nr a0 .. a5 sp pc").  The libc wrapper's
 * instruction pointer is the same for every write() a program makes, so
 * the stack pointer is what tells two call sites of one wrapper apart;
 * with no address randomisation it is the same on every replay.
 *
 * The notification can reach the checker before the thread has gone to
 * sleep waiting for its answer, and until it has, the file reads
 * "running".  So a "running" line is retried, for up to a second. */
static uint64_t
stack_pointer(const Node *nd, const Thread *th)
{
	char		path[64], line[256];
	unsigned long long v[9];
	struct timespec pause = {0, 100 * 1000};

	snprintf(path, sizeof path, "/proc/%d/task/%d/syscall", (int) nd->pid, (int) th->tid);
	for (int tries = 0; tries < 10000; tries++)
	{
		FILE	   *f = fopen(path, "r");
		if (!f) die(1, "node %d: cannot read %s: %s", nd->id, path, strerror(errno));	/* GCOVR_EXCL_LINE: a stopped thread's own /proc entry */
		int			got = fgets(line, sizeof line, f) != NULL;
		fclose(f);
		if (got && strncmp(line, "running", 7) == 0)
		{
			nanosleep(&pause, NULL);
			continue;
		}
		if (got && sscanf(line, "%llu %llx %llx %llx %llx %llx %llx %llx %llx",
						  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]) == 9)
			return v[7];
		break;
	}
	die(1, "node %d: %s does not show a thread in a system call", nd->id, path);
	return 0;
}

/* Is owned fd fd of node nd a connection whose handshake is still out? */
static int
fd_connecting(Node *nd, int fd)
{
	int cs = fd >= 500 ? cfd_slot(nd, fd) : -1;
	return cs >= 0 && nd->cconn[cs] >= 0 && conns[nd->cconn[cs]].connecting;
}

/* Does call nr of node nd observe a socket whose non-blocking connect
 * has not completed?  On Linux the handshake completes on its own time
 * (on loopback, before connect() even returns), so such a call may see
 * it done or not.  The call is parked unserved (BK_CONNWAIT) and the
 * search chooses: complete the handshake first (CONNECT) or carry the
 * call out as still connecting (RUN).  A poll or epoll_wait that watches
 * such a socket counts too.  Only a node with one thread dials this way
 * (serve() refuses the rest). */
static int
connwait(const Node *cnd, long nr, const __u64 *a)
{
	Node *nd = (Node *) cnd;
	if (node_multi(nd)) return 0;
	switch (nr)
	{
		case SYS_poll: case SYS_ppoll:
		{
			int n = (int) a[1];
			struct pollfd pfs[MAXPOLL];
			if (n <= 0 || n > MAXPOLL) return 0;
			if (cr_read(nd->pid, a[0], pfs, (size_t) n * sizeof pfs[0]) != (size_t) n * sizeof pfs[0]) return 0;
			for (int i = 0; i < n; i++) if (fd_connecting(nd, pfs[i].fd)) return 1;
			return 0;
		}
		case SYS_epoll_wait: case SYS_epoll_pwait:
		{
			int eps = ep_slot(nd, (int) a[0]);
			if (eps < 0) return 0;
			for (int i = 0; i < nd->ep[eps].nit; i++) if (fd_connecting(nd, nd->ep[eps].fd[i])) return 1;
			return 0;
		}
		case SYS_read: case SYS_write: case SYS_recvfrom: case SYS_sendto:
		case SYS_readv: case SYS_writev: case SYS_recvmsg: case SYS_sendmsg:
		case SYS_connect: case SYS_getsockopt: case SYS_getpeername:
		case SYS_shutdown: case SYS_close:
			return fd_connecting(nd, (int) a[0]);
		default:
			return 0;
	}
}

/* RUN a thread parked in BK_CONNWAIT: carry its call out now, with the
 * handshake as it stands.  The notification is rebuilt from what the
 * thread recorded when it arrived. */
static void
run_connwait(Node *nd, Thread *th)
{
	memset(g_req, 0, g_sz.seccomp_notif);
	g_req->id = th->block_id; g_req->pid = (__u32) th->tid;
	g_req->data.nr = (int) th->nr; g_req->data.instruction_pointer = th->ip;
	memcpy(g_req->data.args, th->args, sizeof th->args);
	memset(g_resp, 0, g_sz.seccomp_notif_resp); g_resp->id = g_req->id;
	th->block_kind = 0;
	carry_out(nd, th);
}

/* Would write call nr of node nd, on a non-blocking connection a peer
 * reads, be a point where its send buffer may be full?  While the
 * short_writes budget lasts, such a write is parked unserved and the
 * search chooses: carry it out whole (RUN), or cut it short (SHORT,
 * see short_counts()).  A node with more than one thread is not
 * offered this (its writes already park as visible calls). */
static int
shortwait(Node *nd, long nr, const __u64 *a)
{
	if (bud.short_writes <= 0 || node_multi(nd)) return 0;
	if (nr != SYS_write && nr != SYS_sendto && nr != SYS_writev && nr != SYS_sendmsg) return 0;
	int fd = (int) a[0], cs = fd >= 500 ? cfd_slot(nd, fd) : -1;
	if (cs < 0 || !nd->cnonblock[cs] || nd->cconn[cs] < 0 || nd->cpeer[cs] < 0) return 0;
	const Conn *cn = &conns[nd->cconn[cs]];
	int e = nd->cend[cs];
	return !cn->connecting && !cn->full[e] && !cn->reset && !cn->refused && !cn->err[e] &&
		!cn->shut_wr[e] && !cn->closed[1 - e] && conn_has_data(nd->cconn[cs], 1 - e);
}

/* The length of the write thread th is parked in (BK_SHORTW), as its
 * arguments give it: a[2], or its iovec array's total. */
static int
parked_write_len(Node *nd, const Thread *th)
{
	uint64_t iov = th->args[1]; long cnt = (long) th->args[2];
	int total = 0;
	if (th->nr == SYS_write || th->nr == SYS_sendto) return (int) th->args[2];
	if (th->nr == SYS_sendmsg)
	{
		struct msghdr mh;
		if (cr_read(nd->pid, th->args[1], &mh, sizeof mh) != sizeof mh) return 0;
		iov = (uint64_t) (uintptr_t) mh.msg_iov; cnt = (long) mh.msg_iovlen;
	}
	return read_iov(nd, iov, cnt, &total) ? 0 : total;
}

/* The counts a SHORT action may cut a write of len bytes to: 0 (the
 * buffer is full: EAGAIN), and a prefix of 1, len / 2 and len - 1
 * bytes, each once.  Returns how many, written to out[4]. */
static int
short_counts(int len, int *out)
{
	int n = 0, c[3] = {1, len / 2, len - 1};
	out[n++] = 0;
	for (int i = 0; i < 3; i++)
	{
		int dup = c[i] < 1 || c[i] >= len;
		for (int j = 1; j < n; j++) dup |= out[j] == c[i];
		if (!dup) out[n++] = c[i];
	}
	return n;
}

/* Does the modelled-file call nd just carried out end its step?  Each
 * call that changes a modelled file or directory is a point where a
 * crash may fall (while one is in the budget): a crash keeps a prefix of
 * each file's unsynced operations, so a crash after a later call covers
 * an earlier one, except across an fsync, which this makes a point too.
 * A write or truncation is also a point while EIO or ENOSPC is in the
 * budget, and an fsync while EIO is: the search may make it fail
 * (A_FAIL) instead.  nd->undo_kind stays set only while it is held. */
static int
mfs_hold(Node *nd)
{
	int h = 0;

	if (g_fs_hold && !node_multi(nd))
	{
		if (bud.crashes > 0) h = 1;
		if (nd->undo_kind == 'w' && bud.eio + bud.enospc > 0) h = 1;
		if (nd->undo_kind == 's' && bud.eio > 0) h = 1;
	}
	if (!h || g_resp->error) nd->undo_kind = 0;
	g_fs_hold = 0;
	return h;
}

/* Service one ready notification on node nd without blocking the search.
 * Returns 1 if it did something, 0 if nd had no pending notification.
 * serve() carries the call out.  In a node with one thread its answer is
 * sent at once.  In a node with more than one, a visible operation's
 * answer is held instead (BK_DONE) and the thread parks, until the
 * search runs it (A_RUN): every visible operation is one the checker
 * carries out itself, so it has its effect now and returns later. */
static int
try_pending_for(Node *nd)
{
	if (!nd->alive) return 0;
	/* SECCOMP_IOCTL_NOTIF_RECV blocks even on an O_NONBLOCK listener
	 * (measured on Linux 6.18), so ask poll first: a thread busy or
	 * blocked in a call the checker does not see must leave the wait to
	 * drive_node(), whose lull check reports it */
	struct pollfd pf = {nd->listener, POLLIN, 0};
	if (poll(&pf, 1, 0) != 1 || !(pf.revents & POLLIN)) return 0;
	memset(g_req, 0, g_sz.seccomp_notif);
	/* It delivers a notification from ANY of this node's threads, so a
	 * tid already parked in a deferred slot does not stop the search from
	 * servicing another tid's call. */
	if (ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_RECV, g_req) != 0)	/* GCOVR_EXCL_LINE: poll said a notification is ready */
		return 0;
	long nr = g_req->data.nr; __u64 *a = g_req->data.args;
	/* route the notification to its thread's slot (seccomp_notif.pid is
	 * the calling thread's tid); a first-sight tid gets a fresh slot */
	Thread *th = th_for(nd, g_req->pid);
	if (!th) die(1, "node %d has more than %d threads", nd->id, MAXTHREAD);	/* GCOVR_EXCL_LINE: birth() refuses a thread past MAXTHREAD first */
	memset(g_resp, 0, g_sz.seccomp_notif_resp); g_resp->id = g_req->id;
	th->nr = nr; th->ip = g_req->data.instruction_pointer; th->sp = 0;
	memcpy(th->args, a, sizeof th->args);
	if (node_multi(nd) && kernel_fd_call(nr, a))
		die(3, "node %d: a thread of a multi-threaded node uses kernel fd %d (system call %ld); only checker-owned fds and the state hook are modelled between threads", nd->id, (int) a[0], nr);
	if (connwait(nd, nr, a))
	{
		th->block_kind = BK_CONNWAIT; th->block_id = g_req->id;
		return 1;
	}
	if (shortwait(nd, nr, a))
	{
		th->block_kind = BK_SHORTW; th->block_id = g_req->id;
		return 1;
	}
	return carry_out(nd, th);
}

/* Carry out notification g_req of thread th, and answer it now, hold
 * its answer (BK_DONE) or leave it parked, as serve() decides.  Returns
 * 1. */
static int
carry_out(Node *nd, Thread *th)
{
	long nr = g_req->data.nr; __u64 *a = g_req->data.args;
	/* decided before the call is served, which may add or end a thread */
	int hold = (node_multi(nd) && visible(nr, a)) || crash_point(nd, nr, a);
	if (hold || nr == SYS_clone || nr == SYS_clone3) th->sp = stack_pointer(nd, th);
	cap_begin(th);
	int parked = serve(nd, th);
	g_cap = NULL;
	if (parked) return 1;
	if (mfs_hold(nd)) { hold = 1; th->sp = stack_pointer(nd, th); }
	if (!hold)
	{
		if (ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_resp) != 0 && errno != ENOENT) {}
		if (th->cloning) birth(nd, th);
		/* a traced thread lingers after exit until its tracer reaps it */
		if (nr == SYS_exit && (pid_t) g_req->pid != nd->pid) reap_tid((pid_t) g_req->pid);
		return 1;
	}
	th->block_kind = BK_DONE; th->block_id = g_req->id;
	th->held_val = g_resp->val; th->held_err = g_resp->error; th->held_flags = g_resp->flags;
	return 1;
}

/* The clone of thread th has just been sent its CONTINUE: the kernel
 * makes the thread, and stops both.  The parent stops at
 * PTRACE_EVENT_CLONE, which names the newborn; the newborn starts in a
 * ptrace stop (PTRACE_EVENT_STOP, since the node is seized).  Both stay
 * stopped, the newborn as BK_NEWBORN and the parent as BK_CLONED, until
 * the search runs each (PTRACE_CONT).  Holding the parent too is what
 * lets the newborn's first visible operation come before the parent's
 * next one: a call the checker answers takes effect when the thread
 * reaches it, so a parent resumed at once would always act first.  Every
 * other thread of the node is parked, so the parent's stop is the only
 * one to wait for.  A clone the kernel refuses (EAGAIN at a thread limit)
 * returns with no event, and the parent's next call reaches the listener
 * instead; a failed thread creation is not modelled, so that ends the
 * check rather than waiting forever. */
static void
birth(Node *nd, Thread *th)
{
	int			st;
	pid_t		r;

	th->cloning = 0;
	for (int waited = 0;; waited++)
	{
		r = waitpid(th->tid, &st, __WALL | WNOHANG);
		if (r == th->tid) break;
		if (r < 0 && errno != EINTR) die(1, "node %d: lost thread %d in a clone: %s", nd->id, (int) th->tid, strerror(errno));
		struct pollfd pf = {nd->listener, POLLIN, 0};
		if (poll(&pf, 1, 1) > 0 && (pf.revents & POLLIN))
			die(3, "node %d: a thread's clone failed; a failed thread creation is not modelled", nd->id);
		if (waited >= LULL_MS) die(1, "node %d: a clone did not stop its thread within %d ms", nd->id, LULL_MS);
	}
	if (!WIFSTOPPED(st))
		die(3, "node %d died in a clone (status %#x); a signal is not modelled", nd->id, st);
	if (st >> 8 != (SIGTRAP | (PTRACE_EVENT_CLONE << 8)))
		die(3, "node %d stopped with signal %d in a clone; a signal is not modelled", nd->id, WSTOPSIG(st));
	unsigned long nt = 0;
	if (ptrace(PTRACE_GETEVENTMSG, th->tid, 0, &nt) != 0)	/* GCOVR_EXCL_LINE: the thread is in the clone stop just waited for */
		die(1, "node %d: cannot read the new thread of a clone: %s", nd->id, strerror(errno));
	pid_t tid = (pid_t) nt;
	do r = waitpid(tid, &st, __WALL);
	while (r < 0 && errno == EINTR);
	if (r != tid || !WIFSTOPPED(st) || st >> 16 != PTRACE_EVENT_STOP)
		die(1, "node %d: new thread %d did not start in a ptrace stop (status %#x)", nd->id, (int) tid, st);
	Thread *t = th_for(nd, tid);
	if (!t) die(1, "node %d has more than %d threads", nd->id, MAXTHREAD);
	t->block_kind = BK_NEWBORN; t->ctid_addr = th->clone_ctid;
	th->block_kind = BK_CLONED;
	nd->threaded = 1;
	if (!nd->race_ok && !g_unchecked_node) g_unchecked_node = nd->id + 1;
}

/* The sockopt bit of option opt at level lvl, 0 for SO_RCVBUF and
 * SO_SNDBUF (accepted, not recorded), or -1 for an option not modelled */
static int
sockopt_bit(int lvl, int opt)
{
	if (lvl == SOL_SOCKET && opt == SO_KEEPALIVE) return SO_B_KEEPALIVE;
	if (lvl == SOL_SOCKET && opt == SO_REUSEADDR) return SO_B_REUSEADDR;
	if (lvl == SOL_SOCKET && opt == SO_REUSEPORT) return SO_B_REUSEPORT;
	if (lvl == IPPROTO_TCP && opt == TCP_NODELAY) return SO_B_NODELAY;
	if (lvl == SOL_SOCKET && (opt == SO_RCVBUF || opt == SO_SNDBUF)) return 0;
	return -1;
}

/* setsockopt and getsockopt on owned socket fd a[0].  The boolean
 * options SO_KEEPALIVE, SO_REUSEADDR, SO_REUSEPORT and TCP_NODELAY are
 * recorded and read back, and change nothing else: the model has no
 * keepalive timer, no Nagle delay and no port reuse.  SO_RCVBUF and
 * SO_SNDBUF may be set and change nothing (the model has no buffer
 * sizes); reading them back would need Linux's doubled and clamped
 * value, so getsockopt of them is refused.  getsockopt(SO_ERROR) reads
 * and clears the error pending at this end: a refused dial's
 * ECONNREFUSED or a reset's ECONNRESET or EPIPE, as Linux does.  Any
 * other option ends the check.  The value lengths are checked as Linux
 * checks them: setsockopt wants at least an int, and getsockopt copies
 * at most one. */
static void
sock_option(Node *nd, long nr, const __u64 *a)
{
	int fd = (int) a[0], lvl = (int) a[1], opt = (int) a[2];
	int bit = sockopt_bit(lvl, opt);
	/* a dup shares the socket, and so its options */
	if (nd->dupof[fd - 500]) fd = nd->dupof[fd - 500];
	unsigned char *so = &nd->sockopt[fd - 500];
	if (nr == SYS_setsockopt)
	{
		int v;
		if (bit < 0) die(3, "node %d: setsockopt level %d option %d is not modelled", nd->id, lvl, opt);
		if ((int) a[4] < (int) sizeof v) { g_resp->error = -EINVAL; return; }
		if (cr_read(nd->pid, a[3], &v, sizeof v) != sizeof v) { g_resp->error = -EFAULT; return; }
		*so = (unsigned char) (v ? *so | bit : *so & ~bit);
		g_resp->val = 0; return;
	}
	int v, len;
	if (lvl == SOL_SOCKET && opt == SO_ERROR)
	{
		int cs = cfd_slot(nd, fd);
		Conn *cn = cs >= 0 && nd->cconn[cs] >= 0 ? &conns[nd->cconn[cs]] : NULL;
		v = cn ? cn->err[nd->cend[cs]] : 0;
		if (cn) cn->err[nd->cend[cs]] = 0;
	}
	else if (bit > 0) v = (*so & bit) != 0;
	else die(3, "node %d: getsockopt level %d option %d is not modelled", nd->id, lvl, opt);
	if (cr_read(nd->pid, a[4], &len, sizeof len) != sizeof len) { g_resp->error = -EFAULT; return; }
	if (len < 0) { g_resp->error = -EINVAL; return; }
	if (len > (int) sizeof v) len = sizeof v;
	if (cr_write(nd->pid, a[3], &v, (size_t) len) != (size_t) len ||
		cr_write(nd->pid, a[4], &len, sizeof len) != sizeof len)
		cr_fail(nd->id, "getsockopt returns a value");
	g_resp->val = 0;
}

/* getsockname and getpeername on owned fd a[0], as Linux answers them
 * on loopback: every address is 127.0.0.1, node i's listener and the
 * connections it accepts are on port 20000 + i, and a dialler's own
 * port is local_port()'s.  A socket not yet dialled or listening, and
 * a dial not connected, have no peer (ENOTCONN); getsockname on a plain
 * socket or a listener would report what bind() was given, which the
 * model does not keep, so it ends the check.  The length is Linux's: a
 * negative one is EINVAL, and at most 16 bytes are copied. */
static void
sock_name(Node *nd, long nr, const __u64 *a)
{
	int fd = (int) a[0], k = fd_kind(nd, fd);
	if (k == K_NONE || k == K_EFD || k == K_EPOLL) { g_resp->error = k == K_NONE ? -EBADF : -ENOTSOCK; return; }
	int cs = cfd_slot(nd, fd);
	const Conn *cn = cs >= 0 && nd->cconn[cs] >= 0 ? &conns[nd->cconn[cs]] : NULL;
	int port = 0;
	if (nr == SYS_getpeername)
	{
		if (!cn || cn->connecting || cn->refused) { g_resp->error = -ENOTCONN; return; }
		port = nd->cend[cs] == 0 ? BASE + cn->node[1] : cn->lport;
	}
	else if (cn)
		port = nd->cend[cs] == 0 ? cn->lport : BASE + nd->id;
	else
		die(3, "node %d: getsockname on a socket not connected is not modelled", nd->id);
	struct sockaddr_in sa;
	int len;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short) port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (cr_read(nd->pid, a[2], &len, sizeof len) != sizeof len) { g_resp->error = -EFAULT; return; }
	if (len < 0) { g_resp->error = -EINVAL; return; }
	int n = len < (int) sizeof sa ? len : (int) sizeof sa;
	len = sizeof sa;
	if (cr_write(nd->pid, a[1], &sa, (size_t) n) != (size_t) n ||
		cr_write(nd->pid, a[2], &len, sizeof len) != sizeof len)
		cr_fail(nd->id, "a socket name");
	g_resp->val = 0;
}

/* An iovec array of a node, read into the checker: cnt entries at
 * addr, as readv, writev, sendmsg and recvmsg pass them.  Returns 0, or
 * Linux's -errno for the array itself: EINVAL for a count below 0 or
 * past IOV_MAX (1024), EFAULT for an array it cannot read.  A total past
 * the model's per-call limit ends the check, as a long write does. */
#define MAXIOV 1024
static struct iovec g_iov[MAXIOV];
static int
read_iov(Node *nd, uint64_t addr, long cnt, int *total)
{
	if (cnt < 0 || cnt > MAXIOV) return -EINVAL;
	size_t want = (size_t) cnt * sizeof g_iov[0];
	if (cnt > 0 && cr_read(nd->pid, addr, g_iov, want) != want) return -EFAULT;
	long sum = 0;
	for (long i = 0; i < cnt; i++)
	{
		if ((ssize_t) g_iov[i].iov_len < 0) return -EINVAL;
		sum += (long) g_iov[i].iov_len;
		if (sum > M.max_write)
			die(1, "node %d writes or reads %ld bytes in one vector call; the limit is %d (raise [target] max_write, up to %d)", nd->id, sum, M.max_write, MSGLEN);
	}
	*total = (int) sum;
	return 0;
}

/* The bytes named by g_iov (n entries, total bytes), gathered into
 * buf.  Returns 0, -EFAULT when none can be read, as Linux; a buffer
 * that faults partway ends the check, as for write. */
static int
gather_iov(Node *nd, int n, int total, unsigned char *buf)
{
	int off = 0;
	for (int i = 0; i < n; i++)
	{
		size_t l = g_iov[i].iov_len;
		size_t got = cr_read(nd->pid, (uint64_t) (uintptr_t) g_iov[i].iov_base, buf + off, l);
		if (got < l && off == 0 && got == 0) return -EFAULT;
		if (got < l) cr_fail(nd->id, "a vector write sends from a buffer that ends");
		off += (int) l;
	}
	(void) total;
	return 0;
}

/* shutdown(fd, how) on owned socket fd, as Linux answers it on TCP.
 * SHUT_WR sends a FIN behind the bytes already sent, once; a later
 * write fails with EPIPE (and SIGPIPE).  SHUT_RD makes a read with
 * nothing queued return 0 at once, while bytes already queued or still
 * arriving stay readable; once both sides are shut, bytes arriving are
 * answered with an RST (see the write path).  A socket not connected,
 * refused or reset, and one that has sent its FIN and been sent the
 * peer's, is ENOTCONN.  A listener's shutdown would close it, which is
 * not modelled. */
static void
sock_shutdown(Node *nd, int fd, int how)
{
	int k = fd_kind(nd, fd);
	if (k == K_NONE || k == K_EFD || k == K_EPOLL) { g_resp->error = k == K_NONE ? -EBADF : -ENOTSOCK; return; }
	if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) { g_resp->error = -EINVAL; return; }
	if (k == K_LISTEN) die(3, "node %d: shutdown of a listening socket is not modelled", nd->id);
	int cs = cfd_slot(nd, fd);
	int c = cs >= 0 ? nd->cconn[cs] : -1, e = cs >= 0 ? nd->cend[cs] : 0;
	if (cs >= 0 && c < 0) die(3, "node %d: shutdown of a timer connection is not modelled", nd->id);
	Conn *cn = c >= 0 ? &conns[c] : NULL;
	if (cn && cn->connecting) die(3, "node %d: shutdown of a socket whose non-blocking connect has not completed is not modelled", nd->id);
	if (!cn || cn->refused || cn->reset || (cn->shut_wr[e] && fin_toward(c, e))) { g_resp->error = -ENOTCONN; return; }
	if (how != SHUT_WR) cn->shut_rd[e] = 1;
	if (how != SHUT_RD && !cn->shut_wr[e])
	{
		cn->shut_wr[e] = 1;
		if (!cn->closed[1 - e]) bag_add(c, 1 - e, NULL, 0, 1);
	}
	g_resp->val = 0;
}

/* fd, the number holding connection slot cs, is being closed.  If a
 * dup of it is still open, the lowest such number takes the slot over
 * (with its own FD_CLOEXEC, and the socket's options) and the rest now
 * name it, so the socket stays open.  Returns 1 if one did. */
static int
dup_takeover(Node *nd, int cs, int fd)
{
	int heir = 0;
	for (int f = 0; f < MAXOWNED; f++)
		if (nd->dupof[f] == fd) { heir = 500 + f; break; }
	if (!heir) return 0;
	for (int f = 0; f < MAXOWNED; f++)
		if (nd->dupof[f] == fd) nd->dupof[f] = 500 + f == heir ? 0 : heir;
	nd->cfd[cs] = heir;
	nd->ccloexec[cs] = (nd->fdflag[heir - 500] & FD_CX) != 0;
	nd->fdflag[heir - 500] = 0;
	nd->sockopt[heir - 500] = nd->sockopt[fd - 500]; nd->sockopt[fd - 500] = 0;
	if (nd->timer_fd == fd) nd->timer_fd = heir;
	return 1;
}

/* dup, dup2 and dup3 of owned fd a[0].  The new number shares the old
 * one's open socket, as on Linux: its connection, its queue and its
 * O_NONBLOCK; only FD_CLOEXEC is the number's own (clear, or set by
 * dup3's O_CLOEXEC).  A connection is the only kind modelled; a dup of
 * a listener, an eventfd, an epoll instance, a socket not yet dialled
 * or a timer connection ends the check, as does a target fd that is
 * the kernel's (below 500) or an owned fd already open, which dup2
 * would close first.  dup2 onto itself returns the fd; dup3 onto
 * itself is EINVAL. */
static void
sock_dup(Node *nd, long nr, const __u64 *a)
{
	int fd = (int) a[0], to = nr == SYS_dup ? nd->next_fd : (int) a[1];
	int k = fd_kind(nd, fd);
	if (k == K_NONE) { g_resp->error = -EBADF; return; }
	if (nr == SYS_dup3 && (a[2] & ~(uint64_t) O_CLOEXEC)) { g_resp->error = -EINVAL; return; }
	if (to == fd) { if (nr == SYS_dup3) g_resp->error = -EINVAL; else g_resp->val = fd; return; }
	int cs = cfd_slot(nd, fd);
	if (k != K_CONN || nd->cconn[cs] < 0)
		die(3, "node %d: dup of owned fd %d, which is not a connection, is not modelled", nd->id, fd);
	if (to < 500 || to < nd->next_fd)
		die(3, "node %d: dup onto fd %d, which is open or the kernel's, is not modelled", nd->id, to);
	(void) fd_flags(nd, to);
	nd->next_fd = to + 1;
	nd->dupof[to - 500] = nd->cfd[cs];
	nd->fdflag[to - 500] = nr == SYS_dup3 && (a[2] & O_CLOEXEC) ? FD_CX : 0;
	g_resp->val = to;
}

/* nanosleep, clock_nanosleep, and select or pselect6 with no descriptor:
 * a pure wait.  A zero wait (or an absolute one already past) returns at
 * once; any other parks the thread as BK_SLEEP with a deadline on its
 * node's clock, which A_EXPIRE fires (sleep_expire()).  Every clock id a
 * node can name reads the one node clock, so sleeping on any of them is
 * the same wait.  select or pselect6 on descriptors is refused: poll is
 * the modelled readiness call.  Returns 1 if the thread parked, and 0 if
 * g_resp holds the answer. */
static int
sleep_call(Node *nd, Thread *th, long nr, const __u64 *a)
{
	unsigned long long ns = 0; int ok;
	if (nr == SYS_select || nr == SYS_pselect6)
	{
		if ((int) a[0] > 0 && (a[1] || a[2] || a[3]))
			die(3, "node %d: select or pselect6 on descriptors is not modelled (use poll)", nd->id);
		if (!a[4]) die(3, "node %d: select or pselect6 with no descriptor and no timeout (a wait forever) is not modelled", nd->id);
		if (nr == SYS_pselect6) ok = read_timespec(nd, a[4], &ns);
		else
		{
			/* select's timeout is a struct timeval, which Linux counts down */
			struct { long long sec, usec; } tv;
			ok = cr_read(nd->pid, a[4], &tv, sizeof tv) == sizeof tv ? 1 : -1;
			if (ok > 0 && (tv.sec < 0 || tv.usec < 0 || tv.usec >= 1000000LL)) ok = 0;
			if (ok > 0) ns = (unsigned long long) tv.sec * 1000000000ULL + (unsigned long long) tv.usec * 1000ULL;
		}
	}
	else
	{
		uint64_t req = nr == SYS_nanosleep ? a[0] : a[2];
		if (nr == SYS_clock_nanosleep && a[0] != CLOCK_REALTIME && a[0] != CLOCK_MONOTONIC && a[0] != CLOCK_BOOTTIME)
			die(3, "node %d: clock_nanosleep on clock %d is not modelled (only REALTIME, MONOTONIC and BOOTTIME)", nd->id, (int) a[0]);
		ok = read_timespec(nd, req, &ns);
		/* TIMER_ABSTIME: ns is a reading of the node clock to wait for */
		if (ok > 0 && nr == SYS_clock_nanosleep && (a[1] & TIMER_ABSTIME))
			ns = ns > g_clk[nd->id] ? ns - g_clk[nd->id] : 0;
	}
	if (ok < 0) { g_resp->error = -EFAULT; return 0; }
	if (ok == 0) { g_resp->error = -EINVAL; return 0; }
	if (ns == 0) { g_resp->val = 0; return 0; }
	th->block_kind = BK_SLEEP; th->block_id = g_req->id;
	/* select and pselect6 count their timeout down, to zero on expiry */
	th->block_bufaddr = nr == SYS_select || nr == SYS_pselect6 ? a[4] : 0;
	set_deadline(nd, th, 1, ns);
	return 1;
}

/* Carry out notification g_req of thread th on node nd.  Returns 1 if the
 * call parked (a deferred recv, accept, poll or futex wait) or ended the
 * node, and 0 if g_resp holds its answer. */
static int
serve(Node *nd, Thread *th)
{
	long nr = g_req->data.nr; __u64 *a = g_req->data.args;

	if (mfs_call(nd, nr, a)) return 0;

	switch (nr)
	{
		case SYS_socket:
		{
			/* a TCP socket over IPv4, the only kind the model has.  A UDP
			 * or Unix socket would be answered as TCP, so it is refused, as
			 * is a type flag other than SOCK_CLOEXEC (SOCK_NONBLOCK is not
			 * tracked from socket(); set it with fcntl()). */
			int dom = (int) a[0], type = (int) a[1];
			if (dom != AF_INET || (type & ~SOCK_CLOEXEC) != SOCK_STREAM)
				die(3, "node %d: socket(%d, %#x) is not modelled (only AF_INET, SOCK_STREAM)", nd->id, dom, type);
			int fd = nd->next_fd++;
			*fd_flags(nd, fd) = FD_PLAIN | ((type & SOCK_CLOEXEC) ? FD_CX : 0);
			g_resp->val = fd; break;
		}
		case SYS_preadv: case SYS_pwritev:
		case SYS_pread64: case SYS_pwrite64:
		case SYS_dup: case SYS_dup2: case SYS_dup3:
		{
			int fd = (int) a[0];
			if (fd >= 500 && fd < nd->next_fd) { sock_dup(nd, nr, a); break; }
			refuse_on_owned(nd, nr, a);
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		case SYS_sendmmsg: case SYS_recvmmsg:
		case SYS_ioctl: case SYS_splice: case SYS_sendfile:
			refuse_on_owned(nd, nr, a);
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		case SYS_shutdown:
			if ((int) a[0] < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
			sock_shutdown(nd, (int) a[0], (int) a[1]);
			break;
		case SYS_getsockname: case SYS_getpeername:
			if ((int) a[0] < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
			sock_name(nd, nr, a);
			break;
		case SYS_clock_gettime:
			if ((long) a[0] == CMC_PRELOAD_MAGIC) { nd->preload_ok = 1; g_resp->val = 0; break; }
		{
			/* answer with the logical clock (a[1] = struct timespec*) */
			struct { long long sec, nsec; } ts;
			ts.sec = g_clk[nd->id] / 1000000000ULL; ts.nsec = g_clk[nd->id] % 1000000000ULL;
			if (cr_write(nd->pid, a[1], &ts, sizeof ts) != sizeof ts) { g_resp->error = -EFAULT; break; }
			g_resp->val = 0; break;
		}
		case SYS_gettimeofday:
		{
			/* gettimeofday(tv, tz): the buffer is the FIRST argument */
			struct { long long sec, usec; } tv;
			tv.sec = g_clk[nd->id] / 1000000000ULL; tv.usec = (g_clk[nd->id] % 1000000000ULL) / 1000;
			/* a NULL tv is allowed and writes nothing */
			if (a[0] && cr_write(nd->pid, a[0], &tv, sizeof tv) != sizeof tv) { g_resp->error = -EFAULT; break; }
			g_resp->val = 0; break;
		}
		case SYS_sched_yield:
			/* the preload's synchronisation marker (a[0] is CMC_SYNC_MAGIC),
			 * the race runtime's (CMC_RACE_MAGIC), or a plain yield.  In a
			 * node with more than one thread it is visible, so its held
			 * answer makes it a choice point; in a node with one it returns
			 * at once.  The kernel never sees it. */
			if ((long) a[0] == CMC_RACE_MAGIC)
				switch ((int) a[1])
				{
					case RACE_HELLO: nd->race_ok = 1; break;
					case RACE_WEAK: g_weak_seen = 1; break;
					case RACE_TRY: g_try_seen = 1; break;
					case RACE_REPORT:
						if (g_race_node < 0)
						{
							g_race_node = nd->id; g_race_pc[0] = a[2]; g_race_pc[1] = a[3]; g_race_addr = a[4];
						}
						break;
					case RACE_FULL:
						die(3, "node %d: the race runtime ran out of room (table %d); a node may have at most %d threads", nd->id, (int) a[2], MAXTHREAD);
					default: break;
				}
			g_resp->val = 0; break;
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
		case SYS_bind:
		{
			int err = wrong_kind(nd, nr, (int) a[0]);
			if (err) { g_resp->error = -err; break; }
			g_resp->val = 0; break;
		}
		case SYS_setsockopt: case SYS_getsockopt:
		{
			if ((int) a[0] < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
			int err = wrong_kind(nd, nr, (int) a[0]);
			if (err) { g_resp->error = -err; break; }
			sock_option(nd, nr, a);
			break;
		}
		case SYS_listen:
		{
			/* the socket's flags, set while it was plain, are the
			 * listener's from now on */
			int fd = (int) a[0];
			int err = wrong_kind(nd, nr, fd);
			if (err) { g_resp->error = -err; break; }
			if (fd == nd->listener_fd) { g_resp->val = 0; break; }	/* a second listen() */
			unsigned char *f = fd_flags(nd, fd);
			nd->listener_fd = fd;
			nd->ls_nonblock = (*f & FD_NB) != 0; nd->ls_cloexec = (*f & FD_CX) != 0;
			*f = 0;
			g_resp->val = 0; break;
		}
		case SYS_close:
		{
			/* a checker-owned fd: close this end of its connection (a FIN,
			 * or an RST if bytes were left unread) and forget the fd, so the
			 * table and the fingerprint hold only live ones */
			int fd = (int) a[0];
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
			int cs = cfd_slot(nd, fd);
			if (fd - 500 < MAXOWNED && nd->dupof[fd - 500])
			{
				/* one of two numbers for a socket: the socket stays */
				nd->dupof[fd - 500] = 0; nd->fdflag[fd - 500] = 0;
				g_resp->val = 0; break;
			}
			if (cs >= 0 && dup_takeover(nd, cs, fd)) { g_resp->val = 0; break; }
			if (fd - 500 < MAXOWNED) nd->sockopt[fd - 500] = 0;
			if (cs >= 0)
			{
				int c = nd->cconn[cs], e = nd->cend[cs];
				if (fd == nd->timer_fd) nd->timer_fd = -1;
				cfd_del(nd, cs);
				if (c >= 0) end_close(c, e, 0);
			}
			else if (fd < nd->next_fd && fd - 500 < MAXOWNED && (nd->fdflag[fd - 500] & FD_PLAIN))
				nd->fdflag[fd - 500] = 0;	/* a plain socket closed is gone */
			g_resp->val = 0; break;
		}
		case SYS_connect:
		{
			{
				int err = wrong_kind(nd, nr, (int) a[0]);
				if (err) { g_resp->error = -err; break; }
			}
			/* read the dest port to learn the peer id */
			struct { unsigned short fam; unsigned short port; } sa;
			if (cr_read(nd->pid, a[1], &sa, sizeof sa) != sizeof sa) { g_resp->error = -EFAULT; break; }
			int port = (sa.port >> 8) | ((sa.port & 0xff) << 8);
			int peer = port - BASE;
			if (peer < 0 || peer >= M.nodes)
				die(3, "node %d connects to port %d, which is no node's (node i listens on %d+i)", nd->id, port, BASE);
			/* Nothing listens on a dead node's port, so Linux refuses the
			 * connection.  A live node's listener takes it: the connection
			 * waits there until the search lets the peer accept it.
			 * shortcut: a live node that has not yet called listen() is
			 * treated as listening, so a startup race that Linux would
			 * refuse waits instead; upgrade if a target retries connects. */
			/* A socket already dialled.  One whose handshake is still out
			 * is EALREADY.  One a non-blocking connect left (nbdial) gets
			 * the outcome, as Linux reports it: 0 once connected; when
			 * refused, the error not yet read, else ECONNABORTED; either
			 * way it is then a plain refused socket.  A refused one may be
			 * dialled again (its slot and flags are reused), and a
			 * connected one is EISCONN. */
			int fd = (int) a[0], cs = cfd_slot(nd, fd);
			unsigned char plain = cs >= 0 ? 0 : *fd_flags(nd, fd);
			int nb = cs >= 0 ? nd->cnonblock[cs] : (plain & FD_NB) != 0;
			int cx = cs >= 0 ? nd->ccloexec[cs] : (plain & FD_CX) != 0;
			if (cs >= 0)
			{
				int old = nd->cconn[cs];
				Conn *oc = old >= 0 ? &conns[old] : NULL;
				if (oc && oc->connecting && !nb)
					die(3, "node %d: a blocking connect on a socket whose non-blocking connect has not completed is not modelled", nd->id);
				if (oc && oc->connecting) { g_resp->error = -EALREADY; break; }
				if (oc && oc->nbdial)
				{
					oc->nbdial = 0;
					if (!oc->refused) { g_resp->val = 0; break; }
					int err = oc->err[0] ? oc->err[0] : ECONNABORTED;
					oc->err[0] = 0;
					g_resp->error = -err; break;
				}
				if (!oc || !oc->refused) { g_resp->error = -EISCONN; break; }
				oc->closed[0] = 1; conn_maybe_free(old);
				cfd_del(nd, cs);
			}
			if (nb && node_multi(nd))
				die(3, "node %d: a non-blocking connect in a node with more than one thread is not modelled", nd->id);
			if (nb)
			{
				/* Linux answers EINPROGRESS whether or not anything
				 * listens; the handshake completes later, into the peer's
				 * backlog or refused, when the search says (A_CONNECT).
				 * Until then the acceptor end does not exist. */
				int c = conn_new(nd->id, peer);
				conns[c].connecting = conns[c].nbdial = 1; conns[c].closed[1] = 1;
				conns[c].lport = local_port(nd, fd);
				cfd_set(nd, fd, peer, c, 0);
				nd->cnonblock[nd->nc - 1] = 1; nd->ccloexec[nd->nc - 1] = cx;
				if (plain) *fd_flags(nd, fd) = 0;
				g_resp->error = -EINPROGRESS; break;
			}
			/* Across a cut link the SYN or its answer is lost, and Linux
			 * retransmits: the connect waits (BK_SYNWAIT) until the link
			 * heals, when the search may RUN it again, or until it times
			 * out (EXPIRE, ETIMEDOUT).  A node with more than one thread
			 * is not offered the wait. */
			if (!link_up(nd->id, peer) || !link_up(peer, nd->id))
			{
				if (node_multi(nd))
					die(3, "node %d dials node %d across a cut link; a blocking connect that waits in a node with more than one thread is not modelled", nd->id, peer);
				th->block_kind = BK_SYNWAIT; th->block_id = g_req->id; th->block_peer = peer;
				set_deadline(nd, th, 1, SYN_TIMEOUT);
				return 1;
			}
			if (!N[peer].alive)
			{
				/* the socket stays, unconnected: a read fails with
				 * ENOTCONN and a write with EPIPE, as on Linux after a
				 * refusal, until it is dialled again */
				int c = conn_new(nd->id, peer);
				rst_path(peer, nd->id);
				conns[c].closed[1] = conns[c].gone[1] = 1; conns[c].refused = 1;
				conns[c].lport = local_port(nd, fd);
				cfd_set(nd, fd, peer, c, 0);
				nd->ccloexec[nd->nc - 1] = cx;
				if (plain) *fd_flags(nd, fd) = 0;
				g_resp->error = -ECONNREFUSED; break;
			}
			if (N[peer].npend >= MAXCONN) die(1, "more than %d connections wait at node %d", MAXCONN, peer);
			int c = conn_new(nd->id, peer);
			N[peer].pend[N[peer].npend++] = c;
			/* the node sends/recvs on the socket() fd (connect's a[0]);
			 * connect() itself returns 0 */
			conns[c].lport = local_port(nd, fd);
			cfd_set(nd, fd, peer, c, 0);
			nd->ccloexec[nd->nc - 1] = cx;
			if (plain) *fd_flags(nd, fd) = 0;
			g_resp->val = 0; break;
		}
		case SYS_accept: case SYS_accept4:
			/* accept4's flags: SOCK_CLOEXEC sets the new fd's FD_CLOEXEC
			 * (accept_from), and SOCK_NONBLOCK is not carried onto the new
			 * fd, so it is refused rather than ignored */
			if (nr == SYS_accept4 && (a[3] & ~(uint64_t) SOCK_CLOEXEC))
				die(3, "node %d: accept4 flags %#llx are not modelled (set O_NONBLOCK with fcntl())", nd->id, (unsigned long long) a[3]);
			{
				int err = wrong_kind(nd, nr, (int) a[0]);
				if (err) { g_resp->error = -err; g_resp->val = 0; break; }
			}
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
		case SYS_write: case SYS_sendto: case SYS_writev: case SYS_sendmsg:
		{
			int fd = (int) a[0];
			if (nr == SYS_write && (fd == 3 || fd == DURABLE_FD)) { serve_hook(nd, a); break; }
			if (nr == SYS_writev && (fd == 3 || fd == DURABLE_FD))
			{
				/* a vector record write: one record, gathered, so a
				 * durable one is a crash point like write() */
				unsigned char hbuf[MSGLEN]; int hlen = 0;
				int err = read_iov(nd, a[1], (long) a[2], &hlen);
				if (!err) err = gather_iov(nd, (int) a[2], hlen, hbuf);
				if (err) { g_resp->error = err; break; }
				hook_put(nd, fd, hbuf, (size_t) hlen);
				break;
			}
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; } /* not ours (stdout, libs) */
			/* writev and sendmsg gather their iovec array into vbuf and
			 * go the way of write and sendto; sendmsg's flags are
			 * sendto's, and a destination or ancillary data is refused */
			int vec = nr == SYS_writev || nr == SYS_sendmsg, vlen = 0;
			uint64_t wflags = nr == SYS_sendto ? a[3] : nr == SYS_sendmsg ? a[2] : 0;
			unsigned char vbuf[MSGLEN];
			if (vec)
			{
				struct msghdr mh;
				uint64_t iov = a[1]; long cnt = (long) a[2];
				if (nr == SYS_sendmsg)
				{
					if (cr_read(nd->pid, a[1], &mh, sizeof mh) != sizeof mh) { g_resp->error = -EFAULT; break; }
					if (mh.msg_name || mh.msg_control || mh.msg_controllen)
						die(3, "node %d: sendmsg with a destination address or ancillary data is not modelled", nd->id);
					iov = (uint64_t) (uintptr_t) mh.msg_iov; cnt = (long) mh.msg_iovlen;
				}
				if (fd_kind(nd, fd) == K_EFD) die(3, "node %d: a vector write to an eventfd is not modelled", nd->id);
				int err = read_iov(nd, iov, cnt, &vlen);
				if (!err) err = gather_iov(nd, (int) cnt, vlen, vbuf);
				if (err) { g_resp->error = err; break; }
			}
			/* sendto's flags: MSG_NOSIGNAL is honoured; any other would
			 * change the call (MSG_DONTWAIT, MSG_MORE, MSG_OOB) and is
			 * refused, as is a destination address on a connected socket */
			if (nr == SYS_sendto || nr == SYS_sendmsg)
			{
				int err = wrong_kind(nd, nr, fd);
				if (err) { g_resp->error = -err; break; }
			}
			if ((wflags & ~(uint64_t) MSG_NOSIGNAL) || (nr == SYS_sendto && a[4]))
				die(3, "node %d: sendto flags %#llx or a destination address are not modelled", nd->id, (unsigned long long) wflags);
			int nosig = (wflags & MSG_NOSIGNAL) != 0;
			if (fd_kind(nd, fd) == K_PLAIN)
			{
				/* a socket never connected: EPIPE, and the SIGPIPE its
				 * disposition decides unless MSG_NOSIGNAL */
				if (!nosig && sigpipe_kills(nd, th)) return 1;
				g_resp->error = -EPIPE; break;
			}
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
			int cs = cfd_slot(nd, fd);
			if (cs < 0) { g_resp->error = fd_kind(nd, fd) == K_EPOLL ? -EINVAL : -EBADF; break; }
			int peer = nd->cpeer[cs], c = nd->cconn[cs], e = nd->cend[cs];
			int len = vec ? vlen : (int) a[2];
			if (len < 0 || len > M.max_write) die(1, "node %d writes %d bytes in one call; the limit is %d (raise [target] max_write, up to %d)", nd->id, len, M.max_write, MSGLEN);
			if (c < 0) { g_resp->val = len; break; }	/* a timer fd has no reader */
			Conn *cn = &conns[c];
			if (cn->connecting || (cn->full[e] && !cn->err[e] && !cn->reset))
			{
				if (!nd->cnonblock[cs]) die(3, "node %d: a blocking write to a socket whose non-blocking connect has not completed is not modelled", nd->id);
				g_resp->error = -EAGAIN; break;
			}
			/* a pending reset is reported first; EPIPE, from a reset, a
			 * refused dial, or a reported reset, raises SIGPIPE unless the
			 * caller asked for MSG_NOSIGNAL */
			int werr = cn->err[e] ? cn->err[e] : (cn->reset || cn->refused || cn->shut_wr[e]) ? EPIPE : 0;
			if (werr)
			{
				cn->err[e] = 0;
				if (werr == EPIPE && !nosig && sigpipe_kills(nd, th)) return 1;
				g_resp->error = -werr; break;
			}
			if (cn->closed[1 - e])
			{
				/* The peer's descriptor is gone.  If its host died nothing
				 * answers and the bytes are lost; otherwise its stack answers
				 * the data with an RST, and this end, which had its FIN, gets
				 * EPIPE on its next call.  shortcut: the RST arrives at once,
				 * not a round trip later. */
				if (!cn->gone[1 - e]) { rst_path(nd->id, cn->node[1 - e]); rst_path(cn->node[1 - e], nd->id); cn->reset = 1; cn->err[e] = reset_errno(c, e); }
				g_resp->val = len; break;
			}
			unsigned char buf[MSGLEN] = {0};
			if (vec) memcpy(buf, vbuf, (size_t) len);
			size_t got = vec ? (size_t) len : cr_read(nd->pid, a[1], buf, (size_t) len);
			/* nothing readable is the kernel's EFAULT with nothing sent; a
			 * buffer that faults partway would send a prefix, which the
			 * model does not do */
			if (got == 0 && len > 0) { g_resp->error = -EFAULT; break; }
			if (got < (size_t) len) cr_fail(nd->id, "write sends from a buffer that ends");
			/* a SHORT action: the send buffer fills after g_short bytes
			 * (none: EAGAIN), and stays full until the peer reads */
			if (g_short >= 0)
			{
				cn->full[e] = 1;
				if (g_short == 0) { g_resp->error = -EAGAIN; break; }
				len = g_short;
			}
			if (peer == -1)			/* reply to the client: ghost log */
			{
				if (len >= 2 && buf[0] == 'A')
				{
					if (nrep >= MAXREP) die(1, "more than %d client replies", MAXREP);
					rep_src[nrep] = (unsigned char) nd->id; rep_val[nrep] = buf[1]; nrep++;
				}
			}
			else if (len > 0 && cn->shut_rd[1 - e] && cn->shut_wr[1 - e])
			{
				/* the peer shut both sides: its stack answers the bytes
				 * with an RST, as Linux's does in FIN_WAIT */
				rst_path(nd->id, cn->node[1 - e]); rst_path(cn->node[1 - e], nd->id);
				int er[2] = {reset_errno(c, 0), reset_errno(c, 1)};
				conn_discard(c);
				cn->reset = 1; cn->err[0] = er[0]; cn->err[1] = er[1];
				rst_notify(c, 1 - e);
			}
			else if (len > 0)		/* inter-node: buffer in the bag */
				bag_add(c, 1 - e, buf, len, 0);
			g_resp->val = len; break;
		}
		case SYS_read: case SYS_recvfrom: case SYS_readv: case SYS_recvmsg:
		{
			int fd = (int) a[0];
			int rvec = nr == SYS_readv || nr == SYS_recvmsg;
			/* the bytes a read may take: a[2], or the iovec array's total */
			int rwant = (int) a[2];
			th->niov = 0; th->vecread = 0; th->block_msg = 0; th->block_bufaddr = a[1];
			if (rvec && fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; }
			if (rvec)
			{
				/* readv and recvmsg take the read path with their iovec
				 * array recorded on the thread; recvmsg's flags are
				 * recvfrom's (none), and ancillary data is refused */
				uint64_t iov = a[1]; long cnt = (long) a[2];
				if (nr == SYS_recvmsg)
				{
					struct msghdr mh;
					int err = wrong_kind(nd, nr, fd);
					if (err) { g_resp->error = -err; break; }
					if (cr_read(nd->pid, a[1], &mh, sizeof mh) != sizeof mh) { g_resp->error = -EFAULT; break; }
					if (mh.msg_control || mh.msg_controllen || a[2])
						die(3, "node %d: recvmsg flags %#llx or ancillary data are not modelled", nd->id, (unsigned long long) a[2]);
					iov = (uint64_t) (uintptr_t) mh.msg_iov; cnt = (long) mh.msg_iovlen;
					th->block_msg = a[1];
				}
				if (fd_kind(nd, fd) == K_EFD) die(3, "node %d: a vector read of an eventfd is not modelled", nd->id);
				int err = read_iov(nd, iov, cnt, &rwant);
				if (err) { g_resp->error = err; th->block_msg = 0; break; }
				if (cnt > MAXRIOV) die(3, "node %d reads into %ld iovec entries; the limit is %d", nd->id, cnt, MAXRIOV);
				for (long i = 0; i < cnt; i++)
				{ th->iovb[i] = (uint64_t) (uintptr_t) g_iov[i].iov_base; th->iovl[i] = g_iov[i].iov_len; }
				th->niov = (int) cnt; th->vecread = 1;
			}
			/* recvfrom's flags change the call (MSG_PEEK keeps the bytes,
			 * MSG_WAITALL waits for all, MSG_DONTWAIT does not block), so
			 * any is refused; a source address out-parameter is not filled */
			if (nr == SYS_recvfrom && fd >= 500)
			{
				int err = wrong_kind(nd, nr, fd);
				if (err) { g_resp->error = -err; break; }
			}
			if (fd >= 500 && fd_kind(nd, fd) == K_PLAIN) { g_resp->error = -ENOTCONN; break; }
			if (nr == SYS_recvfrom && fd >= 500 && (a[3] || a[4]))
				die(3, "node %d: recvfrom flags %#llx or a source address are not modelled", nd->id, (unsigned long long) a[3]);
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
			int rs = cfd_slot(nd, fd);
			/* an epoll instance cannot be read (EINVAL), and a closed fd is
			 * EBADF, as on Linux; a plain socket was answered above */
			if (rs < 0) { g_resp->error = fd_kind(nd, fd) == K_EPOLL ? -EINVAL : -EBADF; break; }
			int peer = nd->cpeer[rs], c = nd->cconn[rs], e = nd->cend[rs];
			/* a read of no bytes returns 0 at once, as on Linux */
			if (rwant == 0 && c >= 0 && !conns[c].connecting) { g_resp->val = 0; break; }
			if (fd != nd->timer_fd && c >= 0 && conn_head(c, e) < 0)
			{
				/* nothing queued: an unconnected socket reads ENOTCONN, a
				 * pending ECONNRESET is reported once, and after it, or after
				 * the FIN was read, every read returns 0 at once, blocking or
				 * not, as Linux does.  No search choice remains, so answer
				 * immediately. */
				Conn *cn = &conns[c];
				/* a dial still handshaking: EAGAIN.  A refused one reports
				 * its error once; a socket a non-blocking connect left
				 * refused then reads 0, one refused otherwise ENOTCONN, as
				 * Linux does */
				if (cn->connecting)
				{
					if (!nd->cnonblock[rs]) die(3, "node %d: a blocking read of a socket whose non-blocking connect has not completed is not modelled", nd->id);
					g_resp->error = -EAGAIN; break;
				}
				if (cn->refused && cn->err[e]) { g_resp->error = -cn->err[e]; cn->err[e] = 0; break; }
				if (cn->refused && cn->nbdial) { g_resp->val = 0; break; }
				if (cn->refused) { th->poll_woke_fd = -1; g_resp->error = -ENOTCONN; break; }
				if (cn->err[e] == ECONNRESET) { cn->err[e] = 0; th->poll_woke_fd = -1; g_resp->error = -ECONNRESET; break; }
				if (cn->eof[e] || cn->reset || cn->shut_rd[e]) { th->poll_woke_fd = -1; g_resp->val = 0; break; }
			}
			if (fd == nd->timer_fd)
			{
				/* the first recv on a timer connection returns the fired
				 * timeout's one byte 'T', then the fd is an ordinary
				 * (empty, timer-peer) channel */
				static const unsigned char t = 'T';
				int k = rwant < 1 ? rwant : 1;
				nd->timer_fd = -1; th->poll_woke_fd = -1;
				put_read(nd, th, &t, k);
				g_resp->val = k; break;
			}
			/* A non-blocking channel read with nothing deliverable returns
			 * EAGAIN immediately: the search has not just woken this fd, so
			 * no message is ready.  This is sound only because the loop's
			 * next poll() defers (BK_POLL), handing control back to the
			 * search; EAGAIN never lets a node bypass delivery. */
			int cs = cfd_slot(nd, fd);
			if (cs >= 0 && nd->cnonblock[cs] && (th->poll_woke_fd < 0 || cfd_slot(nd, th->poll_woke_fd) != cs))
			{ g_resp->error = -EAGAIN; g_resp->val = 0; break; }
			th->poll_woke_fd = -1;	/* the wake is spent; this read defers */
			/* Always DEFER a blocking channel recv: whether it is satisfied
			 * by a message or by a timeout is an environment choice the
			 * search makes, so delivery must never be automatic (M5).  The
			 * node blocks in the kernel until the search delivers to it. */
			th->block_kind = BK_RECV; th->block_id = g_req->id; th->block_bufaddr = a[1];
			th->block_want = rwant; th->block_peer = peer;
			th->block_conn = c; th->block_end = e;
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
			/* each kind of fd keeps its own flags: a connection's in its
			 * slot, the listener's on the node, an eventfd's O_NONBLOCK with
			 * its counter, and the rest (FD_CLOEXEC of an eventfd, a plain
			 * socket's and an epoll instance's flags) in fdflag */
			int cs = cfd_slot(nd, fd), es = efd_slot(nd, fd);
			int nb, cx;
			int alias = fd - 500 < MAXOWNED && nd->dupof[fd - 500];
			if (cs >= 0) { nb = nd->cnonblock[cs]; cx = alias ? (nd->fdflag[fd - 500] & FD_CX) != 0 : nd->ccloexec[cs]; }
			else if (fd == nd->listener_fd) { nb = nd->ls_nonblock; cx = nd->ls_cloexec; }
			else
			{
				unsigned char f = *fd_flags(nd, fd);
				nb = es >= 0 ? nd->efdnb[es] : (f & FD_NB) != 0;
				cx = (f & FD_CX) != 0;
			}
			if (cmd == F_GETFL) { g_resp->val = nb ? O_NONBLOCK : 0; break; }
			if (cmd == F_GETFD) { g_resp->val = cx ? FD_CLOEXEC : 0; break; }
			if (cmd == F_SETFL) nb = (a[2] & O_NONBLOCK) != 0;
			else if (cmd == F_SETFD) cx = (a[2] & FD_CLOEXEC) != 0;
			else die(1, "node %d: fcntl cmd %d on virtual fd %d has no model", nd->id, cmd, fd);
			if (cs >= 0 && alias) { nd->cnonblock[cs] = nb; nd->fdflag[fd - 500] = cx ? FD_CX : 0; }
			else if (cs >= 0) { nd->cnonblock[cs] = nb; nd->ccloexec[cs] = cx; }
			else if (fd == nd->listener_fd) { nd->ls_nonblock = nb; nd->ls_cloexec = cx; }
			else
			{
				unsigned char *f = fd_flags(nd, fd);
				if (es >= 0) nd->efdnb[es] = nb;
				*f = (unsigned char) ((*f & FD_PLAIN) | (nb && es < 0 ? FD_NB : 0) | (cx ? FD_CX : 0));
			}
			g_resp->val = 0; break;
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
			*fd_flags(nd, fd) = (a[1] & EFD_CLOEXEC) ? FD_CX : 0;
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
			*fd_flags(nd, fd) = (nr == SYS_epoll_create1 && (a[0] & EPOLL_CLOEXEC)) ? FD_CX : 0;
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
			if (fs_isfd(&nd->fs, fd)) { g_resp->error = -EPERM; break; }	/* Linux: a regular file */
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
			 * a negative one blocks, and a positive one is a deadline on the
			 * node's clock (A_EXPIRE). */
			int zero_timeout = (a[3] == 0);
			long long tmo_ms = (int) a[3];
			struct __attribute__((packed)) { uint32_t events; uint64_t data; } out[MAXPOLL];
			int ready = 0;
			for (int i = 0; i < e->nit && ready < maxevents; i++)
			{
				/* poll's and epoll's bits agree (EPOLLIN is POLLIN, and so
				 * on); a watched fd since closed is not reported */
				int fd = e->fd[i];
				short rev = fd >= 500 && fd_kind(nd, fd) == K_NONE ? 0 : local_ready(nd, fd, e->events[i]);
				if (rev) { out[ready].events = (uint16_t) rev; out[ready].data = e->data[i]; ready++; }
			}
			if (ready > 0 || zero_timeout)
			{
				size_t want = (size_t) ready * sizeof out[0];
				size_t got = ready > 0 ? cr_write(nd->pid, arr, out, want) : 0;
				/* nothing written: EFAULT, as on Linux (level-triggered, so
				 * nothing is consumed); written in part is not modelled */
				if (ready > 0 && got == 0) { g_resp->error = -EFAULT; break; }
				if (got < want) cr_fail(nd->id, "epoll_wait returns events");
				g_resp->val = ready; break;
			}
			/* nothing locally ready and the call blocks: a quiescence point,
			 * recorded in the poll slot, but flagged as an epoll_wait so the
			 * wake writes a struct epoll_event (with the stored data) back. */
			th->block_kind = BK_POLL; th->block_id = g_req->id;
			th->block_pollarr = arr; th->pollfd_n = e->nit;
			th->epoll_fd = epfd; th->poll_maxevents = maxevents;
			set_deadline(nd, th, tmo_ms > 0, (unsigned long long) tmo_ms * 1000000ULL);
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
			 * timeout returns at once, and a finite positive one is a
			 * deadline on the node's clock (A_EXPIRE). */
			int zero_timeout = 0, finite = 0;
			unsigned long long tmo_ns = 0;
			if (nr == SYS_poll)
			{
				zero_timeout = (a[2] == 0);
				finite = ((int) a[2] > 0);
				if (finite) tmo_ns = (unsigned long long) (int) a[2] * 1000000ULL;
			}
			else if (a[2] != 0)
			{
				int t = read_timespec(nd, a[2], &tmo_ns);
				if (t < 0) { g_resp->error = -EFAULT; break; }
				if (t == 0) { g_resp->error = -EINVAL; break; }
				zero_timeout = (tmo_ns == 0); finite = !zero_timeout;
			}
			int ready = 0;
			for (int i = 0; i < nfds; i++)
			{
				pfs[i].revents = 0;
				int fd = pfs[i].fd;
				if (fd < 0) continue;
				if (fs_isfd(&nd->fs, fd))
					die(3, "node %d polls modelled file fd %d, which is not modelled", nd->id, fd);
				/* a closed owned fd is ready at once with POLLNVAL, as on Linux */
				pfs[i].revents = local_ready(nd, fd, pfs[i].events);
				if (pfs[i].revents) ready++;
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
			set_deadline(nd, th, finite, tmo_ns);
			for (int i = 0; i < nfds; i++) { th->pollfds[i] = pfs[i].fd; th->pollevents[i] = pfs[i].events; }
			return 1;				/* do NOT answer */
		}
		case SYS_setitimer: case SYS_alarm: case SYS_timer_create: case SYS_timerfd_create:
			/* timers the kernel would run on the real clock: a signal or a
			 * readable fd at a real-time moment no replay reproduces */
			die(3, "node %d: system call %ld arms a real-time timer (setitimer, alarm, timer_create, timerfd), which is not modelled", nd->id, nr);
			return 1;
		case SYS_nanosleep: case SYS_clock_nanosleep: case SYS_select: case SYS_pselect6:
			if (sleep_call(nd, th, nr, a)) return 1;	/* parked: do NOT answer */
			break;
		case SYS_process_vm_readv: case SYS_process_vm_writev:
		case SYS_shmget: case SYS_shmat: case SYS_msgget: case SYS_semget: case SYS_mq_open:
			/* another process's memory, SysV shared memory, message queues
			 * and semaphores, and POSIX message queues: channels between
			 * nodes the model does not have, and the SysV objects outlive
			 * the process, so their state would survive a crash and every
			 * replay outside the world */
			die(3, "node %d: system call %ld shares state between processes outside the model (shared memory, a message queue, another process's memory)", nd->id, nr);
			return 1;
		case SYS_kill: case SYS_tkill: case SYS_tgkill:
		case SYS_rt_sigqueueinfo: case SYS_rt_tgsigqueueinfo:
		{
			/* a signal to the node itself (raise(), abort()) goes to the
			 * kernel, and its stop is handled as a fault or refused there;
			 * to any other process (another node, the checker, a process
			 * group) it is a channel the model does not have */
			pid_t tgt = (pid_t) a[0];	/* the pid, the tgid, or for tkill the tid */
			int self = nr == SYS_tkill ? tid_of_node(nd, tgt) : tgt == nd->pid;
			if (!self)
				die(3, "node %d signals process %d; a signal to another process is not modelled", nd->id, (int) tgt);
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		case SYS_pidfd_open: case SYS_pidfd_send_signal:
			die(3, "node %d: system call %ld reaches another process through a pidfd, which is not modelled", nd->id, nr);
			return 1;
		case SYS_fork: case SYS_vfork:
			refuse_fork(nd);
			break;
		case SYS_open: case SYS_openat: case SYS_openat2: case SYS_creat:
		case SYS_unlink: case SYS_unlinkat: case SYS_rename: case SYS_renameat:
		case SYS_renameat2: case SYS_mkdir: case SYS_mkdirat: case SYS_rmdir:
		case SYS_link: case SYS_linkat: case SYS_symlink: case SYS_symlinkat:
		case SYS_truncate: case SYS_mknod: case SYS_mknodat:
		case SYS_chmod: case SYS_fchmod: case SYS_fchmodat: case SYS_fchmodat2:
		case SYS_chown: case SYS_fchown: case SYS_lchown: case SYS_fchownat:
		case SYS_utime: case SYS_utimes: case SYS_futimesat: case SYS_utimensat:
		case SYS_setxattr: case SYS_lsetxattr: case SYS_fsetxattr: case SYS_setxattrat:
		case SYS_removexattr: case SYS_lremovexattr: case SYS_fremovexattr: case SYS_removexattrat:
		case SYS_file_setattr: case SYS_open_by_handle_at: case SYS_io_uring_setup:
		case SYS_lseek: case SYS_fsync: case SYS_fdatasync: case SYS_ftruncate:
		case SYS_fallocate: case SYS_flock: case SYS_sync_file_range:
			fs_call(nd, nr, a);
			break;
		case SYS_clone: case SYS_clone3:
		{
			/* A new thread shares the address space and the seccomp filter,
			 * so its notifications arrive on this same listener.  The kernel
			 * makes the thread (CONTINUE), and once it has, birth() records
			 * the newborn held in its ptrace start stop, with the
			 * CLONE_CHILD_CLEARTID word the kernel zeroes and wakes on the
			 * thread's exit (what pthread_join waits on).  A clone without
			 * CLONE_THREAD is a fork: a second process in the node that
			 * shares its listener and connections but that the model does
			 * not track, and that outlives the check.  It is refused before
			 * the child exists (see refuse_fork). */
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
			if (!(flags & CLONE_THREAD)) refuse_fork(nd);
			/* every live thread and every clone yet to run needs a slot */
			int need = 1;
			for (int i = 0; i < nd->nth; i++) need += nd->th[i].inuse + nd->th[i].cloning;
			if (need > MAXTHREAD) die(1, "node %d creates more than %d threads", nd->id, MAXTHREAD);
			th->cloning = 1; th->clone_ctid = ctid;
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
				/* a timed wait is a deadline on the node clock, fired as
				 * ETIMEDOUT (A_EXPIRE).  FUTEX_WAIT's timeout is relative;
				 * FUTEX_WAIT_BITSET's is absolute, on either clock, and every
				 * clock reads the node clock. */
				unsigned long long tmo = 0;
				if (a[3])
				{
					int t = read_timespec(nd, a[3], &tmo);
					if (t < 0) { g_resp->error = -EFAULT; break; }
					if (t == 0) { g_resp->error = -EINVAL; break; }
					if (cmd == FUTEX_WAIT_BITSET) tmo = tmo > g_clk[nd->id] ? tmo - g_clk[nd->id] : 0;
					if (tmo == 0) { g_resp->error = -ETIMEDOUT; break; }
				}
				/* honour the kernel's atomic compare: FUTEX_WAIT sleeps only
				 * if *uaddr == val, else it returns EAGAIN at once.  Reading
				 * *uaddr and matching that keeps us from parking a thread the
				 * kernel would have let run (which could wrongly look
				 * quiescent). */
				uint32_t cur = 0;
				if (cr_read(nd->pid, uaddr, &cur, sizeof cur) != sizeof cur) { g_resp->error = -EFAULT; break; }
				if (cur != (uint32_t) a[2]) { g_resp->error = -EAGAIN; g_resp->val = 0; break; }
				th->block_kind = BK_FUTEX; th->block_id = g_req->id; th->futex_addr = uaddr;
				set_deadline(nd, th, a[3] != 0, tmo);
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
				if (cr_write(nd->pid, th->ctid_addr, &zero, sizeof zero) != sizeof zero) cr_fail(nd->id, "a thread exit clears its tid word");	/* GCOVR_EXCL_LINE: the C library's own tid word, in the thread's descriptor */
				futex_wake(nd, th->ctid_addr, 1);
			}
			th->inuse = 0; th->block_kind = 0;
			g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		default: g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
	}
	return 0;
}

static void node_gone(Node *nd);

/* Wait for thread tid, which is exiting or killed, to die.  It is a
 * traced task, so the checker must reap it by its own tid (__WALL).  A
 * wait that reports a ptrace stop (a newborn held in its start stop when
 * the group was killed) waits again, for the death the SIGKILL brings. */
static void
reap_tid(pid_t tid)
{
	int			st;
	pid_t		r;

	do r = waitpid(tid, &st, __WALL);
	while ((r == tid && WIFSTOPPED(st)) || (r < 0 && errno == EINTR));
}

/* Reap every thread of node nd, which is dead or dying: each task still
 * listed in /proc/PID/task, then the leader, whose exit the kernel
 * reports only once the other traced threads are reaped.  The task list
 * names a newborn the checker has not recorded too.  Clears the pid, so
 * kill_all never signals a reused one. */
static void
reap_node(Node *nd)
{
	char		path[64];

	snprintf(path, sizeof path, "/proc/%d/task", (int) nd->pid);
	DIR		   *d = opendir(path);
	if (d)
	{
		struct dirent *e;
		while ((e = readdir(d)) != NULL)
		{
			pid_t		tid = (pid_t) atoi(e->d_name);
			if (tid > 0 && tid != nd->pid) reap_tid(tid);
		}
		closedir(d);
	}
	reap_tid(nd->pid);
	nd->pid = 0;
}

/* end node nd's process: SIGKILL ends every thread, then reap them all */
static void
kill_node(Node *nd)
{
	kill(nd->pid, SIGKILL);
	reap_node(nd);
}

/* Detect that a node has exited, or that one of its threads stopped for
 * a signal or at a sendmsg on a kernel fd (trace_stop).  Only an exit or
 * a fatal signal is a death.  A thread other than the leader stops on
 * its own tid, so each known thread is asked too; an abort() in a
 * worker stops the worker, not the leader.  drive_node() calls this
 * between its waits, so a stopped thread is never waited on forever.
 * Returns 1 if it resumed a thread from a sendmsg stop, which is
 * progress the listener does not show, and 0 otherwise. */
static void signal_stop(Node *nd, pid_t tid, int st);
static int
reap(Node *nd)
{
	if (!nd->alive) return 0;
	int st;
	for (int i = 0; i < nd->nth; i++)
	{
		const Thread *th = &nd->th[i];
		if (!th->inuse || th->tid == nd->pid) continue;
		if (waitpid(th->tid, &st, WNOHANG | __WALL) == th->tid && WIFSTOPPED(st))
		{ signal_stop(nd, th->tid, st); return nd->alive; }
	}
	pid_t r = waitpid(nd->pid, &st, WNOHANG | __WALL);
	if (r != nd->pid) return 0;
	if (WIFSTOPPED(st)) { signal_stop(nd, nd->pid, st); return nd->alive; }
	nd->pid = 0; nd->alive = 0; drain_canon(nd); node_gone(nd);
	return 0;
}

/* The first node a fault signal killed in this replay, and the signal:
 * the search reports it as a violation (see signal_stop).  -1: none. */
static int g_fault_node = -1, g_fault_sig;

/* Thread tid of node nd stopped at the second filter's SECCOMP_RET_TRACE
 * (PTRACE_EVENT_SECCOMP): a sendmsg on a kernel fd, below 500.  In a
 * node with one thread the call goes on to the kernel, as every other
 * call on a kernel fd does.  In a node with more than one it is refused,
 * as kernel_fd_call() refuses the others: another thread may read what
 * it sends, and the socket's contents are not in the world. */
static void
trace_stop(Node *nd, pid_t tid)
{
	struct user_regs_struct regs;
	long nr = -1;
	if (ptrace(PTRACE_GETREGS, tid, 0, &regs) == 0) nr = (long) regs.orig_rax;	/* GCOVR_EXCL_LINE: the thread is in the stop just reported */
	if (nr == SYS_execve || nr == SYS_execveat)
	{
		/* the first is the launch's own exec of the node's binary; any
		 * later one replaces the program under check */
		if (nd->execd)
			die(3, "node %d runs another program (execve); a node is one program, checked as it was launched", nd->id);
		nd->execd = 1;
		if (ptrace(PTRACE_CONT, tid, 0, 0) != 0)	/* GCOVR_EXCL_LINE: the thread is in the stop just reported */
			die(1, "node %d: cannot resume thread %d at its exec: %s", nd->id, (int) tid, strerror(errno));
		return;
	}
	if (node_multi(nd))
	{
		int fd = nr == SYS_sendmsg ? (int) regs.rdi : -1;
		die(3, "node %d: a thread of a multi-threaded node uses kernel fd %d (system call %d); only checker-owned fds and the state hook are modelled between threads", nd->id, fd, SYS_sendmsg);
	}
	if (ptrace(PTRACE_CONT, tid, 0, 0) != 0)	/* GCOVR_EXCL_LINE: the thread is in the stop just reported */
		die(1, "node %d: cannot resume thread %d at a sendmsg: %s", nd->id, (int) tid, strerror(errno));
}

/* Thread tid of node nd is in a signal-delivery stop (waitpid status
 * st): the node is ptrace-seized, so a signal parks the thread instead
 * of acting.  A fault signal (SIGABRT from abort() or a failed assert,
 * SIGSEGV, SIGBUS, SIGFPE, SIGILL) at its default disposition ends the
 * process on Linux.  The checker kills the node instead of delivering
 * it, so no core file is written, and records the fault: a program that
 * kills itself this way has found a bug of its own, and treating that as
 * an ordinary exit could pass it.  A handler would run instead, and is
 * not modelled.  An ignored fault signal may or may not end the process
 * (abort() and the kernel reset it to the default first, kill() and
 * raise() do not), and the stop cannot tell which, so it is not
 * modelled either, nor is any other signal.  Each ends the check. */
static void
signal_stop(Node *nd, pid_t tid, int st)
{
	if (st >> 8 == (SIGTRAP | (PTRACE_EVENT_SECCOMP << 8))) { trace_stop(nd, tid); return; }
	int sig = WSTOPSIG(st);
	int fault = sig == SIGABRT || sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL;
	if (!fault || st >> 16 != 0)
		die(3, "node %d stopped with signal %d; a signal other than SIGPIPE or a fault is not modelled", nd->id, sig);
	uint64_t bit = (uint64_t) 1 << (sig - 1);
	if ((sigmask_of(nd, tid, "SigCgt:") | sigmask_of(nd, tid, "SigIgn:")) & bit)
		die(3, "node %d raised signal %d with a handler installed or the signal ignored; neither is modelled", nd->id, sig);
	kill_node(nd);
	nd->alive = 0; drain_canon(nd); node_gone(nd);
	if (g_fault_node < 0) { g_fault_node = nd->id; g_fault_sig = sig; }
}

/* answer a thread's deferred syscall with return value val (a negative
 * val is an errno), clearing its slot (the thread resumes running).  In a
 * node with more than one thread the answer is held instead (BK_DONE),
 * with whatever the call wrote into the thread's memory, and the thread
 * stays parked until the search runs it (A_RUN): that a parked call
 * completes and that its thread runs on are two steps, and another
 * thread may act between them. */
static void
answer(Node *nd, Thread *th, long val)
{
	if (node_multi(nd))
	{
		if (g_cap != th) th->held_len = 0;
		th->block_kind = BK_DONE; th->held_flags = 0;
		th->held_val = val < 0 ? 0 : val; th->held_err = val < 0 ? (int) val : 0;
		return;
	}
	memset(g_ans, 0, g_sz.seccomp_notif_resp); g_ans->id = th->block_id;
	if (val < 0) g_ans->error = (int) val; else g_ans->val = val;
	ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_ans);
	th->block_kind = 0;
}

/* A_RUN: send thread th its held answer, so it runs to its next visible
 * operation.  Every other thread of the node is parked, so it runs alone. */
static void
run_held(Node *nd, Thread *th)
{
	memset(g_ans, 0, g_sz.seccomp_notif_resp); g_ans->id = th->block_id;
	g_ans->val = th->held_val; g_ans->error = th->held_err; g_ans->flags = th->held_flags;
	ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_ans);
	th->block_kind = 0; th->held_len = 0; nd->undo_kind = 0;
}

/* A_FAIL: the held write, truncation or fsync of a modelled file fails
 * instead, with EIO (arg 0) or ENOSPC (arg 1), and the node runs on.  Its
 * change is undone; a failed fsync also forgets the file's unsynced
 * operations, so what it did not write is lost at a crash, as on Linux
 * after a writeback error. */
static void
fail_held(Node *nd, Thread *th, int arg)
{
	fs_undo(&nd->fs, &nd->undo, nd->undo_kind == 's');
	if (arg == 0) bud.eio--; else bud.enospc--;
	th->held_val = 0; th->held_err = arg == 0 ? -EIO : -ENOSPC;
	run_held(nd, th);
}

/* A_RUN of a thread held in a ptrace stop by a clone (a newborn, or its
 * parent at the clone's return): release it, so it runs to its next
 * visible operation */
static void
run_stopped(Node *nd, Thread *th)
{
	if (ptrace(PTRACE_CONT, th->tid, 0, 0) != 0)	/* GCOVR_EXCL_LINE: the thread is in the stop just reported */
		die(1, "node %d: cannot start thread %d: %s", nd->id, (int) th->tid, strerror(errno));
	th->block_kind = 0;
}

/* Wake up to n threads parked in BK_FUTEX on uaddr, and return how
 * many.  A FUTEX_WAKE from the waker's step; it invents no wakes.  Each
 * woken wait is answered 0, which in a node with more than one thread
 * holds the answer (BK_DONE): whether the woken thread or the waker runs
 * first is the search's choice (A_RUN), so a wake sent before the data it
 * announces is stored is caught.  When n would wake a strict subset of
 * several same-address waiters, which subset wakes is a schedule this
 * model does not enumerate, so rather than pick one silently (and risk
 * hiding a bug) the checker errors.  waiters == 1 and n >= waiters (the
 * condvar-signal and broadcast cases) are unambiguous. */
static int
futex_wake(Node *nd, uint64_t uaddr, int n)
{
	int waiters = 0;
	for (int i = 0; i < nd->nth; i++)
		if (nd->th[i].inuse && nd->th[i].block_kind == BK_FUTEX && nd->th[i].futex_addr == uaddr) waiters++;
	if (waiters == 0) return 0;
	if (n < waiters && n < 0x7fffffff)
		die(1, "node %d: FUTEX_WAKE of %d would wake a subset of %d waiters on one word; the thread model does not enumerate which (a lock contended by a pool is out of scope)", nd->id, n, waiters);
	int woke = 0;
	for (int i = 0; i < nd->nth && woke < n; i++)
		if (nd->th[i].inuse && nd->th[i].block_kind == BK_FUTEX && nd->th[i].futex_addr == uaddr)
		{
			Thread *old = cap_begin(&nd->th[i]);
			answer(nd, &nd->th[i], 0);
			g_cap = old;
			woke++;
		}
	return woke;
}

/* Write the k bytes a read of thread th returns into its buffer, or
 * scatter them over its readv or recvmsg iovec array in order, and set
 * a recvmsg's msg_namelen and msg_flags to 0, as Linux does for TCP. */
static void
put_read(Node *nd, Thread *th, const unsigned char *body, int k)
{
	if (!th->vecread)
	{
		if (k > 0 && cr_write(nd->pid, th->block_bufaddr, body, (size_t) k) != (size_t) k) cr_fail(nd->id, "a recv buffer");
		return;
	}
	int off = 0;
	for (int i = 0; i < th->niov && off < k; i++)
	{
		size_t l = th->iovl[i] < (uint64_t) (k - off) ? (size_t) th->iovl[i] : (size_t) (k - off);
		if (l > 0 && cr_write(nd->pid, th->iovb[i], body + off, l) != l) cr_fail(nd->id, "a readv buffer");
		off += (int) l;
	}
	if (th->block_msg)
	{
		socklen_t zl = 0; int zf = 0;
		if (cr_write(nd->pid, th->block_msg + offsetof(struct msghdr, msg_namelen), &zl, sizeof zl) != sizeof zl ||
			cr_write(nd->pid, th->block_msg + offsetof(struct msghdr, msg_flags), &zf, sizeof zf) != sizeof zf)
			cr_fail(nd->id, "a recvmsg header");
	}
}

/* answer a thread's deferred recv with up to len bytes (0 = EOF);
 * returns how many it took -- a short read leaves the rest queued */
static int
deliver(Node *nd, Thread *th, const unsigned char *body, int len)
{
	int k = len < th->block_want ? len : th->block_want;
	Thread *old = cap_begin(th);
	put_read(nd, th, body, k);
	answer(nd, th, k);
	g_cap = old;
	return k;
}

/* answer a thread's deferred accept with a connection from peer */
static void
accept_from(Node *nd, Thread *th, int peer)
{
	int c = -1;
	for (int k = 0; k < nd->npend; k++)
		if (conns[nd->pend[k]].node[0] == peer)
		{
			c = nd->pend[k];
			for (int j = k; j + 1 < nd->npend; j++) nd->pend[j] = nd->pend[j + 1];
			nd->npend--;
			break;
		}
	if (c < 0) die(1, "node %d accepts from %d, but no connection from it waits", nd->id, peer);
	conns[c].accepted = 1;
	int fd = nd->next_fd++;
	cfd_set(nd, fd, peer, c, 1);
	/* an accepted socket inherits the listener's options, as on Linux */
	(void) fd_flags(nd, fd); nd->sockopt[fd - 500] = nd->sockopt[nd->listener_fd - 500];
	/* accept4's SOCK_CLOEXEC is the new fd's FD_CLOEXEC */
	if (th->nr == SYS_accept4 && (th->args[3] & SOCK_CLOEXEC)) nd->ccloexec[nd->nc - 1] = 1;
	answer(nd, th, fd);
}

/* Has the peer of end e of connection conn been lost without a word: its
 * host crashed, so no FIN or RST will ever arrive, and nothing is queued?
 * A reader there would wait until a keepalive or a timeout notices; the
 * search may let it notice at once (an EOF), standing for that. */
static int
peer_lost(int conn, int e)
{
	if (conn < 0) return 0;
	const Conn *cn = &conns[conn];
	return cn->gone[1 - e] && !cn->reset && !cn->refused && conn_head(conn, e) < 0;
}

/* a waiting connection from `peer` at node nd's listener? */
static int
pend_from(const Node *nd, int peer)
{
	for (int k = 0; k < nd->npend; k++)
		if (conns[nd->pend[k]].node[0] == peer) return 1;
	return 0;
}

/* Node nd is gone.  If its process exited on a live host (it returned,
 * or a signal killed it), its kernel closes every socket as a close()
 * would (a FIN, or an RST where bytes were left unread) and resets the
 * dials still waiting in its backlog.  If the host crashed (crashed
 * set), nothing is sent, and each end just goes dead. */
static void
node_gone(Node *nd)
{
	for (int k = 0; k < nd->nc; k++)
		if (nd->cconn[k] >= 0) end_close(nd->cconn[k], nd->cend[k], nd->crashed);
	nd->nc = 0; nd->timer_fd = -1;
	for (int k = 0; k < nd->npend; k++)
	{
		int c = nd->pend[k];
		Conn *cn = &conns[c];
		cn->closed[1] = 1; cn->gone[1] = nd->crashed;
		conn_discard_to(c, 1);
		if (!nd->crashed)
		{
			/* the kernel of an exited listener resets the dials still in
			 * its backlog; a crashed host answers nothing */
			rst_path(nd->id, cn->node[0]);
			cn->reset = 1; cn->err[0] = reset_errno(c, 0);
			rst_notify(c, 0);
		}
		conn_maybe_free(c);
	}
	nd->npend = 0;
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
	kill_node(nd);
	nd->alive = 0; nd->crashed = 1;
	node_gone(nd);
	nd->nth = 0; memset(nd->th, 0, sizeof nd->th);
	/* volatile is gone; the node's observable state is what was durable */
	memcpy(nd->canon, nd->durable, nd->durable_len); nd->canon_len = nd->durable_len;
	/* close the parent's fds for the dead process (the final drain is done,
	 * the durable bytes are in nd->durable); a RESTART opens fresh ones, so
	 * leaving these open would leak one listener and two pipe ends per
	 * crash across a long search */
	if (nd->listener > 0) close(nd->listener);	/* GCOVR_EXCL_LINE: a launched node always holds them */
	if (nd->fd3[0] > 0) close(nd->fd3[0]);	/* GCOVR_EXCL_LINE: a launched node always holds them */
	if (nd->fd4[0] > 0) close(nd->fd4[0]);	/* GCOVR_EXCL_LINE: a launched node always holds them */
	for (int k = 0; k < 2; k++) if (nd->hook_w[k] > 0) close(nd->hook_w[k]);	/* GCOVR_EXCL_LINE: a launched node always holds them */
	nd->listener = nd->fd3[0] = nd->fd4[0] = nd->hook_w[0] = nd->hook_w[1] = -1;
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
	if (nd->alive || !nd->crashed) return -1;	/* GCOVR_EXCL_LINE: RESTART is offered only for a crashed node */
	unsigned char saved[SNAP]; int savedlen = nd->durable_len;
	memcpy(saved, nd->durable, (size_t) savedlen);
	if (launch(nd)) return -1;	/* GCOVR_EXCL_LINE: a relaunch is a launch, which does not fail in a test */
	memcpy(nd->durable, saved, (size_t) savedlen); nd->durable_len = savedlen;
	nd->durable_read_off = 0;
	return 0;
}

/* Drive node nd, alone, until no thread of it is running (each running
 * thread makes calls until it parks in one the search answers, or the
 * node exits), or until a call of it woke another node that was not
 * running (a close whose RST answered a peer's parked recv).  Then that
 * node may act before nd's next call, as on Linux, which is a search
 * choice (settle()), so nd stops at a call boundary: its next call waits
 * unread.  A node with a thread neither parked nor exited is still
 * running between syscalls, so a lull is waited out, not taken for
 * quiescence. */
static void
drive_node(Node *nd)
{
	int idle[MAXNODES];
	for (int i = 0; i < M.nodes; i++) idle[i] = !node_has_running(&N[i]);
	for (;;)
	{
		reap(nd);
		if (!node_has_running(nd)) return;
		for (int i = 0; i < M.nodes; i++)
			if (idle[i] && &N[i] != nd && node_has_running(&N[i])) return;
		if (try_pending_for(nd)) continue;
		/* wait for the running thread's next call, in slices: a thread
		 * stopped by a signal or at a sendmsg makes no call and wakes no
		 * poll, so look for one (reap) between slices.  shortcut: a
		 * sendmsg on a kernel fd waits up to one slice to be resumed;
		 * wait on SIGCHLD too if a target sends many. */
		struct pollfd pf = {nd->listener, POLLIN, 0};
		int r = 0, moved = 0;
		for (int waited = 0; !moved && waited < LULL_MS; waited += LULL_SLICE_MS)
		{
			r = poll(&pf, 1, LULL_SLICE_MS);
			if (r != 0) break;
			moved |= reap(nd); moved |= !nd->alive;
		}
		if (moved) continue;
		if (r < 0 && errno == EINTR) continue;	/* GCOVR_EXCL_LINE: a wait interrupted by a signal; the checker takes none */
		/* a notify fd that reports POLLHUP has lost its owning process: the
		 * node exited (its last thread ran off the end or called
		 * exit_group).  waitpid(WNOHANG) can momentarily still see a
		 * ptrace-seized tracee as not-yet-reapable, so reap it directly
		 * here rather than spin until WNOHANG catches up. */
		if (r > 0 && (pf.revents & (POLLHUP | POLLERR)))
		{ reap_node(nd); nd->alive = 0; drain_canon(nd); node_gone(nd); continue; }
		if (r == 0)
		{
			/* a running thread made no syscall the checker owns for
			 * LULL_MS: it is busy, or blocked in a call the checker does
			 * not model.  Treating that as quiescence would snapshot a
			 * node mid-step, so it is an error. */
			reap(nd);
			if (node_has_running(nd))
				die(1, "node %d made no checked syscall for %d ms (a thread is busy, or blocked in a call the checker does not model)", nd->id, LULL_MS);
		}
	}
}

/* Is the world quiescent: no node with a running thread?  Only such a
 * world is a state the invariants judge.  Between two nodes' STEPs a
 * node not yet stepped has run only up to a call nobody has answered,
 * so its state record is not one it reports between steps: the
 * invariants, the refinement checks and the seen set wait for the world
 * to settle, and the STEPs to it are part of the action that led there. */
/* The single-threaded node held at a crash point (crash_point()), or -1.
 * Such a world is mid-step: the program's state hook may lag the call
 * just made (a program reports its state after its effects), so no
 * invariant is checked there, and its only actions are to run the node
 * on and to crash it. */
static int
midstep_node(void)
{
	for (int i = 0; i < M.nodes; i++)
		if (N[i].alive && !node_multi(&N[i]))
			for (int t = 0; t < N[i].nth; t++)
				if (N[i].th[t].inuse && N[i].th[t].block_kind == BK_DONE) return i;
	return -1;
}

static int
world_settled(void)
{
	for (int i = 0; i < M.nodes; i++)
		if (node_has_running(&N[i])) return 0;
	return 1;
}

/* Bring the world to a point where the search chooses.  Nodes are
 * driven one at a time, each until it is quiescent (drive_node), never
 * two at once, so what the checker answers a node cannot depend on how
 * far another has got.  When exactly one node has a running thread it is
 * driven with no choice, as a DELIVER or a RUN, which wake one node,
 * always leaves.  When two or more do (at launch, a RESTART, or a call
 * of one node that answered a parked call of another) the order of
 * their steps is a search choice: settle() stops, and enum_actions
 * offers STEP for each.
 *
 * A STEP also ends when the node wakes another (drive_node), so a node
 * an RST woke may act before the closer's next call.
 *
 * Driving two nodes at once, as 2.0.0 did, made the answer to a call
 * that reads another node's state (a connect to a peer that may have
 * exited, a write to a peer that may have closed) depend on the
 * kernel's scheduling: one action sequence reached two worlds, and the
 * search saw one of them at random.  A fixed order would see one always.
 * Each STEP drives one node to its own quiescent point, the same unit a
 * DELIVER drives, or to the call that woke another, so the orders the
 * search explores are the orders of those steps, and the seen set
 * merges orders that reach the same world.  Calls inside one step that
 * reach no other node are not interleaved with another node's. */
static void
settle(void)
{
	for (;;)
	{
		int n = 0, last = -1;
		for (int i = 0; i < M.nodes; i++)
			if (node_has_running(&N[i])) { n++; last = i; }
		if (n != 1) break;
		drive_node(&N[last]);
	}
	for (int i = 0; i < M.nodes; i++) if (N[i].alive) drain_canon(&N[i]);
	/* without the preload a node reads the real clock and the real heap,
	 * and replay is no longer deterministic: refuse rather than guess.
	 * Once no node runs, each has run since its launch. */
	for (int i = 0; world_settled() && i < M.nodes; i++)
		if (!N[i].preload_ok)
			die(3, "node %d did not load the preload %s (set CMC_PRELOAD to its path)", i, preload_path);
}

/* Is every running node fresh: launched (or restarted) and never served
 * a call?  Such a node's future is a function of its launch alone, and
 * its record (no thread yet) says so, so a world between STEPs whose
 * running nodes are all fresh may be merged with an equal one.  A node
 * woken partway (another node's close answered its parked recv) has run
 * on from an answer its record does not hold, so a world with one is
 * searched as a tree. */
static int
running_fresh(void)
{
	for (int i = 0; i < M.nodes; i++)
		if (node_has_running(&N[i]) && N[i].nth > 0) return 0;
	return 1;
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

/* The seen set: an open-addressing table, keyed by the 128-bit
 * fingerprint, whose entries also hold the full world encoding in a
 * byte pool.  Two worlds are the same only if their encodings are equal
 * byte for byte; a fingerprint match with different bytes is a collision
 * and both are kept.  The table grows at half load, so the search never
 * forgets a state. */
typedef struct { uint64_t a, b; size_t off, len; } FP;
static FP *seen; static size_t seen_cap, nseen;
static unsigned char *spool; static size_t spool_cap, spool_used;

static void
seen_put(FP f)
{
	size_t i = f.a & (seen_cap - 1);
	while (seen[i].a | seen[i].b) i = (i + 1) & (seen_cap - 1);
	seen[i] = f;
}

/* 1 if the world (fingerprint fp, encoding w[0..n)) is new, and records
 * it; 0 if this exact world was seen before */
static int
mark(const uint64_t fp[2], const unsigned char *w, size_t n)
{
	uint64_t a = fp[0], b = fp[1];
	if (!(a | b)) a = 1;	/* GCOVR_EXCL_LINE: (0,0) marks an empty slot, and a fingerprint of (0,0) is never seen */
	if (2 * (nseen + 1) > seen_cap)
	{
		FP *old = seen; size_t oc = seen_cap;
		seen_cap = oc ? 2 * oc : 4096;
		seen = calloc(seen_cap, sizeof *seen);
		if (!seen) die(1, "out of memory for the seen set at %zu states", nseen);	/* GCOVR_EXCL_LINE: realloc of the seen set */
		for (size_t i = 0; i < oc; i++) if (old[i].a | old[i].b) seen_put(old[i]);
		free(old);
	}
	size_t i;
	for (i = a & (seen_cap - 1);; i = (i + 1) & (seen_cap - 1))
	{
		if (!(seen[i].a | seen[i].b)) break;
		if (seen[i].a == a && seen[i].b == b && seen[i].len == n && !memcmp(spool + seen[i].off, w, n)) return 0;
	}
	if (spool_used + n > spool_cap)
	{
		while (spool_used + n > spool_cap) spool_cap = spool_cap ? 2 * spool_cap : (size_t) 1 << 20;
		spool = realloc(spool, spool_cap);
		if (!spool) die(1, "out of memory for the seen set at %zu states", nseen);	/* GCOVR_EXCL_LINE: realloc of the seen set */
	}
	memcpy(spool + spool_used, w, n);
	seen[i].a = a; seen[i].b = b; seen[i].off = spool_used; seen[i].len = n;
	spool_used += n; nseen++;
	return 1;
}

/* the semantic class of fd on node nd, as two bytes (class, which), so
 * the encoding never depends on a raw virtual fd number: a connection by
 * its slot in the node's open-order connection list (two connections to
 * the same peer are different fds with different futures, so the peer
 * alone would merge them; the list itself, with each slot's peer, is
 * encoded per node), an eventfd by its slot, the listener, an ignored
 * negative fd, or a kernel fd the model does not track (by its number,
 * which is deterministic).  Writes 2 bytes to r. */
static void
fd_class(const Node *nd, int fd, unsigned char *r)
{
	int cs = cfd_slot((Node *) nd, fd), es = efd_slot((Node *) nd, fd);
	if (cs >= 0) { r[0] = 1; r[1] = (unsigned char) cs; }
	else if (es >= 0) { r[0] = 2; r[1] = (unsigned char) es; }
	else if (fd >= 0 && fd == nd->listener_fd) { r[0] = 3; r[1] = 0; }
	else if (fd < 0) { r[0] = 4; r[1] = 0; }
	else { r[0] = 5; r[1] = (unsigned char) fd; }
}

/* Encode a node's per-thread blocked state into out (<= cap bytes),
 * canonicalised so the raw kernel tid never leaks in: each live thread
 * contributes a fixed-width record of what it waits in and on, and the
 * records are sorted, so the multiset of thread states is the key, not
 * which tid holds which.  A record carries everything the thread's
 * future depends on: a recv's peer and length, every polled fd with its
 * requested events and (for epoll) its user data, a pending poll wake,
 * a futex wait's word by address (stable, since nodes run without
 * address randomisation), and a held answer: the call, its result, and
 * the bytes it wrote into the thread's memory, after a fixed-width head.
 * Returns the number of bytes written, or -1 if out is too small. */
#define THREC (25 + MAXPOLL * 12)		/* fixed width of one thread's record, before a held answer's bytes */
#define THREC_MAX (THREC + 76 + 18 + MSGLEN)	/* the widest record: a head, the call, a held answer and its bytes */
static int
encode_threads(const Node *nd, unsigned char *out, int cap)
{
	static unsigned char rec[MAXTHREAD][THREC_MAX]; int len[MAXTHREAD], nrec = 0;
	int multi = node_multi(nd);
	for (int i = 0; i < nd->nth; i++)
	{
		const Thread *th = &nd->th[i];
		if (!th->inuse) continue;
		unsigned char *r = rec[nrec]; memset(r, 0, THREC); int p = 0;
		r[p++] = (unsigned char) (th->block_kind ? th->block_kind : 3);
		if (th->poll_woke_fd >= 0) { r[p++] = 1; fd_class(nd, th->poll_woke_fd, r + p); p += 2; }
		else r[p++] = 0;
		if (th->block_kind == BK_RECV)
		{
			/* the connection read, by its open-order slot (not the peer:
			 * two connections to one peer have different queues) */
			int cs = -1;
			for (int k = 0; k < nd->nc; k++)
				if (nd->cconn[k] == th->block_conn && nd->cend[k] == th->block_end) { cs = k; break; }
			r[p++] = (unsigned char) (th->block_peer + 4); r[p++] = (unsigned char) (cs + 1);
			r[p++] = (unsigned char) th->block_want; r[p++] = (unsigned char) (th->block_want >> 8);
		}
		else if (th->block_kind == BK_ACCEPT || th->block_kind == BK_SYNWAIT)
			r[p++] = (unsigned char) (th->block_peer + 4);
		else if (th->block_kind == BK_FUTEX)
			for (int b = 0; b < 8; b++) r[p++] = (unsigned char) (th->futex_addr >> (8 * b));
		else if (th->block_kind == BK_POLL)
		{
			/* a leading flag marks an epoll_wait park apart from a poll()
			 * one, so the two parking modes never merge */
			r[p++] = (unsigned char) (th->epoll_fd >= 0 ? 1 : 0);
			r[p++] = (unsigned char) th->pollfd_n;
			for (int k = 0; k < th->pollfd_n; k++)
			{
				r[p++] = (unsigned char) th->pollevents[k]; r[p++] = (unsigned char) (th->pollevents[k] >> 8);
				fd_class(nd, th->pollfds[k], r + p); p += 2;
				if (th->epoll_fd >= 0)
					for (int b = 0; b < 8; b++) r[p++] = (unsigned char) (th->polldata[k] >> (8 * b));
			}
		}
		/* a pending deadline, as the time left on the node's clock: two
		 * worlds whose clocks differ by the same amount as their deadlines
		 * have the same future, apart from what the clock reads (which the
		 * world records too) */
		if (timed_kind(th->block_kind))
		{
			unsigned long long left = th->has_dl && th->dl > g_clk[nd->id] ? th->dl - g_clk[nd->id] : 0;
			r[p++] = (unsigned char) th->has_dl;
			for (int b = 0; b < 8; b++) r[p++] = (unsigned char) (left >> (8 * b));
		}
		if (p > THREC) die(1, "thread record overflow on node %d", nd->id);	/* GCOVR_EXCL_LINE: a record's fields are bounded by THREC */
		p = THREC;
		/* In a node with more than one thread, a thread's position is part
		 * of the world: two threads may be parked with the same kind of
		 * wait at different points of their code (a woken condition wait
		 * and the mutex unlock after it both hold a futex answer of 0).
		 * The call, its arguments, and its instruction and stack pointers
		 * name the point; they are fixed across replays, since nodes run
		 * without address randomisation.  A newborn has none: it is
		 * unstarted, which its BK_NEWBORN kind says.  A thread still holding
		 * an answer when the others have exited keeps its point too. */
		if (multi || th->block_kind >= BK_DONE)
		{
			for (int b = 0; b < 4; b++) r[p++] = (unsigned char) (th->nr >> (8 * b));
			for (int b = 0; b < 8; b++) r[p++] = (unsigned char) (th->ip >> (8 * b));
			for (int b = 0; b < 8; b++) r[p++] = (unsigned char) (th->sp >> (8 * b));
			for (int k = 0; k < 6; k++)
				for (int b = 0; b < 8; b++) r[p++] = (unsigned char) (th->args[k] >> (8 * b));
			/* which thread a pthread_join waits for: its clear-tid word */
			for (int b = 0; b < 8; b++) r[p++] = (unsigned char) (th->ctid_addr >> (8 * b));
		}
		if (th->block_kind == BK_DONE)
		{
			for (int b = 0; b < 8; b++) r[p++] = (unsigned char) ((uint64_t) th->held_val >> (8 * b));
			for (int b = 0; b < 4; b++) r[p++] = (unsigned char) ((uint32_t) th->held_err >> (8 * b));
			for (int b = 0; b < 4; b++) r[p++] = (unsigned char) (th->held_flags >> (8 * b));
			r[p++] = (unsigned char) th->held_len; r[p++] = (unsigned char) (th->held_len >> 8);
			memcpy(r + p, th->held_data, (size_t) th->held_len); p += th->held_len;
		}
		len[nrec++] = p;
	}
	/* sort the records so the multiset, not thread order, is the key: by
	 * the fixed head, then by length, then by the held bytes */
	int ord[MAXTHREAD];
	for (int i = 0; i < nrec; i++) ord[i] = i;
	for (int i = 0; i < nrec; i++)
		for (int j = i + 1; j < nrec; j++)
		{
			int a = ord[i], b = ord[j];
			int c = memcmp(rec[b], rec[a], THREC);
			if (!c) c = len[b] != len[a] ? len[b] - len[a] : memcmp(rec[b], rec[a], (size_t) len[a]);
			if (c < 0) { ord[i] = b; ord[j] = a; }
		}
	int n = 0;
	if (1 + nrec * 2 > cap) return -1;	/* GCOVR_EXCL_LINE: cap is sized for MAXTHREAD full records */
	out[n++] = (unsigned char) nrec;
	for (int i = 0; i < nrec; i++)
	{
		int k = ord[i];
		if (n + 2 + len[k] > cap) return -1;	/* GCOVR_EXCL_LINE: cap is sized for MAXTHREAD full records */
		out[n++] = (unsigned char) len[k]; out[n++] = (unsigned char) (len[k] >> 8);
		memcpy(out + n, rec[k], (size_t) len[k]); n += len[k];
	}
	return n;
}

/* The world encoding: every node's state and liveness, the messages in
 * flight (each channel in its order), the reply log, the environment
 * budgets left and the logical clock.  Everything the future depends on
 * goes in; leaving out a budget, the clock, a durable record or a polled
 * fd merges worlds whose futures differ.  The encoding is written to
 * g_world (g_world_len bytes) and its 128-bit fingerprint to fp; the seen
 * set compares the bytes, so the fingerprint only indexes.  Tids never
 * appear: threads are named by their sorted records.  shortcut: virtual
 * fd numbers are left out too (an fd is named by its class, see
 * fd_class), on the assumption that a target does not branch on an fd's
 * value; record next_fd and the raw numbers if a target ever does. */
static unsigned char g_world[65536]; static size_t g_world_len;

/* Append byte b to the encoding w (cap bytes) at *n.  A world too big
 * for the buffer is an error, never a truncated encoding. */
static void
put_byte(unsigned char *w, size_t *n, size_t cap, unsigned b)
{
	if (*n >= cap) die(1, "world encoding exceeds %zu bytes", cap);
	w[(*n)++] = (unsigned char) b;
}

/* Encode end e of connection c as its holder sees it: the connection's
 * flags, then every message queued toward this end, in order.  A
 * connection is named by where it is held, never by its slot number,
 * which depends on allocation order.  Returns the new length. */
static size_t
put_conn_end(unsigned char *w, size_t n, size_t cap, int c, int e)
{
#define PUTC(b) put_byte(w, &n, cap, (unsigned) (b))
	const Conn *cn = &conns[c];
	PUTC(cn->closed[1 - e] | (cn->gone[1 - e] << 1) | (cn->reset << 2) | (cn->eof[e] << 3) | (cn->accepted << 4) | (cn->refused << 5) |
		 (cn->connecting << 6) | (cn->nbdial << 7));
	PUTC(cn->full[e] | (cn->full[1 - e] << 1));
	PUTC(cn->err[e] == ECONNRESET ? 1 : cn->err[e] == EPIPE ? 2 : cn->err[e] == ECONNREFUSED ? 3 : 0);
	PUTC(cn->shut_wr[e] | (cn->shut_rd[e] << 1) | (cn->shut_wr[1 - e] << 2) | (cn->shut_rd[1 - e] << 3));
	for (int last = -1;;)
	{
		int h = conn_next(c, e, last);
		if (h < 0) break;
		last = bag[h].seq;
		PUTC(bag[h].eof ? 2 : 1); PUTC(bag[h].len); PUTC(bag[h].len >> 8);
		for (int k = 0; k < bag[h].len; k++) PUTC(bag[h].body[k]);
	}
	PUTC(0);
#undef PUTC
	return n;
}

/* one node's file system encoding, before it goes into the world */
static unsigned char fs_buf[65536];

static void
world_fp(uint64_t fp[2])
{
	unsigned char *w = g_world; size_t n = 0;
#define PUT(b) put_byte(w, &n, sizeof g_world, (unsigned) (b))
#define PUT8(x) do { uint64_t _x = (uint64_t) (x); for (int _b = 0; _b < 8; _b++) PUT(_x >> (8 * _b)); } while (0)
	for (int i = 0; i < M.nodes; i++)
	{
		const Node *nd = &N[i];
		/* node liveness: a running node (fully alive), one launched that
		 * has not yet made a call (only between STEPs, see settle()), a
		 * crashed one, or an exited one; then its volatile and its durable
		 * records, and how much of the durable record a startup read has
		 * consumed */
		PUT(nd->alive ? (nd->nth ? 3 : 4) : (nd->crashed ? 0xfe : 0xff));
		PUT(nd->canon_len);
		for (int k = 0; k < nd->canon_len; k++) PUT(nd->canon[k]);
		PUT(nd->durable_len);
		for (int k = 0; k < nd->durable_len; k++) PUT(nd->durable[k]);
		PUT(nd->durable_read_off);
		/* its modelled files, which a crash keeps (in part), and what a
		 * failure of the call it is held at would undo */
		PUT(nd->undo_kind);
		if (nd->undo_kind)
		{
			PUT(nd->undo.ino); PUT(nd->undo.fdslot); PUT8(nd->undo.off);
			int r = fs_encode_ino(&nd->undo.copy, fp128, fs_buf, (int) sizeof fs_buf);
			uint64_t h[2]; fp128(fs_buf, (size_t) r, h); PUT8(h[0]); PUT8(h[1]);
		}
		{
			int r = fs_encode(&nd->fs, fp128, fs_buf, (int) sizeof fs_buf);
			if (r < 0) die(1, "file system encoding overflow on node %d", i);	/* GCOVR_EXCL_LINE: bounded by the FS_ limits */
			for (int k = 0; k < r; k++) PUT(fs_buf[k]);
		}
		if (!nd->alive) { PUT(0); continue; }
		static unsigned char tb[1 + MAXTHREAD * (2 + THREC_MAX)];
		int tl = encode_threads(nd, tb, (int) sizeof tb);
		if (tl < 0) die(1, "thread encoding overflow on node %d", i);
		for (int k = 0; k < tl; k++) PUT(tb[k]);
		/* live connections in the order they were opened (cfd is kept in
		 * that order), by peer, with their flags, and which one is the
		 * fired timer's: which connection goes where, and whether a read
		 * on it would return EAGAIN or a 'T', is node state */
		PUT(nd->nc);
		for (int k = 0; k < nd->nc; k++)
		{
			PUT(nd->cpeer[k] + 4); PUT(nd->cend[k]); PUT(nd->cnonblock[k]); PUT(nd->ccloexec[k]); PUT(nd->cfd[k] == nd->timer_fd);
			/* the queue toward this end, and toward the other end when no
			 * live descriptor there will encode it (still waiting to be
			 * accepted is encoded at the listener below) */
			int c = nd->cconn[k], e = nd->cend[k];
			if (c < 0) continue;
			n = put_conn_end(w, n, sizeof g_world, c, e);
			if (conns[c].closed[1 - e]) n = put_conn_end(w, n, sizeof g_world, c, 1 - e);
		}
		/* eventfds: a non-zero counter is poll-ready, and the flag decides
		 * whether an empty read returns EAGAIN */
		PUT(nd->nefd);
		for (int k = 0; k < nd->nefd; k++) { PUT8(nd->efdval[k]); PUT(nd->efdnb[k]); PUT(nd->fdflag[nd->efd[k] - 500]); }
		PUT(nd->listener_fd >= 0); PUT(nd->ls_nonblock); PUT(nd->ls_cloexec);
		/* plain sockets (made, not yet dialled or listening), by their
		 * flags in fd order: an F_GETFL or F_GETFD on one reads them */
		for (int f = 0; f < nd->next_fd - 500 && f < MAXOWNED; f++)
			if (nd->fdflag[f] & FD_PLAIN) PUT(nd->fdflag[f]);
		PUT(0xfd);
		/* the socket options set, each with its fd's class, as getsockopt
		 * reads them */
		for (int f = 0; f < nd->next_fd - 500 && f < MAXOWNED; f++)
			if (nd->sockopt[f])
			{
				unsigned char c[2]; fd_class(nd, 500 + f, c);
				PUT(c[0]); PUT(c[1]); PUT(nd->sockopt[f]);
			}
		PUT(0xfd);
		/* second numbers for connections (dup), by the slot they name
		 * and their own FD_CLOEXEC, in fd order */
		for (int f = 0; f < nd->next_fd - 500 && f < MAXOWNED; f++)
			if (nd->dupof[f]) { PUT(cfd_slot((Node *) nd, nd->dupof[f])); PUT(nd->fdflag[f]); }
		PUT(0xfd);
		/* epoll instances: each one's interest list, by requested events,
		 * the watched fd's class and the user data epoll_wait returns.  The
		 * data is how the loop routes a ready fd to its handler, so two
		 * worlds that differ in it can differ in what the loop does. */
		PUT(nd->nep);
		for (int e = 0; e < nd->nep; e++)
		{
			const Epoll *ep = &nd->ep[e];
			PUT(nd->fdflag[ep->epfd - 500]);
			PUT(ep->nit);
			for (int k = 0; k < ep->nit; k++)
			{
				unsigned char c[2]; fd_class(nd, ep->fd[k], c);
				PUT(ep->events[k]); PUT(ep->events[k] >> 8); PUT(c[0]); PUT(c[1]); PUT8(ep->data[k]);
			}
		}
	}
	PUT(0xfe);
	/* connections waiting at each listener, grouped by dialler and
	 * oldest first within a dialler (an accept names the dialler and takes
	 * its oldest), each with the bytes already sent on it: an unaccepted
	 * connection has no descriptor at the acceptor to carry them */
	for (int d = 0; d < M.nodes; d++)
		for (int src = -1; src < M.nodes; src++)
		{
			for (int k = 0; k < N[d].npend; k++)
			{
				int c = N[d].pend[k];
				if (conns[c].node[0] != src) continue;
				PUT(src + 4);
				n = put_conn_end(w, n, sizeof g_world, c, 1);
			}
			PUT(0);
		}
	/* a connection whose every end is closed but whose queue still holds
	 * bytes cannot occur: conn_maybe_free() discards it */
	PUT(nrep);
	for (int i = 0; i < nrep; i++) { PUT(rep_src[i]); PUT(rep_val[i]); }
	PUT(bud.timeouts); PUT(bud.crashes); PUT(bud.drops); PUT(bud.restarts); PUT(bud.short_writes);
	PUT(bud.partitions);
	for (int i = 0; i < M.nodes; i++) for (int j = 0; j < M.nodes; j++) PUT(g_cut[i][j]);
	for (int i = 0; i < M.nodes; i++) PUT8(g_clk[i]);
	PUT(bud.eio); PUT(bud.enospc);
#undef PUT8
#undef PUT
	g_world_len = n;
	fp128(w, n, fp);
}

static void
reset_world(void)
{
	memset(bag, 0, sizeof bag); memset(conns, 0, sizeof conns);
	nrep = 0; g_fault_node = -1; g_race_node = -1;
	bud.timeouts = M.timeouts; bud.crashes = M.crashes; bud.drops = M.drops;
	bud.restarts = M.restarts; bud.short_writes = M.short_writes; bud.partitions = M.partitions;
	memset(g_cut, 0, sizeof g_cut);
	for (int i = 0; i < MAXNODES; i++) g_clk[i] = CLK_BASE;
	bud.eio = M.eio; bud.enospc = M.enospc;
	for (int i = 0; i < M.nodes; i++) { fs_init(&N[i].fs); N[i].undo_kind = 0; }
}

/* One step the search can take at a quiescent point.  The list is
 * rebuilt in the same order on every replay, so an index names an
 * action; the shortcut keeps action lists small enough to replay. */
enum { A_ACCEPT, A_DELIVER, A_EOF, A_TIMEOUT, A_CRASH, A_DROP, A_WAKE, A_RESTART, A_RUN, A_STEP, A_CONNECT, A_SHORT, A_CUT, A_HEAL, A_EXPIRE, A_FAIL };
/* an enabled action; arg is the polled index of a channel WAKE, the
 * connection of a DROP, and -1 otherwise */
typedef struct { int kind, node, peer, thr, arg; } Act;

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
 *   - RUN: a thread holding an answer (BK_DONE) gets it and runs to its
 *     next visible operation.  Only threads of a node with more than one
 *     thread hold answers, so which of them runs next is a search choice.
 *   - STEP: a node with a running thread is driven, alone, until it is
 *     quiescent or wakes another (drive_node).  Offered only when two or
 *     more nodes have one (see settle()), which node steps first is a
 *     search choice; while a node
 *     waits for its STEP, its other actions wait too, since the world is
 *     not quiescent.
 * A futex wait (BK_FUTEX) has no environment action: it is woken only by
 * another thread's FUTEX_WAKE, which then holds the waiter's answer for a
 * RUN.  Each per-thread action carries the thread's slot index (thr),
 * stable across replay because threads are allocated in notification
 * order.  Every environment action is bounded
 * so the search stays finite. */
/* Append an action to out (c holds MAXACT entries) at *c.  More enabled
 * at one point than MAXACT is an error, never a silently dropped action. */
static void
add_act(Act *out, int *c, int kind, int node, int peer, int thr, int arg)
{
	if (*c >= MAXACT) die(1, "more than %d actions enabled at one point", MAXACT);
	out[*c].kind = kind; out[*c].node = node; out[*c].peer = peer; out[*c].thr = thr; out[*c].arg = arg;
	(*c)++;
}

/* The thread of node d whose deadline comes first, or -1 when none of
 * its parked threads has one.  Ties go to the lowest slot. */
static int
first_deadline(int d)
{
	const Node *nd = &N[d]; int best = -1;
	if (!nd->alive) return -1;
	for (int ti = 0; ti < nd->nth; ti++)
	{
		const Thread *th = &nd->th[ti];
		if (!th->inuse || !th->has_dl || !timed_kind(th->block_kind)) continue;
		if (best < 0 || th->dl < nd->th[best].dl) best = ti;
	}
	return best;
}

/* x * num / den for the drift rates, without overflowing 64 bits for
 * deadlines up to a century; rounded up when up is set */
static unsigned long long
scale_ns(unsigned long long x, unsigned num, unsigned den, int up)
{
	unsigned long long q = x / den, r = x % den;
	unsigned long long lo = r * num;
	return q * num + lo / den + (up && lo % den ? 1 : 0);
}

/* How far node j's clock moves while node d's moves by D, when the
 * deadline of node d fires with drift variant v (A_EXPIRE's arg): -1
 * moves every clock by D; 2j + 0 moves node j's at the slowest rate the
 * drift allows against d's, and 2j + 1 at the fastest. */
static unsigned long long
drift_advance(int d, int j, unsigned long long D, int v)
{
	if (j == d || v < 0 || v / 2 != j) return D;
	unsigned slow = (unsigned) (1000 - M.drift);
	return (v & 1) ? scale_ns(D, 1000, slow, 0) : scale_ns(D, slow, 1000, 1);
}

/* May node d's first deadline fire with drift variant v?  Only if no
 * other node would pass a deadline of its own on the way: that deadline
 * fires first, as its own action.  Deadlines fire in the order the
 * clocks reach them, never one state per tick. */
static int
expire_ok(int d, unsigned long long D, int v)
{
	for (int j = 0; j < M.nodes; j++)
	{
		int tj = j == d ? -1 : first_deadline(j);
		if (tj < 0) continue;
		unsigned long long dl = N[j].th[tj].dl, left = dl > g_clk[j] ? dl - g_clk[j] : 0;
		if (drift_advance(d, j, D, v) > left) return 0;
	}
	return 1;
}

/* Offer A_EXPIRE for the first deadline of each node: with every clock
 * moving alike, and, when the model allows drift, with one other node's
 * clock moving at the slowest or fastest rate it allows. */
static void
enum_expire(Act *out, int *c)
{
	for (int d = 0; d < M.nodes; d++)
	{
		int ti = first_deadline(d);
		if (ti < 0) continue;
		unsigned long long dl = N[d].th[ti].dl, D = dl > g_clk[d] ? dl - g_clk[d] : 0;
		if (expire_ok(d, D, -1)) add_act(out, c, A_EXPIRE, d, -1, ti, -1);
		if (M.drift == 0 || D == 0) continue;
		for (int j = 0; j < M.nodes; j++)
			for (int s = 0; j != d && s < 2; s++)
				if (expire_ok(d, D, 2 * j + s)) add_act(out, c, A_EXPIRE, d, -1, ti, 2 * j + s);
	}
}

/* A CRASH of node d, once for each way its modelled files can be left
 * (fs_crash_choices()): the action's arg picks one. */
static void
add_crashes(Act *out, int *c, int d)
{
	long n = fs_crash_choices(&N[d].fs);
	if (n == FS_LIMIT)
		die(3, "node %d: a crash could leave its modelled files in more than %d ways; sync them more often", d, FS_MAXCRASH);
	for (long k = 0; k < n; k++) add_act(out, c, A_CRASH, d, -1, -1, (int) k);
}

static int
enum_actions(Act *out)
{
	int c = 0;
#define ADD2(k, nd, pr, tr, x) add_act(out, &c, (k), (nd), (pr), (tr), (x))
#define ADD(k, nd, pr, tr) ADD2(k, nd, pr, tr, -1)
	/* nodes still running: the only choice is which steps first */
	for (int d = 0; d < M.nodes; d++)
		if (node_has_running(&N[d])) ADD(A_STEP, d, -1, -1);
	if (c > 0) return c;
	/* a node held mid-step at a crash point: it runs on, or crashes */
	{
		int d = midstep_node();
		if (d >= 0)
		{
			for (int ti = 0; ti < N[d].nth; ti++)
				if (N[d].th[ti].inuse && N[d].th[ti].block_kind == BK_DONE) ADD(A_RUN, d, -1, ti);
			if (bud.crashes > 0) add_crashes(out, &c, d);
			for (int ti = 0; ti < N[d].nth; ti++)
				if (N[d].th[ti].inuse && N[d].th[ti].block_kind == BK_DONE && N[d].undo_kind)
				{
					if (bud.eio > 0) ADD2(A_FAIL, d, -1, ti, 0);
					if (bud.enospc > 0 && N[d].undo_kind == 'w') ADD2(A_FAIL, d, -1, ti, 1);
				}
			return c;
		}
	}
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
					if (pend_from(nd, s) && link_up(s, d) && link_up(d, s)) ADD(A_ACCEPT, d, s, ti);
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
						int peer = nd->cpeer[cs], ci = nd->cconn[cs], e = nd->cend[cs];
						saw_chan = 1;
						/* readable: a queued byte or FIN, a pending reset, or
						 * a peer whose node is gone (the EOF a peer notices) */
						if ((conn_head(ci, e) >= 0 && CONN_UP(ci, e)) ||
							(ci >= 0 && (conns[ci].err[e] || conns[ci].reset || conns[ci].refused)) ||
							peer_lost(ci, e))
							ADD2(A_WAKE, d, peer, ti, k);
					}
					else if (es >= 0)
					{
						if (nd->efdval[es] > 0) ADD(A_WAKE, d, POLL_LISTENER - 1 - es, ti);
					}
					else if (fd == nd->listener_fd)
					{
						int up = 0;
						for (int q = 0; q < nd->npend; q++)
							up |= link_up(conns[nd->pend[q]].node[0], d) && link_up(d, conns[nd->pend[q]].node[0]);
						if (up) ADD(A_WAKE, d, POLL_LISTENER, ti);
					}
					else
					{
						/* a polled fd that is none of this node's connections,
						 * eventfds or its listener is a kernel fd the model does
						 * not track (a self-pipe).  It is never treated
						 * as the listener, so a wake is not offered for it; a loop
						 * that only ever becomes ready through such an fd makes no
						 * progress, which the no-progress check reports. */
					}
				}
				if (saw_chan && bud.timeouts > 0) ADD(A_TIMEOUT, d, TIMER_PEER, ti);
				continue;
			}
			if (th->block_kind == BK_SLEEP) continue;	/* only its deadline wakes it (A_EXPIRE) */
			if (th->block_kind == BK_SYNWAIT)
			{
				/* the link healed: the next SYN gets through */
				if (link_up(d, th->block_peer) && link_up(th->block_peer, d)) ADD(A_RUN, d, -1, ti);
				continue;
			}
			if (th->block_kind == BK_FUTEX) continue;	/* woken by another thread's step */
			if (th->block_kind == BK_DONE || th->block_kind == BK_NEWBORN || th->block_kind == BK_CLONED ||
				th->block_kind == BK_CONNWAIT || th->block_kind == BK_SHORTW)
			{
				ADD(A_RUN, d, -1, ti);
				if (th->block_kind == BK_SHORTW)
				{
					int ks[4], nk = short_counts(parked_write_len(nd, th), ks);
					for (int j = 0; j < nk; j++) ADD2(A_SHORT, d, -1, ti, ks[j]);
				}
				continue;
			}
			int p = th->block_peer;
			if (conn_head(th->block_conn, th->block_end) >= 0 && CONN_UP(th->block_conn, th->block_end))
				ADD(A_DELIVER, d, p, ti);
			else if (peer_lost(th->block_conn, th->block_end)) ADD(A_EOF, d, p, ti);
			if (bud.timeouts > 0) ADD(A_TIMEOUT, d, p, ti);
		}
	}
	enum_expire(out, &c);
	if (bud.crashes > 0)
		for (int d = 0; d < M.nodes; d++)
			if (N[d].alive) add_crashes(out, &c, d);
	if (bud.drops > 0)
		for (int ci = 0; ci < MAXCONNS; ci++)
		{
			/* a reset of a connection between two nodes, offered while it
			 * carries bytes in either direction and is not already reset.
			 * A reset discards both directions and leaves each live end an
			 * ECONNRESET, so there is nothing to reset twice. */
			const Conn *cn = &conns[ci];
			if (!cn->used || cn->reset || cn->node[0] < 0) continue;
			if (conn_has_data(ci, 0) || conn_has_data(ci, 1)) ADD2(A_DROP, cn->node[1], cn->node[0], -1, ci);
		}
	/* the network cuts a directed link between two nodes (budgeted),
	 * or heals one it cut, and the segments held on it may move again */
	for (int x = 0; x < M.nodes; x++)
		for (int y = 0; y < M.nodes; y++)
		{
			if (x == y) continue;
			if (g_cut[x][y]) ADD(A_HEAL, x, y, -1);
			else if (bud.partitions > 0) ADD(A_CUT, x, y, -1);
		}
	/* a non-blocking dial's handshake completes (CONNECT) */
	for (int ci = 0; ci < MAXCONNS; ci++)
		if (conns[ci].used && conns[ci].connecting && link_up(conns[ci].node[0], conns[ci].node[1]) &&
			link_up(conns[ci].node[1], conns[ci].node[0]))
			ADD2(A_CONNECT, conns[ci].node[0], conns[ci].node[1], -1, ci);
	if (bud.restarts > 0)
		for (int d = 0; d < M.nodes; d++)
			if (!N[d].alive && N[d].crashed) ADD(A_RESTART, d, -1, -1);
#undef ADD
#undef ADD2
	return c;
}

/* Wake a thread parked in poll() or epoll_wait(): mark one watched fd
 * ready (POLLIN/EPOLLIN) and answer the call with count 1, consuming no
 * message.  at is the polled index of a channel fd, or -1, and then
 * target names the fd: TIMER_PEER (the fired timer's fd), POLL_LISTENER
 * (the listener), or POLL_LISTENER-1-slot (an eventfd slot).  The thread
 * then reads/accepts that fd and defers again, which the search
 * services.  A poll() park has its pollfd array updated in place; an
 * epoll_wait park (epoll_fd >= 0) instead gets a single struct
 * epoll_event written back, carrying the user data the loop registered
 * for the ready fd -- that data is how the loop routes to the fd's
 * handler, so it is returned verbatim. */
static void
poll_wake(Node *nd, Thread *th, int target, int at)
{
	int hit = at;
	for (int k = 0; hit < 0 && k < th->pollfd_n; k++)
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
	Thread *old = cap_begin(th);
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
	g_cap = old;
}

/* A thread of nd parked in poll or epoll_wait on fds of which some are
 * now ready with no search choice (local_ready()), as when a dial's
 * handshake completes: answer it with every such fd, as the call would
 * have been answered had it been made now.  Returns 1 if it answered. */
static int
poll_recheck(Node *nd, Thread *th)
{
	struct pollfd pfs[MAXPOLL];
	struct __attribute__((packed)) { uint32_t events; uint64_t data; } ev[MAXPOLL];
	int ready = 0;
	for (int k = 0; k < th->pollfd_n; k++)
	{
		pfs[k].fd = th->pollfds[k]; pfs[k].events = th->pollevents[k];
		pfs[k].revents = local_ready(nd, th->pollfds[k], th->pollevents[k]);
		if (!pfs[k].revents) continue;
		if (th->epoll_fd < 0 || ready < th->poll_maxevents)
		{ ev[ready].events = (uint16_t) pfs[k].revents; ev[ready].data = th->polldata[k]; ready++; }
	}
	if (!ready) return 0;
	Thread *old = cap_begin(th);
	size_t want = th->epoll_fd >= 0 ? (size_t) ready * sizeof ev[0] : (size_t) th->pollfd_n * sizeof pfs[0];
	const void *src = th->epoll_fd >= 0 ? (const void *) ev : (const void *) pfs;
	if (cr_write(nd->pid, th->block_pollarr, src, want) != want) cr_fail(nd->id, "poll returns revents");
	th->poll_woke_fd = -1;
	answer(nd, th, ready);
	g_cap = old;
	return 1;
}

/* A_CONNECT: the handshake of non-blocking dial conn completes.  A live
 * peer's listener takes it into its backlog, as a blocking connect's
 * would be; a peer that is down refuses it, leaving ECONNREFUSED for
 * the dialler to read once.  A dialler parked in poll on the socket
 * then sees it ready. */
static void
complete_connect(int conn)
{
	Conn *cn = &conns[conn];
	int peer = cn->node[1];
	cn->connecting = 0;
	if (N[peer].alive)
	{
		if (N[peer].npend >= MAXCONN) die(1, "more than %d connections wait at node %d", MAXCONN, peer);
		cn->closed[1] = 0;
		N[peer].pend[N[peer].npend++] = conn;
	}
	else
	{
		cn->refused = 1; cn->gone[1] = 1; cn->err[0] = ECONNREFUSED;
	}
	Node *nd = &N[cn->node[0]];
	for (int ti = 0; ti < nd->nth; ti++)
		if (nd->th[ti].inuse && nd->th[ti].block_kind == BK_POLL) poll_recheck(nd, &nd->th[ti]);
}

/* A_EXPIRE: node d's first deadline, on thread th, comes due.  Node d's
 * clock moves to the deadline, and every other clock by as much as
 * drift variant v says (drift_advance()).  The thread's call returns
 * as Linux returns it on expiry: poll and epoll_wait with 0 and nothing
 * ready, a sleep with 0, a futex wait and a connect across a cut link
 * with ETIMEDOUT, and select and pselect6 with 0 and their
 * timeout counted down to zero (a timeval and a timespec are both two
 * zero 64-bit words). */
static void
fire_deadline(int d, Thread *th, int v)
{
	Node *nd = &N[d];
	unsigned long long D = th->dl > g_clk[d] ? th->dl - g_clk[d] : 0;
	for (int j = 0; j < M.nodes; j++) g_clk[j] += drift_advance(d, j, D, v);
	th->has_dl = 0;
	Thread *old = cap_begin(th);
	if (th->block_kind == BK_POLL && th->epoll_fd < 0)
	{
		struct pollfd pfs[MAXPOLL];
		for (int k = 0; k < th->pollfd_n; k++)
		{ pfs[k].fd = th->pollfds[k]; pfs[k].events = th->pollevents[k]; pfs[k].revents = 0; }
		size_t want = (size_t) th->pollfd_n * sizeof pfs[0];
		if (cr_write(nd->pid, th->block_pollarr, pfs, want) != want) cr_fail(nd->id, "poll returns revents");
	}
	else if (th->block_kind == BK_SLEEP && th->block_bufaddr)
	{
		static const struct { long long sec, usec; } zero;
		if (cr_write(nd->pid, th->block_bufaddr, &zero, sizeof zero) != sizeof zero) cr_fail(nd->id, "select returns the time left");
	}
	answer(nd, th, th->block_kind == BK_FUTEX || th->block_kind == BK_SYNWAIT ? -ETIMEDOUT : 0);
	g_cap = old;
}

/* Reset connection conn (a DROP): discard every queued message in both
 * directions, mark it reset, and tell both ends at once.  A node parked
 * in recv on it gets ECONNRESET; a node parked in poll on it is woken so
 * its next read gets ECONNRESET.  Modelling a drop as a reset (not a mid-stream byte loss)
 * keeps the process front end faithful to TCP, which never loses a byte
 * from a live stream -- a failure is always visible to both ends. */
static void
reset_conn(int conn)
{
	Conn *cn = &conns[conn];
	int errs[2] = {reset_errno(conn, 0), reset_errno(conn, 1)};
	conn_discard(conn);
	cn->reset = 1;
	for (int e = 0; e < 2; e++) if (!cn->closed[e]) cn->err[e] = errs[e];
	for (int e = 0; e < 2; e++) rst_notify(conn, e);
}

/* Break the connections peers still hold to a node that is restarting.
 * The crashed process's TCP connections died with it; the new process
 * starts with none.  A peer keeps its stale descriptor until it closes
 * it, as with Linux, and learns of the break as an RST: its next read or
 * write on it fails with ECONNRESET, and it must re-dial to reach the
 * new process.  Dials that waited at the crashed listener were already
 * refused when it died. */
static void
break_peer_links(int node)
{
	/* every connection with an end at the restarting node: its old
	 * process's ends were closed with no FIN when it died, so the surviving
	 * end is reset now, as the new host answers its next segment with an
	 * RST (a parked recv sees ECONNRESET, a parked poll wakes) */
	for (int c = 0; c < MAXCONNS; c++)
	{
		Conn *cn = &conns[c];
		/* a dial still handshaking reaches whichever process listens
		 * when it completes */
		if (!cn->used || cn->reset || cn->connecting) continue;
		if (cn->node[0] == node) rst_path(node, cn->node[1]);
		if (cn->node[1] == node) rst_path(node, cn->node[0]);
		if (cn->node[0] == node || cn->node[1] == node) reset_conn(c);
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
			int h = conn_head(th->block_conn, th->block_end);
			if (bag[h].eof)
			{
				/* the FIN: this and every later read returns 0 */
				conns[th->block_conn].eof[th->block_end] = 1; bag[h].used = 0;
				deliver(nd, th, NULL, 0);
				break;
			}
			int k = deliver(nd, th, bag[h].body, bag[h].len);
			if (k < bag[h].len) { memmove(bag[h].body, bag[h].body + k, (size_t) (bag[h].len - k)); bag[h].len -= k; }
			else bag[h].used = 0;
			/* the reader made room: a writer whose buffer was full may
			 * write again, and a poll for POLLOUT there is answered */
			Conn *rc = &conns[th->block_conn];
			int we = 1 - th->block_end;
			if (rc->full[we])
			{
				rc->full[we] = 0;
				Node *wn = rc->node[we] >= 0 ? &N[rc->node[we]] : NULL;
				for (int ti = 0; wn && wn->alive && ti < wn->nth; ti++)
					if (wn->th[ti].inuse && wn->th[ti].block_kind == BK_POLL) poll_recheck(wn, &wn->th[ti]);
			}
			break;
		}
		case A_EOF: deliver(nd, th, NULL, 0); break;
		case A_TIMEOUT:
		{
			static const unsigned char t = 'T';
			bud.timeouts--;
			for (int i = 0; i < M.nodes; i++) g_clk[i] += CLK_TICK;
			if (th->block_kind == BK_ACCEPT)
			{
				/* answer the accept() with a timer connection; the node's
				 * next recv on it returns 'T' (the recv handler) */
				int fd = nd->next_fd++;
				cfd_set(nd, fd, TIMER_PEER, -1, 0);
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
				poll_wake(nd, th, TIMER_PEER, -1);
			}
			else
				deliver(nd, th, &t, 1);
			break;
		}
		case A_WAKE: poll_wake(nd, th, a->peer, a->arg); break;
		case A_RUN:
			if (th->block_kind == BK_NEWBORN || th->block_kind == BK_CLONED) run_stopped(nd, th);
			else if (th->block_kind == BK_SYNWAIT) { th->has_dl = 0; run_connwait(nd, th); }
			else if (th->block_kind == BK_CONNWAIT || th->block_kind == BK_SHORTW) run_connwait(nd, th);
			else run_held(nd, th);
			break;
		case A_CUT: bud.partitions--; g_cut[a->node][a->peer] = 1; break;
		case A_HEAL: g_cut[a->node][a->peer] = 0; break;
		case A_SHORT:
			bud.short_writes--;
			g_short = a->arg;
			run_connwait(nd, th);
			g_short = -1;
			break;
		case A_CRASH: bud.crashes--; crash_node(nd); fs_crash(&nd->fs, a->arg); nd->undo_kind = 0; break;
		case A_FAIL: fail_held(nd, th, a->arg); break;
		case A_DROP: bud.drops--; reset_conn(a->arg); break;
		case A_RESTART:
			bud.restarts--;
			break_peer_links(a->node);	/* the old process's connections are gone */
			if (relaunch(nd)) die(3, "node %d would not restart", a->node);
			break;
		case A_STEP: drive_node(nd); break;
		case A_CONNECT:
			complete_connect(a->arg);
			/* a call parked on the handshake is no longer a choice */
			for (int ti = 0; ti < nd->nth; ti++)
				if (nd->th[ti].inuse && nd->th[ti].block_kind == BK_CONNWAIT &&
					!connwait(nd, nd->th[ti].nr, (const __u64 *) nd->th[ti].args))
					run_connwait(nd, &nd->th[ti]);
			break;
		case A_EXPIRE: fire_deadline(a->node, th, a->arg); break;
	}
	settle();
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
	g_step_rc = -1;
	for (int i = 0; i < M.nodes; i++) { N[i].id = i; if (launch(&N[i])) return -1; }	/* GCOVR_EXCL_LINE: a launch does not fail in a test */
	if (M.client_write >= 0)
	{
		/* the client dials node 0 and sends WRITE(v) on that connection */
		unsigned char w[2] = {'W', (unsigned char) M.client_write};
		int c = conn_new(-1, 0);
		N[0].pend[N[0].npend++] = c;
		bag_add(c, 1, w, 2, 0);
	}
	settle();
	/* the refinement steps are checked at quiescent worlds only; the
	 * nodes' STEPs between them are part of one step of the spec */
	if (g_step_refine) cmc_refine_reset();
	if (g_step_refine && world_settled() && midstep_node() < 0) { g_step_rc = step_refine_here(&g_step_name); if (g_step_rc != -1) return 0; }
	for (int s = 0; s < seqlen; s++)
	{
		Act a[MAXACT]; int na = enum_actions(a);
		if (seq[s] < 0 || seq[s] >= na)
			die(1, "replay diverged at step %d: action %d of %d (is the target deterministic?)", s, seq[s], na);
		apply(&a[seq[s]]);
		if (g_step_refine && world_settled() && midstep_node() < 0) { g_step_rc = step_refine_here(&g_step_name); if (g_step_rc != -1) return 0; }
	}
	*nact = enum_actions(acts);
	return 0;
}

static void
kill_all(void)
{
	for (int i = 0; i < M.nodes; i++)
		if (N[i].pid > 0) kill_node(&N[i]);
	/* close this run's fds: a long search would otherwise exhaust them */
	for (int i = 0; i < M.nodes; i++)
	{
		if (N[i].listener > 0) close(N[i].listener);
		if (N[i].fd3[0] > 0) close(N[i].fd3[0]);
		if (N[i].fd4[0] > 0) close(N[i].fd4[0]);
		for (int k = 0; k < 2; k++) if (N[i].hook_w[k] > 0) close(N[i].hook_w[k]);
		N[i].listener = N[i].fd3[0] = N[i].fd4[0] = N[i].hook_w[0] = N[i].hook_w[1] = -1;
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
	static const char *k[] = {"accept", "deliver", "eof", "timeout", "crash", "drop", "wake", "restart", "run", "step", "connect", "short", "cut", "heal", "expire", "fail"};
	if (a->kind == A_CRASH && a->arg > 0) snprintf(b, n, "crash n%d (files kept: way %d)", a->node, a->arg);
	else if (a->kind == A_CRASH) snprintf(b, n, "crash n%d", a->node);
	else if (a->kind == A_FAIL) snprintf(b, n, "fail n%d t%d (%s)", a->node, a->thr, a->arg ? "ENOSPC" : "EIO");
	else if (a->kind == A_STEP) snprintf(b, n, "step n%d", a->node);
	else if (a->kind == A_RUN) snprintf(b, n, "run n%d t%d", a->node, a->thr);
	else if (a->kind == A_CUT || a->kind == A_HEAL) snprintf(b, n, "%s n%d->n%d", k[a->kind], a->node, a->peer);
	else if (a->kind == A_SHORT && a->arg == 0) snprintf(b, n, "full n%d t%d (EAGAIN)", a->node, a->thr);
	else if (a->kind == A_SHORT) snprintf(b, n, "short n%d t%d (%d bytes)", a->node, a->thr, a->arg);
	else if (a->kind == A_RESTART) snprintf(b, n, "restart n%d", a->node);
	else if (a->kind == A_TIMEOUT) snprintf(b, n, "timeout n%d", a->node);
	else if (a->kind == A_EXPIRE && a->arg < 0) snprintf(b, n, "expire n%d t%d", a->node, a->thr);
	else if (a->kind == A_EXPIRE)
		snprintf(b, n, "expire n%d t%d, n%d clock %s", a->node, a->thr, a->arg / 2, (a->arg & 1) ? "fast" : "slow");
	else if (a->kind == A_DROP) snprintf(b, n, "reset n%d<->n%d", a->peer, a->node);
	else if (a->kind == A_WAKE && a->peer == POLL_LISTENER) snprintf(b, n, "wake n%d listener", a->node);
	else if (a->kind == A_WAKE && a->peer < POLL_LISTENER) snprintf(b, n, "wake n%d eventfd%d", a->node, POLL_LISTENER - 1 - a->peer);
	else if (a->peer < 0) snprintf(b, n, "%s client->n%d", k[a->kind], a->node);
	else snprintf(b, n, "%s n%d->n%d", k[a->kind], a->peer, a->node);
	return b;
}

/* Replay seq from a fresh launch, printing each action by name.  The
 * environment's actions are numbered; a STEP (which of the nodes that
 * were running together went first) is listed between them unnumbered.
 * Two replays: the first counts, so the heading can say how many. */
static void
print_trace(const int *seq, int len)
{
	char nm[96];
	Act a[MAXACT]; int na, n = 0, k = 0;
	for (int pass = 0; pass < 2; pass++)
	{
		run_seq(NULL, 0, a, &na);
		if (pass == 1) printf("Error: The behavior up to this point is (%d actions):\n", n);
		for (int i = 0; i < len; i++)
		{
			if (seq[i] >= na) die(1, "replay of the counterexample diverged at step %d", i);
			int step = a[seq[i]].kind == A_STEP;
			if (pass == 0) n += !step;
			else if (step) printf("       %s\n", act_name(&a[seq[i]], nm, sizeof nm));
			else printf("    %d. %s\n", ++k, act_name(&a[seq[i]], nm, sizeof nm));
			apply(&a[seq[i]]);
			na = enum_actions(a);
		}
		if (pass == 0) kill_all();
	}
}

/* report a violation: the action sequence, by name, and the world there */
static void
report(int which, const int *seq, int len)
{
	printf("Error: Invariant %s is violated.\n", M.inv[which].name);
	print_trace(seq, len);
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
	print_trace(seq, len);
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

/* report a node that a fault signal killed (signal_stop): the trace to
 * it and the world there, as for a violated invariant */
static void
report_fault(const int *seq, int len)
{
	printf("Error: node %d was killed by signal %d (%s), a fault the program raised (an abort, a failed assert, a bad access).\n",
		   g_fault_node, g_fault_sig, strsignal(g_fault_sig));
	report_refine(seq, len);
}

/* A data race the race runtime found: both accesses' code addresses
 * (nodes run without address randomisation, so addr2line -e BINARY
 * names them) and the schedule that reached it. */
static void
report_race(const int *seq, int len)
{
	printf("Error: data race on node %d at address %#" PRIx64 ": an access at %#" PRIx64 " and one at %#" PRIx64 " that no synchronisation orders.\n",
		   g_race_node, g_race_addr, g_race_pc[0], g_race_pc[1]);
	report_refine(seq, len);
}

/* Is a node alive that has run more than one thread since it was
 * launched?  Such a world is never merged with another (see the search
 * in cmc_proc_main).  Its threads' order decides what is in memory, and
 * the encoding holds a thread's position (its call, arguments,
 * instruction and stack pointers) but not that memory: two iterations of
 * a loop park at one point, glibc gives a thread the stack of one it has
 * joined, and a value two threads update between their calls is seen by
 * no call.  The flag stays set once the node's extra threads have exited,
 * since what they wrote stays in memory; it clears when the process dies
 * or a RESTART starts a fresh one. */
static int
world_threaded(void)
{
	for (int i = 0; i < M.nodes; i++)
		if (N[i].alive && N[i].threaded) return 1;
	return 0;
}

/* The bound on worlds searched as a tree (world_threaded) when no
 * --max-states is given: each one is a node of the tree, and a threaded
 * program that never stops would otherwise grow it forever.  A build may
 * set it (-DCMC_TREE_CAP=n), so a gate can reach it; test/docs_gate.sh
 * checks that cmc(1) states this default. */
#ifndef CMC_TREE_CAP
#define CMC_TREE_CAP 1000000
#endif

/* the BFS frontier: action sequences, appended in one growing pool */
static int *qpool; static size_t qcap, qused;
/* an entry: its sequence in qpool, the environment actions in it (its
 * depth: STEPs are not counted), and the fingerprint of the last
 * quiescent world on its path */
typedef struct { size_t off; int len, depth; uint64_t pfp0, pfp1; } QEnt;
static QEnt *Q; static size_t Qcap, qh, qt;
/* Entries one STEP from a world being expanded: same depth, so they are
 * taken before anything left in Q (a 0-1 breadth-first search).  Taking
 * them in order of sequence length instead would reach a world first by
 * a path with more environment actions and fewer STEPs, and record it
 * deeper than it is. */
static QEnt *Z; static size_t Zcap, zn;

static void
q_push(const int *seq, int len, int next, int depth, uint64_t pfp0, uint64_t pfp1, int step)
{
	if (step && zn == Zcap) { Zcap = Zcap ? 2 * Zcap : 256; Z = realloc(Z, Zcap * sizeof *Z); if (!Z) die(1, "out of memory for the frontier"); }
	if (!step && qt == Qcap) { Qcap = Qcap ? 2 * Qcap : 4096; Q = realloc(Q, Qcap * sizeof *Q); if (!Q) die(1, "out of memory for the frontier"); }
	if (qused + (size_t) len + 1 > qcap)
	{
		while (qused + (size_t) len + 1 > qcap) qcap = qcap ? 2 * qcap : 65536;
		qpool = realloc(qpool, qcap * sizeof *qpool); if (!qpool) die(1, "out of memory for the frontier");	/* GCOVR_EXCL_LINE: realloc of the frontier */
	}
	QEnt *q = step ? &Z[zn++] : &Q[qt++];
	q->off = qused; q->len = len + 1; q->depth = depth; q->pfp0 = pfp0; q->pfp1 = pfp1;
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
	if (!Q) die(1, "out of memory for the frontier");	/* GCOVR_EXCL_LINE: the frontier's first block */
	Q[0].off = 0; Q[0].len = 0; Q[0].depth = 0; Q[0].pfp0 = 0; Q[0].pfp1 = 0; qt = 1;
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
	for (int i = 0; i < MAXNODES; i++) { N[i].binary = M.binary; N[i].listener = N[i].fd3[0] = N[i].fd4[0] = N[i].hook_w[0] = N[i].hook_w[1] = -1; }
	if (seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &g_sz))	/* GCOVR_EXCL_LINE: the gates run where seccomp notification exists */
	{ fprintf(stderr, "cmc: seccomp user notification is not available: %s\n", strerror(errno)); return 3; }
	g_req = calloc(1, g_sz.seccomp_notif); g_resp = calloc(1, g_sz.seccomp_notif_resp);
	g_ans = calloc(1, g_sz.seccomp_notif_resp);
	if (!g_req || !g_resp || !g_ans) die(1, "out of memory");	/* GCOVR_EXCL_LINE: a few hundred bytes */
	double t0 = now_s();

	printf("cmc: model file %s, binary %s, %d node%s, %s\n", po->model_file, M.binary, M.nodes,
		   M.nodes == 1 ? "" : "s", po->simulate ? "random simulation" : "breadth-first search");
	printf("  environment: client_write %d, timeouts %d, crashes %d, drops %d, restarts %d",
		   M.client_write, M.timeouts, M.crashes, M.drops, M.restarts);
	if (M.short_writes) printf(", short_writes %d", M.short_writes);
	if (M.partitions) printf(", partitions %d", M.partitions);
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
		/* a walk takes up to depth environment actions and the STEPs
		 * between them, which a crossing wake can make more than one per
		 * node, so the sequence grows as it needs */
		size_t seqcap = (size_t) (depth + 1) * (MAXNODES + 1);
		int *seq = malloc(seqcap * sizeof *seq);
		if (!seq) die(1, "out of memory");	/* GCOVR_EXCL_LINE: a sequence of a few hundred ints */
		for (w = 0; w < po->walks; w++)
		{
			Act a[MAXACT]; int na, len = 0, envlen = 0;
			if (run_seq(NULL, 0, a, &na)) die(3, "a node would not start");	/* GCOVR_EXCL_LINE: a launch does not fail in a test */
			if (po->refine_steps) cmc_refine_reset();
			for (;;)
			{
				if (g_fault_node < 0 && g_race_node < 0 && !world_settled())
				{
					/* between STEPs: no state to judge yet (world_settled) */
					if ((size_t) len >= seqcap)
					{
						seqcap *= 2;
						seq = realloc(seq, seqcap * sizeof *seq);
						if (!seq) die(1, "out of memory");	/* GCOVR_EXCL_LINE: a sequence of a few hundred ints */
					}
					int pick = (int) (next_rand() % (uint64_t) na);
					seq[len++] = pick;
					apply(&a[pick]);
					na = enum_actions(a);
					continue;
				}
				int which = -1; states++;
				int mid = midstep_node() >= 0;
				if (g_fault_node >= 0 || g_race_node >= 0)
				{
					kill_all();
					printf("Walk %" PRIu64 " (seed %" PRIu64 "):\n", w + 1, po->seed);
					if (g_fault_node >= 0) report_fault(seq, len);
					else report_race(seq, len);
					free(seq);
					return 1;
				}
				if (!mid && !check_invariants(&which))
				{
					if (which == -2) { kill_all(); return 3; }
					kill_all();
					printf("Walk %" PRIu64 " (seed %" PRIu64 "):\n", w + 1, po->seed);
					report(which, seq, len);
					free(seq);
					return 1;
				}
				if (po->refine && !mid)
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
				if (na == 0 || envlen >= depth) break;
				if ((size_t) len >= seqcap)
				{
					seqcap *= 2;
					seq = realloc(seq, seqcap * sizeof *seq);
					if (!seq) die(1, "out of memory");	/* GCOVR_EXCL_LINE: a sequence of a few hundred ints */
				}
				int pick = (int) (next_rand() % (uint64_t) na);
				seq[len++] = pick; envlen++;
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
	 * (stateless), so a world costs one replay of its whole prefix.  A
	 * world with a live node that has run threads is not merged with any
	 * other (world_threaded): it is neither looked up in the seen set nor
	 * added to it, and counts as distinct.  The search below such a world
	 * is a tree that follows every order of the threads' visible
	 * operations, whatever the encoding misses about their memory.  A
	 * stutter (a world that encodes like its parent) is expanded there
	 * too, since the thread that ran may be at a point the encoding
	 * cannot see.  CMC_TREE_CAP bounds the tree. */
	size_t distinct = 0, cut = 0, replays = 0, ntree = 0; int maxlen = 0;
	size_t progressed = 0, stutter_leaves = 0;	/* no-progress (self-loop) detection */
	const char *why = NULL;
	g_step_refine = po->refine_steps;	/* C2 checks run inside run_seq's replay */
	q_root();
	while (zn > 0 || qh < qt)
	{
		QEnt e = zn > 0 ? Z[--zn] : Q[qh++];
		int *seq = malloc(((size_t) e.len + 1) * sizeof *seq);
		if (!seq) die(1, "out of memory");	/* GCOVR_EXCL_LINE: a sequence of a few hundred ints */
		if (e.len) memcpy(seq, qpool + e.off, (size_t) e.len * sizeof *seq);
		Act a[MAXACT]; int na = 0;
		if (run_seq(seq, e.len, a, &na)) die(3, "a node would not start");	/* GCOVR_EXCL_LINE: a launch does not fail in a test */

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

		if (g_fault_node >= 0) { kill_all(); report_fault(seq, e.len); free(seq); return 1; }
		if (g_race_node >= 0) { kill_all(); report_race(seq, e.len); free(seq); return 1; }
		if (!world_settled())
		{
			/* between two nodes' STEPs: not a state the invariants judge
			 * (world_settled), and not counted; expanded by its STEPs */
			uint64_t fp[2]; world_fp(fp);
			int isnew = !running_fresh() || world_threaded() || mark(fp, g_world, g_world_len);
			kill_all();
			if (isnew) for (int i = 0; i < na; i++) q_push(seq, e.len, i, e.depth, e.pfp0, e.pfp1, 1);
			free(seq);
			if (po->time_limit > 0 && now_s() - t0 > po->time_limit) { why = "time limit reached"; break; }
			continue;
		}
		replays++;
		int which = -1; int ok = midstep_node() >= 0 ? 1 : check_invariants(&which);
		if (which == -2) { kill_all(); free(seq); return 3; }
		if (ok && po->refine && !po->refine_steps && midstep_node() < 0)
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
		int tree = world_threaded();
		/* a mid-step world is never merged: its state hook lags the calls
		 * the node has made, so two different histories could share it */
		int isnew = tree || midstep_node() >= 0 || mark(fp, g_world, g_world_len);
		if (isnew) { distinct++; ntree += tree; if (e.depth > maxlen) maxlen = e.depth; }
		/* did this action make progress?  A non-root entry whose reached
		 * world equals its parent's is a stutter: the action was enabled
		 * but changed nothing the fingerprint sees, a self-loop the
		 * program cannot leave by that step.  If every reachable path is a
		 * stutter, the program never advanced past its initial state and a
		 * safety-only pass is vacuous (reported below). */
		if (e.depth > 0)
		{
			if (fp[0] == e.pfp0 && fp[1] == e.pfp1) stutter_leaves++;
			else progressed++;
		}
		kill_all();
		if (!ok) { report(which, seq, e.len); free(seq); return 1; }
		if (isnew && na > 0)
		{
			if (depth >= 0 && e.depth >= depth) cut++;
			else for (int i = 0; i < na; i++) q_push(seq, e.len, i, e.depth + 1, fp[0], fp[1], 0);
		}
		free(seq);
		if (po->max_states && distinct >= po->max_states) { why = "max states reached"; break; }
		if (!po->max_states && ntree >= CMC_TREE_CAP)
		{
			static char tbuf[160];
			snprintf(tbuf, sizeof tbuf, "%d worlds with a multi-threaded node were searched without merging (set the bound with --max-states)", CMC_TREE_CAP);
			why = tbuf; break;
		}
		if (po->time_limit > 0 && now_s() - t0 > po->time_limit) { why = "time limit reached"; break; }
	}
	double dt = now_s() - t0;
	if (!why && cut) why = "depth bound reached";
	if (why)
	{
		printf("INCOMPLETE (no violation found): %s; %zu distinct states, %zu left on queue, %zu cut at depth %ld, %.1f s\n",
			   why, distinct, qt - qh + zn, cut, depth, dt);
		return 2;
	}
	/* The search covered every order of the visible operations, but that
	 * covers every execution only for a program the race runtime checked
	 * (free of data races on every path) running sequentially consistent
	 * atomics.  Otherwise neither a pass nor an unmet `eventually` is a
	 * verdict: some execution may switch threads between two plain
	 * accesses, or reorder a weak atomic, where the search did not. */
	if (g_unchecked_node || g_weak_seen || g_try_seen)
	{
		if (g_unchecked_node)
			printf("INCOMPLETE (no violation found): node %d ran more than one thread without the race runtime, so its memory accesses between synchronisation points were not checked.  Build it with -fsanitize=thread and link libcmc-race.a (cmc(7), THREADS).  %zu distinct states, %.1f s\n",
				   g_unchecked_node - 1, distinct, dt);
		else if (g_try_seen)
			printf("INCOMPLETE (no violation found): a try or timed lock, or a semaphore that admits more than one thread, ran between threads, and the search does not switch threads inside the sections where its other outcome lies (cmc(7), THREADS).  %zu distinct states, %.1f s\n",
				   distinct, dt);
		else
			printf("INCOMPLETE (no violation found): an atomic weaker than seq_cst ran, and the search explores only sequentially consistent orders (cmc(7), THREADS).  %zu distinct states, %.1f s\n",
				   distinct, dt);
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
	if (ntree)
		printf("  %zu of them have a multi-threaded node and were searched as a tree, without merging.\n", ntree);
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
