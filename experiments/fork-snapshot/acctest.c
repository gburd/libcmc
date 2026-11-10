/*
 * acctest.c: does an accept()-blocked worker re-trap after resume that makes fork-from-snapshot DFS work.
 *
 * A frozen "master" node (stopped at a seccomp user-notification) is
 * forked exactly ONCE, yielding (a worker = the parent, which runs this
 * action, and a next frozen master = the child, for the next action).
 * No master is ever re-forked; the DFS holds one frozen master per
 * stack level and chains forward.  This proves the three mechanics the
 * engine needs, which the single-snapshot probe.c did not:
 *
 *   1. Fork once -> (worker, next master); the child is auto-attached
 *      and left stopped at the syscall boundary = a pristine snapshot.
 *   2. Resume a worker with PTRACE_CONT, NOT PTRACE_DETACH: it must stay
 *      our tracee so process_vm_readv keeps permission under yama
 *      ptrace_scope=1 (a detached grandchild gives EPERM).
 *   3. Zero the seccomp_notif buffer before every NOTIF_RECV, or the
 *      ioctl gives EINVAL.
 *
 * Each worker re-issues its blocked write() and raises a FRESH notif the
 * checker services (the old notif is abandoned by the ptrace excursion);
 * every chained master stays pristine (counter 11).
 *
 * Linux x86-64 only.  cc -O2 -o chain chain.c && ./chain   (quiet host)
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
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
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif
static int seccomp(unsigned op, unsigned fl, void *a) { return (int) syscall(SYS_seccomp, op, fl, a); }
#define CELL ((volatile int *) 0x600000000000ULL)

static struct seccomp_notif_sizes sz;
static struct seccomp_notif *req;
static struct seccomp_notif_resp *resp;
static int	lf;

static int
inst(void)
{
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_accept, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
	struct sock_fprog p = {4, f};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return -1;
	return seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &p);
}
static int
sfd(int s, int fd)
{
	char		cb[CMSG_SPACE(sizeof(int))] = {0};
	char		d = 'L';
	struct iovec io = {&d, 1};
	struct msghdr m = {0};

	m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cb; m.msg_controllen = sizeof cb;
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);

	c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &fd, sizeof(int));
	return sendmsg(s, &m, 0) < 0 ? -1 : 0;
}
static int
rfd(int s)
{
	char		cb[CMSG_SPACE(sizeof(int))] = {0};
	char		d;
	struct iovec io = {&d, 1};
	struct msghdr m = {0};

	m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cb; m.msg_controllen = sizeof cb;
	if (recvmsg(s, &m, 0) < 0)
		return -1;
	struct cmsghdr *c = CMSG_FIRSTHDR(&m);
	int			fd = -1;

	if (c)
		memcpy(&fd, CMSG_DATA(c), sizeof(int));
	return fd;
}
static void
node(int w)
{
	if (mmap((void *) CELL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *) CELL)
		_exit(90);
	*CELL = 10;
	int			l = inst();

	if (l < 0 || sfd(w, l))
		_exit(91);
	for (;;)
	{
		*CELL = *CELL + 1;
		(void) !accept(777, 0, 0);   /* traps to the notif; fd is irrelevant, intercepted */
	}
}
static int
rc(pid_t p)
{
	int			v = -1;
	struct iovec a = {&v, sizeof v}, b = {(void *) CELL, sizeof v};

	process_vm_readv(p, &a, 1, &b, 1, 0);
	return v;
}

/* master is ptrace-stopped at the syscall boundary.  Fork it once:
 * *worker = the parent (restored to the boundary, still stopped); return
 * the child = a fresh frozen master (auto-attached, restored, stopped). */
static pid_t
fork_once(pid_t master, struct user_regs_struct saved, pid_t *worker)
{
	ptrace(PTRACE_SETOPTIONS, master, 0, (void *) (long) PTRACE_O_TRACEFORK);
	struct user_regs_struct r = saved;

	r.orig_rax = SYS_fork; r.rax = SYS_fork; r.rip = saved.rip - 2;
	struct iovec wv = {&r, sizeof r};

	ptrace(PTRACE_SETREGSET, master, (void *) (long) NT_PRSTATUS, &wv);
	ptrace(PTRACE_CONT, master, 0, 0);
	int			st;

	waitpid(master, &st, 0);
	if ((st >> 8) != (SIGTRAP | (PTRACE_EVENT_FORK << 8)))
		return -1;
	unsigned long m = 0;

	ptrace(PTRACE_GETEVENTMSG, master, 0, &m);
	pid_t		child = (pid_t) m;

	waitpid(child, &st, 0);			/* the child's PTRACE_EVENT_STOP */
	struct iovec rv = {&saved, sizeof saved};

	ptrace(PTRACE_SETREGSET, master, (void *) (long) NT_PRSTATUS, &rv);
	ptrace(PTRACE_SETREGSET, child, (void *) (long) NT_PRSTATUS, &rv);
	*worker = master;
	return child;
}
/* resume a worker (stay attached via PTRACE_CONT) so it re-issues its
 * write() and raises a fresh notif; service it; return its counter */
static int
resume_worker(pid_t worker)
{
	ptrace(PTRACE_CONT, worker, 0, 0);
	struct pollfd pf = {lf, POLLIN, 0};

	if (poll(&pf, 1, 4000) <= 0)
		return -2;
	memset(req, 0, sz.seccomp_notif);
	if (ioctl(lf, SECCOMP_IOCTL_NOTIF_RECV, req))
		return -3;
	int			cell = rc(worker);

	memset(resp, 0, sz.seccomp_notif_resp);
	resp->id = req->id; resp->val = 1;
	ioctl(lf, SECCOMP_IOCTL_NOTIF_SEND, resp);
	return cell;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(30);
	seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &sz);
	req = calloc(1, sz.seccomp_notif);
	resp = calloc(1, sz.seccomp_notif_resp);
	int			sp[2];

	socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
	pid_t		pid = fork();

	if (pid == 0) { close(sp[0]); node(sp[1]); _exit(99); }
	close(sp[1]);
	lf = rfd(sp[0]);
	ptrace(PTRACE_SEIZE, pid, 0, 0);
	struct pollfd pf = {lf, POLLIN, 0};

	poll(&pf, 1, 4000);
	memset(req, 0, sz.seccomp_notif);
	ioctl(lf, SECCOMP_IOCTL_NOTIF_RECV, req);
	printf("quiescent, counter=%d (want 11)\n", rc(pid));

	ptrace(PTRACE_INTERRUPT, pid, 0, 0);
	int			st;

	waitpid(pid, &st, 0);
	pid_t		master = pid;
	int			ok = 1;

	for (int i = 0; i < 3; i++)
	{
		struct user_regs_struct s2;
		struct iovec r2 = {&s2, sizeof s2};

		if (ptrace(PTRACE_GETREGSET, master, (void *) (long) NT_PRSTATUS, &r2))
		{ printf("FAIL getregs master (iter %d) errno=%d\n", i, errno); ok = 0; break; }
		pid_t		worker;
		pid_t		next = fork_once(master, s2, &worker);

		if (next < 0) { printf("FAIL fork_once (iter %d)\n", i); ok = 0; break; }
		int			wc = resume_worker(worker);
		int			mc = rc(next);

		printf("iter %d: worker counter=%d (want 12) -> %s; next master=%d counter=%d (want 11) -> %s\n",
			   i, wc, wc == 12 ? "OK" : "BAD", next, mc, mc == 11 ? "OK" : "BAD");
		if (wc != 12 || mc != 11)
			ok = 0;
		kill(worker, SIGKILL); { int s; waitpid(worker, &s, 0); }
		master = next;
	}
	kill(master, SIGKILL); { int s; waitpid(master, &s, 0); }
	printf("%s\n", ok ? "OK: snapshot chain -- 3 workers spawned, each master pristine" : "FAIL");
	return ok ? 0 : 1;
}
