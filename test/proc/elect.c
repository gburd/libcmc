/*
 * test/proc/elect.c: a two-node leader-election sketch, written as it ships
 * (test/proc/DESIGN.md, M5).  Ordinary blocking sockets,
 * no libcmc header.  It exercises TIME as an environment action: a
 * follower waiting on recv is satisfied EITHER by a heartbeat from the
 * leader OR by an election TIMEOUT the checker chooses to fire.
 *
 * Node 0 is the initial leader (term 1); node 1 is the follower.  The
 * leader sends one heartbeat to the follower.  The follower blocks on
 * recv; the checker delivers either:
 *   - the heartbeat 'H' <term>: the follower acks and stays a follower;
 *   - a timeout   'T'        : the follower's election timer fired, so it
 *                              promotes itself to leader.
 *
 * Declared state (fd 3): [count=2][role][term], role 1 = leader.
 *
 * Correct: on a timeout the follower becomes leader at term+1 (strictly
 * greater than the old leader's term), so the two are never leaders at
 * the SAME term.  Built -DBUG the follower promotes WITHOUT bumping the
 * term, so node 0 (leader, term 1) and node 1 (leader, term 1) are both
 * leaders at term 1 -- split brain.  The invariant "no two leaders share
 * a term" catches it.
 *
 *   cc -O1 -no-pie -o elect       test/proc/elect.c
 *   cc -O1 -no-pie -DBUG -o elect-bug test/proc/elect.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char role;		/* 0 follower, 1 leader */
static unsigned char term;

static void
emit(void)
{
	unsigned char rec[3] = {2, role, term};	/* [count=2][role][term] */
	(void) !write(3, rec, 3);
}

static int
listen_on(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0); int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0}; a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short) port);
	if (bind(s, (void *) &a, sizeof a) != 0) _exit(2);
	listen(s, 4); return s;
}
static int
connect_to(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0}; a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short) port);
	if (connect(s, (void *) &a, sizeof a) != 0) _exit(3);
	return s;
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;

	if (id == 0)
	{
		role = 1; term = 1; emit();			/* the initial leader */
		int bk = connect_to(BASE + 1);
		unsigned char hb[2] = {'H', term};
		write(bk, hb, 2);					/* one heartbeat to the follower */
		return 0;
	}

	/* follower */
	role = 0; term = 1; emit();
	int ls = listen_on(BASE + 1);
	int cs = accept(ls, 0, 0);
	unsigned char m[2];
	ssize_t n = read(cs, m, 2);
	if (n <= 0) return 0;
	if (m[0] == 'H')
	{
		/* heartbeat: acknowledge the leader, remain a follower */
		term = m[1];
		emit();
	}
	else if (m[0] == 'T')
	{
		/* election timeout: promote to leader */
		role = 1;
#ifdef BUG
		/* forgets to advance the term -> same term as the old leader */
#else
		term = term + 1;
#endif
		emit();
	}
	return 0;
}
