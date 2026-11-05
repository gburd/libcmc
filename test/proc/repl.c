/*
 * test/proc/repl.c: a two-node replicated register, written as it ships
 * (test/proc/DESIGN.md, M4).  Ordinary blocking sockets,
 * no libcmc header, no step function.  The checker runs node 0 (primary)
 * and node 1 (backup) as two processes and owns every socket between
 * them, so it chooses the delivery order.
 *
 * Node id and role come from argv: `repl 0` is the primary, `repl 1` the
 * backup.  A node's stored value is its declared state, emitted on fd 3
 * after each change (state = canon).
 *
 * Primary (node 0):
 *   - accepts one client connection, reads one WRITE(v);
 *   - connects to the backup and sends REPL(v);
 *   - waits for the backup's ACK, then stores v and acks the client.
 * Backup (node 1):
 *   - accepts the primary's connection, reads REPL(v), stores v, acks.
 *
 * Correct order: the client is acked only after the backup has stored v,
 * so once acked the two agree.  Built -DBUG the primary stores and acks
 * the client BEFORE waiting for the backup, so a schedule where the
 * backup's REPL is delayed leaves the backup behind an acked write --
 * the invariant "once the client is acked, backup == primary" fails.
 *
 * Addresses are by convention: node i listens on 127.0.0.1:(BASE+i).
 * The checker intercepts connect/accept/send/recv and never lets a real
 * socket form, so the ports are only logical peer ids.
 *
 *   cc -O1 -no-pie -o repl       test/proc/repl.c
 *   cc -O1 -no-pie -DBUG -o repl-bug test/proc/repl.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char value;		/* the declared state: this node's value */

static void
emit(void)
{
	unsigned char rec[2] = {1, value};	/* [count=1][value] on fd 3 */
	(void) !write(3, rec, 2);
}

static int
listen_on(int port)
{
	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	int			one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (bind(ls, (void *) &a, sizeof a) != 0)
		_exit(2);
	listen(ls, 4);
	return ls;
}

static int
connect_to(int port)
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (connect(s, (void *) &a, sizeof a) != 0)
		_exit(3);
	return s;
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	emit();						/* initial state: value 0 */

	if (id == 1)
	{
		/* backup: accept the primary, store each REPL, ack it */
		int			ls = listen_on(BASE + 1);
		int			cs = accept(ls, 0, 0);
		for (;;)
		{
			unsigned char m[2];
			if (read(cs, m, 2) != 2)
				break;
			if (m[0] == 'R')	/* REPL(v) */
			{
				value = m[1];
				emit();
				unsigned char ack[2] = {'A', m[1]};
				if (write(cs, ack, 2) != 2)
					break;
			}
		}
		return 0;
	}

	/* primary: one client WRITE, replicate to the backup, ack the client */
	int			ls = listen_on(BASE + 0);
	int			client = accept(ls, 0, 0);
	unsigned char m[2];
	if (read(client, m, 2) != 2 || m[0] != 'W')
		return 0;
	unsigned char v = m[1];

	int			bk = connect_to(BASE + 1);
	unsigned char repl[2] = {'R', v};
	write(bk, repl, 2);

#ifdef BUG
	/* store and ack the client before the backup confirms */
	value = v;
	emit();
	unsigned char cack[2] = {'A', v};
	write(client, cack, 2);
	unsigned char ba[2];
	(void) !read(bk, ba, 2);	/* drain the backup ack, too late to matter */
#else
	unsigned char ba[2];
	if (read(bk, ba, 2) == 2 && ba[0] == 'A')
	{
		value = v;				/* commit only after the backup stored it */
		emit();
		unsigned char cack[2] = {'A', v};
		write(client, cack, 2);
	}
#endif
	return 0;
}
