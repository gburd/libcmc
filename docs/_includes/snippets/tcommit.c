/*
 * tcommit.c: a two-phase commit the guide checks against Lamport's
 * TCommit spec (docs/guide/04-from-tla.md).  Written as it ships: TCP,
 * blocking calls, no libcmc header.  Node 0 is the transaction manager;
 * nodes 1 .. 3 are the resource managers.  The manager asks each
 * resource manager in turn to prepare, then tells each in turn to commit
 * if all voted yes, else to abort, and waits for it to apply that.  A resource manager that times
 * out while still working aborts on its own.
 *
 *   cc -no-pie -o tcommit tcommit.c
 *   cmc --model-file tcommit.model --refine tcommit.cml --bind tcommit.bind
 *
 * Built with -DTC_BUG, the manager keeps only the last vote it read, so
 * a "no" from an early resource manager is forgotten.  test/docs_gate.sh
 * builds both and checks what the guide says about them.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 20000				/* node i listens on PORT + i */
#define NRM 3					/* resource managers 1 .. NRM */

/* resource manager states, as TCommit's rmState */
enum
{
	WORKING, PREPARED, COMMITTED, ABORTED
};

/* !region hook */
/* The state hook: a length byte and the state, on fd 3. */
static void
report(unsigned char st)
{
	unsigned char rec[2] = {1, st};

	(void) !write(3, rec, sizeof rec);
}
/* !endregion hook */

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a = {0};

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	return a;
}

/* !region rm */
/* A resource manager: answer the manager's requests until it hangs up. */
static void
resource_manager(int id)
{
	struct sockaddr_in a = addr(PORT + id);
	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	unsigned char st = WORKING, m;

	if (bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 4) != 0)
		exit(2);
	report(st);
	for (;;)
	{
		int			c = accept(ls, NULL, NULL);

		while (read(c, &m, 1) == 1)
		{
			if (m == 'T' && st == WORKING)
				st = ABORTED;	/* timed out before it was asked */
			else if (m == 'P' && st == WORKING)
				st = PREPARED;
			else if (m == 'C' && st == PREPARED)
				st = COMMITTED;
			else if (m == 'A' && st != COMMITTED)
				st = ABORTED;
			report(st);
			if (m == 'P')
			{
				unsigned char vote = st == PREPARED ? 'Y' : 'N';

				(void) !write(c, &vote, 1);
			}
			else if (m == 'C' || m == 'A')
				(void) !write(c, "K", 1);	/* the decision is applied */
		}
		close(c);
	}
}
/* !endregion rm */

/* !region tm */
/* The transaction manager: collect the votes, then send the decision. */
static void
transaction_manager(void)
{
	int			c[NRM + 1];
	int			commit = 1;
	unsigned char m;

	report(0);
	for (int r = 1; r <= NRM; r++)
	{
		struct sockaddr_in a = addr(PORT + r);

		c[r] = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(c[r], (struct sockaddr *) &a, sizeof a) != 0)
			exit(3);
		m = 'P';
		(void) !write(c[r], &m, 1);
		int			yes = read(c[r], &m, 1) == 1 && m == 'Y';

#ifdef TC_BUG
		commit = yes;			/* the last vote overwrites the others */
#else
		commit = commit && yes;
#endif
	}
	report(commit ? 1 : 2);
	m = commit ? 'C' : 'A';
	for (int r = 1; r <= NRM; r++)
	{
		unsigned char k;

		(void) !write(c[r], &m, 1);
		(void) !read(c[r], &k, 1);	/* wait until it is applied */
		close(c[r]);
	}
}
/* !endregion tm */

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;

	if (id == 0)
		transaction_manager();
	else
		resource_manager(id);
	return 0;
}
