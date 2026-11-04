/*
 * search.c: the M1 process-mode search (experiments/process-mode/DESIGN.md).
 *
 * Stateless prefix-replay BFS over a server written as it ships
 * (demo/kv.c): no libcmc header, no step function.  Each state is reached
 * by launching a fresh server under seccomp user-notification interception
 * and replaying a prefix of client messages; the state is the server's
 * declared region cmc_state (found by ELF symbol) plus the environment the
 * checker owns.  A 128-bit fingerprint dedups, so a state reached by two
 * orders is counted once -- the property that lets an exhaustive search
 * over a reconverging protocol terminate.
 *
 * Usage: search ./kv
 * Prints the distinct-state count, how many times dedup merged a diamond,
 * and whether the invariant "a cell once set stays set" holds; on a
 * violation it prints the shortest client-message prefix that reaches it.
 * Exit 0 = invariant held; 1 = violated; 2 = error (including a dedup
 * failure, M1's kill criterion).
 *
 * Verified on Linux 6.18, glibc 2.44.
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

static int	seccomp(unsigned op, unsigned flags, void *args)
{ return (int) syscall(SYS_seccomp, op, flags, args); }

/* ---- the alphabet of client actions -------------------------------- */
/* M1: two independent SETs.  An action is one byte sent to the server. */
static const unsigned char ALPHABET[] = {'a', 'b'};
#define NACT (int)(sizeof ALPHABET)
#define MAXLEN 8				/* longest prefix explored */
#define REGION 2				/* bytes of cmc_state */

/* ---- 128-bit fingerprint (same construction as src/cmc.c) ---------- */
static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t fmix64(uint64_t k)
{ k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
static void
fingerprint(const uint8_t *p, size_t n, uint64_t fp[2])
{
	uint64_t	h1 = 0x9368e53c2f6af274ULL ^ (n * 0x87c37b91114253d5ULL);
	uint64_t	h2 = 0x586dcd208f7cd3fdULL + n;
	for (size_t i = 0; i < n; i++)
	{
		h1 ^= (uint64_t) p[i] << ((i % 8) * 8);
		h1 = rotl64(h1, 27) * 5 + 0x52dce729;
		h2 = rotl64(h2 + p[i] + i, 23) * 0xd6e8feb86659fd93ULL;
	}
	fp[0] = fmix64(h1);
	fp[1] = fmix64(h2 + n);
}

/* ---- visited set + BFS worklist of prefixes ------------------------ */
typedef struct { uint64_t f0, f1; } FP;
static FP	seen[4096];
static int	nseen;
static int	dedup_hits;			/* times a successor was already seen */

static int
mark_seen(uint64_t f0, uint64_t f1)	/* returns 1 if newly inserted */
{
	for (int i = 0; i < nseen; i++)
		if (seen[i].f0 == f0 && seen[i].f1 == f1)
			return 0;
	seen[nseen].f0 = f0;
	seen[nseen].f1 = f1;
	nseen++;
	return 1;
}

typedef struct { int len; unsigned char act[MAXLEN]; } Prefix;
static Prefix q[8192];
static int	qhead, qtail;

/* ---- finding the declared region in the target binary -------------- */
/*
 * The region address is read from the binary's `cmc_state` symbol with
 * nm.  That link-time address is the runtime address only for a non-PIE
 * (ELF type EXEC) binary.  A PIE (type DYN) is relocated by ASLR, so the
 * symbol address would be wrong and every state would read as garbage --
 * a silent wrong answer.  M1 refuses a PIE rather than read it wrong;
 * lifting that means adding the load bias from /proc/PID/maps.
 */
static int
is_pie(const char *binary)
{
	int			fd = open(binary, O_RDONLY);
	unsigned char e[18] = {0};
	if (fd < 0) return -1;
	if (read(fd, e, sizeof e) != (ssize_t) sizeof e) { close(fd); return -1; }
	close(fd);
	if (memcmp(e, "\177ELF", 4) != 0) return -1;
	uint16_t	type = (uint16_t) (e[16] | (e[17] << 8));	/* e_type */
	return type == 3;									/* ET_DYN = PIE */
}

static uint64_t
region_addr(const char *binary)
{
	char		cmd[512];
	snprintf(cmd, sizeof cmd, "nm '%s' 2>/dev/null | awk '$3==\"cmc_state\"{print $1; exit}'", binary);
	FILE	   *p = popen(cmd, "r");
	uint64_t	addr = 0;
	if (p) { if (fscanf(p, "%lx", &addr) != 1) addr = 0; pclose(p); }
	return addr;
}

/* ---- run the server under interception, replay a prefix ------------ */
/*
 * Returns 0 and fills region[REGION] with cmc_state after the whole
 * prefix was acked; returns -1 on an internal error.  Interception serves
 * the socket calls, delivers one client byte per step from `pre`, and
 * reads the declared region from the child after each ACK.
 */
static uint64_t	g_region_addr;
static pid_t	g_child;

static void	cr_read(uint64_t addr, void *buf, size_t n)
{
	struct iovec l = {buf, n}, r = {(void *) (uintptr_t) addr, n};
	if (process_vm_readv(g_child, &l, 1, &r, 1, 0) != (ssize_t) n) memset(buf, 0, n);
}
static void	cr_write(uint64_t addr, const void *buf, size_t n)
{
	struct iovec l = {(void *) (uintptr_t) buf, n}, r = {(void *) (uintptr_t) addr, n};
	(void) process_vm_writev(g_child, &l, 1, &r, 1, 0);
}

static int
install_filter(void)
{
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#define OWN(n) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (n), 0, 1), \
			   BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF)
		OWN(SYS_socket), OWN(SYS_setsockopt), OWN(SYS_bind), OWN(SYS_listen),
		OWN(SYS_accept), OWN(SYS_accept4), OWN(SYS_read), OWN(SYS_write),
		OWN(SYS_recvfrom), OWN(SYS_sendto),
#undef OWN
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {sizeof f / sizeof f[0], f};
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	return seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

/*
 * run_prefix: launch `binary`, replay the `len` client bytes in `act`, and
 * return cmc_state in `out`.  Also fills `trail` with the region after
 * each step (trail[k] = state after k+1 messages) so the caller can
 * enumerate successors and check the invariant at every reached state.
 * Returns the number of states recorded in trail (== len on success).
 */
static int
run_prefix(const char *binary, const unsigned char *act, int len,
		   unsigned char trail[][REGION])
{
	int			sp[2];
	socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
	pid_t		pid = fork();
	if (pid == 0)
	{
		close(sp[0]);
		int			lf = install_filter();
		if (lf < 0) _exit(2);
		char		cbuf[CMSG_SPACE(sizeof(int))] = {0};
		char		d = 'L';
		struct iovec iov = {&d, 1};
		struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1;
		mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &lf, sizeof(int));
		sendmsg(sp[1], &mh, 0);
		execl(binary, binary, "0", (char *) NULL);
		_exit(2);
	}
	close(sp[1]);
	int			listener = -1;
	{
		char		cbuf[CMSG_SPACE(sizeof(int))] = {0};
		char		d;
		struct iovec iov = {&d, 1};
		struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1;
		mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
		if (recvmsg(sp[0], &mh, 0) < 0) return -1;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		if (cm) memcpy(&listener, CMSG_DATA(cm), sizeof(int));
	}
	close(sp[0]);
	if (listener < 0) return -1;

	g_child = pid;
	ptrace(PTRACE_SEIZE, pid, 0, 0);

	struct seccomp_notif_sizes sz;
	seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &sz);
	struct seccomp_notif *req = calloc(1, sz.seccomp_notif);
	struct seccomp_notif_resp *resp = calloc(1, sz.seccomp_notif_resp);

	int			conn_fd = -1, next_fd = 500;
	int			sent = 0, got_ack = 0, states = 0;
	int			rc = 0;

	for (;;)
	{
		memset(req, 0, sz.seccomp_notif);
		if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, req) < 0)
		{ if (errno == EINTR) continue; break; }
		long		nr = req->data.nr;
		__u64	   *ar = req->data.args;
		memset(resp, 0, sz.seccomp_notif_resp);
		resp->id = req->id;

		switch (nr)
		{
			case SYS_socket:      resp->val = next_fd++; break;
			case SYS_setsockopt:
			case SYS_bind:
			case SYS_listen:      resp->val = 0; break;
			case SYS_accept:
			case SYS_accept4:     conn_fd = next_fd++; resp->val = conn_fd; break;

			case SYS_read:
			case SYS_recvfrom:
				if ((int) ar[0] == conn_fd)
				{
					if (sent < len)
					{
						unsigned char byte = act[sent++];
						cr_write(ar[1], &byte, 1);
						resp->val = 1;
					}
					else
						resp->val = 0;	/* EOF: server loop ends */
				}
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;

			case SYS_write:
			case SYS_sendto:
				if ((int) ar[0] == conn_fd)
				{
					/* an ACK: the message `sent-1` is now applied.  Snapshot
					 * the declared region as the state after `sent` msgs. */
					if (sent >= 1 && states < len)
						cr_read(g_region_addr, trail[states++], REGION);
					got_ack++;
					resp->val = (int) ar[2];
				}
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;
			default: resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp) < 0 && errno != ENOENT)
			; /* target syscall gone; ignore */
	}
	free(req); free(resp); close(listener);
	kill(pid, SIGKILL);
	int st; waitpid(pid, &st, 0);
	(void) got_ack;
	return rc == 0 ? states : -1;
}

/* invariant: a cell once set to 1 never returns to 0.  Checked along the
 * trail, where trail[k] is the state after k+1 messages. */
static int
invariant_ok(unsigned char trail[][REGION], int n, int *bad_step)
{
	unsigned char prev[REGION] = {0};
	for (int k = 0; k < n; k++)
	{
		for (int c = 0; c < REGION; c++)
			if (prev[c] == 1 && trail[k][c] == 0)
			{ *bad_step = k; return 0; }
		memcpy(prev, trail[k], REGION);
	}
	return 1;
}

int
main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s ./kv\n", argv[0]); return 2; }
	const char *binary = argv[1];
	int			pie = is_pie(binary);
	if (pie < 0)
	{ fprintf(stderr, "%s: not an ELF binary\n", binary); return 2; }
	if (pie)
	{ fprintf(stderr, "%s is a PIE; M1 needs -no-pie (the symbol address is"
			  " fixed only then)\n", binary); return 2; }
	g_region_addr = region_addr(binary);
	if (!g_region_addr)
	{ fprintf(stderr, "could not find cmc_state in %s (build -no-pie?)\n", binary); return 2; }

	/* the empty prefix: the initial state (all zero) */
	uint8_t		buf[REGION + 1];
	uint64_t	fp[2];
	unsigned char zero[REGION] = {0};
	memcpy(buf, zero, REGION); buf[REGION] = 0;	/* env byte: 0 for M1 */
	fingerprint(buf, sizeof buf, fp);
	mark_seen(fp[0], fp[1]);
	q[qtail].len = 0; qtail++;

	int			distinct = 1;		/* the initial state */
	int			violated = 0;
	Prefix		witness = {0};

	while (qhead < qtail)
	{
		Prefix		pre = q[qhead++];
		for (int ai = 0; ai < NACT; ai++)
		{
			if (pre.len >= MAXLEN) continue;
			Prefix		nx = pre;
			nx.act[nx.len++] = ALPHABET[ai];

			unsigned char trail[MAXLEN][REGION];
			int			n = run_prefix(binary, nx.act, nx.len, trail);
			if (n != nx.len)
			{ fprintf(stderr, "replay short: got %d of %d\n", n, nx.len); return 2; }

			int			bad;
			if (!invariant_ok(trail, n, &bad))
			{ violated = 1; witness = nx; witness.len = bad + 1; goto done; }

			memcpy(buf, trail[n - 1], REGION); buf[REGION] = 0;
			fingerprint(buf, sizeof buf, fp);
			if (mark_seen(fp[0], fp[1]))
			{
				distinct++;
				if (nx.len < MAXLEN) { q[qtail] = nx; qtail++; }
			}
			else
				dedup_hits++;
		}
	}
done:
	if (violated)
	{
		printf("VIOLATION: a cell once set was cleared\n  prefix:");
		for (int i = 0; i < witness.len; i++) printf(" SET-%c", witness.act[i]);
		printf("\n");
		printf("distinct states so far: %d\n", distinct);
		return 1;
	}
	printf("distinct states: %d; dedup merges: %d\n", distinct, dedup_hits);
	if (dedup_hits == 0)
	{
		fprintf(stderr, "KILL CRITERION: no diamond merged -- "
				"two orders to the same state did not hash equal\n");
		return 2;
	}
	printf("ok: invariant holds, the search terminated, the diamond merged\n");
	return 0;
}
