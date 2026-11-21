/*
 * test/proc/inkernel.c: node 0 dials node 1 and sends "ab" in one
 * write; node 1 reads.  Bytes that node 1 has started to read sit in
 * node 1's kernel: a crash of node 0 cannot take them back, and they
 * are readable at once.  The mode,
 * from the target args:
 *   partial  node 1 reads one byte, then reads again: 4 if it gets
 *            the second byte, 1 if EOF (impossible on Linux)
 *   pollwake node 1 polls for POLLIN, then reads: 4 for 'a', 1 for EOF
 *            (reachable through a crash before the write, so the gate
 *            pins this mode by its state count: a crash after the wake
 *            has one way, keeping "ab")
 *   nbrest   node 1 polls, reads one byte, then reads non-blocking: 4
 *            for 'b', 1 for EAGAIN (impossible on Linux)
 *   wakerst  node 1 polls, polls again with a zero timeout, then
 *            reads: 2 unless both polls reported POLLIN alone (bytes
 *            are there), else 4 for 'a', 1 for anything else
 *            (impossible on Linux: a reset after the polls leaves 'a'
 *            readable first)
 * Node 1 reports the result as node[1][0] and then waits; node 0 waits
 * for a reply that never comes.  The model gives node 0 a crash.
 *
 *   cc -O1 -no-pie -o inkernel test/proc/inkernel.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
emit(unsigned char v)
{
	unsigned char r[2] = {1, v};

	(void) !write(3, r, 2);
}

/* node 1: accept one connection and read it as the mode says */
static void
reader(const char *mode)
{
	struct sockaddr_in a;
	int			ls = socket(AF_INET, SOCK_STREAM, 0), x, one = 1;
	unsigned char b;
	struct pollfd p;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(20001);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 4);
	x = accept(ls, NULL, NULL);
	p.fd = x;
	p.events = POLLIN;
	if (strcmp(mode, "partial") != 0)
		poll(&p, 1, -1);

	if (!strcmp(mode, "wakerst"))
	{
		short		r1 = p.revents;

		poll(&p, 1, 0);
		if (r1 != POLLIN || p.revents != POLLIN)
			emit(2);
		else
			emit(read(x, &b, 1) == 1 ? 4 : 1);
		(void) !read(x, &b, 1);
		return;
	}
	if (strcmp(mode, "pollwake") != 0)
	{
		if (read(x, &b, 1) != 1)
			return;
		emit(2);
	}
	if (!strcmp(mode, "nbrest"))
	{
		fcntl(x, F_SETFL, O_NONBLOCK);
		ssize_t		r = read(x, &b, 1);

		emit(r == 1 ? 4 : r < 0 && errno == EAGAIN ? 1 : 3);
		fcntl(x, F_SETFL, 0);
	}
	else
		emit(read(x, &b, 1) == 1 ? 4 : 1);
	(void) !read(x, &b, 1);
}

int
main(int argc, char **argv)
{
	int			id = atoi(argv[1]);
	const char *mode = argc > 2 ? argv[2] : "partial";
	struct sockaddr_in a;
	unsigned char b;

	signal(SIGPIPE, SIG_IGN);
	emit(0);
	if (id == 1)
	{
		reader(mode);
		return 0;
	}
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(20001);
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		return 1;
	(void) !write(s, "ab", 2);
	(void) !read(s, &b, 1);
	return 0;
}
