/*
 * nbconnect.c: a client that connects the way libpq does, two nodes.
 * Node 0 is the client and node 1 the server; the mode is the [target]
 * arg.  At the socket level libpq's connectDBStart() and PQconnectPoll()
 * make a socket, set TCP_NODELAY, make it non-blocking and close-on-exec,
 * set SO_KEEPALIVE, and call connect(), which returns EINPROGRESS.  The
 * caller polls for POLLOUT, PQconnectPoll() reads getsockopt(SO_ERROR)
 * and, if it is 0, getsockname(), then sends the startup packet with
 * send(MSG_NOSIGNAL), polls for POLLIN and reads the reply with recv()
 * until EAGAIN.  This file makes exactly those calls.
 *
 * Node 0 records each call's outcome, as test/proc/syscorner.c does: 1
 * the call succeeded, otherwise 10 + errno modulo 200 (EINPROGRESS 125,
 * EAGAIN 21, ECONNREFUSED 121), and a poll's revents as they came.
 * test/proc_gate.sh runs node 0 natively on loopback first (it forks
 * node 1 itself, on a port the kernel picks) and requires cmc to reach
 * that record and no other.
 *
 * Modes:
 *   ok       the server accepts, reads the startup packet and answers.
 *   refused  nothing listens: node 1 exits at once (natively node 0 binds
 *            the port and does not listen on it).  SO_ERROR is ECONNREFUSED, once; a second connect
 *            is ECONNABORTED and the socket is unconnected.
 *   early    the client's calls before the handshake completes: natively
 *            the server's backlog is full (listen(0) and a dial of its
 *            own), so the SYN waits until node 0 has made these calls and
 *            says so; under cmc the handshake waits for the search.  read and send are EAGAIN, connect EALREADY, SO_ERROR
 *            0, getpeername ENOTCONN, then the connection completes.
 *            On loopback the handshake may also complete between any two
 *            of these calls, and under cmc it may; so the gate asks only
 *            that cmc reach Linux's record, which the two check bytes at
 *            the end of the record name.
 *
 * Canonical state fd 3: [phase][codes...][check][check], phase 1 once
 * node 0 is done.
 *
 *   cc -O1 -no-pie -o nbconnect test/proc/nbconnect.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define BASE 20000

static unsigned char rec[62];
static int	nrec = 1;

static void emit(int phase);
static void raw(int v);

static void
code(long r)
{
	raw(r >= 0 ? 1 : 10 + errno % 200);
}

static void
raw(int v)
{
	if (nrec < (int) sizeof rec)
		rec[nrec++] = (unsigned char) v;
	/* under cmc the record so far is the state after each call: the
	 * checker's state is this record, so two runs that differ only in
	 * which call saw the handshake complete must report so */
	if (getenv("CMC_STATE_FD"))
		emit(0);
}

static void
emit(int phase)
{
	unsigned char out[65];

	unsigned	h1 = 0, h2 = 0;
	int			tail = phase ? 2 : 0;

	/* the finished record ends in two check bytes over the codes, so
	 * one invariant can name the whole record (see the early mode) */
	for (int i = 1; i < nrec; i++)
	{
		h1 = (h1 * 31 + rec[i]) % 251;
		h2 = (h2 * 17 + rec[i] + 1) % 241;
	}
	rec[0] = (unsigned char) phase;
	out[0] = (unsigned char) (nrec + tail);
	memcpy(out + 1, rec, (size_t) nrec);
	out[nrec + 1] = (unsigned char) h1;
	out[nrec + 2] = (unsigned char) h2;
	(void) !write(3, out, (size_t) (nrec + 1 + tail));
}

/* node i's address under cmc, 20000 + i */
static struct sockaddr_in
addr(int id)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) (BASE + id));
	return a;
}

/* A socket bound to *a, listening with BACKLOG unless it is negative.
 * Natively *a names port 0, and the port the kernel picked is written
 * back, so a run never meets another check's socket on the same host. */
static int
listener(struct sockaddr_in *a, int backlog)
{
	int			ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	socklen_t	al = sizeof *a;

	/* a socket that never listens is bound alone: no other socket may
	 * share its port and listen there */
	if (backlog >= 0)
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	bind(ls, (struct sockaddr *) a, sizeof *a);
	if (backlog >= 0)
		listen(ls, backlog);
	if (a->sin_port == 0)
		getsockname(ls, (struct sockaddr *) a, &al);
	return ls;
}

/* Node 1 on listener LS: accept the client, read its startup packet and
 * answer one byte, then wait for the client to hang up.  Natively, early
 * first fills the backlog (listen(0)) with a dial of its own and says so
 * on READY; the client's SYN then waits, and only once the client says
 * on GO that its early calls are made does node 1 accept.  Under cmc the
 * search holds the handshake, and READY and GO are -1. */
static void
server(const char *mode, int ls, int ready, int go)
{
	char		b[16];

	if (!strcmp(mode, "early") && ready >= 0)
	{
		struct sockaddr_in a;
		socklen_t	al = sizeof a;
		int			self = socket(AF_INET, SOCK_STREAM, 0);
		struct pollfd q = {ls, POLLIN, 0};

		getsockname(ls, (struct sockaddr *) &a, &al);
		(void) !connect(self, (struct sockaddr *) &a, sizeof a);
		/* the dial is in the accept queue once the listener is readable */
		(void) !poll(&q, 1, -1);
		(void) !write(ready, "r", 1);
		(void) !read(go, b, 1);
		close(accept(ls, NULL, NULL));
	}
	int			c = accept(ls, NULL, NULL);

	if (read(c, b, sizeof b) > 0)
		(void) !write(c, "R", 1);
	while (read(c, b, sizeof b) > 0)
		;
}

/* Natively node 0 runs the whole check: it makes node 1's listener on
 * *peer and forks node 1 to serve it, and returns the fd on which to say
 * GO (-1 if node 1 waits for nothing).  In refused the socket is bound
 * and does not listen, so the dial is refused.  Pipes, not pauses, order
 * the two processes, so the record does not depend on the host's load. */
static int
native_peer(const char *mode, struct sockaddr_in *peer)
{
	int			early = !strcmp(mode, "early");
	int			ready[2], go[2];
	char		b;

	peer->sin_port = 0;
	if (!strcmp(mode, "refused"))
	{
		(void) listener(peer, -1);
		return -1;
	}
	if (pipe(ready) != 0 || pipe(go) != 0)
		exit(2);

	int			ls = listener(peer, early ? 0 : 8);

	if (fork() == 0)
	{
		server(mode, ls, early ? ready[1] : -1, go[0]);
		_exit(0);
	}
	close(ls);
	if (early)
		(void) !read(ready[0], &b, 1);
	return go[1];
}

/* the finished record, once node 1 (natively, a child) has exited */
static int
finish(void)
{
	emit(1);
	if (!getenv("CMC_STATE_FD"))
		while (wait(NULL) > 0)
			;
	return 0;
}

/* SO_ERROR as getsockopt reads it, recorded raw */
static void
soerror(int s)
{
	int			e = -1;
	socklen_t	l = sizeof e;

	code(getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l));
	raw(e);
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	const char *mode = argc > 2 ? argv[2] : "ok";
	struct sockaddr_in peer = addr(1), me;
	socklen_t	ml = sizeof me;
	int			one = 1, go = -1;
	char		b[16];

	emit(0);
	if (id != 0)
	{
		if (strcmp(mode, "refused") != 0)
			server(mode, listener(&peer, 8), -1, -1);
		return 0;
	}
	if (!getenv("CMC_STATE_FD"))
		go = native_peer(mode, &peer);
	signal(SIGPIPE, SIG_IGN);
	/* connectDBStart(): the socket and its options, then connect() */
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	code(s);
	code(setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one));
	code(fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK));
	code(fcntl(s, F_SETFD, FD_CLOEXEC));
	code(setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one));
	code(connect(s, (struct sockaddr *) &peer, sizeof peer));
	if (!strcmp(mode, "early"))
	{
		struct pollfd p = {s, POLLIN | POLLOUT, 0};

		code(read(s, b, sizeof b));
		code(send(s, "x", 1, MSG_NOSIGNAL));
		code(connect(s, (struct sockaddr *) &peer, sizeof peer));
		soerror(s);
		code(getpeername(s, (struct sockaddr *) &me, &ml));
		code(poll(&p, 1, 0));
		raw(p.revents);
		if (go >= 0)
			(void) !write(go, "g", 1);
	}
	/* the caller waits for the socket to be writable */
	struct pollfd p = {s, POLLOUT, 0};

	code(poll(&p, 1, -1));
	raw(p.revents);
	/* PQconnectPoll(), CONNECTION_STARTED */
	int			e = -1;
	socklen_t	l = sizeof e;

	code(getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l));
	raw(e);
	if (e != 0)
	{
		code(connect(s, (struct sockaddr *) &peer, sizeof peer));
		code(read(s, b, 1));
		return finish();
	}
	code(getsockname(s, (struct sockaddr *) &me, &ml));
	raw(ml == sizeof me && me.sin_family == AF_INET && me.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
	/* CONNECTION_MADE: the startup packet, then wait for the reply */
	code(send(s, "startup", 7, MSG_NOSIGNAL));
	p.events = POLLIN;
	code(poll(&p, 1, -1));
	raw(p.revents);
	ssize_t		r = recv(s, b, sizeof b, 0);

	code(r == 1 && b[0] == 'R' ? 0 : -1);
	code(recv(s, b, sizeof b, 0));
	ml = sizeof me;
	code(getpeername(s, (struct sockaddr *) &me, &ml));
	raw(me.sin_port == peer.sin_port);
	code(close(s));
	return finish();
}
