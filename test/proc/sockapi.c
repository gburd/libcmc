/*
 * sockapi.c: the socket calls a server like PostgreSQL makes past
 * read and write: readv, writev, sendmsg, recvmsg, shutdown, dup, dup2,
 * dup3 and the socket options.  Node 0 dials its own listener, so the
 * same program runs natively on loopback and under cmc, and records
 * each call's outcome in the [target] arg's mode; test/proc_gate.sh
 * requires cmc to reach Linux's record and no other.  Node 1 is idle.
 *
 * A code is 1 for success (or the expected bytes), otherwise 10 + errno
 * modulo 200: EPIPE 42, ENOTCONN 117, EINVAL 32, EAGAIN 21,
 * ECONNRESET 114.  A count is recorded raw where it matters.
 *
 * Modes:
 *   iov    writev and sendmsg of two pieces, read back by readv and
 *          recvmsg, recvmsg's msg_flags and msg_namelen, and the empty
 *          and invalid cases.
 *   shut   SHUT_WR (the peer reads EOF, a write fails EPIPE), SHUT_RD (a
 *          read with nothing queued is 0 at once, queued and later bytes
 *          still arrive), and both shut, where bytes arriving are
 *          answered with an RST.
 *   dup    dup, dup2 and dup3 share the socket and O_NONBLOCK but not
 *          FD_CLOEXEC; closing one number leaves the socket open.
 *   opts   setsockopt and getsockopt of the options the model records,
 *          inheritance by an accepted socket, the length checks, and
 *          SO_ERROR of a reset connection.
 *   errs   the errors: a socket call on an eventfd, an epoll instance or
 *          a closed fd, bad flags, addresses and lengths, a socket not
 *          connected, and an option set through a dup.
 *
 * Canonical state fd 3: [phase][codes...], phase 1 once done.
 *
 *   cc -O1 -no-pie -o sockapi test/proc/sockapi.c
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
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define BASE 20000

#define BAD ((void *) 16)		/* an address no node maps */

static unsigned char rec[64];
static int	port = BASE;		/* the listener's port: BASE under cmc */
static int	nrec = 1;

static void
emit(int phase)
{
	unsigned char out[65];

	rec[0] = (unsigned char) phase;
	out[0] = (unsigned char) nrec;
	memcpy(out + 1, rec, (size_t) nrec);
	(void) !write(3, out, (size_t) nrec + 1);
}

static void
raw(int v)
{
	if (nrec < (int) sizeof rec)
		rec[nrec++] = (unsigned char) v;
	/* the record is the checker's state: report it after every call,
	 * so two points of the program never look alike (cmc(7), the
	 * state hook) */
	if (getenv("CMC_STATE_FD"))
		emit(0);
}

static void
code(long r)
{
	raw(r >= 0 ? 1 : 10 + errno % 200);
}

/* a connection to this node's own listener: *c the dialler, return the
 * accepted end */
static int
pair(int ls, int *c)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	*c = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(*c, (struct sockaddr *) &a, sizeof a) != 0)
		return -1;
	return accept(ls, NULL, NULL);
}

static void
iovs(int c, int s)
{
	char		x[3] = {0}, y[8] = {0};
	struct iovec w[2] = {{"ab", 2}, {"cde", 3}}, r[2] = {{x, 2}, {y, 8}};
	struct msghdr m;
	struct sockaddr_in nm;

	code(writev(c, w, 2));
	code(readv(s, r, 2) == 5 && !memcmp(x, "ab", 2) && !memcmp(y, "cde", 3) ? 0 : -1);
	memset(&m, 0, sizeof m);
	m.msg_iov = w;
	m.msg_iovlen = 2;
	code(sendmsg(c, &m, MSG_NOSIGNAL));
	memset(&m, 0, sizeof m);
	memset(y, 0, sizeof y);
	m.msg_iov = r;
	m.msg_iovlen = 2;
	m.msg_name = &nm;
	m.msg_namelen = sizeof nm;
	m.msg_flags = 0x55;
	code(recvmsg(s, &m, 0) == 5 && !memcmp(y, "cde", 3) ? 0 : -1);
	raw(m.msg_namelen);
	raw(m.msg_flags);
	code(writev(c, w, 0));
	code(readv(s, r, 0));
	code(syscall(SYS_writev, c, w, -1));	/* EINVAL */
	code(syscall(SYS_readv, s, r, 1025));	/* EINVAL */
	code(syscall(SYS_writev, c, (void *) 16, 1));	/* EFAULT */
}

static void
shuts(int c, int s, int ls)
{
	char		b[8];

	code(shutdown(c, 7));		/* EINVAL */
	code(shutdown(c, SHUT_WR));
	code(read(s, b, sizeof b));		/* 0: the FIN */
	raw((int) read(s, b, sizeof b));
	signal(SIGPIPE, SIG_IGN);
	code(write(c, "x", 1));		/* EPIPE */
	code(write(s, "y", 1));
	code(read(c, b, sizeof b) == 1 && b[0] == 'y' ? 0 : -1);
	code(shutdown(c, SHUT_WR));		/* ENOTCONN: both FINs */
	/* SHUT_RD on a fresh pair: nothing queued reads 0 at once, bytes
	 * sent later still arrive */
	int			c2, s2 = pair(ls, &c2);

	code(shutdown(c2, SHUT_RD));
	raw((int) read(c2, b, sizeof b));
	code(write(s2, "z", 1));
	code(read(c2, b, sizeof b) == 1 && b[0] == 'z' ? 0 : -1);
	/* both sides shut: bytes arriving are answered with an RST */
	code(shutdown(c2, SHUT_WR));
	code(read(s2, b, sizeof b));	/* 0: the FIN */
	code(write(s2, "q", 1));
	code(read(c2, b, sizeof b));	/* ECONNRESET */
	code(read(c2, b, sizeof b));	/* 0 */
	code(write(s2, "q", 1));		/* EPIPE */
	code(shutdown(c2, SHUT_RDWR));	/* ENOTCONN */
	int			p = socket(AF_INET, SOCK_STREAM, 0);

	code(shutdown(p, SHUT_WR));		/* ENOTCONN: never dialled */
}

static void
dups(int c, int s)
{
	char		b[8];

	fcntl(c, F_SETFD, FD_CLOEXEC);
	int			d = dup(c);

	code(d);
	code(fcntl(d, F_GETFD) == 0 ? 0 : -1);
	code(fcntl(c, F_SETFL, O_NONBLOCK));
	code(fcntl(d, F_GETFL) & O_NONBLOCK ? 0 : -1);
	code(read(d, b, sizeof b));		/* EAGAIN: shared O_NONBLOCK */
	code(close(c));
	code(write(d, "q", 1));
	code(read(s, b, sizeof b) == 1 && b[0] == 'q' ? 0 : -1);
	code(dup3(d, d, 0));		/* EINVAL */
	code(dup2(d, d) == d ? 0 : -1);
	int			e = dup3(d, d + 40, O_CLOEXEC);

	code(e == d + 40 ? 0 : -1);
	code(fcntl(e, F_GETFD) == FD_CLOEXEC ? 0 : -1);
	code(fcntl(e, F_SETFD, 0));
	code(fcntl(e, F_GETFD) == 0 ? 0 : -1);
	code(fcntl(d, F_GETFD) == 0 ? 0 : -1);
	code(close(d));
	code(write(e, "r", 1));
	code(read(s, b, sizeof b) == 1 && b[0] == 'r' ? 0 : -1);
	code(close(e));
	code(read(s, b, sizeof b));		/* 0: the last number closed */
}

static void
opts(int ls)
{
	int			one = 1, v = 7, c;
	socklen_t	l = sizeof v;
	long long	big = -1;

	code(setsockopt(ls, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one));
	code(setsockopt(ls, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one));
	int			s = pair(ls, &c);

	code(getsockopt(s, IPPROTO_TCP, TCP_NODELAY, &v, &l));
	raw(v);
	code(getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &v, &l));
	raw(v);
	code(getsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, &l));
	raw(v);
	v = 5;
	code(setsockopt(c, SOL_SOCKET, SO_REUSEPORT, &v, sizeof v));
	code(getsockopt(c, SOL_SOCKET, SO_REUSEPORT, &v, &l));
	raw(v);
	code(setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, 2));	/* EINVAL */
	code(setsockopt(c, SOL_SOCKET, SO_SNDBUF, &one, sizeof one));
	l = 2;
	v = 0;
	code(getsockopt(c, SOL_SOCKET, SO_REUSEPORT, &v, &l));
	raw(l);
	raw(v);
	l = sizeof big;
	code(getsockopt(c, SOL_SOCKET, SO_ERROR, &big, &l));
	raw(l);
	raw(big == (long long) 0xffffffff00000000LL);
	/* a reset connection's error, read once by SO_ERROR */
	(void) !write(c, "x", 1);
	close(s);
	l = sizeof v;
	code(getsockopt(c, SOL_SOCKET, SO_ERROR, &v, &l));
	raw(v);
	code(getsockopt(c, SOL_SOCKET, SO_ERROR, &v, &l));
	raw(v);
}

/* the errors of the socket calls: the wrong kind of fd, a closed one,
 * a bad address or length, and a socket in the wrong state */
static void
errs(int c, int s, int ls)
{
	struct sockaddr_in sa;
	socklen_t	l = sizeof sa;
	int			v = 0, neg = -1, efd = eventfd(0, 0), ep = epoll_create1(0);
	int			p = socket(AF_INET, SOCK_STREAM, 0), gone = socket(AF_INET, SOCK_STREAM, 0);

	close(gone);
	code(getsockname(efd, (struct sockaddr *) &sa, &l));	/* ENOTSOCK */
	code(getpeername(ep, (struct sockaddr *) &sa, &l));	/* ENOTSOCK */
	code(shutdown(efd, SHUT_RD));	/* ENOTSOCK */
	code(getsockname(gone, (struct sockaddr *) &sa, &l));	/* EBADF */
	code(shutdown(gone, SHUT_RD));	/* EBADF */
	code(dup(gone));				/* EBADF */
	code(dup3(c, c + 40, 1));		/* EINVAL */
	code(shutdown(c, 99));			/* EINVAL */
	code(getpeername(p, (struct sockaddr *) &sa, &l));	/* ENOTCONN */
	code(shutdown(p, SHUT_RD));		/* ENOTCONN */
	code(getpeername(c, (struct sockaddr *) &sa, (socklen_t *) BAD));	/* EFAULT */
	code(getpeername(c, (struct sockaddr *) &sa, (socklen_t *) &neg));	/* EINVAL */
	l = 4;
	code(getsockname(s, (struct sockaddr *) &sa, &l));
	raw((int) l);
	code(getsockname(c, (struct sockaddr *) &sa, &l));
	code(getpeername(s, (struct sockaddr *) &sa, &l));
	l = sizeof v;
	code(getsockopt(p, SOL_SOCKET, SO_ERROR, &v, &l));
	raw(v);
	code(getsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, (socklen_t *) BAD));	/* EFAULT */
	code(getsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, (socklen_t *) &neg));	/* EINVAL */
	code(setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, BAD, sizeof v));	/* EFAULT */
	{
		/* recvmsg on an eventfd, and readv and recvfrom on a closed fd */
		char		b[4];
		struct iovec iov = {b, sizeof b};
		struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};

		code(recvmsg(efd, &mh, 0));	/* ENOTSOCK */
		code(readv(gone, &iov, 1));	/* EBADF */
		code(recvfrom(gone, b, sizeof b, 0, NULL, NULL));	/* EBADF */
		code(syscall(SYS_readv, c, BAD, 1));	/* EFAULT */
	}
	{
		/* accept4's SOCK_CLOEXEC is the accepted fd's FD_CLOEXEC, and
		 * without it the fd has none */
		struct sockaddr_in me;
		socklen_t	ml = sizeof me;
		int			k = socket(AF_INET, SOCK_STREAM, 0);

		getpeername(c, (struct sockaddr *) &me, &ml);
		code(connect(k, (struct sockaddr *) &me, sizeof me));
		int			x = accept4(ls, NULL, NULL, SOCK_CLOEXEC);

		code(fcntl(x, F_GETFD) == FD_CLOEXEC ? 0 : -1);
		k = socket(AF_INET, SOCK_STREAM, 0);
		code(connect(k, (struct sockaddr *) &me, sizeof me));
		x = accept4(ls, NULL, NULL, 0);
		code(fcntl(x, F_GETFD) == 0 ? 0 : -1);
	}
	int			d = dup(c);

	code(setsockopt(d, SOL_SOCKET, SO_KEEPALIVE, &(int) {1}, sizeof v));
	code(getsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &v, &l));
	raw(v);
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	const char *mode = argc > 2 ? argv[2] : "";
	int			ls = socket(AF_INET, SOCK_STREAM, 0), one = 1, c, s;
	struct sockaddr_in a;

	emit(0);
	if (id != 0)
		return 0;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	/* natively, an ephemeral port, so a run never meets another
	 * program's listener on BASE */
	a.sin_port = getenv("CMC_STATE_FD") ? htons(BASE) : 0;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);
	if (!getenv("CMC_STATE_FD"))
	{
		socklen_t	al = sizeof a;

		getsockname(ls, (struct sockaddr *) &a, &al);
		port = ntohs(a.sin_port);
	}
	if (!strcmp(mode, "opts"))
		opts(ls);
	else if ((s = pair(ls, &c)) < 0)
		raw(99);
	else if (!strcmp(mode, "iov"))
		iovs(c, s);
	else if (!strcmp(mode, "shut"))
		shuts(c, s, ls);
	else if (!strcmp(mode, "dup"))
		dups(c, s);
	else if (!strcmp(mode, "errs"))
		errs(c, s, ls);
	emit(1);
	return 0;
}
