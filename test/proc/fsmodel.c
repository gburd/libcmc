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

int
main(void)
{
	prefix();
	dirsync();
	fsyncgate();
	failed_write();
	unlink_open();
	errors();
	if (fails) return 1;
	printf("fsmodel: all checks passed\n");
	return 0;
}
