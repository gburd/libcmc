/*
 * mc.c: the model-file-driven process-mode checker
 * (experiments/process-mode/DESIGN.md M3; docs/model-file.md;
 * docs/background/decision-model-file.md).
 *
 * This is the shape libcmc is becoming: one engine, a declarative model
 * file, and the shipping binary as the protocol.  mc reads a .model file
 * (what to run, the environment to explore, the invariants), runs the
 * target under process-mode syscall interception, does a breadth-first
 * search over the environment's choices with canonical-state dedup, and
 * evaluates the invariants -- written as a small expression DSL over the
 * node's canonical state and the log of client inputs -- in every state.
 * Nothing about the protocol is written here or in the model file: the
 * running binary is the transition relation.
 *
 * M3 scope: one node, state = canon (the target emits its canonical state
 * on fd 3), a client-insert alphabet, and DSL invariants of the forms the
 * schema's byte-accessor level promises.  Multi-node, crashes, the reply
 * log, named fields and the C escape hatch are later milestones.
 *
 * Usage: mc MODELFILE BINARY
 * Exit 0 = all invariants held and the search was complete; 1 = a
 * violation (shortest client-input prefix printed) or a full table; 2 =
 * incomplete (the depth bound stopped the search, no violation found);
 * 3 = usage or setup error.
 *
 * Verified on Linux 6.18, glibc 2.44, -no-pie targets.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
#ifndef SECCOMP_USER_NOTIF_FLAG_CONTINUE
#define SECCOMP_USER_NOTIF_FLAG_CONTINUE (1UL << 0)
#endif
static int seccomp(unsigned op, unsigned flags, void *a) { return (int) syscall(SYS_seccomp, op, flags, a); }

/* =================================================================== */
/* model file                                                          */
/* =================================================================== */
#define MAXINV 8
#define MAXCLIENTS 16
#define SNAP 256

typedef struct { char name[32]; char expr[256]; } Inv;
static struct {
	int nodes;
	int clients;				/* alphabet size: insert keys 1..clients */
	int depth;
	char statekind[16];			/* "canon" (M3) */
	Inv inv[MAXINV]; int ninv;
} M;

static char *
trim(char *s)
{
	while (*s && isspace((unsigned char) *s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char) e[-1])) *--e = 0;
	return s;
}

static int
load_model(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) { fprintf(stderr, "mc: cannot open %s\n", path); return -1; }
	char line[512], sect[32] = "";
	M.nodes = 1; M.clients = 1; M.depth = 4;
	snprintf(M.statekind, sizeof M.statekind, "canon");
	while (fgets(line, sizeof line, f))
	{
		char *s = trim(line);
		if (!*s || *s == '#') continue;
		if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; snprintf(sect, sizeof sect, "%s", s + 1); } continue; }
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0; char *k = trim(s), *v = trim(eq + 1);
		/* a quoted value keeps its inner text verbatim (it may contain #);
		 * an unquoted value ends at an inline comment */
		if (*v == '"') { v++; char *qc = strrchr(v, '"'); if (qc) *qc = 0; }
		else { char *h = strchr(v, '#'); if (h) *h = 0; v = trim(v); }
		if (!strcmp(sect, "target"))
		{
			if (!strcmp(k, "nodes")) M.nodes = atoi(v);
			else if (!strcmp(k, "state")) snprintf(M.statekind, sizeof M.statekind, "%s", v);
		}
		else if (!strcmp(sect, "environment"))
		{
			if (!strcmp(k, "clients")) M.clients = atoi(v);
			else if (!strcmp(k, "depth")) M.depth = atoi(v);
		}
		else if (!strcmp(sect, "invariants"))
		{
			if (M.ninv < MAXINV)
			{ snprintf(M.inv[M.ninv].name, sizeof M.inv[0].name, "%s", k);
			  snprintf(M.inv[M.ninv].expr, sizeof M.inv[0].expr, "%s", v); M.ninv++; }
		}
	}
	fclose(f);
	if (M.nodes != 1) { fprintf(stderr, "mc: M3 supports nodes=1 only\n"); return -1; }
	if (strcmp(M.statekind, "canon")) { fprintf(stderr, "mc: M3 supports state=canon only\n"); return -1; }
	if (M.clients < 1 || M.clients > MAXCLIENTS) { fprintf(stderr, "mc: clients out of range\n"); return -1; }
	return 0;
}

/* =================================================================== */
/* invariant DSL                                                       */
/*                                                                     */
/* Grammar (M3 subset):                                                */
/*   expr   := quant | cmp | '(' expr ')' | 'not' expr                 */
/*             | expr 'and' expr | expr 'or' expr                      */
/*   quant  := ('all'|'some') IDENT 'in' set ':' expr                  */
/*   cmp    := term 'in' set | term OP term                            */
/*   term   := NUMBER | IDENT ( '[' term ']' )?                        */
/*   set    := 'acted' | 'node' '[' NUMBER ']'                         */
/* IDENT bound by a quantifier ranges over the bytes of its set.       */
/*                                                                     */
/* Evaluation context: the current state's canonical bytes per node    */
/* and the `acted` multiset (client inputs on the path here).          */
/* =================================================================== */

typedef struct {
	const unsigned char *node0; int node0_len;		/* node[0] canonical bytes */
	const unsigned char *acted; int acted_len;		/* client inputs so far */
	/* one bound variable is enough for the M3 grammar (single quantifier) */
	char varname[32]; long varval; int varset;
} Ctx;

/* a minimal recursive-descent parser/evaluator; returns 1 true, 0 false,
 * -1 parse error.  `p` advances. */
static void skipws(const char **p) { while (**p && isspace((unsigned char) **p)) (*p)++; }
static int word(const char **p, const char *w)
{
	skipws(p);
	size_t n = strlen(w);
	if (strncmp(*p, w, n) == 0 && !isalnum((unsigned char)(*p)[n]) && (*p)[n] != '_')
	{ *p += n; return 1; }
	return 0;
}

static int eval_expr(const char **p, Ctx *c);

/* a "set" yields a pointer+len of bytes to range/search over */
static int
get_set(const char **p, Ctx *c, const unsigned char **buf, int *len)
{
	skipws(p);
	if (word(p, "acted")) { *buf = c->acted; *len = c->acted_len; return 0; }
	if (word(p, "node"))
	{
		skipws(p); if (**p != '[') return -1; (*p)++;
		skipws(p); int idx = (int) strtol(*p, (char **) p, 10);
		skipws(p); if (**p != ']') return -1; (*p)++;
		if (idx != 0) return -1;	/* M3: node[0] only */
		*buf = c->node0; *len = c->node0_len; return 0;
	}
	return -1;
}

/* a "term" yields a long value */
static int
get_term(const char **p, Ctx *c, long *out)
{
	skipws(p);
	if (isdigit((unsigned char) **p)) { *out = strtol(*p, (char **) p, 10); return 0; }
	/* an identifier: the bound variable */
	char id[32]; int n = 0;
	while ((isalnum((unsigned char) **p) || **p == '_') && n < 31) id[n++] = *(*p)++;
	id[n] = 0;
	if (n == 0) return -1;
	if (c->varset && !strcmp(id, c->varname)) { *out = c->varval; return 0; }
	return -1;
}

static int
eval_quant_or_cmp(const char **p, Ctx *c)
{
	skipws(p);
	const char *save = *p;
	int isall = word(p, "all"), issome = isall ? 0 : word(p, "some");
	if (isall || issome)
	{
		skipws(p);
		char var[32]; int n = 0;
		while ((isalnum((unsigned char) **p) || **p == '_') && n < 31) var[n++] = *(*p)++;
		var[n] = 0;
		if (!word(p, "in")) return -1;
		const unsigned char *set; int slen;
		if (get_set(p, c, &set, &slen)) return -1;
		skipws(p); if (**p != ':') return -1; (*p)++;
		/* the body is the rest up to an enclosing ')' or end; evaluate it
		 * once per element by binding var */
		const char *body = *p;
		int result = isall ? 1 : 0;
		Ctx inner = *c; inner.varset = 1; snprintf(inner.varname, sizeof inner.varname, "%s", var);
		for (int i = 0; i < slen; i++)
		{
			const char *bp = body;
			inner.varval = set[i];
			int r = eval_expr(&bp, &inner);
			if (r < 0) return -1;
			if (isall && !r) { result = 0; }
			if (issome && r) { result = 1; }
			*p = bp;	/* leave p past the body */
		}
		if (slen == 0) *p = body, eval_expr(p, &inner);	/* advance past body once */
		return result;
	}
	*p = save;
	/* a comparison: term ('in' set | OP term) */
	long a;
	if (get_term(p, c, &a)) return -1;
	skipws(p);
	if (word(p, "in"))
	{
		const unsigned char *set; int slen;
		if (get_set(p, c, &set, &slen)) return -1;
		for (int i = 0; i < slen; i++) if (set[i] == a) return 1;
		return 0;
	}
	const char *op = *p;
	if (!strncmp(op, "==", 2)) { *p += 2; long b; if (get_term(p, c, &b)) return -1; return a == b; }
	if (!strncmp(op, "!=", 2)) { *p += 2; long b; if (get_term(p, c, &b)) return -1; return a != b; }
	if (!strncmp(op, "<=", 2)) { *p += 2; long b; if (get_term(p, c, &b)) return -1; return a <= b; }
	if (!strncmp(op, ">=", 2)) { *p += 2; long b; if (get_term(p, c, &b)) return -1; return a >= b; }
	if (*op == '<') { (*p)++; long b; if (get_term(p, c, &b)) return -1; return a < b; }
	if (*op == '>') { (*p)++; long b; if (get_term(p, c, &b)) return -1; return a > b; }
	return -1;
}

static int
eval_unary(const char **p, Ctx *c)
{
	skipws(p);
	if (word(p, "not")) { int r = eval_unary(p, c); return r < 0 ? r : !r; }
	if (**p == '(') { (*p)++; int r = eval_expr(p, c); skipws(p); if (**p == ')') (*p)++; return r; }
	return eval_quant_or_cmp(p, c);
}

static int
eval_expr(const char **p, Ctx *c)
{
	int r = eval_unary(p, c);
	if (r < 0) return r;
	for (;;)
	{
		skipws(p);
		if (word(p, "and")) { int b = eval_unary(p, c); if (b < 0) return b; r = r && b; }
		else if (word(p, "or")) { int b = eval_unary(p, c); if (b < 0) return b; r = r || b; }
		else break;
	}
	return r;
}

static int
eval_invariant(const char *expr, Ctx *c)
{
	const char *p = expr;
	int r = eval_expr(&p, c);
	return r;					/* 1 holds, 0 violated, <0 parse error */
}

/* =================================================================== */
/* process-mode interception (as in search2.c)                         */
/* =================================================================== */
static pid_t g_child;
static void cr_write(uint64_t addr, const void *buf, size_t n)
{ struct iovec l = {(void*)(uintptr_t)buf, n}, r = {(void*)(uintptr_t)addr, n};
  (void) process_vm_writev(g_child, &l, 1, &r, 1, 0); }

static int
is_pie(const char *b)
{
	int fd = open(b, O_RDONLY); unsigned char e[18] = {0};
	if (fd < 0) return -1;
	if (read(fd, e, sizeof e) != (ssize_t) sizeof e) { close(fd); return -1; }
	close(fd);
	if (memcmp(e, "\177ELF", 4)) return -1;
	return (e[16] | (e[17] << 8)) == 3;
}

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
		OWN(SYS_accept), OWN(SYS_accept4), OWN(SYS_read), OWN(SYS_write),
		OWN(SYS_recvfrom), OWN(SYS_sendto),
#undef OWN
		BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {sizeof f/sizeof f[0], f};
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	return seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

static const char *g_binary;

/* run the target, replay client inputs `act`, snapshot the final canon
 * emit into out[]; return its length or -1 */
static int
run_prefix(const unsigned char *act, int len, unsigned char *out, int cap)
{
	int sp[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
	int fd3[2]; if (pipe(fd3) != 0) return -1;
	pid_t pid = fork();
	if (pid == 0)
	{
		close(sp[0]);
		setenv("LD_PRELOAD", "./detmalloc.so", 1);
		dup2(fd3[1], 3); close(fd3[0]); close(fd3[1]);
		int lf = install_filter(); if (lf < 0) _exit(2);
		char cbuf[CMSG_SPACE(sizeof(int))] = {0}; char d = 'L';
		struct iovec iov = {&d, 1}; struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &lf, sizeof(int));
		sendmsg(sp[1], &mh, 0);
		execl(g_binary, g_binary, "0", (char *) NULL);
		_exit(2);
	}
	close(sp[1]); close(fd3[1]);
	int listener = -1;
	{
		char cbuf[CMSG_SPACE(sizeof(int))] = {0}; char d;
		struct iovec iov = {&d, 1}; struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
		if (recvmsg(sp[0], &mh, 0) < 0) { close(sp[0]); close(fd3[0]); return -1; }
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		if (cm) memcpy(&listener, CMSG_DATA(cm), sizeof(int));
	}
	close(sp[0]);
	if (listener < 0) { close(fd3[0]); return -1; }
	g_child = pid; ptrace(PTRACE_SEIZE, pid, 0, 0);

	struct seccomp_notif_sizes sz; seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &sz);
	struct seccomp_notif *req = calloc(1, sz.seccomp_notif);
	struct seccomp_notif_resp *resp = calloc(1, sz.seccomp_notif_resp);
	int conn_fd = -1, next_fd = 500, sent = 0, acks = 0;
	for (;;)
	{
		memset(req, 0, sz.seccomp_notif);
		if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, req) < 0) { if (errno == EINTR) continue; break; }
		long nr = req->data.nr; __u64 *ar = req->data.args;
		memset(resp, 0, sz.seccomp_notif_resp); resp->id = req->id;
		switch (nr)
		{
			case SYS_socket: resp->val = next_fd++; break;
			case SYS_setsockopt: case SYS_bind: case SYS_listen: resp->val = 0; break;
			case SYS_accept: case SYS_accept4: conn_fd = next_fd++; resp->val = conn_fd; break;
			case SYS_read: case SYS_recvfrom:
				if ((int) ar[0] == conn_fd)
				{ if (sent < len) { unsigned char b = act[sent++]; cr_write(ar[1], &b, 1); resp->val = 1; }
				  else resp->val = 0; }
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;
			case SYS_write: case SYS_sendto:
				if ((int) ar[0] == conn_fd) { acks++; resp->val = (int) ar[2]; }
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;
			default: resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp) < 0 && errno != ENOENT) {}
	}
	free(req); free(resp); close(listener);
	int n = -1;
	if (acks == len)
	{
		unsigned char all[4096]; int tot = 0, r;
		while (tot < (int) sizeof all && (r = (int) read(fd3[0], all + tot, sizeof all - tot)) > 0) tot += r;
		int off = 0, rec = -1, reclen = 0;
		while (off < tot) { int cnt = all[off]; if (off + 1 + cnt > tot) break; rec = off + 1; reclen = cnt; off += 1 + cnt; }
		n = rec >= 0 ? reclen : 0;
		if (n > cap) n = cap;
		if (rec >= 0) memcpy(out, all + rec, n);
	}
	close(fd3[0]);
	kill(pid, SIGKILL); int st; waitpid(pid, &st, 0);
	return n;
}

/* =================================================================== */
/* engine: BFS with canonical dedup + invariant checks                 */
/* =================================================================== */
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t fmix64(uint64_t k)
{ k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
static void fp128(const uint8_t *p, size_t n, uint64_t fp[2])
{
	uint64_t h1 = 0x9368e53c2f6af274ULL ^ (n * 0x87c37b91114253d5ULL), h2 = 0x586dcd208f7cd3fdULL + n;
	for (size_t i = 0; i < n; i++) { h1 ^= (uint64_t)p[i] << ((i%8)*8); h1 = rotl64(h1,27)*5 + 0x52dce729;
		h2 = rotl64(h2 + p[i] + i, 23) * 0xd6e8feb86659fd93ULL; }
	fp[0] = fmix64(h1); fp[1] = fmix64(h2 + n);
}
typedef struct { uint64_t a, b; } FP;
static FP seen[65536]; static int nseen;
static int mark(uint64_t a, uint64_t b)
{ for (int i=0;i<nseen;i++) if (seen[i].a==a&&seen[i].b==b) return 0;
  if (nseen<(int)(sizeof seen/sizeof*seen)){seen[nseen].a=a;seen[nseen].b=b;nseen++;} return 1; }

#define MAXD 12
typedef struct { int len; unsigned char act[MAXD]; } Pre;
static Pre q[200000]; static int qh, qt;

int
main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s MODELFILE BINARY\n", argv[0]); return 3; }
	if (load_model(argv[1])) return 3;
	g_binary = argv[2];
	if (is_pie(g_binary) != 0) { fprintf(stderr, "%s must be a non-PIE ELF\n", g_binary); return 3; }
	if (M.depth > MAXD) { fprintf(stderr, "mc: depth %d exceeds the limit %d\n", M.depth, MAXD); return 3; }

	/* empty prefix = initial state */
	uint64_t fp[2]; fp128((const uint8_t*)"", 0, fp); mark(fp[0], fp[1]);
	int distinct = 1, merges = 0, cut = 0;
	q[qt].len = 0; qt++;

	while (qh < qt)
	{
		Pre pre = q[qh++];
		for (int ai = 1; ai <= M.clients; ai++)
		{
			if (pre.len >= M.depth) { cut = 1; continue; }
			Pre nx = pre; nx.act[nx.len++] = (unsigned char) ai;
			unsigned char snap[SNAP];
			int n = run_prefix(nx.act, nx.len, snap, sizeof snap);
			if (n < 0) { fprintf(stderr, "mc: run failed at depth %d\n", nx.len); return 3; }

			/* check every invariant in this state */
			Ctx c = {0}; c.node0 = snap; c.node0_len = n; c.acted = nx.act; c.acted_len = nx.len;
			for (int k = 0; k < M.ninv; k++)
			{
				int r = eval_invariant(M.inv[k].expr, &c);
				if (r < 0) { fprintf(stderr, "mc: invariant %s: parse error in \"%s\"\n", M.inv[k].name, M.inv[k].expr); return 3; }
				if (!r)
				{
					printf("VIOLATION: invariant %s does not hold\n", M.inv[k].name);
					printf("  client inputs:");
					for (int i = 0; i < nx.len; i++) printf(" %d", nx.act[i]);
					printf("\n  node[0] =");
					for (int i = 0; i < n; i++) printf(" %d", snap[i]);
					printf("\n");
					return 1;
				}
			}
			fp128(snap, (size_t) n, fp);
			if (mark(fp[0], fp[1]))
			{
				distinct++;
				if (qt >= (int) (sizeof q / sizeof *q) || nseen >= (int) (sizeof seen / sizeof *seen))
				{ fprintf(stderr, "mc: ERROR: the frontier or the seen set is full\n"); return 1; }
				q[qt] = nx; qt++;
			}
			else merges++;
		}
	}
	/* the depth bound stopped the search: that is not a pass */
	if (cut)
	{
		printf("INCOMPLETE (no violation found): depth bound %d reached; %d distinct states (%d dedup merges)\n",
			   M.depth, distinct, merges);
		return 2;
	}
	printf("ok: %d invariant(s) held over %d distinct states (%d dedup merges)\n",
		   M.ninv, distinct, merges);
	return 0;
}
