/*
 * idle probe for process-mode M7 (experiments/process-mode).
 *
 * An engine probe, not a protocol: it checks that the checker does not
 * merge away the passage of time.  The server reads the clock at start,
 * then waits for one client request.  An idle timeout may fire while it
 * waits; the server ignores it and keeps waiting, so its canonical state
 * does not change -- but the logical clock has advanced.  A request
 * served after that is served late.
 *
 * The model's invariant (`never_late`) is deliberately false: time CAN
 * pass while the server idles, so the right verdict is a violation on
 * the schedule "timeout, then deliver the request".  A checker that
 * leaves the environment budgets out of its fingerprint merges the
 * post-timeout world into the initial one and wrongly reports a pass.
 *
 * Canonical state on fd 3: [count=1][s], s = 0 waiting, 1 served on
 * time, 2 served late.
 *
 *   cc -O1 -no-pie -o idle demo/idle.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BASE 20000

static long long
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void
emit(int s)
{
	unsigned char rec[2] = {1, (unsigned char) s};
	(void) !write(3, rec, sizeof rec);
}

int
main(void)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = {0};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(BASE + 0);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(ls, (struct sockaddr *) &sa, sizeof sa);
	listen(ls, 8);
	int c = accept(ls, 0, 0);

	long long start = now_ns();
	emit(0);
	for (;;)
	{
		unsigned char m[8];
		if (read(c, m, sizeof m) < 1)
			break;
		if (m[0] == 'T')
			continue;					/* idle timeout: keep waiting */
		emit(now_ns() > start ? 2 : 1);	/* the request, on time or late */
		break;
	}
	close(c);
	close(ls);
	return 0;
}
