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
 * test/proc_gate.sh runs both nodes natively on loopback first and
 * requires cmc to reach that record and no other.
 *
 * Modes:
 *   ok       the server accepts, reads the startup packet and answers.
 *   refused  nothing listens: node 1 exits at once (natively it is never
 *            started).  SO_ERROR is ECONNREFUSED, once; a second connect
 *            is ECONNABORTED and the socket is unconnected.
 *   early    the client's calls before the handshake completes: natively
 *            the server's backlog is full (listen(0) and a dial of its
 *            own), so the SYN waits; under cmc the handshake waits for the
 *            search.  read and send are EAGAIN, connect EALREADY, SO_ERROR
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

/* Node 1.  ok and early: accept the client, read its startup packet and
 * answer one byte, then wait for the client to hang up.  Natively, early
 * first fills the backlog with a dial of its own and accepts the client
 * only after a pause, so the client's handshake is still out when it
 * makes its early calls; under cmc the search holds the handshake. */
static int
server(const char *mode)
{
	int			ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = addr(1);
	int			native = !getenv("CMC_STATE_FD");
	char		b[16];

	if (!strcmp(mode, "refused"))
		return 0;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, !strcmp(mode, "early") && native ? 0 : 8);
	if (!strcmp(mode, "early") && native)
	{
		int			self = socket(AF_INET, SOCK_STREAM, 0);

		(void) !connect(self, (struct sockaddr *) &a, sizeof a);
		usleep(300000);
		close(accept(ls, NULL, NULL));
	}
	int			c = accept(ls, NULL, NULL);

	if (read(c, b, sizeof b) > 0)
		(void) !write(c, "R", 1);
	while (read(c, b, sizeof b) > 0)
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
	int			one = 1;
	char		b[16];

	emit(0);
	if (id != 0)
		return server(mode);
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
		emit(1);
		return 0;
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
	raw(me.sin_port == htons(BASE + 1));
	code(close(s));
	emit(1);
	return 0;
}
