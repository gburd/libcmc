/*
 * kvserver.c: the server the guide checks (docs/guide/02-first-model.md).
 * It is written as it ships: one TCP connection per client, blocking
 * reads and writes, no libcmc header.  A client writes a value; the
 * server makes it durable, then acknowledges it.  Two nodes run this same
 * program: node 0 is the primary, node 1 the backup it replicates to
 * before it acknowledges.
 *
 *   cc -no-pie -o kvserver kvserver.c
 *   cmc --model-file kvserver.model
 *
 * Built with -DKV_BUG, the primary acknowledges as soon as the value is
 * in its memory, before anyone has made it durable.  Nothing goes wrong
 * until a crash: then an acknowledged write can be lost.  make check
 * builds both and checks what the guide says about them
 * (test/docs_gate.sh).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/* !region hook */
/* The state hook: after each change, report the logical state as a
 * length byte and the bytes -- what is in memory on fd 3, what has been
 * made durable on fd 193.  This is the only code the checker asks for. */
static void
report(int fd, unsigned char value)
{
	unsigned char rec[2] = {1, value};

	(void) !write(fd, rec, sizeof rec);
}
/* !endregion hook */

#define PORT 20000				/* node i listens on PORT + i */

static int
listen_on(int port)
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (bind(s, (struct sockaddr *) &a, sizeof a) != 0 || listen(s, 8) != 0)
		exit(2);
	return s;
}

static int
dial(int port)
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		exit(3);
	return s;
}

/* !region server */
int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	int			ls = listen_on(PORT + id);
	int			c = accept(ls, NULL, NULL);
	unsigned char m[2];

	report(3, 0);
	if (read(c, m, 2) != 2)
		return 0;

	if (id == 1)
	{
		/* backup: store the forwarded value durably, then confirm */
		if (m[0] == 'F')
		{
			report(193, m[1]);	/* fsync'd */
			report(3, m[1]);
			unsigned char ok[2] = {'D', m[1]};

			(void) !write(c, ok, 2);
		}
		return 0;
	}

	/* primary: a client's WRITE(v) */
	if (m[0] != 'W')
		return 0;
	unsigned char v = m[1];
	int			bk = dial(PORT + 1);
	unsigned char fwd[2] = {'F', v}, ack[2] = {'A', v}, ok[2];

	(void) !write(bk, fwd, 2);
#ifdef KV_BUG
	report(3, v);				/* in memory only */
	(void) !write(c, ack, 2);	/* acknowledged too early */
	if (read(bk, ok, 2) == 2 && ok[0] == 'D')
		report(193, v);
#else
	if (read(bk, ok, 2) == 2 && ok[0] == 'D')
	{
		report(193, v);			/* durable here and on the backup */
		report(3, v);
		(void) !write(c, ack, 2);	/* only now acknowledged */
	}
#endif
	return 0;
}
/* !endregion server */
