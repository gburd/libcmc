/*
 * test/proc/files.c: the file system is read-only to a node (2.1 plan,
 * item 2).  One node; the mode is its first [target] arg, a path the
 * second.
 *
 *   persist PATH  the node keeps its state in a file instead of fd 193:
 *                 it reads PATH (2 if it holds 'x', else 1), writes 'x'
 *                 into it, and waits in accept().  A crash and a restart
 *                 bring it back to read the file.  On 2.0.0 the file
 *                 survived the crash and every later replay, outside the
 *                 world; its open for writing is refused now.
 *   config PATH   reads the first byte of PATH, lseeks back, reads it
 *                 again and fsyncs: a read-only open, and lseek and fsync
 *                 on a kernel fd in a node with one thread, go to the
 *                 kernel.  Records 1 and the byte if both reads agree.
 *   lseek PATH    reads one byte of PATH, then starts a worker that seeks
 *                 the shared fd to 0 and back to 1 while the main thread
 *                 asks where the offset is.  On Linux main can see 0; on
 *                 2.0.0 neither thread's lseek was seen, so main saw 1 in
 *                 every schedule.  Both are refused now.
 *   devnull       opens /dev/null for writing and writes to it: allowed.
 *   badpath       openat() with a path at an unmapped address: refused.
 *   sockerr       calls lseek, fsync, fdatasync, ftruncate and
 *                 sync_file_range on a TCP socket and records each errno
 *                 as a digit (1 ESPIPE, 2 EINVAL, 0 success, 9 other); the
 *                 gate runs it natively too and requires the same digits.
 *   durfsync      fsync and fdatasync of fd 193 succeed under cmc.
 *   NAME PATH     any other mode is a system call that changes the file
 *                 system, made on PATH (which the gate makes sure does not
 *                 exist) through syscall() with its own number; each one
 *                 must end the check.
 *
 * Canonical state fd 3: [phase][values...].
 *
 *   cc -O1 -no-pie -o files test/proc/files.c -lpthread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

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
#ifndef SYS_file_setattr
#define SYS_file_setattr 469
#endif

static int	fd;
static volatile long seen = -1;

static void
emit(int n, const unsigned char *v)
{
	unsigned char rec[16] = {(unsigned char) n};

	memcpy(rec + 1, v, (size_t) n);
	(void) !write(3, rec, (size_t) n + 1);
}

static void
emit1(int a, int b)
{
	unsigned char v[2] = {(unsigned char) a, (unsigned char) b};

	emit(2, v);
}

static void *
seeker(void *arg)
{
	(void) arg;
	(void) lseek(fd, 0, SEEK_SET);
	(void) lseek(fd, 1, SEEK_SET);
	return NULL;
}

static int
code(long r)
{
	return r == 0 ? 0 : errno == ESPIPE ? 1 : errno == EINVAL ? 2 : 9;
}

/* a system call that changes the file system, by name; -1 if none */
static long
change(const char *m, const char *p)
{
	struct timespec ts[2] = {{0, 0}, {0, 0}};
	struct { unsigned long long flags, mode, resolve; } how = {O_WRONLY, 0, 0};

	if (!strcmp(m, "open_wronly")) return syscall(SYS_open, p, O_WRONLY);
	if (!strcmp(m, "openat_rdwr")) return syscall(SYS_openat, AT_FDCWD, p, O_RDWR);
	if (!strcmp(m, "openat_creat")) return syscall(SYS_openat, AT_FDCWD, p, O_RDONLY | O_CREAT, 0644);
	if (!strcmp(m, "openat_trunc")) return syscall(SYS_openat, AT_FDCWD, p, O_RDONLY | O_TRUNC);
	if (!strcmp(m, "openat_append")) return syscall(SYS_openat, AT_FDCWD, p, O_RDONLY | O_APPEND);
	if (!strcmp(m, "openat_tmpfile")) return syscall(SYS_openat, AT_FDCWD, p, O_TMPFILE | O_RDWR, 0644);
	if (!strcmp(m, "openat2")) return syscall(SYS_openat2, AT_FDCWD, p, &how, sizeof how);
	if (!strcmp(m, "creat")) return syscall(SYS_creat, p, 0644);
	if (!strcmp(m, "unlink")) return syscall(SYS_unlink, p);
	if (!strcmp(m, "unlinkat")) return syscall(SYS_unlinkat, AT_FDCWD, p, 0);
	if (!strcmp(m, "rename")) return syscall(SYS_rename, p, p);
	if (!strcmp(m, "renameat")) return syscall(SYS_renameat, AT_FDCWD, p, AT_FDCWD, p);
	if (!strcmp(m, "renameat2")) return syscall(SYS_renameat2, AT_FDCWD, p, AT_FDCWD, p, 0);
	if (!strcmp(m, "mkdir")) return syscall(SYS_mkdir, p, 0755);
	if (!strcmp(m, "mkdirat")) return syscall(SYS_mkdirat, AT_FDCWD, p, 0755);
	if (!strcmp(m, "rmdir")) return syscall(SYS_rmdir, p);
	if (!strcmp(m, "link")) return syscall(SYS_link, p, p);
	if (!strcmp(m, "linkat")) return syscall(SYS_linkat, AT_FDCWD, p, AT_FDCWD, p, 0);
	if (!strcmp(m, "symlink")) return syscall(SYS_symlink, p, p);
	if (!strcmp(m, "symlinkat")) return syscall(SYS_symlinkat, p, AT_FDCWD, p);
	if (!strcmp(m, "truncate")) return syscall(SYS_truncate, p, 0);
	if (!strcmp(m, "mknod")) return syscall(SYS_mknod, p, S_IFIFO | 0644, 0);
	if (!strcmp(m, "mknodat")) return syscall(SYS_mknodat, AT_FDCWD, p, S_IFIFO | 0644, 0);
	if (!strcmp(m, "chmod")) return syscall(SYS_chmod, p, 0644);
	if (!strcmp(m, "fchmodat")) return syscall(SYS_fchmodat, AT_FDCWD, p, 0644);
	if (!strcmp(m, "fchmodat2")) return syscall(SYS_fchmodat2, AT_FDCWD, p, 0644, 0);
	if (!strcmp(m, "chown")) return syscall(SYS_chown, p, -1, -1);
	if (!strcmp(m, "lchown")) return syscall(SYS_lchown, p, -1, -1);
	if (!strcmp(m, "fchownat")) return syscall(SYS_fchownat, AT_FDCWD, p, -1, -1, 0);
	if (!strcmp(m, "utime")) return syscall(SYS_utime, p, NULL);
	if (!strcmp(m, "utimes")) return syscall(SYS_utimes, p, NULL);
	if (!strcmp(m, "futimesat")) return syscall(SYS_futimesat, AT_FDCWD, p, NULL);
	if (!strcmp(m, "utimensat")) return syscall(SYS_utimensat, AT_FDCWD, p, ts, 0);
	if (!strcmp(m, "setxattr")) return syscall(SYS_setxattr, p, "user.x", "1", 1, 0);
	if (!strcmp(m, "lsetxattr")) return syscall(SYS_lsetxattr, p, "user.x", "1", 1, 0);
	if (!strcmp(m, "setxattrat")) return syscall(SYS_setxattrat, AT_FDCWD, p, 0, NULL, 0);
	if (!strcmp(m, "removexattr")) return syscall(SYS_removexattr, p, "user.x");
	if (!strcmp(m, "lremovexattr")) return syscall(SYS_lremovexattr, p, "user.x");
	if (!strcmp(m, "removexattrat")) return syscall(SYS_removexattrat, AT_FDCWD, p, 0, "user.x");
	if (!strcmp(m, "file_setattr")) return syscall(SYS_file_setattr, AT_FDCWD, p, NULL, 0, 0);
	if (!strcmp(m, "io_uring_setup")) return syscall(SYS_io_uring_setup, 1, NULL);
	/* the calls on an fd: on a read-only fd of a file the gate made, or
	 * stdout for the ones that need a valid fd number */
	int			r = open(p, O_RDONLY);

	if (!strcmp(m, "fchmod")) return syscall(SYS_fchmod, r, 0644);
	if (!strcmp(m, "fchown")) return syscall(SYS_fchown, r, -1, -1);
	if (!strcmp(m, "futimens")) return syscall(SYS_utimensat, r, NULL, ts, 0);
	if (!strcmp(m, "fsetxattr")) return syscall(SYS_fsetxattr, r, "user.x", "1", 1, 0);
	if (!strcmp(m, "fremovexattr")) return syscall(SYS_fremovexattr, r, "user.x");
	if (!strcmp(m, "ftruncate")) return syscall(SYS_ftruncate, r, 0);
	if (!strcmp(m, "fallocate")) return syscall(SYS_fallocate, r, 0, 0, 1);
	if (!strcmp(m, "flock")) return syscall(SYS_flock, r, LOCK_SH | LOCK_NB);
	return -2;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";
	const char *path = argc > 3 ? argv[3] : "";
	unsigned char c = 0, d = 0;

	emit1(0, 0);
	if (!strcmp(mode, "persist"))
	{
		int			ls = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(20000)};

		fd = open(path, O_RDONLY);
		emit1(fd >= 0 && read(fd, &c, 1) == 1 && c == 'x' ? 2 : 1, 0);
		if (fd >= 0)
			close(fd);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		(void) !write(fd, "x", 1);
		close(fd);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 8);
		(void) accept(ls, NULL, NULL);
		return 0;
	}
	if (!strcmp(mode, "config"))
	{
		fd = open(path, O_RDONLY);
		int			ok = read(fd, &c, 1) == 1 && lseek(fd, 0, SEEK_SET) == 0 &&
			read(fd, &d, 1) == 1 && c == d && fsync(fd) == 0;

		emit1(ok ? 1 : 9, c);
		return 0;
	}
	if (!strcmp(mode, "lseek"))
	{
		pthread_t	t;

		fd = open(path, O_RDONLY);
		(void) !read(fd, &c, 1);
		pthread_create(&t, NULL, seeker, NULL);
		seen = lseek(fd, 0, SEEK_CUR);
		pthread_join(t, NULL);
		emit1(1, (int) seen);
		return 0;
	}
	if (!strcmp(mode, "devnull"))
	{
		fd = open("/dev/null", O_WRONLY);
		emit1(fd >= 0 && write(fd, "x", 1) == 1 ? 1 : 9, 0);
		return 0;
	}
	if (!strcmp(mode, "badpath"))
	{
		emit1(syscall(SYS_openat, AT_FDCWD, (char *) 16, O_RDONLY) < 0 ? 1 : 9, 0);
		return 0;
	}
	if (!strcmp(mode, "sockerr"))
	{
		unsigned char v[8];
		int			s = socket(AF_INET, SOCK_STREAM, 0);

		v[0] = 1;
		v[1] = (unsigned char) code(lseek(s, 0, SEEK_SET));
		v[2] = (unsigned char) code(lseek(s, 0, 99));
		v[3] = (unsigned char) code(fsync(s));
		v[4] = (unsigned char) code(fdatasync(s));
		v[5] = (unsigned char) code(ftruncate(s, 0));
		v[6] = (unsigned char) code(sync_file_range(s, 0, 0, 0));
		v[7] = (unsigned char) code(sync_file_range(s, 0, 0, 99));
		emit(8, v);
		return 0;
	}
	if (!strcmp(mode, "durfsync"))
	{
		emit1(fsync(193) == 0 && fdatasync(193) == 0 ? 1 : 9, 0);
		return 0;
	}
	if (!strcmp(mode, "xattrprobe"))
	{
		/* run natively by proc_gate: the errno of an xattr write, 0 if it
		 * succeeded, so the gate can tell a sandbox that answers the call
		 * before any filter of the checker's */
		long		r = syscall(SYS_setxattr, path, "user.cmc", "1", 1, 0);

		emit1(1, r == 0 ? 0 : errno);
		return 0;
	}
	if (*mode)
	{
		long		r = change(mode, path);

		emit1(r == -2 ? 8 : 1, 0);
	}
	return 0;
}
