/*
 * cmc_fs.c: the modelled file system of one node.  See cmc_fs.h for the
 * model.  Plain data and libc only: no system calls.
 */
#include <errno.h>
#include <string.h>

#include "cmc_fs.h"

void
fs_init(Fs *fs)
{
	memset(fs, 0, sizeof *fs);
	fs->ino[0].used = 1;
	fs->ino[0].isdir = 1;
}

/* the open-file slot of fd, or -1 */
static int
fd_slot(const Fs *fs, int fd)
{
	for (int i = 0; i < fs->nfd; i++)
		if (fs->fd[i].fd == fd) return i;
	return -1;
}

int
fs_isfd(const Fs *fs, int fd)
{
	return fd_slot(fs, fd) >= 0;
}

int
fs_isdirfd(const Fs *fs, int fd)
{
	int s = fd_slot(fs, fd);
	return s >= 0 && fs->ino[fs->fd[s].ino].isdir;
}

/* The entry index of name in directory d's current entries, or -1. */
static int
ent_find(const FsIno *d, const char *name)
{
	for (int i = 0; i < d->nent; i++)
		if (!strcmp(d->ent[i].name, name)) return i;
	return -1;
}

/* Walk rel from the modelled directory.  Returns the inode rel names,
 * or a negative errno; *dir gets the directory holding the last
 * component (-1 for "" itself), and *last points at that component. */
static int
walk(const Fs *fs, const char *rel, int *dir, const char **last)
{
	int cur = 0;
	const char *p = rel;

	*dir = -1; *last = rel;
	if (!*rel) return 0;
	for (;;)
	{
		const char *sl = strchr(p, '/');
		size_t n = sl ? (size_t) (sl - p) : strlen(p);
		char name[FS_NAME];

		if (n >= FS_NAME) return -ENAMETOOLONG;
		if (!fs->ino[cur].isdir) return -ENOTDIR;
		memcpy(name, p, n); name[n] = 0;
		int e = ent_find(&fs->ino[cur], name);
		if (!sl) { *dir = cur; *last = p; return e < 0 ? -ENOENT : fs->ino[cur].ent[e].ino; }
		if (e < 0) return -ENOENT;
		cur = fs->ino[cur].ent[e].ino;
		p = sl + 1;
	}
}

/* Record operation o on inode i, since its last fsync. */
static int
add_op(FsIno *i, const FsOp *o)
{
	if (i->nop >= FS_MAXOP) return FS_LIMIT;
	i->op[i->nop++] = *o;
	return 0;
}

/* Apply directory operation o to the entry list ent (n entries). */
static void
dir_apply(FsEnt *ent, int *n, const FsOp *o)
{
	int k = -1, d = -1;

	for (int j = 0; j < *n; j++)
	{
		if (!strcmp(ent[j].name, o->n1)) k = j;
		if (o->kind == 'R' && !strcmp(ent[j].name, o->n2)) d = j;
	}
	if (o->kind == 'L')
	{
		strcpy(ent[*n].name, o->n1); ent[*n].ino = o->ino; (*n)++;
		return;
	}
	if (k < 0) return;
	if (o->kind == 'R')
	{
		strcpy(ent[k].name, o->n2);
		if (d < 0) return;
		k = d;
	}
	/* remove entry k, keeping the order of the rest */
	for (int j = k; j + 1 < *n; j++) ent[j] = ent[j + 1];
	(*n)--;
}

/* Each link takes a fresh inode, which the link keeps referenced, so a
 * directory names fewer inodes, now or on disk, than there are. */
_Static_assert(FS_MAXENT >= FS_MAXINO - 1, "a directory can hold every inode");

/* Change directory d now (its current entries) and record the change
 * for the disk, which sees it at the next fsync of d. */
static int
dir_change(FsIno *d, const FsOp *o)
{
	if (add_op(d, o)) return FS_LIMIT;
	dir_apply(d->ent, &d->nent, o);
	return 0;
}

/* Is inode n named by an entry of any directory, now or on disk or in a
 * pending link, or open?  An inode that is none of these is gone, and
 * its slot is free again. */
static int
referenced(const Fs *fs, int n)
{
	for (int i = 0; i < FS_MAXINO; i++)
	{
		const FsIno *d = &fs->ino[i];
		if (!d->used || !d->isdir) continue;
		for (int j = 0; j < d->nent; j++) if (d->ent[j].ino == n) return 1;
		for (int j = 0; j < d->ndent; j++) if (d->dent[j].ino == n) return 1;
		for (int j = 0; j < d->nop; j++) if (d->op[j].kind == 'L' && d->op[j].ino == n) return 1;
	}
	for (int i = 0; i < fs->nfd; i++) if (fs->fd[i].ino == n) return 1;
	return 0;
}

/* Free every inode but the root that nothing references. */
static void
gc(Fs *fs)
{
	for (int again = 1; again;)
	{
		again = 0;
		for (int n = 1; n < FS_MAXINO; n++)
			if (fs->ino[n].used && !referenced(fs, n))
			{
				memset(&fs->ino[n], 0, sizeof fs->ino[n]);
				again = 1;
			}
	}
}

/* A free inode slot, cleared, or FS_LIMIT. */
static int
ino_alloc(Fs *fs, int isdir)
{
	for (int n = 1; n < FS_MAXINO; n++)
		if (!fs->ino[n].used)
		{
			memset(&fs->ino[n], 0, sizeof fs->ino[n]);
			fs->ino[n].used = 1; fs->ino[n].isdir = isdir;
			return n;
		}
	return FS_LIMIT;
}

/* Give fd to inode n with the open flags. */
static int
fd_add(Fs *fs, int fd, int n, int flags)
{
	if (fs->nfd >= FS_MAXFD) return FS_LIMIT;
	fs->fd[fs->nfd].fd = fd; fs->fd[fs->nfd].ino = n;
	fs->fd[fs->nfd].flags = flags; fs->fd[fs->nfd].off = 0;
	fs->nfd++;
	return 0;
}

/* Truncate file f to len, now, recording it for the disk. */
static int
truncate_to(FsIno *f, long len)
{
	FsOp o = {'T', (int) len, 0, 0, 0, "", ""};

	if (len > FS_FILEMAX) return FS_LIMIT;
	if (add_op(f, &o)) return FS_LIMIT;
	if (len > f->vlen) memset(f->vol + f->vlen, 0, (size_t) (len - f->vlen));
	f->vlen = (int) len;
	return 0;
}

int
fs_open(Fs *fs, const char *rel, int flags, int fd)
{
	int dir; const char *last;
	int n = walk(fs, rel, &dir, &last);

	if (n == -ENOENT && (flags & FS_O_CREAT) && dir >= 0 && !(flags & FS_O_DIRECTORY))
	{
		FsOp o = {'L', 0, 0, 0, 0, "", ""};
		n = ino_alloc(fs, 0);
		if (n < 0) return n;
		strcpy(o.n1, last); o.ino = n;
		if (dir_change(&fs->ino[dir], &o)) return FS_LIMIT;
	}
	else if (n >= 0 && (flags & FS_O_CREAT) && (flags & FS_O_EXCL)) return -EEXIST;
	if (n < 0) return n;
	FsIno *i = &fs->ino[n];
	if ((flags & FS_O_DIRECTORY) && !i->isdir) return -ENOTDIR;
	if (i->isdir && (flags & FS_O_WRITE)) return -EISDIR;
	if ((flags & FS_O_TRUNC) && (flags & FS_O_WRITE) && i->vlen > 0)
	{
		int r = truncate_to(i, 0);
		if (r) return r;
	}
	return fd_add(fs, fd, n, flags) ? FS_LIMIT : fd;
}

int
fs_close(Fs *fs, int fd)
{
	int s = fd_slot(fs, fd);

	if (s < 0) return -EBADF;
	for (int j = s; j + 1 < fs->nfd; j++) fs->fd[j] = fs->fd[j + 1];
	fs->nfd--;
	gc(fs);
	return 0;
}

/* Read up to n bytes at off (off < 0: at the fd's offset, which moves). */
long
fs_read(Fs *fs, int fd, unsigned char *buf, long n, long off)
{
	int s = fd_slot(fs, fd);

	if (s < 0) return -EBADF;
	FsFd *f = &fs->fd[s];
	FsIno *i = &fs->ino[f->ino];
	if (i->isdir) return -EISDIR;
	if (!(f->flags & FS_O_READ)) return -EBADF;
	long at = off < 0 ? f->off : off;
	long k = at >= i->vlen ? 0 : i->vlen - at;
	if (k > n) k = n;
	if (k > 0) memcpy(buf, i->vol + at, (size_t) k);
	if (off < 0) f->off += k;
	return k;
}

/* Save what a change to the inode of fd slot s may alter. */
static void
undo_save(const Fs *fs, int s, FsUndo *u)
{
	if (!u) return;
	u->fdslot = s; u->ino = fs->fd[s].ino; u->off = fs->fd[s].off;
	u->copy = fs->ino[u->ino];
}

/* Write n bytes at off (off < 0: at the fd's offset, which moves).  With
 * O_APPEND every write goes at the end, pwrite too, as on Linux. */
long
fs_write(Fs *fs, int fd, const unsigned char *buf, long n, long off, FsUndo *u)
{
	int s = fd_slot(fs, fd);

	if (s < 0) return -EBADF;
	FsFd *f = &fs->fd[s];
	FsIno *i = &fs->ino[f->ino];
	if (!(f->flags & FS_O_WRITE)) return -EBADF;
	long at = (f->flags & FS_O_APPEND) ? i->vlen : off >= 0 ? off : f->off;
	if (at + n > FS_FILEMAX) return FS_LIMIT;
	if (i->opbytes + n > FS_FILEMAX) return FS_LIMIT;
	undo_save(fs, s, u);
	FsOp o = {'W', (int) at, (int) n, i->opbytes, 0, "", ""};
	if (add_op(i, &o)) return FS_LIMIT;
	memcpy(i->opdata + i->opbytes, buf, (size_t) n);
	i->opbytes += (int) n;
	if (at > i->vlen) memset(i->vol + i->vlen, 0, (size_t) (at - i->vlen));
	memcpy(i->vol + at, buf, (size_t) n);
	if (at + n > i->vlen) i->vlen = (int) (at + n);
	if (off < 0) f->off = at + n;
	return n;
}

long
fs_lseek(Fs *fs, int fd, long off, int whence)
{
	int s = fd_slot(fs, fd);

	if (s < 0) return -EBADF;
	FsFd *f = &fs->fd[s];
	long base = whence == 0 ? 0 : whence == 1 ? f->off : whence == 2 ? fs->ino[f->ino].vlen : -1;
	if (base < 0) return -EINVAL;
	if (base + off < 0) return -EINVAL;
	f->off = base + off;
	return f->off;
}

int
fs_ftruncate(Fs *fs, int fd, long len, FsUndo *u)
{
	int s = fd_slot(fs, fd);

	if (s < 0) return -EBADF;
	if (fs->ino[fs->fd[s].ino].isdir || !(fs->fd[s].flags & FS_O_WRITE)) return -EINVAL;
	if (len < 0) return -EINVAL;
	undo_save(fs, s, u);
	return truncate_to(&fs->ino[fs->fd[s].ino], len);
}

/* Apply file operation o to the disk copy dur (length *len) of i. */
static void
file_apply(const FsIno *i, unsigned char *dur, int *len, const FsOp *o)
{
	if (o->kind == 'T')
	{
		if (o->off > *len) memset(dur + *len, 0, (size_t) (o->off - *len));
		*len = o->off;
		return;
	}
	if (o->off > *len) memset(dur + *len, 0, (size_t) (o->off - *len));
	memcpy(dur + o->off, i->opdata + o->at, (size_t) o->len);
	if (o->off + o->len > *len) *len = o->off + o->len;
}

/* Make the first k operations of inode i durable, and forget them. */
static void
settle_ops(FsIno *i, int k)
{
	for (int j = 0; j < k; j++)
		if (i->isdir) dir_apply(i->dent, &i->ndent, &i->op[j]);
		else file_apply(i, i->dur, &i->dlen, &i->op[j]);
	i->nop = 0; i->opbytes = 0;
}

/* fsync and fdatasync: the inode's operations reach the disk.  For a
 * directory that is its entries, so a create or rename in it becomes
 * durable; the files it names keep their own pending writes. */
int
fs_fsync(Fs *fs, int fd, FsUndo *u)
{
	int s = fd_slot(fs, fd);

	if (s < 0) return -EBADF;
	undo_save(fs, s, u);
	settle_ops(&fs->ino[fs->fd[s].ino], fs->ino[fs->fd[s].ino].nop);
	return 0;
}

int
fs_mkdir(Fs *fs, const char *rel)
{
	int dir; const char *last;
	int n = walk(fs, rel, &dir, &last);

	if (n >= 0) return -EEXIST;
	if (n != -ENOENT || dir < 0) return n;
	n = ino_alloc(fs, 1);
	if (n < 0) return n;
	FsOp o = {'L', 0, 0, 0, n, "", ""};
	strcpy(o.n1, last);
	return dir_change(&fs->ino[dir], &o);
}

/* unlink of a file; a directory is EISDIR, as on Linux */
int
fs_unlink(Fs *fs, const char *rel)
{
	int dir; const char *last;
	int n = walk(fs, rel, &dir, &last);

	if (n < 0) return n;
	if (dir < 0 || fs->ino[n].isdir) return -EISDIR;
	FsOp o = {'U', 0, 0, 0, n, "", ""};
	strcpy(o.n1, last);
	int r = dir_change(&fs->ino[dir], &o);
	gc(fs);
	return r;
}

/* rename within one directory, replacing a file at to.  A rename
 * between directories, or of a directory, is not modelled: on disk it is
 * two directory changes, durable separately, which the one-directory
 * operation log cannot express. */
int
fs_rename(Fs *fs, const char *from, const char *to)
{
	int d1, d2; const char *l1, *l2;
	int n = walk(fs, from, &d1, &l1);
	int m = walk(fs, to, &d2, &l2);

	if (n < 0) return n;
	if (m < 0 && m != -ENOENT) return m;
	if (m == -ENOENT && d2 < 0) return -ENOENT;	/* a missing directory on the way to to */
	if (d1 < 0 || d2 < 0) return -EBUSY;
	if (d1 != d2 || fs->ino[n].isdir) return FS_NOTMOD;
	if (m == n) return 0;
	if (m >= 0 && fs->ino[m].isdir) return -EISDIR;
	FsOp o = {'R', 0, 0, 0, n, "", ""};
	strcpy(o.n1, l1); strcpy(o.n2, l2);
	int r = dir_change(&fs->ino[d1], &o);
	gc(fs);
	return r;
}

/* fstat (rel NULL) or stat of a path: the size and whether a directory;
 * returns the inode number, or a negative errno */
int
fs_stat(const Fs *fs, int fd, const char *rel, long *size, int *isdir)
{
	int n;

	if (rel)
	{
		int dir; const char *last;
		n = walk(fs, rel, &dir, &last);
		if (n < 0) return n;
	}
	else
	{
		int s = fd_slot(fs, fd);
		if (s < 0) return -EBADF;
		n = fs->fd[s].ino;
	}
	*size = fs->ino[n].isdir ? 4096 : fs->ino[n].vlen;
	*isdir = fs->ino[n].isdir;
	return n;
}

/* The search made a write, truncation or fsync fail instead: put the
 * inode and the fd's offset back.  A failed fsync also forgets the
 * inode's unsynced operations (drop_ops): Linux marks the pages clean
 * after a writeback error, so a later fsync succeeds without writing
 * them, and a crash loses them, while a read still sees them. */
void
fs_undo(Fs *fs, const FsUndo *u, int drop_ops)
{
	fs->ino[u->ino] = u->copy;
	fs->fd[u->fdslot].off = u->off;
	if (drop_ops) { fs->ino[u->ino].nop = 0; fs->ino[u->ino].opbytes = 0; }
}

/* Bytes the node's files hold: every file's contents now and on disk,
 * and its unsynced writes. */
long
fs_bytes(const Fs *fs)
{
	long b = 0;

	for (int n = 0; n < FS_MAXINO; n++)
		if (fs->ino[n].used) b += fs->ino[n].vlen + fs->ino[n].dlen + fs->ino[n].opbytes;
	return b;
}

/* How many ways a crash can leave the disk: each inode keeps a prefix
 * of its unsynced operations, from none to all, independently.  At most
 * FS_MAXCRASH; past it FS_LIMIT. */
long
fs_crash_choices(const Fs *fs)
{
	long c = 1;

	for (int n = 0; n < FS_MAXINO; n++)
		if (fs->ino[n].used && fs->ino[n].nop > 0)
		{
			c *= fs->ino[n].nop + 1;
			if (c > FS_MAXCRASH) return FS_LIMIT;
		}
	return c;
}

/* Crash with choice (0 <= choice < fs_crash_choices()): read in mixed
 * radix, one digit per inode with operations in inode order, a digit is
 * how many of them survive.  The disk becomes the only copy, the open
 * files are gone, and what no directory on disk names is freed. */
void
fs_crash(Fs *fs, long choice)
{
	for (int n = 0; n < FS_MAXINO; n++)
	{
		FsIno *i = &fs->ino[n];
		if (!i->used) continue;
		if (i->nop > 0)
		{
			settle_ops(i, (int) (choice % (i->nop + 1)));
			choice /= i->nop + 1;
		}
		settle_ops(i, 0);
		memcpy(i->vol, i->dur, (size_t) i->dlen); i->vlen = i->dlen;
		memcpy(i->ent, i->dent, sizeof i->ent); i->nent = i->ndent;
	}
	fs->nfd = 0;
	gc(fs);
}

/* Append n bytes at p to out (cap bytes), or fail. */
#define PUTN(p, n) do { if (k + (int) (n) > cap) return -1; memcpy(out + k, (p), (size_t) (n)); k += (int) (n); } while (0)
#define PUTI(x) do { int32_t _v = (int32_t) (x); PUTN(&_v, 4); } while (0)
#define PUTS(s) PUTN((s), strlen(s) + 1)

/* Inode i in out: its kind, a hash h of each body of bytes (contents
 * now, on disk, and pending) with its length, a directory's entries now
 * and on disk, and the operations since the last fsync.  Returns the
 * length, or -1 past cap. */
int
fs_encode_ino(const FsIno *i, FsHash h, unsigned char *out, int cap)
{
	int k = 0;
	uint64_t fp[2];

	{
		PUTI(i->isdir);
		PUTI(i->vlen); h(i->vol, (size_t) i->vlen, fp); PUTN(fp, sizeof fp);
		PUTI(i->dlen); h(i->dur, (size_t) i->dlen, fp); PUTN(fp, sizeof fp);
		PUTI(i->opbytes); h(i->opdata, (size_t) i->opbytes, fp); PUTN(fp, sizeof fp);
		PUTI(i->nent);
		for (int j = 0; j < i->nent; j++) { PUTS(i->ent[j].name); PUTI(i->ent[j].ino); }
		PUTI(i->ndent);
		for (int j = 0; j < i->ndent; j++) { PUTS(i->dent[j].name); PUTI(i->dent[j].ino); }
		PUTI(i->nop);
		for (int j = 0; j < i->nop; j++)
		{
			const FsOp *o = &i->op[j];
			PUTI(o->kind); PUTI(o->off); PUTI(o->len); PUTI(o->at); PUTI(o->ino);
			PUTS(o->n1); PUTS(o->n2);
		}
	}
	return k;
}

/* The file system's part of the world, in out: each inode in use by its
 * number (which fstat reports) and fs_encode_ino(), then the open files.
 * Returns the length, or -1 past cap. */
int
fs_encode(const Fs *fs, FsHash h, unsigned char *out, int cap)
{
	int k = 0;

	for (int n = 0; n < FS_MAXINO; n++)
	{
		if (!fs->ino[n].used) continue;
		PUTI(n);
		int r = fs_encode_ino(&fs->ino[n], h, out + k, cap - k);
		if (r < 0) return -1;
		k += r;
	}
	PUTI(-1);
	PUTI(fs->nfd);
	for (int j = 0; j < fs->nfd; j++)
	{
		PUTI(fs->fd[j].fd); PUTI(fs->fd[j].ino); PUTI(fs->fd[j].flags); PUTI(fs->fd[j].off);
	}
	return k;
}
#undef PUTS
#undef PUTI
#undef PUTN
