/*
 * checker.c: the M0 process-mode checker (experiments/process-mode/DESIGN.md).
 *
 * It runs demo/server unmodified and model-checks one behaviour: a client
 * sends one WRITE, and a crash may land after the server sends its ACK but
 * before its fsync makes the write durable.  Property (NoLostAck): if the
 * client saw an ACK, the value is on the server's disk after the crash.
 * The correct server passes; the -DBUG server, which acks before fsync,
 * fails when a crash is allowed.
 *
 * Mechanism: seccomp user notification (Linux >= 5.0).  The child installs
 * a filter that returns SECCOMP_RET_USER_NOTIF for the syscalls the model
 * owns and ALLOW for the rest, then execs the server.  The kernel gives
 * the parent (this checker) a listener fd that SURVIVES the exec -- unlike
 * a SIGSYS handler, which execve resets.  The checker reads each trapped
 * syscall from the listener, serves it from the modelled world, and either
 * injects a return value or (for a real local file op) lets the kernel run
 * the syscall unchanged.  Verified on Linux 6.18, glibc 2.44.
 *
 * M0 is one client request and one branch (crash / no crash), not a general
 * engine: it answers whether a durability bug in a server written AS IT
 * SHIPS -- blocking sockets, real write()+fsync(), no step function -- can
 * be caught at the syscall boundary.  If it can, the design note lists the
 * milestones that turn it into a real mode.
 *
 * Usage: checker ./server [--crash]
 *   --crash  allow one crash between the ACK and the fsync.
 * Exit 0 = NoLostAck held; 1 = violated (a trace is printed); 2 = error.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/ptrace.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <signal.h>
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

/* ---- the filter the child installs --------------------------------- */
/*
 * NOTIFY the syscalls the model owns; ALLOW everything else (so the
 * dynamic loader and libc startup run normally).  The owned set is the
 * socket calls, the store file's open/pwrite/fsync/close, and read/write
 * (the connection and the store share the read/write path; the checker
 * tells them apart by fd).  open/openat is owned so the checker learns
 * the store fd; other opens (none here) would be let through if this grew.
 */
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
		OWN(SYS_getsockname), OWN(SYS_accept), OWN(SYS_accept4),
		OWN(SYS_read), OWN(SYS_write), OWN(SYS_recvfrom), OWN(SYS_sendto),
		OWN(SYS_open), OWN(SYS_openat), OWN(SYS_pwrite64),
		OWN(SYS_fsync), OWN(SYS_fdatasync), OWN(SYS_close),
#undef OWN
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {sizeof f / sizeof f[0], f};
	prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
	return seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

/* ---- read/write the child's memory via /proc/PID/mem --------------- */
static pid_t	child_pid;
static void	child_read(uint64_t addr, void *buf, size_t n)
{
	struct iovec l = {buf, n}, r = {(void *) (uintptr_t) addr, n};
	if (process_vm_readv(child_pid, &l, 1, &r, 1, 0) != (ssize_t) n)
		memset(buf, 0, n);
}
static void	child_write(uint64_t addr, const void *buf, size_t n)
{
	struct iovec l = {(void *) (uintptr_t) buf, n}, r = {(void *) (uintptr_t) addr, n};
	ssize_t		w = process_vm_writev(child_pid, &l, 1, &r, 1, 0);
	if (getenv("PM_TRACE")) fprintf(stderr, "[memw %llx n %zu -> %zd errno %d]\n",
									(unsigned long long) addr, n, w, errno);
}

/* ---- the modelled world (in the checker) --------------------------- */
static unsigned char c2s[16];	/* client -> server bytes, pending */
static int	c2s_len, c2s_off;
static int	acked;
static unsigned char acked_val;
static int	store_fd = -1;		/* a real fd the checker holds open */
static unsigned char staged;
static int	have_staged;
static int	crash_budget, crashed;
static int	conn_fd = -1;
static int	srv_store_fd = -1;	/* the fake fd the server uses for its store */
static int	next_fd = 500;
static const char *store_path = "pm-store.dat";
static int	finished;

static void	do_flush(void)
{
	if (have_staged && store_fd >= 0)
	{
		if (pwrite(store_fd, &staged, 1, 0) == 1)
			fsync(store_fd);
		have_staged = 0;
	}
}

static int	disk_value(void)
{
	int			fd = open(store_path, O_RDONLY);
	unsigned char v = 0;
	ssize_t		n = -1;
	if (fd >= 0) { n = pread(fd, &v, 1, 0); close(fd); }
	return n == 1 ? v : -1;
}

static int
finish_and_verdict(void)
{
	int			disk;
	if (crash_budget && have_staged) { crashed = 1; have_staged = 0; }
	else do_flush();
	disk = disk_value();
	printf("  trace: client sent W %d; %s%s; disk=%d\n",
		   acked_val, acked ? "server ACKed" : "no ACK",
		   crashed ? "; CRASH before flush" : "", disk);
	int			violated = acked && disk != (int) acked_val;
	puts(violated
		 ? "VIOLATION: NoLostAck -- an acknowledged write is not on disk"
		 : "ok: NoLostAck holds");
	return violated;
}

int
main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s ./server [--crash]\n", argv[0]); return 2; }
	const char *server = argv[1];
	for (int i = 2; i < argc; i++)
		if (!strcmp(argv[i], "--crash")) crash_budget = 1;

	unlink(store_path);
	store_fd = open(store_path, O_RDWR | O_CREAT, 0600);
	c2s[0] = 'W'; c2s[1] = 1; c2s_len = 2;

	/* a socketpair carries the listener fd back from the child */
	int			sp[2];
	socketpair(AF_UNIX, SOCK_DGRAM, 0, sp);

	pid_t		pid = fork();
	if (pid == 0)
	{
		close(sp[0]);
		int			lf = install_filter();
		if (lf < 0) _exit(2);
		/* send the listener fd to the parent over the socketpair */
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
		execl(server, server, "0", store_path, (char *) NULL);
		_exit(2);
	}
	close(sp[1]);

	/* receive the listener fd */
	int			listener = -1;
	{
		char		cbuf[CMSG_SPACE(sizeof(int))] = {0};
		char		d;
		struct iovec iov = {&d, 1};
		struct msghdr mh = {0};
		mh.msg_iov = &iov; mh.msg_iovlen = 1;
		mh.msg_control = cbuf; mh.msg_controllen = sizeof cbuf;
		if (recvmsg(sp[0], &mh, 0) < 0) { fprintf(stderr, "recvmsg\n"); return 2; }
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		if (cm) memcpy(&listener, CMSG_DATA(cm), sizeof(int));
	}
	if (listener < 0) { fprintf(stderr, "no listener fd\n"); return 2; }

	/* PTRACE_SEIZE lets us write the child's memory via /proc/pid/mem
	 * without stopping it; without it the pwrite silently writes 0. */
	if (ptrace(PTRACE_SEIZE, pid, 0, 0) != 0)
		fprintf(stderr, "ptrace seize: %s\n", strerror(errno));
	child_pid = pid;

	struct seccomp_notif_sizes sz;
	seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &sz);
	struct seccomp_notif *req = calloc(1, sz.seccomp_notif);
	struct seccomp_notif_resp *resp = calloc(1, sz.seccomp_notif_resp);

	int			verdict = 0;
	for (;;)
	{
		memset(req, 0, sz.seccomp_notif);
		if (ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, req) < 0)
		{
			if (errno == EINTR) continue;
			break;				/* the child is gone */
		}
		long		nr = req->data.nr;
		if (getenv("PM_TRACE")) fprintf(stderr, "[nr %ld fd %llu]\n", nr, (unsigned long long) req->data.args[0]);
		__u64      *a = req->data.args;
		memset(resp, 0, sz.seccomp_notif_resp);
		resp->id = req->id;
		resp->flags = 0;
		resp->val = 0;
		resp->error = 0;

		switch (nr)
		{
			case SYS_socket:        resp->val = next_fd++; break;
			case SYS_setsockopt:
			case SYS_bind:
			case SYS_listen:
			case SYS_getsockname:   resp->val = 0; break;
			case SYS_accept:
			case SYS_accept4:       conn_fd = next_fd++; resp->val = conn_fd; break;

			case SYS_read:
			case SYS_recvfrom:
				if ((int) a[0] == conn_fd)
				{
					int			want = (int) a[2], got = 0;
					unsigned char tmp[16];
					while (got < want && c2s_off < c2s_len && got < (int) sizeof tmp)
						tmp[got++] = c2s[c2s_off++];
					if (got == 0 && !finished)
					{
						finished = 1;
						verdict = finish_and_verdict();
						/* let the blocking read return 0 (EOF): the server
						 * loop then exits and the run ends */
						resp->val = 0;
					}
					else
					{
						child_write(a[1], tmp, (size_t) got);
						resp->val = got;
					}
				}
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;

			case SYS_write:
			case SYS_sendto:
				if ((int) a[0] == conn_fd)
				{
					int			len = (int) a[2];
					unsigned char tmp[16];
					int			k = len < (int) sizeof tmp ? len : (int) sizeof tmp;
					child_read(a[1], tmp, (size_t) k);
					for (int i = 0; i + 1 < k; i += 2)
						if (tmp[i] == 'A')
						{
							acked = 1; acked_val = tmp[i + 1];
							/* The client has now observed the ACK.  If a crash
							 * is budgeted and the write is still unflushed, the
							 * crash may land in exactly this window; model it. */
							if (crash_budget && have_staged && !finished)
							{
								crashed = 1; have_staged = 0;
								finished = 1;
								resp->val = len;
								if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp) < 0 && errno != ENOENT) {}
								verdict = finish_and_verdict();
								goto done;
							}
						}
					resp->val = len;
				}
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;  /* stdout etc. */
				break;

			case SYS_open:
			case SYS_openat:
			{
				/* Only hijack the server opening ITS store file; let the
				 * loader open libraries for real.  seccomp cannot match the
				 * path, so read it from the child and compare here. */
				uint64_t	pathp = (nr == SYS_openat) ? a[1] : a[0];
				char		path[256] = {0};
				child_read(pathp, path, sizeof path - 1);
				const char *base = strrchr(path, '/');
				base = base ? base + 1 : path;
				if (!strcmp(base, store_path))
				{
					srv_store_fd = next_fd++;
					resp->val = srv_store_fd;
				}
				else
					resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;
			}

			case SYS_pwrite64:
				if ((int) a[0] == srv_store_fd)
				{
					unsigned char b;
					child_read(a[1], &b, 1);
					staged = b; have_staged = 1;
					resp->val = (int) a[2];
				}
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;

			case SYS_fsync:
			case SYS_fdatasync:
				if ((int) a[0] == srv_store_fd) { do_flush(); resp->val = 0; }
				else resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
				break;

			case SYS_close:         resp->val = 0; break;
			default:                resp->flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE; break;
		}
		if (ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, resp) < 0 && errno != ENOENT)
			; /* ENOENT: the target syscall was interrupted/gone; ignore */
	}

done:
	kill(pid, SIGKILL);
	int			st = 0;
	waitpid(pid, &st, 0);
	printf("checker: %s%s -> %s\n", server, crash_budget ? " --crash" : "",
		   verdict ? "NoLostAck VIOLATED" : "ok");
	return verdict;
}
