/*
 * test/proc/fslog.c: one node keeps a log in modelled files.  It takes
 * WRITE(v) from the client, puts v in /data/log with pwrite and
 * fdatasync, then commits by writing a control file /data/ctl.tmp,
 * fsyncing it, renaming it to /data/ctl and fsyncing /data, and only
 * then acks.  On startup it recovers: the control file says how many log
 * bytes are committed, and it reports the first of them.
 *
 * -DBUG leaves out the fsync of /data, so the rename (and the log's own
 * creation) can be lost at a crash after the ack.  -DBUG2 leaves out the
 * fdatasync of the log, so the control file can survive while the log
 * byte it names does not.
 *
 * Canonical state on fd 3: [1][value], value 0 while nothing is
 * committed.
 *
 *   cc -O1 -no-pie -o fslog test/proc/fslog.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void
emit(unsigned char v)
{
	unsigned char rec[2] = {1, v};
	(void) !write(3, rec, 2);
}

/* the committed value, or 0 */
static unsigned char
recover(void)
{
	unsigned char n = 0, v = 0;
	struct stat st;
	int ctl = open("/data/ctl", O_RDONLY);
	if (ctl < 0) return 0;
	if (read(ctl, &n, 1) != 1) n = 0;
	close(ctl);
	int log = open("/data/log", O_RDONLY);
	if (log < 0) return 0;
	if (n > 0 && fstat(log, &st) == 0 && st.st_size >= n && pread(log, &v, 1, 0) != 1) v = 0;
	close(log);
	return v;
}

static void
commit(unsigned char v)
{
	unsigned char n = 1;
	int log = openat(AT_FDCWD, "/data/log", O_CREAT | O_RDWR, 0644);
	(void) !pwrite(log, &v, 1, 0);
#ifndef BUG2
	fdatasync(log);
#endif
	close(log);
	int tmp = open("/data/ctl.tmp", O_CREAT | O_TRUNC | O_WRONLY, 0644);
	(void) !write(tmp, &n, 1);
	fsync(tmp);
	close(tmp);
	rename("/data/ctl.tmp", "/data/ctl");
#ifndef BUG
	int dir = open("/data", O_RDONLY | O_DIRECTORY);
	fsync(dir);
	close(dir);
#endif
}

int
main(void)
{
	struct sockaddr_in a = {0};
	int ls = socket(AF_INET, SOCK_STREAM, 0), c;
	unsigned char m[2];

	emit(recover());
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20000);
	bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 1);
	c = accept(ls, NULL, NULL);
	if (c >= 0 && read(c, m, 2) == 2 && m[0] == 'W')
	{
		unsigned char ack[2] = {'A', m[1]};
		commit(m[1]);
		emit(m[1]);
		(void) !write(c, ack, 2);
	}
	for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	return 0;
}
