/*
 * mc2.c: the multi-node process-mode checker (experiments/process-mode/
 * DESIGN.md, M4).  It runs N node processes under one checker, owns every
 * socket between them, and explores the order in which inter-node messages
 * are delivered -- the first step toward checking a real replicated /
 * consensus implementation written as it ships.
 *
 * Mechanism, built on M0-M3:
 *   - Each node is a real process under seccomp user-notification; the
 *     checker holds one listener fd per node (they survive execve).
 *   - The checker is the network.  It intercepts socket/bind/listen/
 *     connect/accept/send/recv; no real socket ever forms.  A connect()'s
 *     destination port identifies the peer node (port BASE+i -> node i),
 *     so sends/recvs are tagged to a logical channel (src, dst).
 *   - A recv on a channel with no pending message is a BLOCKED node: the
 *     checker DEFERS that seccomp notification (leaves it unanswered) and
 *     services another node.  When the search delivers a message to that
 *     channel, the checker answers the deferred recv with the bytes.  A
 *     deferred, unanswered notification IS a blocked node (proved in
 *     scratch/probe_defer.c).
 *   - When every node is blocked-on-recv or exited, the world is
 *     quiescent: snapshot each node's canonical state (fd 3), check the
 *     invariants, and the deliverable messages are the next actions.
 *
 * Search: stateless replay over action sequences.  To explore a
 * sequence, relaunch all nodes, drive to quiescence, apply the sequence's
 * actions (re-driving to quiescence after each), then snapshot.  An
 * action is ACCEPT, DELIVER, EOF, TIMEOUT, CRASH or DROP (enum_actions).
 * Connections are routed by the port a node dials, so any topology the
 * nodes set up works; each connection is a FIFO channel, as TCP is.  BFS
 * gives the shortest counterexample; --simulate takes seeded random walks.
 *
 * M7: verdicts are honest.  The fingerprint holds everything the future
 * depends on (node states, open connections, channels in order, budgets
 * left, the clock).  A search stopped by a depth, state or time limit is
 * INCOMPLETE, never a pass.  A cap the run exceeds, a write longer than
 * one message, a node silent in an unmodelled call, or a replay that
 * diverges is an error, never truncated.
 *
 * Usage: mc2 MODELFILE BINARY [--bfs | --simulate [--seed N] [--walks K]]
 *            [--depth D] [--max-states N] [--time S]
 * Exit 0 = no violation and the search was complete; 1 = a violation
 * (shortest action sequence printed, by name) or an engine rule broken;
 * 2 = incomplete (a limit stopped the search, no violation found); 3 =
 * usage or setup error -- the exit codes of cmc(1).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <stdarg.h>
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
#define MSGLEN 64				/* bytes per write; a longer write is an error */
#define MAXREP 64				/* client replies recorded */
#define MAXCONN 16				/* connections per node */
#define LULL_MS 10000			/* a running node silent this long is an error */
#define SNAP 64					/* canonical state bytes per node */
/* The durable canonical-emit stream.  fd 3 is the volatile stream; the
 * durable one must avoid the low fds the dynamic loader uses while
 * handling LD_PRELOAD (dup'ing a pipe onto fd 4 breaks the preload on
 * glibc 2.44), and stay below the 500 threshold that marks a channel. */
#define DURABLE_FD 193
/* actions enabled at one point: per node at most MAXNODES + 1 accepts, a
 * delivery or an EOF, and a timeout; then one crash per node and one
 * drop per channel -- 2 * MAXNODES * (MAXNODES + 3) bounds the sum */
#define MAXACT (2 * MAXNODES * (MAXNODES + 3))
#define MAXINV 8

/* =================================================================== */
/* model file (subset of docs/model-file.md; shares mc.c's DSL)        */
/* =================================================================== */
typedef struct { char name[32]; char expr[256]; } Inv;
static struct { int nodes; int client_val; int client; int timeouts; int crashes; int drops; int depth; Inv inv[MAXINV]; int ninv; } M;

static char *trim(char *s){ while(*s&&isspace((unsigned char)*s))s++; char*e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }

static int
load_model(const char *path)
{
	FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "mc2: cannot open %s\n", path); return -1; }
	char line[512], sect[32] = ""; M.nodes = 2; M.client_val = 5; M.client = 1; M.timeouts = 0; M.crashes = 0; M.drops = 0; M.depth = -1;
	while (fgets(line, sizeof line, f))
	{
		char *s = trim(line); if (!*s || *s == '#') continue;
		if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; snprintf(sect, sizeof sect, "%s", s + 1); } continue; }
		char *eq = strchr(s, '='); if (!eq) continue; *eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = 0; }
		else { char *h = strchr(v, '#'); if (h) *h = 0; v = trim(v); }
		if (!strcmp(sect, "target")) { if (!strcmp(k, "nodes")) M.nodes = atoi(v); }
		else if (!strcmp(sect, "environment")) { if (!strcmp(k, "client_val")) M.client_val = atoi(v); else if (!strcmp(k, "client")) M.client = atoi(v); else if (!strcmp(k, "timeouts")) M.timeouts = atoi(v); else if (!strcmp(k, "crashes")) M.crashes = atoi(v); else if (!strcmp(k, "drops")) M.drops = atoi(v); else if (!strcmp(k, "depth")) M.depth = atoi(v); }
		else if (!strcmp(sect, "invariants")) { if (M.ninv < MAXINV) { snprintf(M.inv[M.ninv].name, 32, "%s", k); snprintf(M.inv[M.ninv].expr, 256, "%s", v); M.ninv++; } }
	}
	fclose(f);
	if (M.nodes < 1 || M.nodes > MAXNODES) { fprintf(stderr, "mc2: nodes out of range\n"); return -1; }
	return 0;
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
} Ctx;
static void skipws(const char **p){ while(**p&&isspace((unsigned char)**p))(*p)++; }
static int word(const char **p,const char*w){ skipws(p); size_t n=strlen(w); if(!strncmp(*p,w,n)&&!isalnum((unsigned char)(*p)[n])&&(*p)[n]!='_'){*p+=n;return 1;} return 0; }
static int eval_expr(const char **p, Ctx *c);

/* a set yields bytes to range over, OR the reply log (special) */
enum { SET_BYTES, SET_REPLIES };
static int
get_set(const char **p, Ctx *c, const unsigned char **buf, int *len, int *kind)
{
	skipws(p);
	if (word(p, "replies")) { *kind = SET_REPLIES; *len = c->nrepl; *buf = NULL; return 0; }
	if (word(p, "node"))
	{
		skipws(p); if (**p != '[') return -1; (*p)++;
		long i = strtol(*p, (char **) p, 10); skipws(p); if (**p != ']') return -1; (*p)++;
		if (i < 0 || i >= c->nnodes) return -1;
		*kind = SET_BYTES; *buf = c->node[i]; *len = c->node_len[i]; return 0;
	}
	return -1;
}

/* a term yields a value: NUMBER, bound var, or r.src / r.val */
static int
get_term(const char **p, Ctx *c, long *out)
{
	skipws(p);
	if (isdigit((unsigned char) **p)) { *out = strtol(*p, (char **) p, 10); return 0; }
	char id[32]; int n = 0; while ((isalnum((unsigned char) **p) || **p == '_') && n < 31) id[n++] = *(*p)++; id[n] = 0;
	if (n == 0) return -1;
	/* node[i][k] : byte k of node i's canonical state */
	if (!strcmp(id, "node"))
	{
		skipws(p); if (**p != '[') return -1; (*p)++;
		long i = strtol(*p, (char **) p, 10); skipws(p); if (**p != ']') return -1; (*p)++;
		skipws(p); if (**p != '[') return -1; (*p)++;
		long k = strtol(*p, (char **) p, 10); skipws(p); if (**p != ']') return -1; (*p)++;
		if (i < 0 || i >= c->nnodes || k < 0 || k >= c->node_len[i]) { *out = -1; return 0; }
		*out = c->node[i][k]; return 0;
	}
	/* r.src / r.val when ranging over replies */
	if (c->ranging_replies && c->varset && !strcmp(id, c->var))
	{
		if (**p == '.') { (*p)++; char fld[8]; int m = 0; while (isalpha((unsigned char) **p) && m < 7) fld[m++] = *(*p)++; fld[m] = 0;
			if (!strcmp(fld, "src")) { *out = c->repl_src[c->val]; return 0; }
			if (!strcmp(fld, "val")) { *out = c->repl_val[c->val]; return 0; }
			return -1; }
		*out = c->val; return 0;
	}
	if (c->varset && !strcmp(id, c->var)) { *out = c->val; return 0; }
	return -1;
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
		const char *body = *p; int result = isall ? 1 : 0;
		Ctx inner = *c; inner.varset = 1; snprintf(inner.var, sizeof inner.var, "%s", var);
		inner.ranging_replies = (kind == SET_REPLIES);
		for (int i = 0; i < slen; i++)
		{
			const char *bp = body;
			inner.val = (kind == SET_REPLIES) ? i : set[i];
			int r = eval_expr(&bp, &inner); if (r < 0) return -1;
			if (isall && !r) result = 0;
			if (issome && r) result = 1;
			*p = bp;
		}
		if (slen == 0) { const char *bp = body; eval_expr(&bp, &inner); *p = bp; }
		return result;
	}
	*p = save;
	long a; if (get_term(p, c, &a)) return -1;
	skipws(p);
	if (word(p, "in"))
	{
		const unsigned char *set; int slen, kind; if (get_set(p, c, &set, &slen, &kind)) return -1;
		if (kind == SET_REPLIES) return -1;	/* 'in replies' not defined */
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
static int eval_unary(const char **p, Ctx *c){ skipws(p);
	if(word(p,"not")){int r=eval_unary(p,c);return r<0?r:!r;}
	if(**p=='('){(*p)++;int r=eval_expr(p,c);skipws(p);if(**p==')')(*p)++;return r;}
	return eval_qc(p,c); }
static int eval_expr(const char **p, Ctx *c){ int r=eval_unary(p,c); if(r<0)return r;
	for(;;){ skipws(p); if(word(p,"and")){int b=eval_unary(p,c);if(b<0)return b;r=r&&b;}
		else if(word(p,"or")){int b=eval_unary(p,c);if(b<0)return b;r=r||b;} else break; } return r; }

/* =================================================================== */
/* per-node process + the inter-node network                           */
/* =================================================================== */
typedef struct {
	pid_t pid; int listener; int alive;
	/* association list: which peer each connection fd talks to.
	 * peer -1 = the synthetic client, >=0 = a node id. */
	int cfd[MAXCONN]; int cpeer[MAXCONN]; int nc;
	int pend[MAXCONN]; int npend;	/* connectors queued at this node's listener */
	int blocked; int block_kind; uint64_t block_id; uint64_t block_bufaddr; int block_want; int block_peer;
	int next_fd;
	const char *binary; int id;
	int fd3[2];					/* volatile canonical-emit pipe */
	int fd4[2];					/* durable canonical-emit pipe (survives a crash) */
	unsigned char canon[SNAP]; int canon_len;
	unsigned char durable[SNAP]; int durable_len;
	int crashed;				/* killed by a CRASH action: volatile lost, durable kept */
} Node;

static Node N[MAXNODES];
enum { BK_RECV = 1, BK_ACCEPT = 2 };	/* what a blocked node waits in */

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
static void cfd_set(Node *nd, int fd, int peer){ if(nd->nc>=MAXCONN) die(1,"node %d has more than %d connections",nd->id,MAXCONN); nd->cfd[nd->nc]=fd; nd->cpeer[nd->nc]=peer; nd->nc++; }
static int  cfd_peer(Node *nd, int fd){ for(int i=0;i<nd->nc;i++) if(nd->cfd[i]==fd) return nd->cpeer[i]; return -2; }

/* message bag: a pending message src->dst */
typedef struct { int used, src, dst, len, chseq; unsigned char body[MSGLEN]; } Msg;
static Msg bag[MAXMSG];
/* sends so far on each channel (src+1, dst): a message's place in its
 * channel, so delivery is FIFO per connection, as TCP is */
static int chcount[MAXNODES + 1][MAXNODES];

/* ghost reply log */
static unsigned char rep_src[MAXREP], rep_val[MAXREP]; static int nrep;

/* environment budgets left on this path; part of the fingerprint */
typedef struct { int timeouts, crashes, drops; } Budget;
static Budget bud;

/* M5b: the logical clock the checker owns.  It starts at a base and
 * advances by LEASE_TICK only when the checker fires a timeout, so a
 * target's clock_gettime/gettimeofday reads are a pure function of how
 * many timeouts have fired -- stateless replay stays deterministic. */
#define CLK_BASE 1000000000ULL		/* 1e9 ns = 1 s, an arbitrary epoch */
#define CLK_TICK 1000000000ULL		/* each fired timeout advances 1 s */
static unsigned long long logical_ns;

static struct seccomp_notif_sizes g_sz;

static void
cr_write(pid_t pid, uint64_t addr, const void *buf, size_t n)
{ struct iovec l = {(void*)(uintptr_t)buf, n}, r = {(void*)(uintptr_t)addr, n};
  (void) process_vm_writev(pid, &l, 1, &r, 1, 0); }
static void
cr_read(pid_t pid, uint64_t addr, void *buf, size_t n)
{ struct iovec l = {buf, n}, r = {(void*)(uintptr_t)addr, n};
  if (process_vm_readv(pid, &l, 1, &r, 1, 0) != (ssize_t) n) memset(buf, 0, n); }

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
		setenv("LD_PRELOAD", "./detmalloc.so", 1);
		dup2(nd->fd3[1], 3); close(nd->fd3[0]); close(nd->fd3[1]);
		dup2(nd->fd4[1], DURABLE_FD); close(nd->fd4[0]); close(nd->fd4[1]);
		int lf = install_filter(); if (lf < 0) _exit(2);
		char cb[CMSG_SPACE(sizeof(int))] = {0}; char d = 'L';
		struct iovec iov = {&d, 1}; struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &lf, sizeof(int)); sendmsg(sp[1], &mh, 0);
		char idbuf[8]; snprintf(idbuf, sizeof idbuf, "%d", nd->id);
		execl(nd->binary, nd->binary, idbuf, (char *) NULL);
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
	nd->pid = pid; nd->alive = 1; nd->blocked = 0; nd->block_kind = 0; nd->next_fd = 500; nd->nc = 0;
	nd->npend = 0; nd->crashed = 0; nd->canon_len = 0; nd->durable_len = 0;
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
 * A recv on an empty channel is DEFERRED (nd->blocked set, not answered).
 */
static struct seccomp_notif *g_req;
static struct seccomp_notif_resp *g_resp;

static int
try_pending_for(Node *nd)
{
	if (!nd->alive || nd->blocked) return 0;
	memset(g_req, 0, g_sz.seccomp_notif);
	/* non-blocking RECV: use ioctl; returns -1/ENOENT if none ready */
	if (ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_RECV, g_req) != 0)
		return 0;
	long nr = g_req->data.nr; __u64 *a = g_req->data.args;
	memset(g_resp, 0, g_sz.seccomp_notif_resp); g_resp->id = g_req->id;

	switch (nr)
	{
		case SYS_socket:  g_resp->val = nd->next_fd++; break;
		case SYS_clock_gettime:
		{
			/* answer with the logical clock (a[1] = struct timespec*) */
			struct { long long sec, nsec; } ts;
			ts.sec = logical_ns / 1000000000ULL; ts.nsec = logical_ns % 1000000000ULL;
			cr_write(nd->pid, a[1], &ts, sizeof ts); g_resp->val = 0; break;
		}
		case SYS_gettimeofday:
		{
			struct { long long sec, usec; } tv;
			tv.sec = logical_ns / 1000000000ULL; tv.usec = (logical_ns % 1000000000ULL) / 1000;
			cr_write(nd->pid, a[1], &tv, sizeof tv); g_resp->val = 0; break;
		}
		case SYS_getrandom:
		{
			/* engine-chosen randomness: a fixed byte pattern, so the search
			 * is reproducible (a real nondeterministic source would need its
			 * own budget; deterministic is the honest default here) */
			unsigned char z[256]; size_t n = a[1]; if (n > sizeof z) n = sizeof z;
			memset(z, 0, n); cr_write(nd->pid, a[0], z, n);
			g_resp->val = (long) n; break;
		}
		case SYS_setsockopt: case SYS_bind: case SYS_listen: g_resp->val = 0; break;
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
			cr_read(nd->pid, a[1], &sa, sizeof sa);
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
			/* DEFER: which waiting connection the node accepts, and when,
			 * is a search action, so the topology is whatever the nodes
			 * dial (shortcut: the sockaddr out-parameter is not filled) */
			nd->blocked = 1; nd->block_kind = BK_ACCEPT; nd->block_id = g_req->id; nd->block_peer = -2;
			return 1;
		case SYS_write: case SYS_sendto:
		{
			int fd = (int) a[0];
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; } /* not ours (stdout, fd 3, libs) */
			int peer = cfd_peer(nd, fd);
			if (peer == -2) die(1, "node %d writes fd %d, which is no connection", nd->id, fd);
			int len = (int) a[2];
			if (len < 0 || len > MSGLEN) die(1, "node %d writes %d bytes in one call; the limit is %d", nd->id, len, MSGLEN);
			unsigned char buf[MSGLEN] = {0};
			cr_read(nd->pid, a[1], buf, (size_t) len);
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
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; } /* loader/lib reads */
			int peer = cfd_peer(nd, fd);
			if (peer == -2) die(1, "node %d reads fd %d, which is no connection", nd->id, fd);
			/* Always DEFER a channel recv: whether it is satisfied by a
			 * message or by a timeout is an environment choice the search
			 * makes, so delivery must never be automatic (M5).  The node
			 * blocks in the kernel until the search delivers to it. */
			nd->blocked = 1; nd->block_kind = BK_RECV; nd->block_id = g_req->id; nd->block_bufaddr = a[1];
			nd->block_want = (int) a[2]; nd->block_peer = peer;
			return 1;				/* do NOT answer */
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

/* answer a blocked node's deferred syscall with return value val */
static void
answer(Node *nd, long val)
{
	memset(g_resp, 0, g_sz.seccomp_notif_resp); g_resp->id = nd->block_id; g_resp->val = val;
	ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_resp);
	nd->blocked = 0;
}

/* answer a blocked node's deferred recv with up to len bytes (0 = EOF);
 * returns how many it took -- a short read leaves the rest queued */
static int
deliver(Node *nd, const unsigned char *body, int len)
{
	int k = len < nd->block_want ? len : nd->block_want;
	if (k > 0) cr_write(nd->pid, nd->block_bufaddr, body, (size_t) k);
	answer(nd, k);
	return k;
}

/* answer a blocked node's deferred accept with a connection from peer */
static void
accept_from(Node *nd, int peer)
{
	for (int k = 0; k < nd->npend; k++)
		if (nd->pend[k] == peer) { nd->pend[k] = nd->pend[--nd->npend]; break; }
	int fd = nd->next_fd++;
	cfd_set(nd, fd, peer);
	answer(nd, fd);
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
	nd->pid = 0; nd->alive = 0; nd->blocked = 0; nd->crashed = 1; nd->npend = 0;
	/* volatile is gone; the node's observable state is what was durable */
	memcpy(nd->canon, nd->durable, nd->durable_len); nd->canon_len = nd->durable_len;
}

/* run all nodes to quiescence: every live node blocked on a recv, or
 * exited.  A node that is neither blocked nor exited is still running
 * between syscalls -- we must wait for its next notification, not treat
 * the lull as quiescence. */
static void
drive_quiescent(void)
{
	for (;;)
	{
		for (int i = 0; i < M.nodes; i++) reap(&N[i]);
		/* service every node that has a ready notification now */
		int did = 0;
		for (int i = 0; i < M.nodes; i++)
			if (N[i].alive && !N[i].blocked)
				did |= try_pending_for(&N[i]);
		if (did) continue;
		/* which live nodes are still running (not blocked)? wait for one */
		struct pollfd pf[MAXNODES]; int np = 0;
		for (int i = 0; i < M.nodes; i++)
			if (N[i].alive && !N[i].blocked) { pf[np].fd = N[i].listener; pf[np].events = POLLIN; np++; }
		if (np == 0) break;			/* all blocked or dead: quiescent */
		int r = poll(pf, np, LULL_MS);
		if (r < 0 && errno == EINTR) continue;
		if (r == 0)
		{
			/* a running node made no syscall the checker owns for
			 * LULL_MS: it is busy, or blocked in a call the checker does
			 * not model.  Treating that as quiescence would snapshot a
			 * node mid-step, so it is an error. */
			for (int i = 0; i < M.nodes; i++)
			{
				reap(&N[i]);
				if (N[i].alive && !N[i].blocked)
					die(1, "node %d made no checked syscall for %d ms (busy, or blocked in a call the checker does not model)", i, LULL_MS);
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
		PUT(N[i].alive ? (N[i].blocked ? N[i].block_kind : 3) : (N[i].crashed ? 0xfe : 0xff));
		PUT(N[i].blocked ? N[i].block_peer + 2 : 0);
		PUT(N[i].canon_len);
		for (int k = 0; k < N[i].canon_len; k++) PUT(N[i].canon[k]);
		/* live connections in the order they were opened (cfd is kept in
		 * that order), by peer: which connection goes where is node
		 * state.  shortcut: absolute fd numbers are left out, on the
		 * assumption that a target does not branch on an fd's value */
		PUT(N[i].alive ? N[i].nc : 0);
		for (int k = 0; N[i].alive && k < N[i].nc; k++) PUT(N[i].cpeer[k] + 2);
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
	PUT(bud.timeouts); PUT(bud.crashes); PUT(bud.drops);
	for (int k = 0; k < 8; k++) PUT(logical_ns >> (8 * k));
#undef PUT
	fp128(w, n, fp);
}

static void
reset_world(void)
{
	memset(bag, 0, sizeof bag); memset(chcount, 0, sizeof chcount);
	nrep = 0; logical_ns = CLK_BASE;
	bud.timeouts = M.timeouts; bud.crashes = M.crashes; bud.drops = M.drops;
}

/* One step the search can take at a quiescent point.  The list is
 * rebuilt in the same order on every replay, so an index names an
 * action; the shortcut keeps action lists small enough to replay. */
enum { A_ACCEPT, A_DELIVER, A_EOF, A_TIMEOUT, A_CRASH, A_DROP };
typedef struct { int kind, node, peer; } Act;

/* Enumerate the actions enabled now:
 *   - ACCEPT: a node blocked in accept takes a connection waiting at it;
 *   - DELIVER: a node blocked in recv gets the head of its channel;
 *   - EOF: its peer has crashed and the channel is drained, so recv
 *     returns 0;
 *   - TIMEOUT (budgeted): the recv is satisfied by the timer instead;
 *   - CRASH (budgeted): a live node dies, keeping only its durable state;
 *   - DROP (budgeted): the network loses the head of a channel.
 * Every environment action is bounded so the search stays finite. */
static int
enum_actions(Act *out)
{
	int c = 0;
#define ADD(k, nd, pr) do { if (c >= MAXACT) die(1, "more than %d actions enabled at one point", MAXACT); \
	out[c].kind = (k); out[c].node = (nd); out[c].peer = (pr); c++; } while (0)
	for (int d = 0; d < M.nodes; d++)
	{
		Node *nd = &N[d];
		if (!nd->alive || !nd->blocked) continue;
		if (nd->block_kind == BK_ACCEPT)
		{
			for (int s = -1; s < M.nodes; s++)
			{
				int waiting = 0;
				for (int k = 0; k < nd->npend; k++) waiting |= nd->pend[k] == s;
				if (waiting) ADD(A_ACCEPT, d, s);
			}
			continue;
		}
		int p = nd->block_peer;
		if (chan_head(p, d) >= 0) ADD(A_DELIVER, d, p);
		else if (p >= 0 && !N[p].alive) ADD(A_EOF, d, p);	/* peer gone, channel drained */
		if (bud.timeouts > 0) ADD(A_TIMEOUT, d, p);
	}
	if (bud.crashes > 0)
		for (int d = 0; d < M.nodes; d++)
			if (N[d].alive) ADD(A_CRASH, d, -1);
	if (bud.drops > 0)
		for (int s = -1; s < M.nodes; s++)
			for (int d = 0; d < M.nodes; d++)
				if (chan_head(s, d) >= 0) ADD(A_DROP, d, s);
#undef ADD
	return c;
}

static void
apply(const Act *a)
{
	Node *nd = &N[a->node];
	switch (a->kind)
	{
		case A_ACCEPT: accept_from(nd, a->peer); break;
		case A_DELIVER:
		{
			/* one write per read: a read never joins two queued writes
			 * (shortcut: real TCP may coalesce; upgrade if a target
			 * relies on it) */
			int h = chan_head(a->peer, a->node);
			int k = deliver(nd, bag[h].body, bag[h].len);
			if (k < bag[h].len) { memmove(bag[h].body, bag[h].body + k, (size_t) (bag[h].len - k)); bag[h].len -= k; }
			else bag[h].used = 0;
			break;
		}
		case A_EOF: deliver(nd, NULL, 0); break;
		case A_TIMEOUT:
		{
			static const unsigned char t = 'T';
			bud.timeouts--;
			logical_ns += CLK_TICK;			/* time advances only here */
			deliver(nd, &t, 1);
			break;
		}
		case A_CRASH: bud.crashes--; crash_node(nd); break;
		case A_DROP: bud.drops--; bag[chan_head(a->peer, a->node)].used = 0; break;
	}
	drive_quiescent();
}

/* Replay `seq` from a fresh launch.  Returns 0, or -1 if a node would not
 * start.  On return the world is at the quiescent point after the last
 * action, and acts/nact list what is enabled there. */
static int
run_seq(const int *seq, int seqlen, Act *acts, int *nact)
{
	reset_world();
	for (int i = 0; i < M.nodes; i++) { N[i].id = i; if (launch(&N[i])) return -1; }
	if (M.client)
	{
		/* the client dials node 0 and sends WRITE(v) on that connection */
		unsigned char w[2] = {'W', (unsigned char) M.client_val};
		N[0].pend[N[0].npend++] = -1;
		bag_add(-1, 0, w, 2);
	}
	drive_quiescent();
	for (int s = 0; s < seqlen; s++)
	{
		Act a[MAXACT]; int na = enum_actions(a);
		if (seq[s] < 0 || seq[s] >= na)
			die(1, "replay diverged at step %d: action %d of %d (is the target deterministic?)", s, seq[s], na);
		apply(&a[seq[s]]);
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

static int
check_invariants(int *which)
{
	Ctx c = {0}; c.nnodes = M.nodes;
	for (int i = 0; i < M.nodes; i++) { c.node[i] = N[i].canon; c.node_len[i] = N[i].canon_len; }
	c.repl_src = rep_src; c.repl_val = rep_val; c.nrepl = nrep;
	for (int k = 0; k < M.ninv; k++) {
		const char *p = M.inv[k].expr; int r = eval_expr(&p, &c);
		if (r < 0) { fprintf(stderr, "mc2: invariant %s parse error\n", M.inv[k].name); *which = -2; return 0; }
		if (!r) { *which = k; return 0; }
	}
	return 1;
}

static const char *
act_name(const Act *a, char *b, size_t n)
{
	static const char *k[] = {"accept", "deliver", "eof", "timeout", "crash", "drop"};
	if (a->kind == A_CRASH) snprintf(b, n, "crash n%d", a->node);
	else if (a->peer < 0) snprintf(b, n, "%s client->n%d", k[a->kind], a->node);
	else snprintf(b, n, "%s n%d->n%d", k[a->kind], a->peer, a->node);
	return b;
}

/* report a violation: the action sequence, by name, and the world there */
static void
report(int which, const int *seq, int len)
{
	char nm[48];
	printf("VIOLATION: invariant %s does not hold\n", M.inv[which].name);
	printf("  actions (%d):", len);
	for (int i = 0; i < len; i++) printf(" %d", seq[i]);
	printf("\n");
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
		for (int k = 0; k < N[i].canon_len; k++) printf("%s%d", k ? "," : "", N[i].canon[k]);
	}
	printf("\n  replies:"); for (int i = 0; i < nrep; i++) printf(" (n%d,%d)", rep_src[i], rep_val[i]);
	printf("\n");
	kill_all();
}

/* the BFS frontier: action sequences, appended in one growing pool */
static int *qpool; static size_t qcap, qused;
typedef struct { size_t off; int len; } QEnt;
static QEnt *Q; static size_t Qcap, qh, qt;

static void
q_push(const int *seq, int len, int next)
{
	if (qt == Qcap) { Qcap = Qcap ? 2 * Qcap : 4096; Q = realloc(Q, Qcap * sizeof *Q); if (!Q) die(1, "out of memory for the frontier"); }
	if (qused + (size_t) len + 1 > qcap)
	{
		while (qused + (size_t) len + 1 > qcap) qcap = qcap ? 2 * qcap : 65536;
		qpool = realloc(qpool, qcap * sizeof *qpool); if (!qpool) die(1, "out of memory for the frontier");
	}
	Q[qt].off = qused; Q[qt].len = len + 1; qt++;
	if (len) memcpy(qpool + qused, seq, (size_t) len * sizeof *seq);
	qpool[qused + (size_t) len] = next;
	qused += (size_t) len + 1;
}

/* the root: the empty sequence */
static void
q_root(void)
{
	Q = malloc(4096 * sizeof *Q); Qcap = 4096;
	if (!Q) die(1, "out of memory for the frontier");
	Q[0].off = 0; Q[0].len = 0; qt = 1;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double) t.tv_sec + t.tv_nsec / 1e9; }

/* splitmix64: the walk's seeded choices */
static uint64_t rng;
static uint64_t next_rand(void) { uint64_t z = (rng += 0x9e3779b97f4a7c15ULL); z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL; z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; return z ^ (z >> 31); }

static void
usage(const char *p)
{
	fprintf(stderr,
			"usage: %s MODELFILE BINARY [--bfs | --simulate [--seed N] [--walks K]]\n"
			"          [--depth D] [--max-states N] [--time S]\n"
			"exit: 0 = no violation and the search was complete; 1 = a violation,\n"
			"      or an engine rule broken; 2 = incomplete (a limit stopped the\n"
			"      search, no violation found); 3 = usage or setup error\n", p);
	exit(3);
}

int
main(int argc, char **argv)
{
	if (argc < 3) usage(argv[0]);
	int simulate = 0; long depth = -1; uint64_t seed = 1, walks = 1000; double tlimit = 0; size_t max_states = 0;
	for (int i = 3; i < argc; i++)
	{
		const char *o = argv[i]; const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(o, "--bfs")) simulate = 0;
		else if (!strcmp(o, "--simulate")) simulate = 1;
		else if (!strcmp(o, "--seed") && v) { seed = strtoull(v, NULL, 10); i++; }
		else if (!strcmp(o, "--walks") && v) { walks = strtoull(v, NULL, 10); i++; }
		else if (!strcmp(o, "--depth") && v) { depth = strtol(v, NULL, 10); i++; }
		else if (!strcmp(o, "--max-states") && v) { max_states = strtoull(v, NULL, 10); i++; }
		else if (!strcmp(o, "--time") && v) { tlimit = strtod(v, NULL); i++; }
		else usage(argv[0]);
	}
	if (load_model(argv[1])) return 3;
	if (depth < 0) depth = M.depth;			/* the model file's bound, if any */
	const char *binary = argv[2];
	if (is_pie(binary) != 0) { fprintf(stderr, "mc2: %s must be a non-PIE ELF\n", binary); return 3; }
	for (int i = 0; i < MAXNODES; i++) { N[i].binary = binary; N[i].listener = N[i].fd3[0] = N[i].fd4[0] = -1; }
	seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &g_sz);
	g_req = calloc(1, g_sz.seccomp_notif); g_resp = calloc(1, g_sz.seccomp_notif_resp);
	double t0 = now_s();

	if (simulate)
	{
		/* seeded random walks: the same seed takes the same walks.  Not
		 * exhaustive, so a clean finish is "no violation found", exit 2. */
		if (depth < 0) depth = 100;
		rng = seed;
		uint64_t states = 0, w;
		int *seq = malloc((size_t) depth * sizeof *seq);
		for (w = 0; w < walks; w++)
		{
			Act a[MAXACT]; int na, len = 0;
			if (run_seq(NULL, 0, a, &na)) die(3, "a node would not start");
			for (;;)
			{
				int which = -1; states++;
				if (!check_invariants(&which))
				{
					if (which == -2) { kill_all(); return 3; }
					kill_all();
					printf("walk %" PRIu64 " (seed %" PRIu64 "):\n", w + 1, seed);
					report(which, seq, len);
					return 1;
				}
				if (na == 0 || len >= depth) break;
				int pick = (int) (next_rand() % (uint64_t) na);
				seq[len++] = pick;
				apply(&a[pick]);
				na = enum_actions(a);
			}
			kill_all();
			if (tlimit > 0 && now_s() - t0 > tlimit) { w++; break; }
		}
		printf("INCOMPLETE (no violation found): %" PRIu64 " walks of up to %ld actions, %" PRIu64 " states checked, seed %" PRIu64 " (not exhaustive), %.1f s\n",
			   w, depth, states, seed, now_s() - t0);
		return 2;
	}

	/* breadth-first search: shortest counterexample, every distinct world
	 * expanded once.  Each frontier entry is replayed from a fresh launch
	 * (stateless), so a world costs one replay of its whole prefix. */
	size_t distinct = 0, merges = 0, cut = 0, replays = 0; int maxlen = 0;
	const char *why = NULL;
	q_root();
	while (qh < qt)
	{
		QEnt e = Q[qh++];
		int *seq = malloc(((size_t) e.len + 1) * sizeof *seq);
		if (e.len) memcpy(seq, qpool + e.off, (size_t) e.len * sizeof *seq);
		Act a[MAXACT]; int na = 0;
		if (run_seq(seq, e.len, a, &na)) die(3, "a node would not start");
		replays++;

		int which = -1; int ok = check_invariants(&which);
		if (which == -2) { kill_all(); return 3; }
		uint64_t fp[2]; world_fp(fp);
		int isnew = mark(fp[0], fp[1]);
		if (isnew) { distinct++; if (e.len > maxlen) maxlen = e.len; } else merges++;
		kill_all();
		if (!ok) { report(which, seq, e.len); return 1; }
		if (isnew && na > 0)
		{
			if (depth >= 0 && e.len >= depth) cut++;
			else for (int i = 0; i < na; i++) q_push(seq, e.len, i);
		}
		free(seq);
		if (max_states && distinct >= max_states) { why = "max states reached"; break; }
		if (tlimit > 0 && now_s() - t0 > tlimit) { why = "time limit reached"; break; }
	}
	double dt = now_s() - t0;
	if (!why && cut) why = "depth bound reached";
	if (why)
	{
		printf("INCOMPLETE (no violation found): %s; %zu distinct states, %zu left on queue, %zu cut at depth %ld, %zu replays, %.1f s\n",
			   why, distinct, qt - qh, cut, depth, replays, dt);
		return 2;
	}
	printf("ok: %d invariant(s) held over %zu distinct states (%zu merges), complete to depth %d, %zu replays, %.1f s (%.0f replays/s)\n",
		   M.ninv, distinct, merges, maxlen, replays, dt, dt > 0 ? replays / dt : 0.0);
	return 0;
}
