/*
 * durlog demo for process-mode M6 (experiments/process-mode).
 *
 * A two-node replicated log.  A client's write is "committed" once the
 * primary acknowledges it; the safety property is that a committed write
 * is never lost -- it must be durable on some node that survives a crash.
 *
 * Node 0 (primary) receives the client's WRITE(v).  Node 1 (backup)
 * stores a forwarded value durably and acks.  Durability is declared by
 * emitting the stored value on fd 193 (survives a crash); volatile state is
 * emitted on fd 3 (lost on a crash).  The checker owns crashes: it may
 * CRASH the primary at any quiescent point, after which the primary keeps
 * only its fd-193 (durable) state.
 *
 * Correct: the primary forwards to the backup, waits for the backup's
 * durable-ack, makes the write durable locally (fd 4), and only THEN acks
 * the client.  A crash after the ack cannot lose the write: it is durable
 * on both nodes.
 *
 * -DBUG: the primary acks the client as soon as it has forwarded, before
 * anyone has made the write durable.  If it then crashes before the
 * backup stored, the committed write is gone -- it is in neither node's
 * durable state.
 *
 * Ordinary blocking sockets, no libcmc header.  Node i listens on BASE+i.
 * Canonical state fd 3 (volatile) and fd 193 (durable): [count=1][value].
 *
 *   cc -O1 -no-pie -o durlog     demo/durlog.c
 *   cc -O1 -no-pie -DBUG -o durlog-bug demo/durlog.c
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
		/* backup: receive a forwarded value, store it durably, ack */
		int c = accept(ls, 0, 0);
		unsigned char m[8];
		int n = read(c, m, sizeof m);
		if (n >= 2 && m[0] == 'F')
		{
			emit(193, m[1]);				/* durable: survives a crash */
			emit(3, m[1]);
			unsigned char ack[2] = {'D', m[1]};
			write(c, ack, 2);			/* durable-ack back to primary */
		}
		close(c);
		close(ls);
		return 0;
	}

	/* primary: take the client's WRITE, replicate, ack */
	int c = accept(ls, 0, 0);
	unsigned char m[8];
	int n = read(c, m, sizeof m);
	if (n >= 2 && m[0] == 'W')
	{
		int v = m[1];
		int bk = dial(BASE + 1);
		unsigned char fwd[2] = {'F', (unsigned char) v};
		write(bk, fwd, 2);				/* forward to backup */

#ifdef BUG
		/* ack before anyone has made it durable */
		unsigned char ack[2] = {'A', (unsigned char) v};
		write(c, ack, 2);
		unsigned char dack[2];
		read(bk, dack, 2);				/* (then) wait for the backup */
		emit(193, v);
		emit(3, v);
#else
		/* wait for the backup's durable-ack, make it durable here, THEN ack */
		unsigned char dack[2];
		read(bk, dack, 2);
		emit(193, v);						/* durable locally */
		emit(3, v);
		unsigned char ack[2] = {'A', (unsigned char) v};
		write(c, ack, 2);				/* only now is it committed */
#endif
		close(bk);
	}
	close(c);
	close(ls);
	return 0;
}
