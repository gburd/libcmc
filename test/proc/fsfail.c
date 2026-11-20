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
 *   odirect, mmap, link, xattr, cross
 *            a call the model refuses (exit 3)
 *
 * Canonical state on fd 3: [1][value], value 0 while nothing is stored.
 *
 *   cc -O1 -no-pie -o fsfail test/proc/fsfail.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/xattr.h>
#include <unistd.h>

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
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "rewrite";
	struct sockaddr_in a = {0};
	int ls = socket(AF_INET, SOCK_STREAM, 0), c;
	unsigned char m[2];

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
