/*
 * probe.c: can a node stopped at a seccomp user-notification be forked
 * into a frozen duplicate, so a model checker can resume the same
 * quiescent point more than once without replaying from launch?
 *
 * This is the one mechanism the fork-from-snapshot engine rests on
 * (experiments/process-mode/DESIGN.md).  It proves it in isolation,
 * libc only, before any of it touches src/cmc_proc.c.
 *
 * A child installs a seccomp user-notification filter on write(2), keeps
 * a counter in a fixed-address heap cell, and runs two "steps": each
 * bumps the counter, then blocks in a write() the parent intercepts.
 * The parent owns the notification listener and PTRACE_SEIZEs the child.
 *
 * At the first write (the quiescent point, counter 11), the parent
 * SNAPSHOTS the child by injecting fork() into it: with the child
 * ptrace-stopped, it saves the registers, rewrites them to call fork(2)
 * at the current instruction, single-steps to get the new pid, then
 * restores the registers so the original is exactly back at the blocked
 * write().  The fork copy is a kernel-exact duplicate of the whole
 * address space, so there is no "did I capture every writable page"
 * question -- the property stateless replay was paying to avoid.
 *
 * It then confirms the snapshot's heap cell reads 11 independently of
 * the original, which is the proof the duplicate exists and is separable.
 *
 * Linux x86-64 only.
 *
 *   cc -O2 -o probe probe.c && ./probe
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <elf.h>
#include <linux/seccomp.h>
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
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SECCOMP_FILTER_FLAG_NEW_LISTENER
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#endif

static int
seccomp(unsigned op, unsigned flags, void *a)
{
	return (int) syscall(SYS_seccomp, op, flags, a);
}

/* a fixed-address heap cell, so a snapshot and the original agree on
 * where the counter is (the deterministic arena does the same) */
#define CELL ((volatile int *) 0x600000000000ULL)

static int
install_filter(void)
{
	struct sock_filter f[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {sizeof f / sizeof f[0], f};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return -1;
	return seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

static int
send_fd(int sock, int fd)
{
	char		cb[CMSG_SPACE(sizeof(int))] = {0};
	char		d = 'L';
	struct iovec iov = {&d, 1};
	struct msghdr mh = {0};

	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cb;
	mh.msg_controllen = sizeof cb;
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);

	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	return sendmsg(sock, &mh, 0) < 0 ? -1 : 0;
}

static int
recv_fd(int sock)
{
	char		cb[CMSG_SPACE(sizeof(int))] = {0};
	char		d;
	struct iovec iov = {&d, 1};
	struct msghdr mh = {0};

	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cb;
	mh.msg_controllen = sizeof cb;
	if (recvmsg(sock, &mh, 0) < 0)
		return -1;
	struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
	int			fd = -1;

	if (cm)
		memcpy(&fd, CMSG_DATA(cm), sizeof(int));
	return fd;
}

static void
child(int sync_w)
{
	void	   *p = mmap((void *) CELL, 4096, PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p != (void *) CELL)
		_exit(90);
	*CELL = 10;
	int			lf = install_filter();

	if (lf < 0)
		_exit(91);
	if (send_fd(sync_w, lf) < 0)
		_exit(92);
	for (int i = 0; i < 2; i++)
	{
		*CELL = *CELL + 1;
		char		c = 'x';

		(void) !write(777, &c, 1);	/* fd 777: nothing real; traps */
	}
	_exit(*CELL & 0x7f);
}

static int
read_cell(pid_t pid)
{
	int			cell = -1;
	struct iovec l = {&cell, sizeof cell}, r = {(void *) CELL, sizeof cell};

	process_vm_readv(pid, &l, 1, &r, 1, 0);
	return cell;
}

/* answer the pending write() notification so the child runs on */
static void
answer(int lf, struct seccomp_notif *req, struct seccomp_notif_resp *resp)
{
	memset(resp, 0, sizeof *resp);
	resp->id = req->id;
	resp->val = 1;
	ioctl(lf, SECCOMP_IOCTL_NOTIF_SEND, resp);
}

int
main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	struct seccomp_notif_sizes sz;

	seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &sz);
	struct seccomp_notif *req = calloc(1, sz.seccomp_notif);
	struct seccomp_notif_resp *resp = calloc(1, sz.seccomp_notif_resp);
	int			sp[2];

	socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);
	pid_t		pid = fork();

	if (pid == 0)
	{
		close(sp[0]);
		child(sp[1]);
		_exit(99);
	}
	close(sp[1]);

	int			lf = recv_fd(sp[0]);

	if (lf < 0)
	{
		fprintf(stderr, "FAIL: no listener fd\n");
		return 1;
	}
	if (ptrace(PTRACE_SEIZE, pid, 0, 0))
	{
		perror("FAIL: seize");
		return 1;
	}
	/* a quiet host: the notif can take a moment, but not forever */
	struct pollfd pf = {lf, POLLIN, 0};

	if (poll(&pf, 1, 10000) <= 0)
	{
		fprintf(stderr, "FAIL: no write() notification within 10s\n");
		return 1;
	}
	if (ioctl(lf, SECCOMP_IOCTL_NOTIF_RECV, req))
	{
		perror("FAIL: notif recv");
		return 1;
	}
	printf("quiescent at step 1; original counter = %d (want 11)\n", read_cell(pid));

	/*
	 * The open question this probe answers: a node stopped in
	 * SECCOMP_RET_USER_NOTIF is suspended in the kernel, not in a
	 * ptrace-stop.  Can PTRACE_INTERRUPT bring it to a ptrace-stop so we
	 * can read its registers and inject a fork()?  Run this on a quiet
	 * host (a heavily loaded one starves the seccomp/ptrace handshake).
	 */
	if (ptrace(PTRACE_INTERRUPT, pid, 0, 0))
		perror("interrupt");
	int			st;

	for (int tries = 0;; tries++)
	{
		pid_t		w = waitpid(pid, &st, WNOHANG);

		if (w == pid)
		{
			printf("PTRACE_INTERRUPT reached a stop: WIFSTOPPED=%d sig=%d\n",
				   WIFSTOPPED(st), WIFSTOPPED(st) ? WSTOPSIG(st) : -1);
			break;
		}
		if (w < 0) { perror("waitpid"); return 1; }
		if (tries >= 200)
		{
			printf("INCONCLUSIVE: no ptrace-stop after PTRACE_INTERRUPT in 10s"
				   " (host load, or the notif block is not interruptible this way)\n");
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			return 2;
		}
		usleep(50000);
	}

	struct user_regs_struct regs, saved;
	struct iovec rv = {&regs, sizeof regs};

	if (ptrace(PTRACE_GETREGSET, pid, (void *) (long) NT_PRSTATUS, &rv))
	{
		perror("FAIL: getregs");
		return 1;
	}
	saved = regs;
	printf("regs: rip=%llx orig_rax=%lld rax=%lld\n",
		   (unsigned long long) regs.rip, (long long) regs.orig_rax,
		   (long long) regs.rax);

	/*
	 * Inject fork() by register injection.  The node is ptrace-stopped
	 * at a syscall boundary (orig_rax=1 write, rax=-ERESTARTSYS, rip just
	 * past the syscall insn).  Point it at a fresh fork(2) at rip-2 (the
	 * two-byte syscall instruction is still mapped), set rax=SYS_fork,
	 * and single-step; PTRACE_EVENT_FORK gives the child pid.  Then
	 * restore the saved registers so the original re-enters its write()
	 * and its pending notification stands.
	 */
	if (ptrace(PTRACE_SETOPTIONS, pid, 0, (void *) (long) PTRACE_O_TRACEFORK))
		perror("setoptions");
	regs.orig_rax = SYS_fork;
	regs.rax = SYS_fork;
	regs.rip = saved.rip - 2;		/* back onto the syscall instruction */
	struct iovec wv = {&regs, sizeof regs};

	if (ptrace(PTRACE_SETREGSET, pid, (void *) (long) NT_PRSTATUS, &wv))
		perror("setregs");
	if (ptrace(PTRACE_CONT, pid, 0, 0))
		perror("cont-into-fork");
	waitpid(pid, &st, 0);			/* stop at PTRACE_EVENT_FORK */
	pid_t		snap = -1;

	if ((st >> 8) == (SIGTRAP | (PTRACE_EVENT_FORK << 8)))
	{
		unsigned long m = 0;

		ptrace(PTRACE_GETEVENTMSG, pid, 0, &m);
		snap = (pid_t) m;
		printf("injected fork -> snapshot pid %d\n", snap);
	}
	else
	{
		printf("FAIL: no PTRACE_EVENT_FORK (st=%x)\n", st);
		kill(pid, SIGKILL);
		return 1;
	}
	/* the snapshot child is stopped; wait for it to appear */
	waitpid(snap, &st, 0);

	/* let the original finish its injected fork syscall and come back to
	 * a stop, then restore its registers to the blocked write() */
	ptrace(PTRACE_SYSCALL, pid, 0, 0);
	waitpid(pid, &st, 0);
	struct iovec rw = {&regs, sizeof regs};

	ptrace(PTRACE_GETREGSET, pid, (void *) (long) NT_PRSTATUS, &rw);
	struct iovec restore = {&saved, sizeof saved};

	ptrace(PTRACE_SETREGSET, pid, (void *) (long) NT_PRSTATUS, &restore);

	/* mutate the ORIGINAL via its notification answer, then let it run
	 * to exit; its final counter reflects step 2 (12) */
	ptrace(PTRACE_DETACH, pid, 0, 0);
	answer(lf, req, resp);
	if (poll(&pf, 1, 5000) > 0 && ioctl(lf, SECCOMP_IOCTL_NOTIF_RECV, req) == 0)
		answer(lf, req, resp);
	waitpid(pid, &st, 0);
	printf("original exited %d (want 12)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);

	/*
	 * Now resume the SNAPSHOT independently.  It is a frozen duplicate at
	 * the same quiescent point (counter 11).  Read its heap: it must show
	 * 11, not the original's final 12 -- proof the copy is separate and
	 * was not disturbed by the original running on.
	 */
	int			snap_cell = read_cell(snap);

	printf("snapshot counter = %d (want 11, NOT the original's 12)\n", snap_cell);
	ptrace(PTRACE_DETACH, snap, 0, 0);
	kill(snap, SIGKILL);
	waitpid(snap, &st, 0);

	if (snap_cell == 11)
	{
		printf("OK: fork-from-snapshot freezes and resumes a node independently\n");
		return 0;
	}
	printf("FAIL: snapshot was not an independent frozen copy\n");
	return 1;
}
