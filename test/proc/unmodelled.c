/*
 * unmodelled.c: node 0 exercises, in the mode named by its [target] arg,
 * a construct the process engine once answered wrongly.  Without a
 * recognised mode the node does nothing and the check passes, which
 * shows each verdict comes from the construct.
 *
 * Modes:
 *   badptr     write() from an unmapped buffer.  Linux returns EFAULT, so
 *              the node records 2; the engine used to send zero bytes and
 *              report success (1), which the model's invariant forbids.
 *   pollto     poll() with a finite timeout       (refused: exit 3)
 *   epollet    an edge-triggered epoll registration (refused: exit 3)
 *   semaphore  an EFD_SEMAPHORE eventfd           (refused: exit 3)
 *   longrec    a 65-byte state record, past the 64 stored (refused)
 *   partrec    a state record cut short before it parks  (refused)
 *
 *   cc -O1 -no-pie -o unmodelled test/proc/unmodelled.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

static void
emit(int v)
{
	unsigned char rec[2] = {1, (unsigned char) v};

	(void) !write(3, rec, sizeof rec);
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";

	emit(0);
	if (!strcmp(mode, "badptr"))
	{
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);

		if (connect(s, (struct sockaddr *) &a, sizeof a) == 0)
			emit(write(s, (void *) 16, 1) < 0 ? 2 : 1);
		return 0;
	}
	if (!strcmp(mode, "pollto"))
	{
		struct pollfd pf = {.fd = socket(AF_INET, SOCK_STREAM, 0), .events = POLLIN};
		struct sockaddr_in a = addr(BASE + 0);

		bind(pf.fd, (struct sockaddr *) &a, sizeof a);
		listen(pf.fd, 8);
		emit(poll(&pf, 1, 100) == 0 ? 1 : 2);
		return 0;
	}
	if (!strcmp(mode, "epollet"))
	{
		int ep = epoll_create1(0), e = eventfd(0, 0);
		struct epoll_event ev = {.events = EPOLLIN | EPOLLET};

		epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev);
		return 0;
	}
	if (!strcmp(mode, "semaphore"))
	{
		(void) eventfd(2, EFD_SEMAPHORE);
		return 0;
	}
	if (!strcmp(mode, "longrec") || !strcmp(mode, "partrec"))
	{
		/* a record whose count byte promises 65 bytes: written whole, or
		 * cut to its first ten bytes, then the node parks in accept() */
		unsigned char rec[66];
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 0);

		memset(rec, 1, sizeof rec);
		rec[0] = 65;
		(void) !write(3, rec, !strcmp(mode, "longrec") ? sizeof rec : 10);
		bind(s, (struct sockaddr *) &a, sizeof a);
		listen(s, 8);
		(void) accept(s, NULL, NULL);
		return 0;
	}
	return 0;
}
