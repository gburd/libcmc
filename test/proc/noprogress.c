/*
 * test/proc/noprogress.c: the no-progress reproducer from the davros
 * evaluation (B3, ask 4).  A server polls a fixed set that holds, at
 * slot 0, an fd that libcmc does not track (here a negative sentinel,
 * standing in for libxtc's kernel self-pipe); the real listener is at
 * slot 1.  With -DNEGFD the loop only ever treats slot 0 as the ready
 * fd, so it polls, sees nothing it can act on, polls again, and never
 * accepts: it makes no progress.  Under a safety-only model that used to
 * pass vacuously; cmc now reports INCOMPLETE (no progress).  Without
 * -DNEGFD it accepts the listener and stores the byte, so it progresses
 * and the safety invariant is a real pass.
 *
 * Canonical state fd 3, node 1: [count=1][stored byte].
 *
 *   cc -no-pie -o noprogress test/proc/noprogress.c            (progresses)
 *   cc -no-pie -DNEGFD -o noprogress-stuck test/proc/noprogress.c (stuck)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char st;

static void
emit(void)
{
	unsigned char rec[2] = {1, st};
	(void) !write(3, rec, sizeof rec);
}

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	return a;
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;

	if (id == 0)
	{
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);
		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
			return 3;
		unsigned char b = 'x';
		(void) !write(s, &b, 1);
		return 0;
	}

	st = 0;
	emit();
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + 1);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);

	int conn = -1;
	for (;;)
	{
		struct pollfd pf[2];
		pf[0].fd = -1;				/* an untracked/kernel fd slot */
		pf[0].events = POLLIN;
		pf[0].revents = 0;
		pf[1].fd = conn < 0 ? ls : conn;
		pf[1].events = POLLIN;
		pf[1].revents = 0;
		if (poll(pf, 2, -1) <= 0)
			continue;
#ifdef NEGFD
		/* the stuck loop only ever acts on slot 0, which is never a real
		 * fd, so it never accepts: no progress */
		if (pf[0].revents)
			continue;
		continue;
#else
		if (pf[1].revents)
		{
			if (conn < 0)
			{
				conn = accept(ls, NULL, NULL);
				continue;
			}
			unsigned char buf[4];
			ssize_t n = read(conn, buf, sizeof buf);
			if (n > 0) { st = buf[0]; emit(); break; }
			if (n == 0) break;
		}
#endif
	}
	return 0;
}
