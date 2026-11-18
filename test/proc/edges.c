/*
 * edges.c: paths of the process engine no other fixture reaches (plan
 * 2.1, coverage).  Node 0 runs the mode named by its [target] arg.  Each
 * mode either passes a limit or a malformed argument, which must end the
 * check with the refusal or the error proc_gate names, or exercises a
 * corner whose outcome the node records.  A mode that ends the check
 * never records; a mode that records writes [phase 1][outcome codes]
 * (codes as in syscorner.c: 1 success, else 10 + errno).
 *
 *   cc -O1 -no-pie -o edges test/proc/edges.c -lpthread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>

#define BASE 20000

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
	a.sin_port = htons((unsigned short) port);
	return a;
}

static int
dial(int node)
{
	struct sockaddr_in a = addr(BASE + node);
	int			s = socket(AF_INET, SOCK_STREAM, 0);

	connect(s, (struct sockaddr *) &a, sizeof a);
	return s;
}

/* a buffer whose last `inside` bytes are mapped and the rest are not: a
 * transfer of more than `inside` bytes from it ends partway */
static unsigned char *
edge_buffer(size_t inside)
{
	long		pg = sysconf(_SC_PAGESIZE);
	unsigned char *m = mmap(NULL, (size_t) pg * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	munmap(m + pg, (size_t) pg);
	memset(m, 'e', (size_t) pg);
	return m + pg - inside;
}

static int	pipefd[2];
static const char *wmode;

/* a worker thread's call on a kernel fd, in a node with two threads */
static void *
worker(void *arg)
{
	char		c = 'x';
	struct iovec iov = {&c, 1};

	(void) arg;
	if (!strcmp(wmode, "recvfrom")) (void) recvfrom(pipefd[0], &c, 1, MSG_DONTWAIT, NULL, NULL);
	else if (!strcmp(wmode, "sendto")) (void) sendto(pipefd[1], &c, 1, 0, NULL, 0);
	else if (!strcmp(wmode, "close")) close(pipefd[0]);
	else if (!strcmp(wmode, "fcntl")) (void) fcntl(pipefd[0], F_GETFL);
	else if (!strcmp(wmode, "readv")) (void) readv(pipefd[0], &iov, 1);
	else if (!strcmp(wmode, "writev")) (void) writev(pipefd[1], &iov, 1);
	else if (!strcmp(wmode, "pread")) (void) pread(pipefd[0], &c, 1, 0);
	else if (!strcmp(wmode, "pwrite")) (void) pwrite(pipefd[1], &c, 1, 0);
	else if (!strcmp(wmode, "dup")) (void) dup(pipefd[0]);
	else if (!strcmp(wmode, "ioctl")) (void) ioctl(pipefd[0], FIONREAD, &c);
	else if (!strcmp(wmode, "stdin_tty")) (void) ioctl(0, TCGETS, &c);
	else if (!strcmp(wmode, "stdout")) (void) !write(1, "", 0);
	else if (!strcmp(wmode, "durable")) { (void) fsync(193); (void) !read(193, &c, 0); }
	return NULL;
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	const char *mode = argc > 2 ? argv[2] : "";

	emit(0);
	if (id != 0)
	{
		int			ls = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + id);

		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 8);
		for (;;)
			if (accept(ls, NULL, NULL) < 0)
				break;
		return 0;
	}

	/* transfers that end partway through a buffer: refused (exit 3) */
	if (!strcmp(mode, "partwrite"))
	{
		int			s = dial(1);

		code(write(s, edge_buffer(2), 8));
	}
	else if (!strcmp(mode, "parthook"))
		code(write(3, edge_buffer(1), 3));
	else if (!strcmp(mode, "partrandom"))
		code(getrandom(edge_buffer(2), 8, 0));
	else if (!strcmp(mode, "partefd"))
	{
		int			e = eventfd(1, 0);

		code(read(e, edge_buffer(3), 8));
	}
	else if (!strcmp(mode, "partpoll"))
	{
		int			e = eventfd(1, 0);
		struct pollfd *p = (struct pollfd *) edge_buffer(sizeof(struct pollfd) + 4);

		p[0].fd = e;
		p[0].events = POLLIN;
		code(syscall(SYS_poll, p, 2, 0));
	}
	else if (!strcmp(mode, "bighook"))
	{
		static unsigned char big[5000];

		code(write(3, big, sizeof big));
	}
	else if (!strcmp(mode, "floodhook"))
	{
		/* more state records between two quiescent points than the
		 * checker's pipe holds */
		unsigned char r[2] = {1, 7};

		for (int i = 0; i < 100000; i++)
			(void) !write(3, r, sizeof r);
	}
	/* limits: each ends the check (exit 1) */
	else if (!strcmp(mode, "manyconn"))
		for (int i = 0; i < 20; i++)
			code(dial(1));
	else if (!strcmp(mode, "epollitems"))
	{
		/* 8 eventfds and 9 connections: 17 fds, past an instance's 16 */
		int			ep = epoll_create1(0);

		for (int i = 0; i < 17; i++)
		{
			int			fd = i < 8 ? eventfd(0, 0) : dial(1);
			struct epoll_event ev = {.events = EPOLLIN};

			code(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev));
		}
	}
	else if (!strcmp(mode, "negpoll"))
		code(syscall(SYS_poll, NULL, -1, 0));
	/* calls refused on a checker-owned fd */
	else if (!strcmp(mode, "dup2"))
	{
		int			s = dial(1);

		code(dup2(1, s));
	}
	else if (!strcmp(mode, "dup3"))
	{
		int			s = dial(1);

		code(dup3(1, s, 0));
	}
	else if (!strcmp(mode, "splice"))
	{
		int			p[2], s = dial(1);

		(void) !pipe(p);
		code(splice(p[0], NULL, s, NULL, 1, 0));
	}
	else if (!strcmp(mode, "sendfile"))
	{
		int			s = dial(1);

		code(sendfile(s, 0, NULL, 1));
	}
	/* a call on a kernel fd from a worker thread */
	else if (!strncmp(mode, "worker-", 7))
	{
		pthread_t	t;

		wmode = mode + 7;
		(void) !pipe(pipefd);
		pthread_create(&t, NULL, worker, NULL);
		pthread_join(t, NULL);
	}
	/* signals */
	else if (!strcmp(mode, "segvhandler"))
	{
		signal(SIGSEGV, SIG_IGN);
		raise(SIGSEGV);
	}
	else if (!strcmp(mode, "usr1"))
		raise(SIGUSR1);
	else if (!strcmp(mode, "threads"))
	{
		/* more threads than the checker tracks */
		pthread_t	t[10];

		wmode = "none";
		for (int i = 0; i < 10; i++)
			pthread_create(&t[i], NULL, worker, NULL);
		for (int i = 0; i < 10; i++)
			pthread_join(t[i], NULL);
	}
	else if (!strcmp(mode, "replies"))
	{
		/* more client replies than the reply log holds */
		struct sockaddr_in a = addr(BASE);
		int			ls = socket(AF_INET, SOCK_STREAM, 0);

		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 8);
		int			c = accept(ls, NULL, NULL);

		for (int i = 0; i < 70; i++)
			(void) !write(c, "A1", 2);
	}
	else if (!strcmp(mode, "badsocket"))
		code(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
	else if (!strcmp(mode, "badsendto"))
	{
		int			s = dial(1);

		code(sendto(s, "x", 1, MSG_DONTWAIT, NULL, 0));
	}
	else if (!strcmp(mode, "badrecvfrom"))
	{
		int			s = dial(1);
		char		c;
		struct sockaddr_in from;
		socklen_t	fl = sizeof from;

		code(recvfrom(s, &c, 1, 0, (struct sockaddr *) &from, &fl));
	}
	else if (!strcmp(mode, "badaccept4"))
	{
		int			ls = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE);

		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 8);
		code(accept4(ls, NULL, NULL, SOCK_NONBLOCK));
	}
	/* corners that record */
	else if (!strcmp(mode, "corners2"))
	{
		/* a non-blocking listener with nothing waiting; epoll DEL of the
		 * first of two entries; a dial twice (EISCONN); sendto with
		 * MSG_NOSIGNAL; accept4 with SOCK_CLOEXEC is flagged on the fd;
		 * a read of the durable record */
		int			ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_in me = addr(BASE);

		bind(ls, (struct sockaddr *) &me, sizeof me);
		listen(ls, 8);
		code(fcntl(ls, F_GETFD) == FD_CLOEXEC ? 0 : -1);
		code(fcntl(ls, F_SETFL, O_NONBLOCK));
		errno = 0;
		code(accept(ls, NULL, NULL));	/* EAGAIN */
		code(fcntl(ls, F_SETFL, 0));
		int			ep = epoll_create1(EPOLL_CLOEXEC);
		int			e1 = eventfd(1, 0), e2 = eventfd(0, 0);
		struct epoll_event ev = {.events = EPOLLIN}, out[2];

		code(fcntl(ep, F_GETFD) == FD_CLOEXEC ? 0 : -1);
		code(epoll_ctl(ep, EPOLL_CTL_ADD, e1, &ev));
		code(epoll_ctl(ep, EPOLL_CTL_ADD, e2, &ev));
		code(epoll_ctl(ep, EPOLL_CTL_DEL, e1, NULL));
		code(epoll_wait(ep, out, 2, 0) == 0 ? 0 : -1);
		int			s = dial(1);
		struct sockaddr_in a = addr(BASE + 1);

		errno = 0;
		code(connect(s, (struct sockaddr *) &a, sizeof a));	/* EISCONN */
		code(sendto(s, "q", 1, MSG_NOSIGNAL, NULL, 0));
		char		d[4];

		code(read(193, d, sizeof d) == 0 ? 0 : -1);
		struct pollfd pp[2] = {{-1, POLLIN, 0}, {e2, POLLIN, 0}};

		code(poll(pp, 2, 0) == 0 ? 0 : -1);
	}
	else if (!strcmp(mode, "corners"))
	{
		int			s = dial(1);
		int			e = eventfd(0, EFD_NONBLOCK);
		int			ep = epoll_create1(0);
		uint64_t	v;
		struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT, .data.u64 = 9}, out[4];

		code(fcntl(s, F_SETFD, FD_CLOEXEC));
		code(fcntl(s, F_GETFD) == FD_CLOEXEC ? 0 : -1);
		errno = 0;
		code(read(e, &v, 8));	/* EAGAIN */
		code(epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev));
		code(epoll_wait(ep, out, 4, 0) == 1 && out[0].events == EPOLLOUT && out[0].data.u64 == 9 ? 0 : -1);
		code(send(s, "", 0, MSG_NOSIGNAL));
		code(close(socket(AF_INET, SOCK_STREAM, 0)));
		struct pollfd p = {s, POLLOUT, 0};

		code(poll(&p, 1, -1) == 1 && p.revents == POLLOUT ? 0 : -1);
	}
	emit(1);
	return 0;
}
