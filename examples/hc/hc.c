/*
 * hc.c: Humming Consensus / chain replication, checked as it ships
 * (examples/hc).  The protocol shape libcmc was built for: a replicated
 * log kept by a chain of nodes under an epoch-fenced projection, where
 * the safety property is that an acknowledged write is never lost, even
 * across a crash or a reconfiguration.
 *
 * Reference: chain replication is van Renesse & Schneider, "Chain
 * Replication for Supporting High Throughput and Availability", OSDI
 * 2004 (https://www.cs.cornell.edu/home/rvr/papers/OSDI04.pdf); Humming
 * Consensus is the Machi chain-management protocol by Scott Lystig
 * Fritchie.  This is a faithful miniature of its commit path.
 *
 * Three nodes over ordinary blocking TCP, no libcmc header; node i
 * listens on 127.0.0.1 port 20000 + i.  The chain for epoch 1 is
 * head 0 -> tail 1; node 2 is a spare.  A reconfiguration (driven by a
 * timeout at the spare) moves to epoch 2 with chain head 0 -> tail 2,
 * inheriting the committed value, so a failed tail does not lose data.
 *
 * Commit rule (chain replication): a client write is committed -- and
 * only then acknowledged -- once the TAIL has stored it.  The head
 * forwards the write to the tail and waits for the tail's ack before
 * acking the client; the write is durable at the tail (the commit point)
 * and then at the head.
 *
 * Messages, request/reply on a fresh connection (3 bytes):
 *   WRITE 'W' v _      client -> head
 *   REPL  'R' v epoch  head -> tail (the tail stores and acks)
 *   PULL  'P' _ _      new tail -> head: report the committed value
 *   ADOPT 'E' epoch _  new tail -> head: adopt the new projection
 * Reply byte 'a' = stored/committed, 'A' v = client ack.
 *
 * Canonical state fd 3 (volatile) and fd 193 (durable/committed):
 *   [count=2][epoch][value]   value 0 = none.  A crash keeps only the
 * durable record.
 *
 * -DBUG: the head acks the client as soon as the write is in its memory,
 * before the tail has committed it.  If the head then crashes before the
 * tail stored it, the acknowledged write is on no node's durable state
 * -- a lost ack.  cmc finds it.
 *
 *   cc -no-pie -o hc examples/hc/hc.c
 *   cmc --model-file examples/hc/hc.model --binary ./hc --crashes 1
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static int		id;
static unsigned char epoch = 1;
static unsigned char value;		/* committed value, 0 = none */

static void
emit_committed(void)
{
	unsigned char rec[3] = {2, epoch, value};

	(void) !write(193, rec, sizeof rec);	/* durable: survives a crash */
	(void) !write(3, rec, sizeof rec);
}

static void
emit_volatile(unsigned char v)
{
	unsigned char rec[3] = {2, epoch, v};

	(void) !write(3, rec, sizeof rec);		/* in memory only, not committed */
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
	int			got = 0;

	while (got < n)
	{
		ssize_t		r = read(fd, m + got, (size_t) (n - got));

		if (r <= 0)
			return -1;
		got += (int) r;
	}
	return 0;
}

static int
rpc(int to, const unsigned char req[3], unsigned char rep[3])
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + to);

	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
		return -1;
	(void) !write(s, req, 3);
	int			r = read_full(s, rep, 3);

	close(s);
	return r;
}

static int
tail_of(unsigned char e)
{
	return e >= 2 ? 2 : 1;
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	value = 0;
	emit_committed();

	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + id);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);

	for (;;)
	{
		int			c = accept(ls, NULL, NULL);

		if (c < 0)
			break;
		unsigned char m[3], r[3] = {'a', 0, 0};
		ssize_t		n = read(c, m, 3);

		if (n == 1 && m[0] == 'T')		/* timeout: the spare reconfigures */
		{
			close(c);
			if (id == 2 && epoch < 2)
			{
				unsigned char pull[3] = {'P', 0, 0}, rep[3];

				if (rpc(0, pull, rep) == 0)
				{
					epoch = 2;
					value = rep[1];			/* inherit the committed value */
					emit_committed();
					unsigned char adopt[3] = {'E', 2, 0}, r2[3];

					(void) !rpc(0, adopt, r2);
				}
			}
			continue;
		}
		if (n == 2 && m[0] == 'W')		/* client write to the head (2 bytes) */
		{
			unsigned char v = m[1];
			int			t = tail_of(epoch);
			unsigned char repl[3] = {'R', v, epoch}, rep[3];
			unsigned char ack[2] = {'A', v};

#ifdef BUG
			/* ack the client before the tail has committed: a crash of
			 * the head now loses an acknowledged write */
			emit_volatile(v);
			(void) !write(c, ack, 2);
			close(c);
			if (rpc(t, repl, rep) == 0 && rep[0] == 'a')
			{
				value = v;				/* commits late, too late if crashed */
				emit_committed();
			}
#else
			/* chain replication: commit at the tail first, then ack */
			if (rpc(t, repl, rep) == 0 && rep[0] == 'a')
			{
				value = v;				/* now committed on the chain */
				emit_committed();
				(void) !write(c, ack, 2);
			}
#endif
			close(c);
			continue;
		}
		if (n != 3)
		{
			close(c);
			continue;
		}

		if (m[0] == 'P')				/* reconfig pull: report committed value */
		{
			r[1] = value;
			(void) !write(c, r, 3);
		}
		else if (m[0] == 'E')			/* adopt a new projection */
		{
			if (m[1] > epoch)
			{
				epoch = m[1];
				emit_committed();
			}
		}
		else if (m[0] == 'R')			/* replicate: the tail commits */
		{
			if (m[2] >= epoch)
			{
				epoch = m[2];
				value = m[1];
				emit_committed();		/* the tail's store is the commit */
			}
			r[0] = 'a';
		}
		(void) !write(c, r, 3);
		close(c);
	}
	return 0;
}
