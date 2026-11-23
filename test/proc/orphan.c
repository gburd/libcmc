/*
 * test/proc/orphan.c: a one-node server whose accept loop retries a
 * failed accept() forever, as many shipping servers do.  It is the
 * fixture for a counterexample's exit path: cmc must kill every node it
 * launched before it exits.  A node it forgets loses its checker (its
 * accept fails) and spins here, an orphan that outlives cmc.
 *
 * Canonical state on fd 3: [count=1][c]; a timeout sets c to 1.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static void
emit(unsigned char c)
{
	unsigned char rec[2] = {1, c};

	(void) !write(3, rec, sizeof rec);
}

int
main(void)
{
	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(20000);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 4);
	emit(0);
	for (;;)
	{
		int			cfd = accept(ls, NULL, NULL);

		if (cfd >= 0)
		{
			close(cfd);
			emit(1);
		}
	}
}
