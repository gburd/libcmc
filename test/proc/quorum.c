/*
 * test/proc/quorum.c: a coordinator that commits a value only once a
 * QUORUM MAJORITY of workers has voted for it.  This fixture exists to
 * exercise the invariant language's counting and arithmetic (Milestone
 * A): the safety property "if the coordinator committed v, then a
 * majority of workers hold v" is a cardinality statement -- it cannot be
 * written without `count` -- so it pins that feature with teeth.
 *
 * Four nodes over blocking TCP, no libcmc header.  Node 0 is the
 * coordinator; nodes 1, 2, 3 are workers.  Node i listens on
 * 127.0.0.1:(BASE+i).
 *
 *   - a client writes value v to the coordinator (two bytes `W v`);
 *   - the coordinator asks each worker to store v (`S v _`); a worker
 *     stores v and acks (`a`);
 *   - the coordinator commits v once it has a MAJORITY of acks (2 of 3),
 *     records the commit, and acks the client (`A v`).
 *
 * Canonical state, fd 3 (volatile) and fd 193 (durable):
 *   coordinator: [count=1][committed value]   (0 = nothing committed)
 *   worker:      [count=1][stored value]       (0 = nothing stored)
 *
 * -DBUG: the coordinator commits as soon as the FIRST worker acks (a
 * sub-quorum of one), so it can commit a value only a minority holds.
 * The quorum invariant -- commit implies a majority stored it -- catches
 * it; cmc prints the shortest schedule.
 *
 *   cc -no-pie -o quorum test/proc/quorum.c
 *   cmc --model-file test/proc/quorum.model --binary ./quorum
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000
#define NWORKER 3
#define MAJ 2					/* a majority of NWORKER */

static int id;
static unsigned char committed;	/* coordinator: committed value, 0 = none */
static unsigned char stored;	/* worker: stored value, 0 = none */

static void
emit(void)
{
	unsigned char rec[2] = {1, (unsigned char) (id == 0 ? committed : stored)};

	(void) !write(193, rec, sizeof rec);	/* durable */
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

static int
read_full(int fd, unsigned char *m, int n)
{
	int got = 0;

	while (got < n)
	{
		ssize_t r = read(fd, m + got, (size_t) (n - got));

		if (r <= 0)
			return -1;
		got += (int) r;
	}
	return 0;
}

/* one request/reply on a fresh connection to node `to` */
static int
rpc(int to, const unsigned char req[3], unsigned char rep[3])
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + to);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		return -1;
	(void) !write(s, req, 3);
	int r = read_full(s, rep, 3);

	close(s);
	return r;
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	committed = 0;
	stored = 0;
	emit();

	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);

	for (;;)
	{
		int c = accept(ls, NULL, NULL);

		if (c < 0)
			break;
		unsigned char m[3], r[3] = {0, 0, 0};
		ssize_t n = read(c, m, 3);

		if (n == 2 && m[0] == 'W' && id == 0)	/* client write to coordinator */
		{
			unsigned char v = m[1];
			int acks = 0;

			for (int w = 1; w <= NWORKER; w++)
			{
				unsigned char req[3] = {'S', v, 0}, rep[3];

				if (rpc(w, req, rep) == 0 && rep[0] == 'a')
				{
					acks++;
#ifdef BUG
					/* commit on the FIRST ack -- a sub-quorum of one */
					if (acks >= 1)
					{
						committed = v;
						emit();
						break;
					}
#endif
				}
			}
#ifndef BUG
			/* commit only once a majority of workers has stored v */
			if (acks >= MAJ)
			{
				committed = v;
				emit();
			}
#endif
			unsigned char ack[2] = {'A', committed};

			(void) !write(c, ack, 2);
			close(c);
			continue;
		}
		if (n == 3 && m[0] == 'S' && id != 0)	/* worker: store and ack */
		{
			stored = m[1];
			emit();
			r[0] = 'a';
			(void) !write(c, r, 3);
			close(c);
			continue;
		}
		close(c);
	}
	return 0;
}
