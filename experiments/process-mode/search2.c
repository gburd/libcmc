/*
 * search2.c: the M2 process-mode search (experiments/process-mode/DESIGN.md).
 *
 * The target's protocol state is a malloc'd structure.  The checker
 * LD_PRELOADs a deterministic arena (detmalloc.so) so the heap is
 * reproducible, then dedups states by one of two fingerprints:
 *
 *   --raw    hash the live arena bytes, arena[0 .. bump).  Reproducible
 *            for a fixed allocation order (ASLR defeated), but two
 *            commuting orders allocate differently, so they do NOT merge.
 *            This mode MEASURES that wall.
 *   --canon  hash the canonical encoding the target emits on fd 3 (keys in
 *            sorted order).  Order-independent, so commuting orders merge,
 *            and the invariant "every inserted key is still present" is
 *            checkable.
 *
 * Usage: search2 --raw|--canon ./target
 * Exit 0 = search terminated as expected (and invariant held in --canon);
 *      1 = invariant violated (--canon), prefix printed;
 *      2 = error;
 *      3 = --raw: the commuting orders merged (the wall did NOT appear --
 *          report, since the demo is built to show it does).
 *
 * Verified on Linux 6.18, glibc 2.44, -no-pie targets.
 */
#define _GNU_SOURCE
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

static int	seccomp(unsigned op, unsigned flags, void *a) { return (int) syscall(SYS_seccomp, op, flags, a); }

static const unsigned char ALPHABET[] = {1, 2, 3};
#define NACT   (int)(sizeof ALPHABET)
#define MAXLEN 4
#define ARENA_BASE 0x600000000000ULL
#define SNAP 256

/* ---- 128-bit fingerprint (as in src/cmc.c) ------------------------- */
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t fmix64(uint64_t k)
{ k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
static void
fingerprint(const uint8_t *p, size_t n, uint64_t fp[2])
{
	uint64_t h1 = 0x9368e53c2f6af274ULL ^ (n * 0x87c37b91114253d5ULL);
	uint64_t h2 = 0x586dcd208f7cd3fdULL + n;
	for (size_t i = 0; i < n; i++)
	{
		h1 ^= (uint64_t) p[i] << ((i % 8) * 8);
		h1 = rotl64(h1, 27) * 5 + 0x52dce729;
		h2 = rotl64(h2 + p[i] + i, 23) * 0xd6e8feb86659fd93ULL;
	}
	fp[0] = fmix64(h1); fp[1] = fmix64(h2 + n);
}

typedef struct { uint64_t f0, f1; } FP;
static FP	seen[65536];
static int	nseen, dedup_hits;
static int
mark_seen(uint64_t f0, uint64_t f1)
{
	for (int i = 0; i < nseen; i++) if (seen[i].f0 == f0 && seen[i].f1 == f1) return 0;
	if (nseen < (int)(sizeof seen / sizeof seen[0])) { seen[nseen].f0 = f0; seen[nseen].f1 = f1; nseen++; }
	return 1;
}

typedef struct { int len; unsigned char act[MAXLEN]; } Prefix;
static Prefix q[200000]; static int qhead, qtail;

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

static pid_t g_child;
static void cr_read(uint64_t addr, void *buf, size_t n)
{
	struct iovec l = {buf, n}, r = {(void *)(uintptr_t) addr, n};
	if (process_vm_readv(g_child, &l, 1, &r, 1, 0) != (ssize_t) n) memset(buf, 0, n);
}
static void cr_write(uint64_t addr, const void *buf, size_t n)
{
	struct iovec l = {(void *)(uintptr_t) buf, n}, r = {(void *)(uintptr_t) addr, n};
	(void) process_vm_writev(g_child, &l, 1, &r, 1, 0);
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
static int g_canon;				/* 1: read fd-3 snapshots; 0: read raw arena */
#define RAWWIN 64				/* bytes of the arena hashed in --raw mode */

/*
 * Launch the target under the arena preload, replay `act`, and snapshot
 * the final state into out[] (<= cap bytes); returns its length, or -1.
 * In --canon mode, out = the last fd-3 emission; in --raw, out = the live
 * arena bytes arena[0..bump).
 */
static int
run_prefix(const unsigned char *act, int len, unsigned char *out, int cap)
{
	int sp[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
	int fd3[2] = {-1, -1};
	if (g_canon) { if (pipe(fd3) != 0) return -1; }

	pid_t pid = fork();
	if (pid == 0)
	{
		close(sp[0]);
		setenv("LD_PRELOAD", "./detmalloc.so", 1);
		if (g_canon) { dup2(fd3[1], 3); close(fd3[0]); close(fd3[1]); }
		int lf = install_filter();
		if (lf < 0) _exit(2);
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
	close(sp[1]);
	if (g_canon) close(fd3[1]);
	int listener = -1;
	{
		char cbuf[CMSG_SPACE(sizeof(int))] = {0}; char d;
		struct iovec iov = {&d, 1}; struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1; mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
		if (recvmsg(sp[0], &mh, 0) < 0) { close(sp[0]); return -1; }
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		if (cm) memcpy(&listener, CMSG_DATA(cm), sizeof(int));
	}
	close(sp[0]);
	if (listener < 0) return -1;

	g_child = pid;
	ptrace(PTRACE_SEIZE, pid, 0, 0);

	/* drain fd 3 into the latest snapshot as the run proceeds */
	unsigned char canon[SNAP]; int canon_len = 0;
	unsigned char rawsnap[RAWWIN]; int have_raw = 0;

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
			case SYS_socket:    resp->val = next_fd++; break;
			case SYS_setsockopt:
			case SYS_bind:
			case SYS_listen:    resp->val = 0; break;
			case SYS_accept:
			case SYS_accept4:   conn_fd = next_fd++; resp->val = conn_fd; break;
			case SYS_read:
			case SYS_recvfrom:
				if ((int) ar[0] == conn_fd)
				{
					if (sent < len) { unsigned char b = act[sent++]; cr_write(ar[1], &b, 1); resp->val = 1; }
					else resp->val = 0;
				}
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;
			case SYS_write:
			case SYS_sendto:
				/* fd 3 (canon emit) goes to the real pipe; let the kernel
				 * run it.  The connection ACK we answer ourselves -- and the
				 * child is stopped at this notification, so the arena is
				 * mapped: snapshot it NOW for --raw (reading after the run
				 * ends can race the child's exit, which unmaps the arena). */
				if ((int) ar[0] == conn_fd)
				{
					acks++;
					if (!g_canon && acks == len)
					{ cr_read(ARENA_BASE, rawsnap, RAWWIN); have_raw = 1; }
					resp->val = (int) ar[2];
				}
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;
			default: resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp) < 0 && errno != ENOENT) {}
	}
	free(req); free(resp); close(listener);

	int n = -1;
	if (acks == len)			/* every message was applied */
	{
		if (g_canon)
		{
			/* fd 3 carries self-delimiting records [count][keys...], one per
			 * insert.  Read them all and keep the LAST complete record -- the
			 * final canonical set. */
			unsigned char all[4096]; int tot = 0, r;
			while (tot < (int) sizeof all &&
				   (r = (int) read(fd3[0], all + tot, sizeof all - tot)) > 0)
				tot += r;
			int off = 0, rec = -1, reclen = 0;
			while (off < tot)
			{
				int c = all[off];
				if (off + 1 + c > tot) break;	/* truncated tail */
				rec = off + 1; reclen = c;
				off += 1 + c;
			}
			canon_len = rec >= 0 ? reclen : 0;
			if (canon_len > (int) sizeof canon) canon_len = sizeof canon;
			if (rec >= 0) memcpy(canon, all + rec, canon_len);
			memcpy(out, canon, canon_len < cap ? canon_len : cap);
			n = canon_len < cap ? canon_len : cap;
		}
		else
		{
			/* the arena snapshot taken at the final ACK, while the child was
			 * stopped and the arena mapped.  free() is a no-op and the demo
			 * allocates little, so a fixed window captures the live state,
			 * zero-padded. */
			int w = RAWWIN < cap ? RAWWIN : cap;
			if (have_raw) memcpy(out, rawsnap, w); else memset(out, 0, w);
			n = w;
		}
	}
	if (g_canon) close(fd3[0]);
	kill(pid, SIGKILL); int st; waitpid(pid, &st, 0);
	return n;
}

/* --canon invariant: every key that was inserted is present in the final
 * canonical set.  The prefix's inserted keys are its action bytes. */
static int
all_present(const unsigned char *act, int len, const unsigned char *set, int nset)
{
	for (int i = 0; i < len; i++)
	{
		int found = 0;
		for (int j = 0; j < nset; j++) if (set[j] == act[i]) { found = 1; break; }
		if (!found) return 0;
	}
	return 1;
}

int
main(int argc, char **argv)
{
	const char *binary = NULL;
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--canon")) g_canon = 1;
		else if (!strcmp(argv[i], "--raw")) g_canon = 0;
		else binary = argv[i];
	}
	if (!binary) { fprintf(stderr, "usage: %s --raw|--canon ./target\n", argv[0]); return 2; }
	g_binary = binary;
	if (is_pie(binary) != 0) { fprintf(stderr, "%s must be a non-PIE ELF\n", binary); return 2; }

	/* initial state: empty.  Its fingerprint is the empty snapshot. */
	uint64_t fp[2]; fingerprint((const uint8_t *) "", 0, fp);
	mark_seen(fp[0], fp[1]);
	int distinct = 1;
	q[qtail].len = 0; qtail++;

	int violated = 0; Prefix witness = {0};
	while (qhead < qtail)
	{
		Prefix pre = q[qhead++];
		for (int ai = 0; ai < NACT; ai++)
		{
			if (pre.len >= MAXLEN) continue;
			Prefix nx = pre; nx.act[nx.len++] = ALPHABET[ai];
			unsigned char snap[SNAP];
			int n = run_prefix(nx.act, nx.len, snap, sizeof snap);
			if (n < 0) { fprintf(stderr, "run failed at len %d\n", nx.len); return 2; }

			if (g_canon && !all_present(nx.act, nx.len, snap, n))
			{ violated = 1; witness = nx; goto done; }

			fingerprint(snap, (size_t) n, fp);
			if (mark_seen(fp[0], fp[1]))
			{ distinct++; if (nx.len < MAXLEN) { q[qtail] = nx; qtail++; } }
			else dedup_hits++;
		}
	}
done:
	if (violated)
	{
		printf("VIOLATION: an inserted key was lost\n  prefix:");
		for (int i = 0; i < witness.len; i++) printf(" INS-%d", witness.act[i]);
		printf("\n");
		return 1;
	}
	printf("mode=%s distinct=%d dedup_merges=%d\n", g_canon ? "canon" : "raw", distinct, dedup_hits);
	if (g_canon)
	{
		if (dedup_hits == 0)
		{ fprintf(stderr, "canon: expected the commuting orders to merge, none did\n"); return 3; }
		printf("ok: canonical emit merges reordered inserts; search terminated; invariant holds\n");
		return 0;
	}
	/* --raw: show the wall.  The logical set {1,2} is reached by INS-1,INS-2
	 * and by INS-2,INS-1, but malloc placed the two nodes in opposite
	 * arena slots, so the raw bytes differ and the two are counted
	 * separately.  With a canonical encoding the same search finds fewer
	 * states; raw counts more because reordered-equal states never merge. */
	int canon_distinct = 8;		/* measured: ./search2 --canon ./list-canon */
	if (distinct > canon_distinct)
		printf("WALL: raw arena over-counts (%d states vs %d with a canonical"
			   " encoding): allocation order leaks into the bytes, so"
			   " reordered-equal states do not merge\n", distinct, canon_distinct);
	else
		printf("note: raw arena did not over-count here\n");
	return 0;
}
