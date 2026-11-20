/*
 * syscorner.c: the corners of the system calls the process engine
 * answers itself (plan 2.1, coverage).  Node 0 runs the mode named by its
 * [target] arg and records each call's outcome, so a gate row can compare
 * the checker's answers with Linux's; the modes that pass a limit or a
 * bad argument the model refuses must end the check instead.
 *
 * Each recorded outcome is a small code: 1 the call succeeded (or, for a
 * read, returned the expected byte), otherwise 10 + errno modulo 200, so
 * EFAULT (14) is 24, EAGAIN (11) is 21, EEXIST (17) 27, ENOENT (2) 12,
 * EISCONN (106) 116.  Run natively (no checker), modes that use only
 * kernel objects give the same codes; proc_gate checks both.
 *
 * Canonical state fd 3: [phase][codes...], phase 1 once done.
 *
 *   cc -O1 -no-pie -o syscorner test/proc/syscorner.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BASE 20000
#define BAD ((void *) 16)		/* an address no node maps */

static unsigned char rec[64];
static int	nrec = 1;

static void
code(long r)
{
	if (nrec < (int) sizeof rec)
		rec[nrec++] = (unsigned char) (r >= 0 ? 1 : 10 + errno % 200);
}

static void
emit(int phase)
{
	unsigned char out[65];

	rec[0] = (unsigned char) phase;
	out[0] = (unsigned char) nrec;
	memcpy(out + 1, rec, (size_t) nrec);
	(void) !write(3, out, (size_t) nrec + 1);
}

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	/* natively, away from 20000, where another check on the same host
	 * may be listening; under cmc node i is 20000 + i */
	a.sin_port = htons((unsigned short) (port + (getenv("CMC_STATE_FD") ? 0 : 9300)));
	return a;
}

static int
listener(int port)
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(port);

	bind(s, (struct sockaddr *) &a, sizeof a);
	listen(s, 8);
	return s;
}

/* bad buffers: each call the engine answers that writes into or reads
 * from the node's memory gets EFAULT for an address it cannot reach */
static void
badbuf(void)
{
	int			e = eventfd(0, EFD_NONBLOCK);
	int			ep = epoll_create1(0);
	struct sockaddr_in a = addr(BASE + 1);
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	code(syscall(SYS_clock_gettime, CLOCK_REALTIME, BAD));
	code(syscall(SYS_gettimeofday, BAD, NULL));
	code(syscall(SYS_getrandom, BAD, 8, 0));
	code(syscall(SYS_write, e, BAD, 8));
	code(syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, e, BAD));
	code(syscall(SYS_poll, BAD, 1, 0));
	code(syscall(SYS_connect, s, BAD, sizeof a));
	code(syscall(SYS_futex, BAD, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0));
	{
		struct timespec *ts = BAD;

		code(syscall(SYS_ppoll, NULL, 0, ts, NULL, 8));
	}
	code(syscall(SYS_gettimeofday, NULL, NULL));
}

/* eventfd: counter, nonblocking read of zero, a write that is not 8
 * bytes, a read of fewer than 8 */
static void
eventfds(void)
{
	int			e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	uint64_t	v = 3, got = 0;

	code(read(e, &got, 8));		/* EAGAIN */
	code(write(e, &v, 8));
	code(read(e, &got, 8) == 8 && got == 3 ? 0 : -1);
	code(fcntl(e, F_GETFL) & O_NONBLOCK ? 0 : -1);
	code(fcntl(e, F_GETFD) & FD_CLOEXEC ? 0 : -1);
	code(fcntl(e, F_SETFD, 0));
	code(fcntl(e, F_GETFD) == 0 ? 0 : -1);
	code(fcntl(e, F_SETFL, 0));
	code(fcntl(e, F_GETFL) & O_NONBLOCK ? -1 : 0);
	code(close(e));
}

/* epoll_ctl: ADD twice, MOD, DEL, MOD and DEL of an fd it does not hold,
 * and an epoll_wait that returns the eventfd ready */
static void
epolls(void)
{
	int			ep = epoll_create1(EPOLL_CLOEXEC);
	int			e = eventfd(1, 0);
	struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 7}, out[2];

	code(epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev));
	errno = 0;
	code(epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev));	/* EEXIST */
	ev.events = EPOLLIN | EPOLLOUT;
	code(epoll_ctl(ep, EPOLL_CTL_MOD, e, &ev));
	code(epoll_wait(ep, out, 2, 0) == 1 && out[0].data.u64 == 7 ? 0 : -1);
	code(epoll_ctl(ep, EPOLL_CTL_DEL, e, NULL));
	code(epoll_ctl(ep, EPOLL_CTL_MOD, e, &ev));	/* ENOENT */
	code(epoll_ctl(ep, EPOLL_CTL_DEL, e, NULL));	/* ENOENT */
	code(epoll_wait(ep, out, 2, 0) == 0 ? 0 : -1);
	code(epoll_pwait(ep, out, 2, 0, NULL) == 0 ? 0 : -1);
	/* two ready, room for one; and the older epoll_create */
	int			ep2 = epoll_create(4);
	int			r1 = eventfd(1, 0), r2 = eventfd(1, 0);

	ev.events = EPOLLIN;
	epoll_ctl(ep2, EPOLL_CTL_ADD, r1, &ev);
	epoll_ctl(ep2, EPOLL_CTL_ADD, r2, &ev);
	code(epoll_wait(ep2, out, 1, 0) == 1 ? 0 : -1);
	code(fcntl(ep2, F_GETFD) == 0 ? 0 : -1);
}

/* poll and ppoll with zero and no timeout on a ready eventfd and a
 * listener; POLLOUT on a socket is always ready */
static void
polls(void)
{
	int			e = eventfd(2, 0);
	int			ls = listener(BASE);
	struct pollfd p[2] = {{e, POLLIN, 0}, {ls, POLLIN | POLLOUT, 0}};
	struct timespec zero = {0, 0};

	code(poll(p, 2, 0) == 2 && (p[0].revents & POLLIN) && (p[1].revents & POLLOUT) ? 0 : -1);
	code(ppoll(p, 2, &zero, NULL) == 2 ? 0 : -1);
	code(ppoll(p, 2, NULL, NULL) == 2 ? 0 : -1);
	p[0].events = 0;
	p[1].events = POLLIN;
	code(poll(p, 2, 0) == 0 ? 0 : -1);
	code(fcntl(ls, F_GETFD) == 0 ? 0 : -1);
	code(fcntl(ls, F_SETFD, FD_CLOEXEC));
	code(fcntl(ls, F_GETFD) == FD_CLOEXEC ? 0 : -1);
}

/* the clock, gettimeofday and getrandom past its 256-byte answer */
static void
clocks(void)
{
	struct timespec ts;
	struct timeval tv;
	unsigned char big[300];

	code(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts));
	code(syscall(SYS_gettimeofday, &tv, NULL));
	{
		/* past 256 bytes Linux may return a short count, so 256 and 300
		 * are both its answer */
		long		r = syscall(SYS_getrandom, big, sizeof big, 0);

		code(r == 256 || r == (long) sizeof big ? 0 : -1);
	}
	code(syscall(SYS_sched_yield));
	code(fcntl(0, F_GETFD) >= 0 ? 0 : -1);
}

/* futex: a wait whose value has changed is EAGAIN; a wake of nobody is 0 */
static void
futexes(void)
{
	static uint32_t word = 5;

	code(syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 4, NULL, NULL, 0));	/* EAGAIN */
	code(syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) == 0 ? 0 : -1);
	code(syscall(SYS_futex, &word, FUTEX_WAKE_BITSET_PRIVATE, 1, NULL, NULL, FUTEX_BITSET_MATCH_ANY) == 0 ? 0 : -1);
}

/* connect to a node that is not there, twice to the same peer, and to a
 * port outside the cluster */
static void
connects(void)
{
	struct sockaddr_in a = addr(BASE + 1), far = addr(BASE + 99);
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	code(connect(s, (struct sockaddr *) &a, sizeof a));
	code(connect(s, (struct sockaddr *) &a, sizeof a));	/* EISCONN */
	int			t = socket(AF_INET, SOCK_STREAM, 0);

	code(connect(t, (struct sockaddr *) &far, sizeof far));
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	const char *mode = argc > 2 ? argv[2] : "";

	emit(0);
	if (id != 0)
	{
		/* node 1: listen, accept one connection, then park in accept */
		int			ls = listener(BASE + id);

		for (;;)
			if (accept(ls, NULL, NULL) < 0)
				break;
		return 0;
	}
	if (!strcmp(mode, "badbuf")) badbuf();
	else if (!strcmp(mode, "eventfd")) eventfds();
	else if (!strcmp(mode, "epoll")) epolls();
	else if (!strcmp(mode, "poll")) polls();
	else if (!strcmp(mode, "clock")) clocks();
	else if (!strcmp(mode, "futex")) futexes();
	else if (!strcmp(mode, "connect")) connects();
	else if (!strcmp(mode, "efdwrite4"))
	{
		int			e = eventfd(0, 0);
		uint32_t	v = 1;

		code(write(e, &v, 4));	/* not 8 bytes: the model ends the check */
	}
	else if (!strcmp(mode, "efdread4"))
	{
		int			e = eventfd(1, 0);
		uint32_t	v;

		code(read(e, &v, 4));
	}
	else if (!strcmp(mode, "efdflags"))
		code(eventfd(0, EFD_SEMAPHORE << 1));
	else if (!strcmp(mode, "manyefd"))
		for (int i = 0; i < 40; i++)
			code(eventfd(0, 0));
	else if (!strcmp(mode, "manyepoll"))
		for (int i = 0; i < 40; i++)
			code(epoll_create1(0));
	else if (!strcmp(mode, "epollmax"))
	{
		int			ep = epoll_create1(0);
		struct epoll_event out;

		code(epoll_wait(ep, &out, 0, 0));
	}
	else if (!strcmp(mode, "epollnotep"))
	{
		int			e = eventfd(0, 0);
		struct epoll_event ev = {.events = EPOLLIN};

		code(epoll_ctl(e, EPOLL_CTL_ADD, e, &ev));
	}
	else if (!strcmp(mode, "epollwaitnotep"))
	{
		int			e = eventfd(0, 0);
		struct epoll_event out;

		code(epoll_wait(e, &out, 1, 0));
	}
	else if (!strcmp(mode, "manypoll"))
	{
		struct pollfd p[80];

		memset(p, 0, sizeof p);
		code(poll(p, 80, 0));
	}
	else if (!strcmp(mode, "bigwrite"))
	{
		struct sockaddr_in a = addr(BASE + 1);
		int			s = socket(AF_INET, SOCK_STREAM, 0);
		static char buf[4096];

		connect(s, (struct sockaddr *) &a, sizeof a);
		code(write(s, buf, sizeof buf));
	}
	else if (!strcmp(mode, "nbepoll"))
	{
		/* an epoll fd made non-blocking leaves the listener blocking */
		int			ls = listener(BASE);
		int			ep = epoll_create1(0);

		code(fcntl(ep, F_SETFL, O_NONBLOCK));
		code(fcntl(ep, F_GETFL) & O_NONBLOCK ? 0 : -1);
		code(fcntl(ls, F_GETFL) & O_NONBLOCK ? -1 : 0);
		code(fcntl(ep, F_SETFD, FD_CLOEXEC));
		code(fcntl(ep, F_GETFD) == FD_CLOEXEC ? 0 : -1);
		code(fcntl(ls, F_GETFD) == 0 ? 0 : -1);
	}
	else if (!strcmp(mode, "wrongfd"))
	{
		/* socket calls on the wrong kind of fd: Linux refuses each one
		 * (ENOTSOCK on an eventfd or an epoll instance, EINVAL or EISCONN
		 * on a socket in the wrong state, EBADF on a closed fd) */
		int			ls = listener(BASE);
		int			e = eventfd(0, 0), ep = epoll_create1(0);
		int			plain = socket(AF_INET, SOCK_STREAM, 0);
		int			gone = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);
		int			one = 1;

		close(gone);
		code(listen(ls, 8));
		code(connect(e, (struct sockaddr *) &a, sizeof a));
		code(connect(ep, (struct sockaddr *) &a, sizeof a));
		code(connect(ls, (struct sockaddr *) &a, sizeof a));
		code(connect(gone, (struct sockaddr *) &a, sizeof a));
		code(listen(e, 4));
		code(bind(e, (struct sockaddr *) &a, sizeof a));
		code(setsockopt(ep, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one));
		code(accept(e, NULL, NULL));
		code(accept(plain, NULL, NULL));
		code(recvfrom(e, &one, 1, 0, NULL, NULL));
		code(sendto(ep, &one, 1, 0, NULL, 0));
		code(send(plain, "x", 1, MSG_NOSIGNAL));
		code(recv(plain, &one, 1, MSG_DONTWAIT));
		/* listen and bind on a connection: one to the node's own
		 * listener, which is there natively as under cmc */
		int			c = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in me = addr(BASE);

		code(connect(c, (struct sockaddr *) &me, sizeof me));
		code(listen(c, 4));
		code(bind(c, (struct sockaddr *) &me, sizeof me));
	}
	else if (!strcmp(mode, "notconn"))
	{
		/* reads and writes on fds that are no connection: a plain socket
		 * (ENOTCONN; EPIPE, with SIGPIPE ignored here), an epoll instance
		 * (EINVAL), a closed socket (EBADF) */
		int			s = socket(AF_INET, SOCK_STREAM, 0);
		int			ep = epoll_create1(0);
		int			gone = socket(AF_INET, SOCK_STREAM, 0);
		char		c = 'x';

		signal(SIGPIPE, SIG_IGN);
		close(gone);
		code(write(s, &c, 1));
		code(read(s, &c, 1));
		code(read(ep, &c, 1));
		code(write(ep, &c, 1));
		code(read(gone, &c, 1));
		code(write(gone, &c, 1));
	}
	else if (!strcmp(mode, "robuf"))
	{
		/* answers written into read-only memory: Linux gives EFAULT for
		 * each, after the call is made (the poll array and epoll's
		 * events array are read, or the eventfd is ready, first) */
		long		pg = sysconf(_SC_PAGESIZE);
		unsigned char *ro = mmap(NULL, (size_t) pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		int			e = eventfd(1, 0);
		int			ep = epoll_create1(0);
		struct epoll_event ev = {.events = EPOLLIN};
		struct pollfd *p = (struct pollfd *) ro;

		epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev);
		p->fd = e;
		p->events = POLLIN;
		p->revents = 0;
		mprotect(ro, (size_t) pg, PROT_READ);
		code(syscall(SYS_poll, p, 1, 0));
		code(syscall(SYS_epoll_wait, ep, ro + 64, 1, 0));
		code(syscall(SYS_clock_gettime, CLOCK_REALTIME, ro + 128));
		code(syscall(SYS_getrandom, ro + 256, 8, 0));
	}
	else if (!strcmp(mode, "robufefd"))
	{
		/* an eventfd read into read-only memory: Linux consumes the
		 * counter and returns EFAULT; the model refuses it */
		long		pg = sysconf(_SC_PAGESIZE);
		unsigned char *ro = mmap(NULL, (size_t) pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		int			e = eventfd(1, 0);

		code(syscall(SYS_read, e, ro, 8));
	}
	else if (!strcmp(mode, "selfdial"))
	{
		int			ls = listener(BASE);
		struct sockaddr_in me = addr(BASE);
		int			c = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

		code(connect(c, (struct sockaddr *) &me, sizeof me));
		code(fcntl(c, F_GETFD) == FD_CLOEXEC ? 0 : -1);
		int			s = accept4(ls, NULL, NULL, SOCK_CLOEXEC);

		code(s);
		code(fcntl(s, F_GETFD) == FD_CLOEXEC ? 0 : -1);
		code(write(c, "z", 1));
		char		ch = 0;

		code(read(s, &ch, 1) == 1 && ch == 'z' ? 0 : -1);
	}
	else if (!strcmp(mode, "pollclosed"))
	{
		/* poll of a closed fd: POLLNVAL at once, as on Linux */
		int			s = socket(AF_INET, SOCK_STREAM, 0);
		struct pollfd p;

		close(s);
		p.fd = s;
		p.events = POLLIN;
		p.revents = 0;
		code(poll(&p, 1, -1) == 1 && p.revents == POLLNVAL ? 0 : -1);
	}
	else if (!strcmp(mode, "fileops"))
	{
		/* the file calls on an owned socket, as Linux answers them, and a
		 * read-only open through open() and openat2() */
		int			ls = listener(BASE);
		struct sockaddr_in me = addr(BASE);
		int			c = socket(AF_INET, SOCK_STREAM, 0);
		struct
		{
			uint64_t	flags, mode, resolve;
		}			how = {O_RDONLY, 0, 0};

		connect(c, (struct sockaddr *) &me, sizeof me);
		(void) ls;
		code(lseek(c, 0, SEEK_SET));	/* ESPIPE */
		code(lseek(c, 0, 99));		/* EINVAL */
		code(fsync(c));				/* EINVAL */
		code(ftruncate(c, 0));		/* EINVAL */
		code(syscall(SYS_sync_file_range, c, 0, 0, 0));	/* ESPIPE */
		int			r = (int) syscall(SYS_open, "/dev/null", O_RDONLY);

		code(r);
		code(r >= 0 ? close(r) : -1);
		r = (int) syscall(SYS_openat2, AT_FDCWD, "/dev/null", &how, sizeof how);
		code(r);
	}
	else if (!strcmp(mode, "durread"))
	{
		/* the durable record read a byte at a time: a fresh launch has
		 * none, so the first read is EOF, as a restart's would not be */
		unsigned char c;

		code(read(193, &c, 1) == 0 ? 0 : -1);
		code(fsync(193));
		code(lseek(193, 0, SEEK_CUR) < 0 ? 0 : -1);
	}
	else if (!strcmp(mode, "ipv6"))
		code(socket(AF_INET6, SOCK_STREAM, 0));
	else if (!strcmp(mode, "connectlow"))
	{
		struct sockaddr_in low = addr(BASE - 1);
		int			s = socket(AF_INET, SOCK_STREAM, 0);

		code(connect(s, (struct sockaddr *) &low, sizeof low));
	}
	else if (!strcmp(mode, "sendtoaddr"))
	{
		struct sockaddr_in a = addr(BASE + 1);
		int			s = socket(AF_INET, SOCK_STREAM, 0);

		connect(s, (struct sockaddr *) &a, sizeof a);
		code(sendto(s, "x", 1, 0, (struct sockaddr *) &a, sizeof a));
	}
	else if (!strcmp(mode, "fcntldup"))
	{
		int			e = eventfd(0, 0);

		code(fcntl(e, F_DUPFD, 0));
	}
	else if (!strcmp(mode, "epollop"))
	{
		int			ep = epoll_create1(0), e = eventfd(0, 0);
		struct epoll_event ev = {.events = EPOLLIN};

		code(epoll_ctl(ep, 99, e, &ev));
	}
	else if (!strcmp(mode, "futexrequeue"))
	{
		static uint32_t w1, w2;

		code(syscall(SYS_futex, &w1, FUTEX_REQUEUE_PRIVATE, 1, 1, &w2, 0));
	}
	else if (!strcmp(mode, "notconnpipe"))
	{
		/* the same write with SIGPIPE at its default: the node dies */
		int			s = socket(AF_INET, SOCK_STREAM, 0);
		char		c = 'x';

		code(write(s, &c, 1));
	}
	emit(1);
	return 0;
}
