/*
 * test/proc/fsfail.c: the failures of a modelled file system, one mode
 * per run (argv[2]; argv[1] is the node id).  The node takes WRITE(v),
 * stores v in /data/v, and acks only once it believes v is durable.
 *
 *   rewrite  on a failed write or fsync, write v again and fsync again,
 *            until both succeed: correct under EIO
 *   retry    on a failed fsync, fsync again without rewriting: wrong,
 *            since a failed fsync forgets the unwritten data (an acked
 *            value is lost at a crash)
 *   big      write 2000 bytes, past the model's bytes budget (exit 3)
 *   odirect, mmap, link, xattr, cross, noreplace, rmdir, dirmv,
 *   dotdot, longname, dirrel, pollfile
 *            a call the model refuses (exit 3)
 *   ops, ways
 *            a limit of the file model: more unsynced writes to one file
 *            than it holds, and more ways a crash could leave the files
 *            than it enumerates (exit 3)
 *   calls DIR
 *            makes each file call the model answers on files under
 *            DIR, by its own system call number, and records each
 *            answer (an errno, or a count or size) in one record; the
 *            gate runs it natively in an empty DIR too and requires
 *            the same record (Linux's answers)
 *
 * Canonical state on fd 3: [1][value], value 0 while nothing is stored.
 *
 *   cc -O1 -no-pie -o fsfail test/proc/fsfail.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif

static void
emit(unsigned char v)
{
	unsigned char rec[2] = {1, v};
	(void) !write(3, rec, 2);
}

/* the stored value, or 0 */
static unsigned char
recover(void)
{
	unsigned char v = 0;
	int fd = open("/data/v", O_RDONLY);
	if (fd < 0) return 0;
	if (read(fd, &v, 1) != 1) v = 0;
	close(fd);
	return v;
}

/* store v and make it durable; 1 when the node may ack */
static int
store(const char *mode, unsigned char v)
{
	int fd = open("//data/./v", O_CREAT | O_RDWR, 0644);	/* the same file as /data/v */
	int dir = open("/data", O_RDONLY | O_DIRECTORY);
	if (fd < 0 || dir < 0 || fsync(dir) != 0) return 0;
	if (!strcmp(mode, "retry"))
	{
		if (pwrite(fd, &v, 1, 0) != 1) return 0;
		if (fsync(fd) != 0 && fsync(fd) != 0) return 0;
		return 1;
	}
	while (pwrite(fd, &v, 1, 0) != 1 || fsync(fd) != 0)
		;
	return 1;
}

/* a call the model refuses, or a write past the bytes budget */
static void
refused(const char *mode)
{
	static unsigned char big[2000];
	int fd = open("/data/f", O_CREAT | O_RDWR, 0644);

	if (!strcmp(mode, "big")) (void) !write(fd, big, sizeof big);
	if (!strcmp(mode, "odirect")) (void) !open("/data/g", O_CREAT | O_RDWR | O_DIRECT, 0644);
	if (!strcmp(mode, "mmap")) (void) !mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
	if (!strcmp(mode, "link")) (void) !link("/data/f", "/data/g");
	if (!strcmp(mode, "xattr")) (void) !fgetxattr(fd, "user.x", NULL, 0);
	if (!strcmp(mode, "cross")) (void) !rename("/data/f", "/tmp/f");
	if (!strcmp(mode, "noreplace")) (void) !syscall(SYS_renameat2, AT_FDCWD, "/data/f", AT_FDCWD, "/data/g", 1);
	if (!strcmp(mode, "rmdir")) (void) !syscall(SYS_unlinkat, AT_FDCWD, "/data/f", AT_REMOVEDIR);
	if (!strcmp(mode, "dotdot")) (void) !open("/data/../data/f", O_RDONLY);
	if (!strcmp(mode, "longname")) (void) !open("/data/abcdefghijklmnopqrstuvwxyz0123456789", O_RDONLY);
	if (!strcmp(mode, "dirrel"))
	{
		int d = open("/data", O_RDONLY | O_DIRECTORY);
		(void) !openat(d, "f", O_RDONLY);
	}
	if (!strcmp(mode, "pollfile")) { struct pollfd p = {fd, POLLIN, 0}; (void) !poll(&p, 1, 0); }
	if (!strcmp(mode, "dirmv")) { (void) !mkdir("/data/d", 0755); (void) !rename("/data/d", "/data/e"); }
	if (!strcmp(mode, "ops")) for (int i = 0; i < 20; i++) (void) !pwrite(fd, "x", 1, i);
	if (!strcmp(mode, "ways"))
	{
		/* four files of four unsynced writes each: 5^4 ways to crash */
		for (int k = 0; k < 4; k++)
		{
			char p[16];
			snprintf(p, sizeof p, "/data/w%d", k);
			int w = open(p, O_CREAT | O_RDWR, 0644);
			for (int i = 0; i < 4; i++) (void) !pwrite(w, "x", 1, i);
		}
	}
}

/* the answer of one call as a byte: 100 + errno on failure, else r
 * (a count or size, at most 99) */
static unsigned char
ans(long r)
{
	return (unsigned char) (r < 0 ? 100 + errno : r > 99 ? 99 : r);
}

/* The calls mode: each file call the model answers, made on files under
 * dir by its system call number so that libc picks no other, with the
 * answers in one record.  Every path is absolute except two made
 * relative to the working directory, /, which mfs_arg() joins. */
static void
calls(const char *dir)
{
	char f[64], g[64], s[64], rel[64];
	static unsigned char big[8192];
	unsigned char v[64], b[8] = {0};
	struct stat st;
	struct statx sx;
	int n = 0, fd, d;

	if (chdir("/") != 0) return;
	snprintf(f, sizeof f, "%s/f", dir); snprintf(g, sizeof g, "%s/g", dir);
	snprintf(s, sizeof s, "%s/s", dir); snprintf(rel, sizeof rel, "%s/f", dir + 1);
	fd = (int) syscall(SYS_open, f, O_CREAT | O_EXCL | O_RDWR | O_APPEND | O_CLOEXEC, 0644);
	v[n++] = ans(fd < 0 ? -1 : 0);
	v[n++] = ans(syscall(SYS_open, f, O_CREAT | O_EXCL | O_RDWR, 0644));
	v[n++] = ans(syscall(SYS_write, fd, "abc", 3));
	v[n++] = ans(syscall(SYS_pwrite64, fd, "z", 1, 0L));	/* O_APPEND: at the end */
	v[n++] = ans(syscall(SYS_pread64, fd, b, 8, 0L));
	v[n++] = b[3];
	v[n++] = ans(syscall(SYS_pread64, fd, b, 8, (long) -1));
	v[n++] = ans(syscall(SYS_pread64, fd, big, sizeof big, 0L));
	v[n++] = ans(syscall(SYS_lseek, fd, 1, SEEK_SET));
	v[n++] = ans(syscall(SYS_read, fd, b, 2));
	v[n++] = ans(syscall(SYS_ftruncate, fd, 2));
	v[n++] = ans(syscall(SYS_fdatasync, fd));
	v[n++] = ans(syscall(SYS_fcntl, fd, F_GETFD));
	v[n++] = ans(syscall(SYS_fcntl, fd, F_SETFD, 0));
	v[n++] = ans(syscall(SYS_fcntl, fd, F_GETFD));
	v[n++] = ans(syscall(SYS_fstat, fd, &st));
	v[n++] = (unsigned char) st.st_size;
	v[n++] = ans(syscall(SYS_newfstatat, fd, "", &st, AT_EMPTY_PATH));
	v[n++] = ans(syscall(SYS_statx, fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx));
	v[n++] = (unsigned char) sx.stx_size;
	v[n++] = ans(syscall(SYS_stat, dir, &st));
	v[n++] = S_ISDIR(st.st_mode);
	v[n++] = ans(syscall(SYS_lstat, g, &st));
	v[n++] = ans(syscall(SYS_newfstatat, AT_FDCWD, rel, &st, 0));
	v[n++] = ans(syscall(SYS_statx, AT_FDCWD, f, 0, STATX_BASIC_STATS, &sx));
	v[n++] = ans(syscall(SYS_access, f, R_OK));
	v[n++] = ans(syscall(SYS_faccessat, AT_FDCWD, rel, R_OK));
	v[n++] = ans(syscall(SYS_faccessat2, AT_FDCWD, f, R_OK, 0));
	v[n++] = ans(syscall(SYS_readlink, f, (char *) b, sizeof b));
	v[n++] = ans(syscall(SYS_mkdir, s, 0755));
	v[n++] = ans(syscall(SYS_mkdirat, AT_FDCWD, s, 0755));
	v[n++] = ans(syscall(SYS_unlinkat, AT_FDCWD, s, 0x8000));
	v[n++] = ans(syscall(SYS_renameat, AT_FDCWD, f, AT_FDCWD, g));
	v[n++] = ans(syscall(SYS_renameat2, AT_FDCWD, g, AT_FDCWD, f, 0));
	v[n++] = ans(syscall(SYS_unlink, g));
	d = (int) syscall(SYS_openat, AT_FDCWD, dir, O_RDONLY | O_DIRECTORY);
	v[n++] = ans(syscall(SYS_fsync, d));
	v[n++] = ans(syscall(SYS_read, d, b, 1));
	v[n++] = ans(syscall(SYS_close, d));
	v[n++] = ans(syscall(SYS_unlinkat, AT_FDCWD, f, 0));
	v[n++] = ans(syscall(SYS_close, fd));
	/* the errors: a bad address, a closed fd, the wrong access mode */
	fd = (int) syscall(SYS_creat, g, 0644);
	v[n++] = ans(syscall(SYS_write, fd, (char *) 16, 1));
	v[n++] = ans(syscall(SYS_fstat, fd, (char *) 16));
	v[n++] = ans(syscall(SYS_statx, AT_FDCWD, g, 0, STATX_BASIC_STATS, (char *) 16));
	v[n++] = ans(syscall(SYS_read, fd, b, 1));
	v[n++] = ans(syscall(SYS_write, fd, "q", 1));
	v[n++] = ans(syscall(SYS_read, (int) syscall(SYS_open, g, O_RDONLY), (char *) 16, 1));
	v[n++] = ans(syscall(SYS_unlink, s));
	v[n++] = ans(syscall(SYS_access, f, F_OK));
	v[n++] = ans(syscall(SYS_open, s, O_WRONLY));
	v[n++] = ans(syscall(SYS_mkdir, f, 0755));
	v[n++] = ans(syscall(SYS_rename, g, s));
	/* paths relative to a kernel dirfd: the root, and an fd not open;
	 * and faccessat2 with AT_EMPTY_PATH on a modelled fd */
	d = (int) syscall(SYS_open, "/", O_RDONLY | O_DIRECTORY);
	v[n++] = ans(syscall(SYS_faccessat, d, rel, F_OK));
	v[n++] = ans(syscall(SYS_faccessat, 450, rel, F_OK));
	v[n++] = ans(syscall(SYS_close, d));
	v[n++] = ans(syscall(SYS_faccessat2, fd, "", F_OK, AT_EMPTY_PATH));
	{
		struct epoll_event ev = {EPOLLIN, {0}};
		v[n++] = ans(syscall(SYS_epoll_ctl, (int) syscall(SYS_epoll_create1, 0), EPOLL_CTL_ADD, fd, &ev));
	}
	unsigned char rec[1 + sizeof v];
	rec[0] = (unsigned char) n;
	memcpy(rec + 1, v, (size_t) n);
	(void) !write(3, rec, (size_t) n + 1);
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "rewrite";
	struct sockaddr_in a = {0};
	int ls = socket(AF_INET, SOCK_STREAM, 0), c;
	unsigned char m[2];

	if (!strcmp(mode, "calls")) { calls(argc > 3 ? argv[3] : "/data"); return 0; }
	if (strcmp(mode, "rewrite") && strcmp(mode, "retry")) refused(mode);
	emit(recover());
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20000);
	bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 1);
	c = accept(ls, NULL, NULL);
	if (c >= 0 && read(c, m, 2) == 2 && m[0] == 'W' && store(mode, m[1]))
	{
		unsigned char ack[2] = {'A', m[1]};
		emit(m[1]);
		(void) !write(c, ack, 2);
	}
	for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	return 0;
}
