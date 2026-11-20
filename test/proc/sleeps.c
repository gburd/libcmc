/*
 * sleeps: every call with a finite timeout the process engine models
 * (test/proc, 2.2).
 *
 * Node 0 makes one call, named by argv[2], with a finite timeout and
 * nothing that could end it early, then records what the call returned
 * and how far its clock moved across it.  The checker parks the call
 * as a deadline on node 0's clock and fires it (A_EXPIRE), so the call
 * returns as it would on expiry and the clock reads exactly the timeout
 * later.  Run natively, the record is the same apart from the clock,
 * which moves at least the timeout.
 *
 * Canonical state on fd 3: [count=4][done][result][errno][ms moved],
 * with the result + 1 (so a -1 records as 0).
 *
 *   cc -O1 -no-pie -o sleeps test/proc/sleeps.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define WAIT_MS 7

static long long
now_ms(int clk)
{
	struct timespec ts;
	clock_gettime(clk, &ts);
	return (long long) ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void
emit(int done, long r, int err, long long moved)
{
	unsigned char rec[5] = {4, (unsigned char) done, (unsigned char) (r + 1), (unsigned char) err, (unsigned char) moved};
	(void) !write(3, rec, sizeof rec);
}

/* make the call named by mode; returns its result, errno in *err */
static long
call(const char *mode, int *err)
{
	struct timespec ts = {0, WAIT_MS * 1000000L};
	long r = -2;
	errno = 0;
	if (!strcmp(mode, "poll"))
	{
		/* a listener nobody dials: readiness never comes, the time does */
		struct sockaddr_in a = {0};
		a.sin_family = AF_INET; a.sin_port = htons(20000); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 8);
		struct pollfd p = {ls, POLLIN, 0};
		r = poll(&p, 1, WAIT_MS);
		if (r == 0 && p.revents != 0) r = -3;
	}
	else if (!strcmp(mode, "ppoll")) r = ppoll(NULL, 0, &ts, NULL);
	else if (!strcmp(mode, "epoll"))
	{
		struct epoll_event ev;
		r = epoll_wait(epoll_create1(0), &ev, 1, WAIT_MS);
	}
	else if (!strcmp(mode, "select"))
	{
		struct timeval tv = {0, WAIT_MS * 1000L};
		r = select(0, NULL, NULL, NULL, &tv);
		if (r == 0 && (tv.tv_sec != 0 || tv.tv_usec != 0)) r = -3;	/* Linux counts it down */
	}
	else if (!strcmp(mode, "futex") || !strcmp(mode, "futexabs"))
	{
		/* a timed futex wait nobody wakes: ETIMEDOUT, recorded as 0 */
		static uint32_t w;
		struct timespec at;
		clock_gettime(CLOCK_MONOTONIC, &at);
		at.tv_nsec += WAIT_MS * 1000000L;
		if (at.tv_nsec >= 1000000000L) { at.tv_sec++; at.tv_nsec -= 1000000000L; }
		if (!strcmp(mode, "futex")) r = syscall(SYS_futex, &w, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
		else r = syscall(SYS_futex, &w, FUTEX_WAIT_BITSET_PRIVATE, 0, &at, NULL, FUTEX_BITSET_MATCH_ANY);
		r = (r == -1 && errno == ETIMEDOUT) ? 0 : -3;
	}
	else if (!strcmp(mode, "condtimed"))
	{
		/* pthread_cond_timedwait waits on CLOCK_REALTIME, absolute */
		static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
		static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
		struct timespec at;
		clock_gettime(CLOCK_REALTIME, &at);
		at.tv_nsec += WAIT_MS * 1000000L;
		if (at.tv_nsec >= 1000000000L) { at.tv_sec++; at.tv_nsec -= 1000000000L; }
		pthread_mutex_lock(&m);
		r = pthread_cond_timedwait(&cv, &m, &at) == ETIMEDOUT ? 0 : -3;
		pthread_mutex_unlock(&m);
	}
	else if (!strcmp(mode, "selectfd"))
	{
		/* select on a descriptor: refused, poll is the modelled call */
		struct timeval tv = {0, WAIT_MS * 1000L};
		fd_set rd;
		FD_ZERO(&rd); FD_SET(0, &rd);
		r = select(1, &rd, NULL, NULL, &tv);
	}
	else if (!strcmp(mode, "setitimer"))
	{
		struct itimerval it = {{0, 0}, {0, WAIT_MS * 1000L}};
		r = setitimer(ITIMER_REAL, &it, NULL);
	}
	else if (!strcmp(mode, "alarm")) r = alarm(1);
	else if (!strcmp(mode, "timer_create"))
	{
		timer_t t;
		r = syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, &t);
	}
	else if (!strcmp(mode, "timerfd")) r = syscall(SYS_timerfd_create, CLOCK_MONOTONIC, 0);
	else if (!strcmp(mode, "selectforever")) r = select(0, NULL, NULL, NULL, NULL);
	else if (!strcmp(mode, "clockcpu"))
		r = -clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID, 0, &ts, NULL);
	else if (!strcmp(mode, "pselect")) r = pselect(0, NULL, NULL, NULL, &ts, NULL);
	else if (!strcmp(mode, "nanosleep")) r = nanosleep(&ts, NULL);
	else if (!strcmp(mode, "clock_nanosleep")) r = -clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
	else if (!strcmp(mode, "abstime"))
	{
		struct timespec at;
		clock_gettime(CLOCK_REALTIME, &at);
		at.tv_nsec += WAIT_MS * 1000000L;
		if (at.tv_nsec >= 1000000000L) { at.tv_sec++; at.tv_nsec -= 1000000000L; }
		r = -clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &at, NULL);
	}
	*err = r < 0 ? errno : 0;
	return r;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";
	int err;
	emit(0, 0, 0, 0);
	long long t0 = now_ms(CLOCK_MONOTONIC);
	long r = call(mode, &err);
	emit(1, r, err, now_ms(CLOCK_MONOTONIC) - t0);
	return 0;
}
