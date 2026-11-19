/*
 * test/proc/durack.c: a worker makes a value durable, then acknowledges
 * it (soundness plan 3.8).  One node, two threads, and a client.
 *
 * The main thread accepts the client's connection, reads its WRITE(v),
 * starts a worker and joins it.  The worker writes v to the durable
 * record (fd 193) and then sends the client its ack.  A crash between
 * the worker's two calls leaves v durable and the client unacknowledged,
 * a state Linux reaches whenever the host dies at that point; a recovery
 * protocol has to handle it.
 *
 * A checker that carried out a state-hook write only when the search
 * next ran the thread, and the thread's following call at once, would
 * do the two as one step, so no crash could come between them.
 *
 * Canonical state: fd 3 holds [0, 0] (two bytes) while the node runs;
 * fd 193 holds [v] (one byte), which is all a crashed node reports.
 *
 * With -DSINGLE the main thread makes the two calls itself: a crash can
 * fall between any two calls of a single-threaded node too, not only
 * between its steps (crash_point() in src/cmc_proc.c).
 *
 *   cc -O1 -no-pie -o durack test/proc/durack.c -lpthread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static int	conn;
static unsigned char val;

static void *
worker(void *arg)
{
	unsigned char rec[2] = {1, val};
	unsigned char ack[2] = {'A', val};

	(void) arg;
	(void) !write(193, rec, sizeof rec);	/* durable first */
	(void) !write(conn, ack, sizeof ack);	/* then the ack */
	return NULL;
}

int
main(void)
{
	unsigned char rec[3] = {2, 0, 0};
	unsigned char req[2];
	struct sockaddr_in a;
	pthread_t	t;
	int			ls = socket(AF_INET, SOCK_STREAM, 0);

	(void) !write(3, rec, sizeof rec);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(BASE + 0);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 8);
	conn = accept(ls, NULL, NULL);
	if (read(conn, req, sizeof req) != 2 || req[0] != 'W')
		return 1;
	val = req[1];
#ifdef SINGLE
	(void) t;
	worker(NULL);
#else
	pthread_create(&t, NULL, worker, NULL);
	pthread_join(t, NULL);
#endif
	return 0;
}
