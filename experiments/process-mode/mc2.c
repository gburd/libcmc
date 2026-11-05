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
 * Search: stateless replay over delivery sequences.  To explore a
 * sequence, relaunch all nodes, drive to quiescence, apply the sequence's
 * deliveries (re-driving to quiescence after each), then snapshot.  BFS
 * gives the shortest counterexample.
 *
 * M4 scope: a fixed client script (one WRITE to node 0), no crashes, no
 * clock.  The model file gives nodes, the client value, depth and the
 * invariants (reusing mc.c's DSL, extended to node[i]).
 *
 * Usage: mc2 MODELFILE BINARY
 * Exit 0 = invariants held and search terminated; 1 = violation
 * (shortest delivery sequence printed); 2 = error.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
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

#define BASE 20000				/* node i listens on 127.0.0.1:BASE+i */
#define MAXNODES 4
#define MAXMSG 64				/* messages in flight, total */
#define MSGLEN 8				/* bytes per message (demo uses 2) */
#define SNAP 64					/* canonical state bytes per node */
#define MAXD 8					/* delivery-sequence depth */
#define MAXINV 8

/* =================================================================== */
/* model file (subset of docs/model-file.md; shares mc.c's DSL)        */
/* =================================================================== */
typedef struct { char name[32]; char expr[256]; } Inv;
static struct { int nodes; int client_val; int depth; Inv inv[MAXINV]; int ninv; } M;

static char *trim(char *s){ while(*s&&isspace((unsigned char)*s))s++; char*e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }

static int
load_model(const char *path)
{
	FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "mc2: cannot open %s\n", path); return -1; }
	char line[512], sect[32] = ""; M.nodes = 2; M.client_val = 5; M.depth = 6;
	while (fgets(line, sizeof line, f))
	{
		char *s = trim(line); if (!*s || *s == '#') continue;
		if (*s == '[') { char *e = strchr(s, ']'); if (e) { *e = 0; snprintf(sect, sizeof sect, "%s", s + 1); } continue; }
		char *eq = strchr(s, '='); if (!eq) continue; *eq = 0;
		char *k = trim(s), *v = trim(eq + 1);
		if (*v == '"') { v++; char *q = strrchr(v, '"'); if (q) *q = 0; }
		else { char *h = strchr(v, '#'); if (h) *h = 0; v = trim(v); }
		if (!strcmp(sect, "target")) { if (!strcmp(k, "nodes")) M.nodes = atoi(v); }
		else if (!strcmp(sect, "environment")) { if (!strcmp(k, "client_val")) M.client_val = atoi(v); else if (!strcmp(k, "depth")) M.depth = atoi(v); }
		else if (!strcmp(sect, "invariants")) { if (M.ninv < MAXINV) { snprintf(M.inv[M.ninv].name, 32, "%s", k); snprintf(M.inv[M.ninv].expr, 256, "%s", v); M.ninv++; } }
	}
	fclose(f);
	if (M.nodes < 1 || M.nodes > MAXNODES) { fprintf(stderr, "mc2: nodes out of range\n"); return -1; }
	if (M.depth > MAXD) M.depth = MAXD;
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
	int cfd[8]; int cpeer[8]; int nc;
	int blocked; uint64_t block_id; uint64_t block_bufaddr; int block_want; int block_peer;
	int next_fd;
	const char *binary; int id;
	int fd3[2];					/* canonical-emit pipe */
	unsigned char canon[SNAP]; int canon_len;
} Node;

static Node N[MAXNODES];

/* record / look up which peer a connection fd talks to */
static void cfd_set(Node *nd, int fd, int peer){ if(nd->nc<8){nd->cfd[nd->nc]=fd; nd->cpeer[nd->nc]=peer; nd->nc++;} }
static int  cfd_peer(Node *nd, int fd){ for(int i=0;i<nd->nc;i++) if(nd->cfd[i]==fd) return nd->cpeer[i]; return -2; }

/* message bag: a pending message src->dst */
typedef struct { int used, src, dst, len; unsigned char body[MSGLEN]; } Msg;
static Msg bag[MAXMSG];

/* ghost reply log */
static unsigned char rep_src[64], rep_val[64]; static int nrep;

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
	pid_t pid = fork();
	if (pid == 0)
	{
		close(sp[0]);
		setenv("LD_PRELOAD", "./detmalloc.so", 1);
		dup2(nd->fd3[1], 3); close(nd->fd3[0]); close(nd->fd3[1]);
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
	close(sp[1]); close(nd->fd3[1]);
	char cb[CMSG_SPACE(sizeof(int))] = {0}; char d;
	struct iovec iov = {&d, 1}; struct msghdr mh = {0};
	mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cb; mh.msg_controllen = sizeof cb;
	if (recvmsg(sp[0], &mh, 0) < 0) { close(sp[0]); return -1; }
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); nd->listener = -1;
	if (cm) memcpy(&nd->listener, CMSG_DATA(cm), sizeof(int));
	close(sp[0]);
	if (nd->listener < 0) return -1;
	nd->pid = pid; nd->alive = 1; nd->blocked = 0; nd->next_fd = 500; nd->nc = 0;
	ptrace(PTRACE_SEIZE, pid, 0, 0);
	return 0;
}

/* read any canonical-emit records this node wrote; keep the last */
static void
drain_canon(Node *nd)
{
	unsigned char all[4096]; int tot = 0, r;
	/* non-blocking: the pipe holds whatever the node emitted so far */
	int fl = fcntl(nd->fd3[0], F_GETFL); fcntl(nd->fd3[0], F_SETFL, fl | O_NONBLOCK);
	while (tot < (int) sizeof all && (r = (int) read(nd->fd3[0], all + tot, sizeof all - tot)) > 0) tot += r;
	int off = 0, rec = -1, rl = 0;
	while (off < tot) { int cnt = all[off]; if (off + 1 + cnt > tot) break; rec = off + 1; rl = cnt; off += 1 + cnt; }
	if (rec >= 0) { nd->canon_len = rl > SNAP ? SNAP : rl; memcpy(nd->canon, all + rec, nd->canon_len); }
}

/* bag helpers */
static void bag_add(int src, int dst, const unsigned char *b, int len)
{ for (int i=0;i<MAXMSG;i++) if(!bag[i].used){ bag[i].used=1; bag[i].src=src; bag[i].dst=dst; bag[i].len=len>MSGLEN?MSGLEN:len; memcpy(bag[i].body,b,bag[i].len); return; } }

/* the synthetic client: one WRITE(v) to node 0, delivered at start.
 * modeled as a message src=-1 (client) dst=0 already in node 0's recv. */

/*
 * Service one ready notification on node nd without blocking the search.
 * Returns 1 if it did something, 0 if nd had no pending notification.
 * A recv on an empty channel is DEFERRED (nd->blocked set, not answered).
 */
static struct seccomp_notif *g_req;
static struct seccomp_notif_resp *g_resp;

static int
try_pending_for(Node *nd, int client_val)
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
		case SYS_setsockopt: case SYS_bind: case SYS_listen: g_resp->val = 0; break;
		case SYS_connect:
		{
			/* read the dest port to learn the peer id */
			struct { unsigned short fam; unsigned short port; } sa;
			cr_read(nd->pid, a[1], &sa, sizeof sa);
			int port = (sa.port >> 8) | ((sa.port & 0xff) << 8);
			int peer = port - BASE;
			/* the node sends/recvs on the socket() fd (connect's a[0]);
			 * connect() itself returns 0 */
			cfd_set(nd, (int) a[0], peer);
			g_resp->val = 0; break;
		}
		case SYS_accept: case SYS_accept4:
		{
			int fd = nd->next_fd++;
			/* node 0 accepts the client (peer -1); any other node accepts
			 * from node 0 (the primary), for the demo's topology */
			cfd_set(nd, fd, (nd->id == 0) ? -1 : 0);
			g_resp->val = fd; break;
		}
		case SYS_write: case SYS_sendto:
		{
			int fd = (int) a[0];
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; } /* not ours (stdout, fd 3, libs) */
			int peer = cfd_peer(nd, fd);
			int len = (int) a[2];
			unsigned char buf[MSGLEN]; int k = len > MSGLEN ? MSGLEN : len;
			cr_read(nd->pid, a[1], buf, k);
			if (peer == -1)			/* reply to the client: ghost log */
			{ if (nr == SYS_write || nr == SYS_sendto) if (buf[0] == 'A' && nrep < 64) { rep_src[nrep] = nd->id; rep_val[nrep] = buf[1]; nrep++; } }
			else if (peer >= 0)		/* inter-node: buffer in the bag */
				bag_add(nd->id, peer, buf, k);
			g_resp->val = len; break;
		}
		case SYS_read: case SYS_recvfrom:
		{
			int fd = (int) a[0];
			if (fd < 500) { g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break; } /* loader/lib reads */
			int peer = cfd_peer(nd, fd);
			/* is a message for this node from `peer` pending? */
			int found = -1;
			for (int i = 0; i < MAXMSG; i++)
				if (bag[i].used && bag[i].dst == nd->id && (peer < 0 || bag[i].src == peer)) { found = i; break; }
			if (found >= 0)
			{
				int k = bag[found].len; if (k > (int) a[2]) k = (int) a[2];
				cr_write(nd->pid, a[1], bag[found].body, k);
				bag[found].used = 0; g_resp->val = k; break;
			}
			/* nothing pending: DEFER -- this node is blocked on recv */
			nd->blocked = 1; nd->block_id = g_req->id; nd->block_bufaddr = a[1];
			nd->block_want = (int) a[2]; nd->block_peer = peer;
			return 1;				/* do NOT answer */
		}
		default: g_resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
	}
	(void) client_val;
	if (ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_resp) != 0 && errno != ENOENT) {}
	return 1;
}

/* detect that a node has exited (its listener gives ENOTCONN/POLLHUP) */
static void
reap(Node *nd)
{
	if (!nd->alive) return;
	int st; pid_t r = waitpid(nd->pid, &st, WNOHANG);
	if (r == nd->pid) { nd->alive = 0; drain_canon(nd); }
}

/* answer a blocked node's deferred recv with a delivered message */
static void
deliver(Node *nd, const unsigned char *body, int len)
{
	int k = len; if (k > nd->block_want) k = nd->block_want;
	cr_write(nd->pid, nd->block_bufaddr, body, k);
	memset(g_resp, 0, g_sz.seccomp_notif_resp); g_resp->id = nd->block_id; g_resp->val = k;
	ioctl(nd->listener, SECCOMP_IOCTL_NOTIF_SEND, g_resp);
	nd->blocked = 0;
}

/* run all nodes to quiescence: every live node blocked on a recv, or
 * exited.  A node that is neither blocked nor exited is still running
 * between syscalls -- we must wait for its next notification, not treat
 * the lull as quiescence. */
static void
drive_quiescent(int client_val)
{
	for (;;)
	{
		for (int i = 0; i < M.nodes; i++) reap(&N[i]);
		/* service every node that has a ready notification now */
		int did = 0;
		for (int i = 0; i < M.nodes; i++)
			if (N[i].alive && !N[i].blocked)
				did |= try_pending_for(&N[i], client_val);
		if (did) continue;
		/* which live nodes are still running (not blocked)? wait for one */
		struct pollfd pf[MAXNODES]; int np = 0;
		for (int i = 0; i < M.nodes; i++)
			if (N[i].alive && !N[i].blocked) { pf[np].fd = N[i].listener; pf[np].events = POLLIN; np++; }
		if (np == 0) break;			/* all blocked or dead: quiescent */
		int r = poll(pf, np, 2000);
		if (r <= 0)
		{
			/* timeout: a running node made no syscall in 2s.  Re-reap in
			 * case it exited; if still stuck, break to avoid a hang (a
			 * node doing pure computation would be a target bug for M4). */
			int any = 0; for (int i = 0; i < M.nodes; i++) { reap(&N[i]); if (N[i].alive && !N[i].blocked) any = 1; }
			if (!any) break;
			break;
		}
	}
	for (int i = 0; i < M.nodes; i++) if (N[i].alive) drain_canon(&N[i]);
}

/* =================================================================== */
/* stateless-replay BFS over delivery sequences                       */
/* =================================================================== */
static uint64_t rotl64(uint64_t x,int r){return (x<<r)|(x>>(64-r));}
static uint64_t fmix64(uint64_t k){k^=k>>33;k*=0xff51afd7ed558ccdULL;k^=k>>33;k*=0xc4ceb9fe1a85ec53ULL;k^=k>>33;return k;}
static void fp128(const uint8_t*p,size_t n,uint64_t fp[2]){
	uint64_t h1=0x9368e53c2f6af274ULL^(n*0x87c37b91114253d5ULL),h2=0x586dcd208f7cd3fdULL+n;
	for(size_t i=0;i<n;i++){h1^=(uint64_t)p[i]<<((i%8)*8);h1=rotl64(h1,27)*5+0x52dce729;h2=rotl64(h2+p[i]+i,23)*0xd6e8feb86659fd93ULL;}
	fp[0]=fmix64(h1);fp[1]=fmix64(h2+n);
}
typedef struct{uint64_t a,b;}FP; static FP seen[65536]; static int nseen;
static int mark(uint64_t a,uint64_t b){for(int i=0;i<nseen;i++)if(seen[i].a==a&&seen[i].b==b)return 0;
	if(nseen<(int)(sizeof seen/sizeof*seen)){seen[nseen].a=a;seen[nseen].b=b;nseen++;}return 1;}

/* world fingerprint = concat of each node's canon + the bag (sorted) +
 * the reply log */
static void
world_fp(uint64_t fp[2])
{
	unsigned char w[1024]; int n = 0;
	for (int i = 0; i < M.nodes; i++) {
		w[n++] = (unsigned char) N[i].canon_len;
		for (int k = 0; k < N[i].canon_len && n < (int)sizeof w; k++) w[n++] = N[i].canon[k];
		w[n++] = (unsigned char) (N[i].alive ? 0 : 0xff);
	}
	/* bag, in a canonical order (sort by src,dst,body) */
	int idx[MAXMSG], m = 0;
	for (int i = 0; i < MAXMSG; i++) if (bag[i].used) idx[m++] = i;
	for (int a = 0; a < m; a++) for (int b = a + 1; b < m; b++) {
		Msg *x = &bag[idx[a]], *y = &bag[idx[b]];
		int cmp = x->src - y->src; if (!cmp) cmp = x->dst - y->dst; if (!cmp) cmp = memcmp(x->body, y->body, MSGLEN);
		if (cmp > 0) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; } }
	for (int a = 0; a < m && n + 3 + MSGLEN < (int)sizeof w; a++) {
		Msg *x = &bag[idx[a]]; w[n++] = x->src + 1; w[n++] = x->dst + 1; w[n++] = x->len;
		for (int k = 0; k < x->len; k++) w[n++] = x->body[k]; }
	w[n++] = (unsigned char) nrep;
	for (int i = 0; i < nrep && n + 2 < (int)sizeof w; i++) { w[n++] = rep_src[i]; w[n++] = rep_val[i]; }
	fp128(w, n, fp);
}

static void reset_world(void){ memset(bag,0,sizeof bag); nrep=0; }

/* launch fresh nodes, inject the client WRITE to node 0, drive to
 * quiescence, then apply the delivery sequence `seq` (each an index into
 * the deliverable list at that step), re-driving after each.  Fills the
 * deliverable list at the final quiescent point.  Returns 0 or -1. */
typedef struct { int src, dst, len; unsigned char body[MSGLEN]; } Deliv;

static int
run_seq(const int *seq, int seqlen, Deliv *deliverable, int *ndeliv)
{
	reset_world();
	for (int i = 0; i < M.nodes; i++) { N[i].id = i; if (launch(&N[i])) return -1; }
	/* the client's WRITE(v) to node 0: seed the bag as a message src=client(-1)->0 */
	unsigned char w[2] = {'W', (unsigned char) M.client_val};
	bag_add(-1, 0, w, 2);
	drive_quiescent(M.client_val);

	for (int s = 0; s < seqlen; s++)
	{
		/* enumerate deliverables now and pick seq[s] */
		int pick = seq[s], c = 0, chosen = -1;
		for (int i = 0; i < MAXMSG; i++) if (bag[i].used) {
			/* a message is deliverable if its dst node is blocked waiting
			 * (or client messages to node 0 already consumed at start) */
			int d = bag[i].dst;
			if (d >= 0 && d < M.nodes && N[d].alive && N[d].blocked &&
				(N[d].block_peer < 0 || N[d].block_peer == bag[i].src)) {
				if (c == pick) { chosen = i; break; } c++; }
		}
		if (chosen < 0) break;		/* seq ran past the deliverables */
		Msg mm = bag[chosen]; bag[chosen].used = 0;
		deliver(&N[mm.dst], mm.body, mm.len);
		drive_quiescent(M.client_val);
	}
	/* final deliverable list */
	int c = 0;
	for (int i = 0; i < MAXMSG; i++) if (bag[i].used) {
		int d = bag[i].dst;
		if (d >= 0 && d < M.nodes && N[d].alive && N[d].blocked &&
			(N[d].block_peer < 0 || N[d].block_peer == bag[i].src)) {
			deliverable[c].src = bag[i].src; deliverable[c].dst = d; deliverable[c].len = bag[i].len;
			memcpy(deliverable[c].body, bag[i].body, MSGLEN); c++; }
	}
	*ndeliv = c;
	return 0;
}

static void kill_all(void){ for(int i=0;i<M.nodes;i++) if(N[i].pid>0){kill(N[i].pid,SIGKILL); int st; waitpid(N[i].pid,&st,0); N[i].pid=0;} }

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

typedef struct { int len; int seq[MAXD]; } Seq;
static Seq Q[200000]; static int qh, qt;

int
main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s MODELFILE BINARY\n", argv[0]); return 2; }
	if (load_model(argv[1])) return 2;
	const char *binary = argv[2];
	if (is_pie(binary) != 0) { fprintf(stderr, "%s must be a non-PIE ELF\n", binary); return 2; }
	for (int i = 0; i < MAXNODES; i++) N[i].binary = binary;
	seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &g_sz);
	g_req = calloc(1, g_sz.seccomp_notif); g_resp = calloc(1, g_sz.seccomp_notif_resp);

	int distinct = 0, merges = 0;
	Q[qt].len = 0; qt++;

	while (qh < qt)
	{
		Seq cur = Q[qh++];
		Deliv dv[MAXMSG]; int nd = 0;
		if (run_seq(cur.seq, cur.len, dv, &nd)) { kill_all(); fprintf(stderr, "mc2: run failed\n"); return 2; }

		int which = -1; int ok = check_invariants(&which);
		if (which == -2) { kill_all(); return 2; }
		uint64_t fp[2]; world_fp(fp);
		int isnew = mark(fp[0], fp[1]);
		if (isnew) distinct++; else merges++;

		if (!ok) { kill_all();
			printf("VIOLATION: invariant %s does not hold\n", M.inv[which].name);
			printf("  delivery sequence (indices):");
			for (int i = 0; i < cur.len; i++) printf(" %d", cur.seq[i]);
			printf("\n  node states:");
			for (int i = 0; i < M.nodes; i++) { printf(" node[%d]=", i); for (int k = 0; k < N[i].canon_len; k++) printf("%d", N[i].canon[k]); }
			printf("\n  replies:"); for (int i = 0; i < nrep; i++) printf(" (n%d,%d)", rep_src[i], rep_val[i]);
			printf("\n");
			return 1; }

		if (isnew && cur.len < M.depth)
			for (int i = 0; i < nd; i++) { Seq nx = cur; nx.seq[nx.len++] = i; if (qt < (int)(sizeof Q/sizeof*Q)) Q[qt++] = nx; }
		kill_all();
	}
	printf("ok: %d invariant(s) held over %d distinct states (%d merges)\n", M.ninv, distinct, merges);
	return 0;
}
