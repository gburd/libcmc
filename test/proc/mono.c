/*
 * test/proc/mono.c: a one-node monotonic counter, for the action-level
 * refinement test (Milestone C2).  Each timeout the checker fires makes
 * the node advance its counter and re-emit its state.  The abstract
 * model test/proc/mono.cml says the counter may only step up by ONE;
 * this implementation does exactly that, so it refines the model both at
 * the state level (C1) and at the step level (C2).
 *
 * Built -DBUG, the node steps up by TWO (0 -> 2 -> ... ).  Every state
 * it reaches (0, 2) is still in range, so the state-level invariant
 * holds throughout and a C1 check PASSES it; only the C2 action-level
 * check catches the forbidden two-step.
 *
 * Canonical state on fd 3: [count=1][c]  -- byte 0 is the counter.
 *
 *   cc -no-pie -o mono test/proc/mono.c
 *   cmc --model-file test/proc/mono.model --binary ./mono \
 *       --refine test/proc/mono.cml --bind test/proc/mono.bind --refine-steps
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000
#define M 3

static unsigned char c;

static void
emit(void)
{
	unsigned char rec[2] = {1, c};

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

int
main(void)
{
	c = 0;
	emit();

	/* listen so the engine has a quiescent point and a timer to fire */
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE);

	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 4);

	for (;;)
	{
		/* block in accept; a fired timeout wakes it (as it does a read) */
		int cfd = accept(ls, NULL, NULL);

		if (cfd >= 0)
			close(cfd);
#ifdef BUG
		/* forbidden: step up by two */
		if (c + 2 <= M) c = (unsigned char) (c + 2);
		else break;
#else
		if (c < M) c = (unsigned char) (c + 1);
		else break;
#endif
		emit();
	}
	return 0;
}
