/*
 * restart demo for process-mode M6 (test/proc): crash and recover.
 *
 * A two-node pair.  The client (node 0) takes the synthetic client's
 * WRITE(v), forwards it to the server (node 1), and acks the synthetic
 * client only once the server has made v durable.  The server stores v
 * durably on descriptor 193 and acks.  The checker may CRASH the server
 * and then RESTART it: a restarted process is a fresh program whose
 * surviving fd-193 record is handed back to it on startup, so a server
 * that reads its own durable log recovers.
 *
 * Safety property (no_lost_ack): a value the client acked (it is durable
 * somewhere) is still present after the server restarts.
 *
 * Correct: the server reads its fd-193 record on startup and re-emits it
 * to its canonical state, so a value made durable before the crash is
 * back after the restart.  The committed write is never lost.
 *
 * -DBUG: the server does not read fd 193 on startup, so after a restart
 * it recovers nothing -- the committed value is on neither node and the
 * checker reports no_lost_ack violated.  Without a RESTART action the
 * crash is final (the server never comes back), so the bug cannot show:
 * RESTART is what makes recovery checkable.
 *
 * Ordinary blocking sockets, no libcmc header.  Node i listens on BASE+i.
 * Canonical state fd 3 (volatile) and fd 193 (durable): [count=1][value].
 *
 *   cc -O1 -no-pie -o restart     test/proc/restart.c
 *   cc -O1 -no-pie -DBUG -o restart-bug test/proc/restart.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static int
listen_on(int port)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in sa = {0};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(ls, (struct sockaddr *) &sa, sizeof sa);
	listen(ls, 8);
	return ls;
}

static int
dial(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = {0};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	connect(s, (struct sockaddr *) &sa, sizeof sa);
	return s;
}

static void
emit(int fd, int value)
{
	unsigned char rec[2] = {1, (unsigned char) value};
	write(fd, rec, sizeof rec);
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	int ls = listen_on(BASE + id);

	if (id == 1)
	{
		/* server: recover from the durable log, then serve writes, storing
		 * each durably and acking.  After serving a request it loops back
		 * to accept(), so there is a quiescent point at which it is alive
		 * and holds a committed (durable) value -- the point the checker
		 * can crash and then restart it. */
#ifndef BUG
		/* recovery: read back the value this process made durable before a
		 * crash (the checker hands the surviving fd-193 record to a
		 * restarted process); re-emit it so the recovered value is part of
		 * the restarted server's canonical state. */
		{
			unsigned char rec[2];
			int n = read(193, rec, sizeof rec);
			if (n >= 2 && rec[0] == 1)
				emit(3, rec[1]);			/* volatile: it is back in memory */
		}
#endif
		for (;;)
		{
			int c = accept(ls, 0, 0);
			if (c < 0)
				break;
			unsigned char m[8];
			int n = read(c, m, sizeof m);
			if (n >= 2 && m[0] == 'F')
			{
				emit(193, m[1]);			/* durable: survives a crash */
				emit(3, m[1]);
				unsigned char ack[2] = {'D', m[1]};
				write(c, ack, 2);		/* durable-ack back to the client */
			}
			close(c);
		}
		close(ls);
		return 0;
	}

	/* client-facing node: take the synthetic client's WRITE, forward it to
	 * the server, and ack the synthetic client only once the server has
	 * made it durable. */
	int c = accept(ls, 0, 0);
	unsigned char m[8];
	int n = read(c, m, sizeof m);
	if (n >= 2 && m[0] == 'W')
	{
		int v = m[1];
		int sv = dial(BASE + 1);
		unsigned char fwd[2] = {'F', (unsigned char) v};
		write(sv, fwd, 2);				/* forward to the server */
		unsigned char dack[2];
		if (read(sv, dack, 2) == 2 && dack[0] == 'D')
		{
			unsigned char ack[2] = {'A', (unsigned char) v};
			write(c, ack, 2);			/* now committed: ack the client */
		}
		close(sv);
	}
	close(c);
	close(ls);
	return 0;
}
