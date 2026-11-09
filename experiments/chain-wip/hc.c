/*
 * hc.c: Humming Consensus / chain replication, checked as it ships
 * (examples/hc).  This is the protocol shape libcmc was built for: a
 * replicated log kept by a chain of nodes under an epoch-fenced
 * "projection", where the safety property is that an acknowledged write
 * is never lost across a reconfiguration.
 *
 * Three nodes, each a process over ordinary blocking TCP, no libcmc
 * header.  node i listens on 127.0.0.1 port 20000 + i.
 *
 * A projection is (epoch, head, tail): the chain for that epoch runs
 * head -> ... -> tail.  Here the chain is two nodes, head and tail; the
 * third is a spare that a reconfiguration can promote.  Node 0 acts as
 * the sequencer that issues projections (epoch 1 = [0,1], and after a
 * reconfiguration epoch 2 = [0,2], replacing the failed tail 1 with the
 * spare 2).  A node adopts a projection by message; it serves writes
 * only in its current epoch.
 *
 * A client write goes to the head, which forwards down the chain; the
 * tail, having stored it, acks back up, and the head acks the client.
 * The acked write must then be held by every member of the current
 * projection and of every later one -- no lost ack (HC's S3).
 *
 * The checker owns the message order, the reconfiguration, and crashes,
 * so it explores the dangerous interleavings: a write in flight when the
 * projection changes, a tail that crashes after storing but before
 * acking, a stale node serving an old epoch.
 *
 * Messages (3 bytes): request/reply on a fresh connection.
 *   WRITE  'W' v  _        client -> head
 *   REPL   'R' v  epoch    head   -> tail   (replicate down the chain)
 *   newep  'E' epoch head  sequencer -> node (adopt projection)
 * Replies: 'a' = stored/acked.
 *
 * Durable state (fd 193, survives a crash): epoch and the stored value.
 * Volatile (fd 3) is the same layout.  Canonical state:
 *   [count=2][epoch][value]   (value 0 = none)
 *
 * -DBUG: the head acks the client as soon as it has stored locally,
 * before the tail confirms -- so a reconfiguration that drops the old
 * tail loses an acknowledged write.
 *
 *   cc -no-pie -o hc examples/hc/hc.c
 *   cmc --model-file examples/hc/hc.model --binary ./hc
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
static unsigned char epoch;		/* current projection epoch (persistent) */
static unsigned char value;		/* stored value, 0 = none (persistent) */

#ifdef BUG
static void
emit_vol(void)
{
	unsigned char rec[3] = {2, epoch, value};

	(void) !write(3, rec, sizeof rec);		/* volatile only: not yet committed */
}
#endif

static void
emit_dur(void)
{
	unsigned char rec[3] = {2, epoch, value};

	(void) !write(193, rec, sizeof rec);	/* durable: a committed value */
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

/* one request/reply RPC on a fresh connection to node `to` */
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

/* the tail of epoch e, given head h: the two-node chain is h then the
 * other member; epoch 1 chain is [0,1], epoch 2 chain is [0,2] */
static int
tail_of(unsigned char e)
{
	return e >= 2 ? 2 : 1;
}

int
main(int argc, char **argv)
{
	id = argc > 1 ? atoi(argv[1]) : 0;
	epoch = 1;
	value = 0;
	emit_dur();

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

		if (n == 1 && m[0] == 'T')	/* timeout (1 byte): the spare reconfigures */
		{
			close(c);
			if (id == 2 && epoch < 2)
			{
				/* new projection epoch 2 = [head 0, tail 2].  HC requires
				 * the new chain to inherit the committed state, so the
				 * new tail first pulls the head's value, then both adopt
				 * the new epoch. */
				unsigned char pull[3] = {'P', 0, 2}, rep[3];

				if (rpc(0, pull, rep) == 0)
				{
					epoch = 2;
					value = rep[1];		/* inherit the head's value */
					emit_dur();
					unsigned char adopt[3] = {'E', 2, 0}, r2[3];

					(void) !rpc(0, adopt, r2);	/* head adopts epoch 2 */
				}
			}
			continue;
		}
		if (n != 3)
		{
			close(c);
			continue;
		}

		if (m[0] == 'P')			/* a reconfig pull: report my COMMITTED value */
		{
			r[0] = 'a';
			r[1] = value;			/* value is only ever the committed one */
			(void) !write(c, r, 3);
			close(c);
			continue;
		}
		if (m[0] == 'E')			/* adopt a new projection (epoch, head) */
		{
			if (m[1] > epoch)
			{
				epoch = m[1];
				emit_dur();
			}
		}
		else if (m[0] == 'R')		/* replicate: the tail commits the value */
		{
			if (m[2] >= epoch)		/* only in a current-or-newer epoch */
			{
				epoch = m[2];
				value = m[1];
				emit_dur();			/* the tail's store IS the commit */
			}
			r[0] = 'a';
		}
		else if (m[0] == 'W')		/* client write to the head */
		{
			unsigned char v = m[1];
			int			t = tail_of(epoch);
			unsigned char inflight = v;	/* held, not yet committed */

			(void) inflight;
#ifdef BUG
			/* ack the client after storing only VOLATILE at the head,
			 * before the tail commits: a crash of the head in the window
			 * before the tail stores loses this acknowledged write -- its
			 * value is durable nowhere */
			value = v;
			emit_vol();
			r[0] = 'A';
			r[1] = v;
			(void) !write(c, r, 3);
			close(c);
			unsigned char rep[3], repl[3] = {'R', v, epoch};

			if (rpc(t, repl, rep) == 0)
				emit_dur();			/* commits late, too late if crashed */
			continue;
#else
			/* chain replication: the write is committed only once the
			 * tail has stored it.  The head holds it volatile meanwhile. */
			unsigned char rep[3], repl[3] = {'R', v, epoch};

			if (rpc(t, repl, rep) == 0 && rep[0] == 'a')
			{
				value = v;			/* commit at the head after the tail */
				emit_dur();
				r[0] = 'A';
				r[1] = v;
			}
			else
				r[0] = 'n';
#endif
		}
		(void) !write(c, r, 3);
		close(c);
	}
	return 0;
}
