/*
 * tcpsem.c: TCP close and reset semantics, two nodes, one scenario per
 * mode (the [target] arg).  Node 0 records what its calls returned as a
 * code, so test/proc_gate.sh can compare cmc's answer with Linux's: it
 * runs the same binary natively on loopback and requires the same code,
 * then requires cmc to reach exactly that code.
 *
 * A code is a decimal digit string packed in one byte, each digit one
 * call's result: 1 = success, 2 = ECONNRESET, 3 = EPIPE, 4 = 0 (EOF),
 * 5 = ENOTCONN, 6 = ECONNREFUSED, 7 = the byte the peer sent first,
 * 8 = ETIMEDOUT.
 *
 * Under cmc each scenario is driven by its model in test/proc_gate.sh
 * (timers, and for redial a crash and a restart); natively node 0 forks
 * node 1 and the two order their calls over pipes (start_native()).
 *
 * Modes:
 *   finwrite  node 1 closes its end cleanly (a FIN); node 0 then writes
 *             twice and reads twice.  Linux: 1, EPIPE, 0, 0.  SIGPIPE is
 *             ignored so node 0 survives to record it.
 *   rstdata   node 1 sends 'x', then closes with node 0's byte unread (an
 *             RST).  Linux: node 0 reads 'x', then ECONNRESET, then 0.
 *   backlog   node 1 listens and exits without accepting; node 0's dial
 *             waits in its backlog.  Linux: ECONNRESET, then 0.
 *   redial    node 0 dials node 1 while it is down (refused), reads,
 *             then, once node 1 is back and listening, dials again on the
 *             same socket.  Linux: ECONNREFUSED, ENOTCONN, then success.
 *             Natively node 1's socket is bound and not yet listening;
 *             under cmc it crashes, a host that answers nothing, so the
 *             first dial times out instead (ETIMEDOUT).
 *
 * Node 0's code is two bytes, the high digits then the low two.
 *
 *   cc -O1 -no-pie -o tcpsem test/proc/tcpsem.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define BASE 20000

static int	code;
static int	native;			/* run natively, not under cmc */
static int	lsock[2];		/* natively, each node's listening socket */
static unsigned short nport[2]; /* natively, their ports */
static int	sig_in = -1, sig_out = -1;	/* natively, the pipes to the peer */

static struct sockaddr_in
addr(int id)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	/* under cmc node i is 20000 + i; natively see start_native() */
	a.sin_port = native ? nport[id] : htons((unsigned short) (BASE + id));
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

/* append one call's result r (as from read or write) to the code */
static void
note(ssize_t r, int byte)
{
	int			d = r > 0 ? (byte ? 7 : 1) : r == 0 ? 4 :
		errno == ECONNRESET ? 2 : errno == EPIPE ? 3 :
		errno == ENOTCONN ? 5 : errno == ECONNREFUSED ? 6 : errno == ETIMEDOUT ? 8 : 9;

	code = code * 10 + d;
}

static void
emit(void)
{
	unsigned char rec[3] = {2, (unsigned char) (code / 100), (unsigned char) (code % 100)};

	(void) !write(3, rec, sizeof rec);
}

static int
listener(int id)
{
	int			s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = addr(id);

	if (native)
		return lsock[id];
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	bind(s, (struct sockaddr *) &a, sizeof a);
	listen(s, 8);
	return s;
}

/* Wait until the peer has done its part.  Under cmc the peer says so by
 * dialling this node's listener (done()), so no schedule runs ahead of
 * it; natively it writes a byte on a pipe. */
static void
settle(int ls)
{
	unsigned char t;

	if (native)
	{
		(void) !read(sig_in, &t, 1);
		return;
	}
	for (;;)
	{
		/* the peer's signal carries no bytes; a fired timer's connection
		 * carries a 'T' and is not the signal */
		int			c = accept(ls, NULL, NULL);
		int			timer = read(c, &t, 1) == 1;

		close(c);
		if (!timer)
			return;
	}
}

/* tell node `to` this node's part is done (see settle): dial it and
 * close at once, so its read sees the end of a stream with no bytes */
static void
done(int to)
{
	struct sockaddr_in a = addr(to);
	int			s;

	if (native)
	{
		(void) !write(sig_out, "d", 1);
		return;
	}
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(s, (struct sockaddr *) &a, sizeof a) == 0)
		close(s);
}

/* natively, node 0 at exit: let node 1 see the end of its pipe, then
 * reap it, so the record is final once node 0 has exited */
static void
reap(void)
{
	close(sig_out);
	while (wait(NULL) > 0)
		;
}

/* Natively node 0 runs the whole scenario: it makes both nodes'
 * listening sockets on ports the kernel picks, so two checks on one host
 * never meet, and forks node 1, which returns 1 from here.  Pipes stand
 * where cmc's done() dials stand, so no pause orders the two processes.
 * In redial node 1's socket is bound and does not listen until node 0
 * has been refused: a host whose process is not running. */
static int
start_native(const char *mode)
{
	int			to1[2], to0[2];

	native = 1;
	for (int i = 0; i < 2; i++)
	{
		struct sockaddr_in a;
		socklen_t	al = sizeof a;
		int			one = 1;

		memset(&a, 0, sizeof a);
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		lsock[i] = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(lsock[i], SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		if (bind(lsock[i], (struct sockaddr *) &a, sizeof a) != 0)
			exit(2);
		if (i == 0 || strcmp(mode, "redial") != 0)
			listen(lsock[i], 8);
		getsockname(lsock[i], (struct sockaddr *) &a, &al);
		nport[i] = a.sin_port;
	}
	if (pipe(to1) != 0 || pipe(to0) != 0)
		exit(2);
	if (fork() == 0)
	{
		close(3);				/* the record is node 0's */
		close(lsock[0]);
		close(to1[1]);
		close(to0[0]);
		sig_in = to1[0];
		sig_out = to0[1];
		return 1;
	}
	close(lsock[1]);
	close(to1[0]);
	close(to0[1]);
	sig_in = to0[0];
	sig_out = to1[1];
	atexit(reap);
	return 0;
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	const char *mode = argc > 2 ? argv[2] : "";

	if (!getenv("CMC_STATE_FD"))
		id = start_native(mode);

	struct sockaddr_in peer = addr(1 - id);
	unsigned char b;
	int			ls = listener(id);

	signal(SIGPIPE, SIG_IGN);
	emit();
	if (!strcmp(mode, "redial"))
	{
		/* Node 1 listens and waits; node 0 dials it only once it is down,
		 * so the dial is refused, then dials again on the same socket once
		 * it is back.  Under cmc node 1 goes down by a CRASH and comes back
		 * by a RESTART, and node 0 waits for each on a timer; natively node
		 * 1 listens only once node 0 says it was refused.  A
		 * first dial that reached node 1 before the crash is a different
		 * run, which node 0 marks with code 9. */
		if (id == 1)
		{
			if (native)
			{
				settle(ls);		/* node 0 has been refused */
				listen(ls, 8);
			}
			done(0);			/* the restarted node announces itself */
			close(accept(ls, NULL, NULL));
			return 0;
		}
		int			s = socket(AF_INET, SOCK_STREAM, 0);

		if (!native)
			settle(ls);			/* node 1 is down */
		int			r = connect(s, (struct sockaddr *) &peer, sizeof peer);

		if (r == 0)
		{
			code = 9;
			emit();
			return 0;
		}
		note(r, 0);
		note(read(s, &b, 1), 0);
		if (native)
			done(1);			/* node 1 may come back */
		settle(ls);				/* node 1 is back and listening */
		note(connect(s, (struct sockaddr *) &peer, sizeof peer) == 0 ? 1 : -1, 0);
		emit();
		return 0;
	}
	if (!strcmp(mode, "backlog"))
	{
		/* Node 1 never calls accept(), so node 0's dial stays in its
		 * backlog until node 1 exits.  Under cmc node 1 exits when a
		 * timer fires on a read of its own (an outbound connection node 0
		 * never accepts); natively once node 0's dial is in the backlog. */
		if (id == 1)
		{
			if (!native)
			{
				int			s = socket(AF_INET, SOCK_STREAM, 0);

				if (connect(s, (struct sockaddr *) &peer, sizeof peer) == 0)
					(void) !read(s, &b, 1);	/* only a timer answers this */
			}
			else
			{
				struct pollfd q = {ls, POLLIN, 0};

				settle(ls);		/* node 0's dial has returned */
				(void) !poll(&q, 1, -1);	/* and it is in the backlog */
			}
			return 0;
		}
		int			s = socket(AF_INET, SOCK_STREAM, 0);

		if (connect(s, (struct sockaddr *) &peer, sizeof peer) != 0)
			return 3;
		if (native)
			done(1);
		note(read(s, &b, 1), 0);
		note(read(s, &b, 1), 0);
		emit();
		return 0;
	}
	if (id == 1)
	{
		int			c = accept(ls, NULL, NULL);

		if (!strcmp(mode, "rstdata"))
		{
			(void) !write(c, "x", 1);
			settle(ls);			/* node 0's byte has been sent, unread here */
		}
		close(c);				/* finwrite: a FIN; rstdata: an RST */
		done(0);
		settle(ls);				/* node 0 is finished */
		return 0;
	}
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	if (connect(s, (struct sockaddr *) &peer, sizeof peer) != 0)
		return 3;
	if (!strcmp(mode, "finwrite"))
	{
		settle(ls);				/* node 1 has closed */
		note(write(s, "a", 1), 0);
		if (native)
		{
			/* node 1's stack answers the write with an RST, and on a
			 * loaded host it may not be in yet; only it sets POLLHUP
			 * (the FIN alone does not), and the scenario is the write
			 * after it */
			struct pollfd q = {s, 0, 0};

			while (poll(&q, 1, -1) == 1 && !(q.revents & POLLHUP))
				;
		}
		note(write(s, "a", 1), 0);
		note(read(s, &b, 1), 0);
		note(read(s, &b, 1), 0);
	}
	else if (!strcmp(mode, "rstdata"))
	{
		(void) !write(s, "y", 1);
		done(1);
		settle(ls);				/* node 1 has sent 'x' and closed */
		ssize_t		r = read(s, &b, 1);

		note(r, r == 1 && b == 'x');
		note(read(s, &b, 1), 0);
		note(read(s, &b, 1), 0);
	}
	emit();
	done(1);
	return 0;
}
