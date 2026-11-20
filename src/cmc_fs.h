/*
 * cmc_fs.h: the modelled file system of one node (internal to cmc).
 *
 * The checker owns the files under the directory a model file names
 * ([files] dir), the way it owns sockets.  This module holds them as
 * plain data, with no system calls: cmc_proc.c reads a node's arguments,
 * calls these functions, and writes the answers back.
 *
 * Each inode (a file or a directory) keeps three things: its contents
 * now (what a read sees), its contents on disk as of its last fsync, and
 * the operations made since, in order.  A file's operations are writes
 * and truncations; a directory's are the links, unlinks and renames of
 * its entries.  fsync applies the operations to the disk copy.  A crash
 * keeps, for each inode, the disk copy plus a prefix of its operations,
 * chosen by the search; so a write or a rename not yet synced may or
 * may not survive, and a create or rename survives only once its
 * directory's operations are synced.
 *
 * Paths given to this module are relative to the modelled directory,
 * with no leading slash, no empty, "." or ".." components ("" names the
 * directory itself).  cmc_proc.c normalises them first.
 */
#ifndef CMC_FS_H
#define CMC_FS_H

#include <stddef.h>
#include <stdint.h>

#define FS_MAXINO 16			/* files and directories per node, the root included */
#define FS_MAXENT 16			/* entries in one directory */
#define FS_MAXOP 16				/* operations on one inode since its last fsync */
#define FS_NAME 32				/* a path component, with its NUL */
#define FS_MAXFD 16				/* open modelled files per node */
#define FS_MAXCRASH 256			/* ways one crash may leave a node's files (each is an action) */
#define FS_FILEMAX 4096			/* bytes in one file, and in one inode's pending writes */
#define FS_LIMIT (-100000)		/* a return: a limit above was reached */
#define FS_NOTMOD (-100001)		/* a return: the call is not modelled */

/* open flags this module acts on (the caller refuses the rest) */
#define FS_O_READ 1
#define FS_O_WRITE 2
#define FS_O_CREAT 4
#define FS_O_EXCL 8
#define FS_O_TRUNC 16
#define FS_O_APPEND 32
#define FS_O_DIRECTORY 64

typedef struct { char name[FS_NAME]; int ino; } FsEnt;

/* An operation since the last fsync.  On a file: W writes len bytes
 * (from the inode's opdata at at) at off, T truncates to off.  On a
 * directory: L links n1 to ino, U unlinks n1, R renames n1 to n2. */
typedef struct { char kind; int off, len, at, ino; char n1[FS_NAME], n2[FS_NAME]; } FsOp;

typedef struct {
	int used, isdir;
	int vlen, dlen;				/* a file's length now, and on disk */
	unsigned char vol[FS_FILEMAX], dur[FS_FILEMAX];
	int nent, ndent;			/* a directory's entries now, and on disk */
	FsEnt ent[FS_MAXENT], dent[FS_MAXENT];
	int nop, opbytes;
	FsOp op[FS_MAXOP];
	unsigned char opdata[FS_FILEMAX];
} FsIno;

typedef struct { int fd, ino, flags; long off; } FsFd;

typedef struct {
	FsIno ino[FS_MAXINO];		/* ino[0] is the modelled directory */
	FsFd fd[FS_MAXFD];
	int nfd;
} Fs;

/* What one write, truncation or fsync changed: enough to undo it when
 * the search makes the call fail instead (fs_undo()). */
typedef struct { int ino; long off; int fdslot; FsIno copy; } FsUndo;

typedef void (*FsHash) (const uint8_t *p, size_t n, uint64_t fp[2]);

/* Each call returns what Linux returns: a count or 0, or a negative
 * errno; or FS_LIMIT when a limit above is reached, which the caller
 * reports as an error rather than answer.  A call that changes a file
 * fills *u, when u is not NULL, with what fs_undo() needs. */
void fs_init(Fs *fs);
int fs_isfd(const Fs *fs, int fd);
int fs_isdirfd(const Fs *fs, int fd);
int fs_open(Fs *fs, const char *rel, int flags, int fd);
int fs_close(Fs *fs, int fd);
long fs_read(Fs *fs, int fd, unsigned char *buf, long n, long off);
long fs_write(Fs *fs, int fd, const unsigned char *buf, long n, long off, FsUndo *u);
long fs_lseek(Fs *fs, int fd, long off, int whence);
int fs_ftruncate(Fs *fs, int fd, long len, FsUndo *u);
int fs_fsync(Fs *fs, int fd, FsUndo *u);
int fs_mkdir(Fs *fs, const char *rel);
int fs_unlink(Fs *fs, const char *rel);
int fs_rename(Fs *fs, const char *from, const char *to);
int fs_stat(const Fs *fs, int fd, const char *rel, long *size, int *isdir);
void fs_undo(Fs *fs, const FsUndo *u, int drop_ops);
long fs_bytes(const Fs *fs);
long fs_crash_choices(const Fs *fs);
void fs_crash(Fs *fs, long choice);
int fs_encode(const Fs *fs, FsHash h, unsigned char *out, int cap);
int fs_encode_ino(const FsIno *i, FsHash h, unsigned char *out, int cap);

#endif
