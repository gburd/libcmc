/*
 * test/proc/fsmodel.c: the modelled file system (src/cmc_fs.c) on its
 * own, as plain data.  Each check is a crash-consistency rule a program
 * relies on; the gate runs this before the process rows that use it.
 *
 *   cc -std=c11 -Isrc -o fsmodel test/proc/fsmodel.c src/cmc_fs.c
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "cmc_fs.h"

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the first byte of file rel, -1 if it has none, -2 if it is missing */
static int
byte0(Fs *fs, const char *rel)
{
	unsigned char b;
	int fd = fs_open(fs, rel, FS_O_READ, 900);
	if (fd < 0) return -2;
	long n = fs_read(fs, fd, &b, 1, 0);
	fs_close(fs, fd);
	return n == 1 ? b : -1;
}

/* a fresh file system with file f created and its directory synced */
static void
setup(Fs *fs)
{
	fs_init(fs);
	fs_close(fs, fs_open(fs, "f", FS_O_READ | FS_O_WRITE | FS_O_CREAT, 500));
	int d = fs_open(fs, "", FS_O_READ | FS_O_DIRECTORY, 501);
	fs_fsync(fs, d, NULL);
	fs_close(fs, d);
}

/* writes since an fsync survive a crash as a prefix, in order */
static void
prefix(void)
{
	Fs fs;
	setup(&fs);
	int fd = fs_open(&fs, "f", FS_O_WRITE, 502);
	unsigned char a = 'a', b = 'b';
	fs_write(&fs, fd, &a, 1, 0, NULL);
	fs_fsync(&fs, fd, NULL);
	fs_write(&fs, fd, &b, 1, 0, NULL);
	CHECK(fs_crash_choices(&fs) == 2);
	Fs c0 = fs, c1 = fs;
	fs_crash(&c0, 0); fs_crash(&c1, 1);
	CHECK(byte0(&c0, "f") == 'a');
	CHECK(byte0(&c1, "f") == 'b');
	CHECK(c0.nfd == 0);
}

/* a create or rename is durable only once its directory is synced */
static void
dirsync(void)
{
	Fs fs;
	fs_init(&fs);
	int fd = fs_open(&fs, "t", FS_O_WRITE | FS_O_CREAT, 500);
	unsigned char v = 7;
	fs_write(&fs, fd, &v, 1, 0, NULL);
	fs_fsync(&fs, fd, NULL);
	fs_close(&fs, fd);
	CHECK(fs_rename(&fs, "t", "c") == 0);
	CHECK(byte0(&fs, "c") == 7 && byte0(&fs, "t") == -2);
	Fs lost = fs;
	fs_crash(&lost, 0);
	CHECK(byte0(&lost, "c") == -2 && byte0(&lost, "t") == -2);
	int d = fs_open(&fs, "", FS_O_READ | FS_O_DIRECTORY, 501);
	fs_fsync(&fs, d, NULL);
	fs_crash(&fs, 0);
	CHECK(byte0(&fs, "c") == 7 && byte0(&fs, "t") == -2);
}

/* a failed fsync forgets the unsynced data: a later fsync saves nothing
 * and a crash loses it, while a read still sees it (fsyncgate) */
static void
fsyncgate(void)
{
	Fs fs;
	FsUndo u;
	setup(&fs);
	int fd = fs_open(&fs, "f", FS_O_READ | FS_O_WRITE, 502);
	unsigned char v = 9;
	fs_write(&fs, fd, &v, 1, 0, NULL);
	fs_fsync(&fs, fd, &u);
	fs_undo(&fs, &u, 1);
	CHECK(byte0(&fs, "f") == 9);
	CHECK(fs_crash_choices(&fs) == 1);
	fs_fsync(&fs, fd, NULL);
	fs_crash(&fs, 0);
	CHECK(byte0(&fs, "f") == -1);
}

/* a failed write is undone, offset included */
static void
failed_write(void)
{
	Fs fs;
	FsUndo u;
	setup(&fs);
	int fd = fs_open(&fs, "f", FS_O_READ | FS_O_WRITE, 502);
	unsigned char v = 3;
	CHECK(fs_write(&fs, fd, &v, 1, -1, &u) == 1);
	fs_undo(&fs, &u, 0);
	CHECK(fs_lseek(&fs, fd, 0, 1) == 0);
	CHECK(byte0(&fs, "f") == -1 && fs_crash_choices(&fs) == 1);
}

/* an unlinked file stays readable through an open fd, then is freed */
static void
unlink_open(void)
{
	Fs fs;
	setup(&fs);
	int fd = fs_open(&fs, "f", FS_O_READ | FS_O_WRITE, 502);
	unsigned char v = 4, b = 0;
	fs_write(&fs, fd, &v, 1, 0, NULL);
	CHECK(fs_unlink(&fs, "f") == 0);
	CHECK(byte0(&fs, "f") == -2);
	CHECK(fs_read(&fs, fd, &b, 1, 0) == 1 && b == 4);
	fs_close(&fs, fd);
	/* still named on disk until the directory is synced */
	CHECK(fs.ino[1].used);
	int d = fs_open(&fs, "", FS_O_READ | FS_O_DIRECTORY, 503);
	fs_fsync(&fs, d, NULL);
	fs_close(&fs, d);
	CHECK(!fs.ino[1].used);
}

/* errors as Linux gives them */
static void
errors(void)
{
	Fs fs;
	setup(&fs);
	CHECK(fs_open(&fs, "f", FS_O_WRITE | FS_O_CREAT | FS_O_EXCL, 502) == -EEXIST);
	CHECK(fs_open(&fs, "g", FS_O_READ, 502) == -ENOENT);
	CHECK(fs_open(&fs, "f/x", FS_O_READ, 502) == -ENOTDIR);
	CHECK(fs_open(&fs, "", FS_O_WRITE, 502) == -EISDIR);
	CHECK(fs_mkdir(&fs, "f") == -EEXIST);
	CHECK(fs_unlink(&fs, "") == -EISDIR);
	int fd = fs_open(&fs, "f", FS_O_READ, 502);
	unsigned char v = 1;
	CHECK(fs_write(&fs, fd, &v, 1, 0, NULL) == -EBADF);
	CHECK(fs_ftruncate(&fs, fd, 0, NULL) == -EINVAL);
	CHECK(fs_lseek(&fs, fd, -1, 0) == -EINVAL);
	CHECK(fs_close(&fs, 777) == -EBADF);
}

/* paths through subdirectories, and the errors a lookup gives */
static void
paths(void)
{
	Fs fs;
	long size;
	int isdir;
	char big[FS_NAME + 1];

	setup(&fs);
	CHECK(fs_mkdir(&fs, "d") == 0);
	CHECK(fs_mkdir(&fs, "d") == -EEXIST);
	CHECK(fs_mkdir(&fs, "") == -EEXIST);
	CHECK(fs_mkdir(&fs, "x/y") == -ENOENT);
	CHECK(fs_mkdir(&fs, "f/y") == -ENOTDIR);
	CHECK(fs_mkdir(&fs, "d/e") == 0);
	int fd = fs_open(&fs, "d/e/g", FS_O_WRITE | FS_O_CREAT, 502);
	CHECK(fd == 502 && fs_isfd(&fs, 502) && !fs_isdirfd(&fs, 502));
	CHECK(!fs_isfd(&fs, 503) && !fs_isdirfd(&fs, 503));
	CHECK(fs_open(&fs, "x/g", FS_O_WRITE | FS_O_CREAT, 503) == -ENOENT);
	CHECK(fs_open(&fs, "d", FS_O_READ | FS_O_DIRECTORY | FS_O_CREAT, 503) == 503);
	CHECK(fs_isdirfd(&fs, 503));
	CHECK(fs_open(&fs, "d/h", FS_O_READ | FS_O_DIRECTORY | FS_O_CREAT, 504) == -ENOENT);
	CHECK(fs_open(&fs, "f", FS_O_READ | FS_O_DIRECTORY, 504) == -ENOTDIR);
	memset(big, 'n', FS_NAME); big[FS_NAME] = 0;
	CHECK(fs_open(&fs, big, FS_O_READ, 504) == -ENAMETOOLONG);
	CHECK(fs_stat(&fs, 0, "d/e/g", &size, &isdir) > 0 && size == 0 && !isdir);
	CHECK(fs_stat(&fs, 503, NULL, &size, &isdir) > 0 && size == 4096 && isdir);
	CHECK(fs_stat(&fs, 0, "q", &size, &isdir) == -ENOENT);
	CHECK(fs_stat(&fs, 777, NULL, &size, &isdir) == -EBADF);
	CHECK(fs_unlink(&fs, "q") == -ENOENT);
	CHECK(fs_unlink(&fs, "d") == -EISDIR);
	CHECK(fs_unlink(&fs, "d/e/g") == 0);
	CHECK(fs_isfd(&fs, 502) && fs_close(&fs, 502) == 0);
}

/* every limit of the model is FS_LIMIT, never a wrong answer */
static void
limits(void)
{
	Fs fs;
	char name[8];
	unsigned char v = 1;

	fs_init(&fs);
	for (int k = 0; k < FS_MAXINO - 1; k++)
	{
		snprintf(name, sizeof name, "f%d", k);
		CHECK(fs_open(&fs, name, FS_O_WRITE | FS_O_CREAT, 500 + k) == 500 + k);
	}
	/* every inode is taken; then the root's operation log fills */
	CHECK(fs_mkdir(&fs, "m") == FS_LIMIT);
	CHECK(fs_open(&fs, "g", FS_O_WRITE | FS_O_CREAT, 600) == FS_LIMIT);
	CHECK(fs_rename(&fs, "f0", "r0") == 0);
	CHECK(fs_rename(&fs, "r0", "f0") == FS_LIMIT);
	/* every fd slot is taken */
	CHECK(fs_open(&fs, "", FS_O_READ | FS_O_DIRECTORY, 499) == 499);
	CHECK(fs_open(&fs, "f1", FS_O_READ, 600) == FS_LIMIT);
	for (int k = 0; k < FS_MAXOP; k++) CHECK(fs_write(&fs, 500, &v, 1, k, NULL) == 1);
	CHECK(fs_write(&fs, 500, &v, 1, 0, NULL) == FS_LIMIT);
	CHECK(fs_ftruncate(&fs, 500, 0, NULL) == FS_LIMIT);
	CHECK(fs_open(&fs, "r0", FS_O_WRITE | FS_O_TRUNC, 600) == FS_LIMIT);
	CHECK(fs_crash_choices(&fs) == FS_LIMIT);
	CHECK(fs_ftruncate(&fs, 501, FS_FILEMAX + 1, NULL) == FS_LIMIT);
	CHECK(fs_write(&fs, 501, &v, 1, FS_FILEMAX, NULL) == FS_LIMIT);
}

/* rename over a file replaces it, on disk only once the directory is
 * synced; rename of a directory or across directories is not modelled */
static void
renames(void)
{
	Fs fs;

	setup(&fs);
	fs_close(&fs, fs_open(&fs, "g", FS_O_WRITE | FS_O_CREAT, 502));
	CHECK(fs_mkdir(&fs, "d") == 0);
	CHECK(fs_rename(&fs, "q", "g") == -ENOENT);
	CHECK(fs_rename(&fs, "f", "q/r") == -ENOENT);
	CHECK(fs_rename(&fs, "", "g") == -EBUSY);
	CHECK(fs_rename(&fs, "f", "") == -EBUSY);
	CHECK(fs_rename(&fs, "f", "d/f") == FS_NOTMOD);
	CHECK(fs_rename(&fs, "d", "e") == FS_NOTMOD);
	CHECK(fs_rename(&fs, "f", "f") == 0);
	CHECK(fs_rename(&fs, "f", "d") == -EISDIR);
	Fs before = fs;
	CHECK(fs_rename(&fs, "f", "g") == 0);
	CHECK(byte0(&fs, "f") == -2 && byte0(&fs, "g") == -1);
	/* the crash keeps 0..3 of the root's links of g and d and the rename */
	CHECK(fs_crash_choices(&fs) == 4);
	Fs c = fs;
	fs_crash(&c, 3);
	CHECK(byte0(&c, "f") == -2 && byte0(&c, "g") == -1);
	c = fs;
	fs_crash(&c, 0);
	CHECK(byte0(&c, "f") == -1 && byte0(&c, "g") == -2);
	c = before;
	CHECK(fs_unlink(&c, "g") == 0 && fs_rename(&c, "f", "g") == 0);
	fs_crash(&c, 0);
	CHECK(byte0(&c, "f") == -1);
}

/* reads, writes and seeks at and past the end */
static void
rw(void)
{
	Fs fs;
	unsigned char b[4] = {1, 2, 3, 4}, r[4];

	setup(&fs);
	int fd = fs_open(&fs, "f", FS_O_READ | FS_O_WRITE | FS_O_APPEND, 502);
	CHECK(fs_write(&fs, fd, b, 2, -1, NULL) == 2);
	CHECK(fs_write(&fs, fd, b, 2, 0, NULL) == 2);
	CHECK(fs_lseek(&fs, fd, 0, 2) == 4 && fs_lseek(&fs, fd, 0, 7) == -EINVAL);
	CHECK(fs_lseek(&fs, 777, 0, 0) == -EBADF);
	CHECK(fs_lseek(&fs, fd, -2, 1) == 2);
	CHECK(fs_read(&fs, fd, r, 4, -1) == 2 && r[0] == 1 && fs_lseek(&fs, fd, 0, 1) == 4);
	CHECK(fs_read(&fs, fd, r, 4, 9) == 0);
	CHECK(fs_read(&fs, 777, r, 1, 0) == -EBADF);
	CHECK(fs_write(&fs, 777, r, 1, 0, NULL) == -EBADF);
	CHECK(fs_ftruncate(&fs, 777, 0, NULL) == -EBADF);
	CHECK(fs_ftruncate(&fs, fd, -1, NULL) == -EINVAL);
	CHECK(fs_fsync(&fs, 777, NULL) == -EBADF);
	fs_fsync(&fs, fd, NULL);
	fs_close(&fs, fd);
	/* a hole reads as zeros, now and after a crash; growth by truncate too */
	fd = fs_open(&fs, "f", FS_O_READ | FS_O_WRITE | FS_O_TRUNC, 502);
	CHECK(fs_write(&fs, fd, b, 1, 3, NULL) == 1);
	CHECK(fs_ftruncate(&fs, fd, 6, NULL) == 0);
	CHECK(fs_read(&fs, fd, r, 4, 0) == 4 && r[0] == 0 && r[3] == 1);
	CHECK(fs_bytes(&fs) == 6 + 4 + 1);
	Fs c = fs;
	fs_crash(&c, 3);
	CHECK(c.ino[1].dlen == 6 && c.ino[1].dur[2] == 0 && c.ino[1].dur[3] == 1);
	c = fs;
	fs_crash(&c, 2);
	CHECK(c.ino[1].dlen == 4 && c.ino[1].dur[0] == 0);
	int d = fs_open(&fs, "", FS_O_READ | FS_O_DIRECTORY, 503);
	CHECK(fs_read(&fs, d, r, 1, 0) == -EISDIR);
	CHECK(fs_ftruncate(&fs, d, 0, NULL) == -EINVAL);
	CHECK(fs_open(&fs, "f", FS_O_WRITE | FS_O_TRUNC, 504) == 504);
	CHECK(fs_open(&fs, "f", FS_O_READ | FS_O_TRUNC, 505) == 505);
	CHECK(fs_read(&fs, 504, r, 1, 0) == -EBADF);
	CHECK(fs_open(&fs, "f", FS_O_WRITE | FS_O_TRUNC, 506) == 506);
	CHECK(fs_rename(&fs, "f", "f/x") == -ENOTDIR);
	/* pending writes past FS_FILEMAX in all, though the file stays small */
	static unsigned char z[FS_FILEMAX];
	fs_fsync(&fs, 504, NULL);
	CHECK(fs_write(&fs, 504, z, FS_FILEMAX, 0, NULL) == FS_FILEMAX);
	CHECK(fs_write(&fs, 504, z, 1, 0, NULL) == FS_LIMIT);
}

/* inodes stay while anything names them: a pending link behind a
 * rename, or an open fd once the unlink is synced */
static void
lifetimes(void)
{
	Fs fs;

	setup(&fs);
	CHECK(fs_rename(&fs, "f", "h") == 0);
	fs_close(&fs, fs_open(&fs, "g", FS_O_WRITE | FS_O_CREAT, 502));
	CHECK(fs_unlink(&fs, "g") == 0);
	CHECK(fs.ino[2].used);
	int fd = fs_open(&fs, "h", FS_O_READ, 502);
	CHECK(fs_unlink(&fs, "h") == 0);
	int d = fs_open(&fs, "", FS_O_READ | FS_O_DIRECTORY, 503);
	fs_fsync(&fs, d, NULL);
	CHECK(fs_close(&fs, d) == 0 && fs.ino[1].used && !fs.ino[2].used);
	CHECK(fs_close(&fs, fd) == 0 && !fs.ino[1].used);
}

/* a failed fsync of a directory forgets its pending link; the unlink
 * after it then names nothing on disk, and the disk stays empty */
static void
dirgate(void)
{
	Fs fs;
	FsUndo u;

	fs_init(&fs);
	int d = fs_open(&fs, "", FS_O_READ | FS_O_DIRECTORY, 501);
	fs_close(&fs, fs_open(&fs, "g", FS_O_WRITE | FS_O_CREAT, 502));
	fs_fsync(&fs, d, &u);
	fs_undo(&fs, &u, 1);
	CHECK(fs_unlink(&fs, "g") == 0);
	fs_fsync(&fs, d, NULL);
	CHECK(fs.ino[0].ndent == 0 && fs.ino[0].nent == 0);
}

/* a full operation log refuses a create even with inodes free */
static void
oplog(void)
{
	Fs fs;

	fs_init(&fs);
	fs_close(&fs, fs_open(&fs, "a", FS_O_WRITE | FS_O_CREAT, 500));
	for (int k = 1; k < FS_MAXOP; k++)
		CHECK(fs_rename(&fs, k % 2 ? "a" : "b", k % 2 ? "b" : "a") == 0);
	CHECK(fs_open(&fs, "c", FS_O_WRITE | FS_O_CREAT, 500) == FS_LIMIT);
}

static void
hash(const uint8_t *p, size_t n, uint64_t fp[2])
{
	fp[0] = n; fp[1] = n ? p[0] : 0;
}

/* the encoding fails cleanly at every short buffer, and tells a pending
 * rename from a synced one */
static void
encode(void)
{
	Fs fs;
	unsigned char out[4096], out2[4096];
	unsigned char v = 5;

	setup(&fs);
	int fd = fs_open(&fs, "f", FS_O_WRITE, 502);
	fs_write(&fs, fd, &v, 1, 0, NULL);
	CHECK(fs_rename(&fs, "f", "g") == 0);
	int n = fs_encode(&fs, hash, out, (int) sizeof out);
	CHECK(n > 0);
	for (int cap = 0; cap < n; cap++) CHECK(fs_encode(&fs, hash, out2, cap) == -1);
	int d = fs_open(&fs, "", FS_O_READ | FS_O_DIRECTORY, 503);
	fs_fsync(&fs, d, NULL);
	int m = fs_encode(&fs, hash, out2, (int) sizeof out2);
	CHECK(m > 0 && (m != n || memcmp(out, out2, (size_t) n)));
}

int
main(void)
{
	prefix();
	dirsync();
	fsyncgate();
	failed_write();
	unlink_open();
	errors();
	paths();
	limits();
	renames();
	rw();
	encode();
	lifetimes();
	dirgate();
	oplog();
	if (fails) return 1;
	printf("fsmodel: all checks passed\n");
	return 0;
}
