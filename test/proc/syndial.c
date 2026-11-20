/*
 * test/proc/syndial.c: node 1 dials node 0 after a one-second sleep and
 * reports how the dial ended, as node[1][0]: 1 ETIMEDOUT, 2 connected,
 * 3 ECONNREFUSED, 4 another error.  Node 0 listens and reads from each
 * connection it accepts; in mode exit it exits at once instead (its
 * host stays up).  The mode, from the target args:
 *   nb     a non-blocking connect, poll(POLLOUT, -1), then SO_ERROR;
 *          node 1 also records poll's revents as node[1][1]
 *   block  a blocking connect
 *   exit   a blocking connect to node 0, which has exited
 * With partitions, a cut link loses the SYNs and Linux gives up after
 * its retries (ETIMEDOUT, about 127 s); with crashes, a host that lost
 * power answers nothing either, while one whose process exited refuses
 * at once (ECONNREFUSED).
 *
 *   cc -O1 -no-pie -o syndial test/proc/syndial.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void
emit(unsigned char v, unsigned char w)
{
	unsigned char r[3] = {2, v, w};

	(void) !write(3, r, 3);
}

static int
code(int err)
{
	return err == ETIMEDOUT ? 1 : err == 0 ? 2 : err == ECONNREFUSED ? 3 : 4;
}

int
main(int argc, char **argv)
{
	int			id = atoi(argv[1]);
	const char *mode = argc > 2 ? argv[2] : "nb";
	struct sockaddr_in a;
	struct timespec t = {1, 0};
	unsigned char b;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(20000);
	emit(0, 0);
	if (id == 0)
	{
		if (!strcmp(mode, "exit"))
			return 0;
		int			ls = socket(AF_INET, SOCK_STREAM, 0), x;

		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 4);
		while ((x = accept(ls, NULL, NULL)) >= 0)
			(void) !read(x, &b, 1);
		return 0;
	}
	nanosleep(&t, NULL);
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	if (strcmp(mode, "nb") != 0)
	{
		int			r = connect(s, (struct sockaddr *) &a, sizeof a);

		emit((unsigned char) code(r == 0 ? 0 : errno), 0);
	}
	else
	{
		struct pollfd p = {s, POLLOUT, 0};
		int			e = 0;
		socklen_t	l = sizeof e;

		fcntl(s, F_SETFL, O_NONBLOCK);
		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0 && errno != EINPROGRESS)
		{
			emit(4, 0);
			return 0;
		}
		poll(&p, 1, -1);
		getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l);
		emit((unsigned char) code(e), (unsigned char) p.revents);
		fcntl(s, F_SETFL, 0);
	}
	(void) !read(s, &b, 1);
	return 0;
}
