/*
 * test/proc/twothread.c: a two-threaded server node, the example for
 * per-thread scheduling (docs/plans/threads.md, Tier 3 ask 8).  It is
 * written the way a real runtime splits work across threads: a main loop
 * thread that owns the socket, and a worker thread that does the work and
 * reports the result.  The two hand a job over a mutex + condition
 * variable (a futex underneath).
 *
 * Before libcmc modelled threads the checker could not run this node: the
 * worker parks in futex making no owned syscall, so drive_quiescent hit
 * LULL_MS and died ("blocked in a call the checker does not model"); and
 * even reaching that, the single deferred slot could not hold both the
 * main thread's accept/recv and the worker's futex wait.
 *
 * Two nodes.  Node 1 is the server.  Its main thread listens, accepts one
 * connection, and reads one request byte; it then hands that byte to the
 * worker thread (job + have_job under the mutex, signalled on the
 * condition variable) and loops back to read again.  The worker waits on
 * the condition variable, takes the job, stores it as the node's state,
 * and emits.  Node 0 is a client: it connects and sends one byte
 * ('x' = 120).  The invariant (twothread.model) is that once the server
 * has stored a byte, it is the byte the client sent -- the data must
 * survive the thread handoff.
 *
 * Canonical state fd 3, node 1: [recvd][stored byte].  recvd is 0 until
 * the worker has stored a byte, so the invariant can tell "nothing
 * processed yet" from "processed and stored".  The worker owns the emit,
 * so a search state is only snapshotted once both threads are parked
 * (main in accept/recv, worker in futex): the per-thread quiescence rule.
 *
 * -DBUG: the worker stores a fixed wrong byte (0) instead of the job it
 * was handed, so the data is dropped across the handoff.  The invariant
 * catches it and cmc prints the shortest schedule.
 *
 *   cc -O1 -no-pie -o twothread     test/proc/twothread.c -lpthread
 *   cc -O1 -no-pie -DBUG -o twothread-bug test/proc/twothread.c -lpthread
 *   cmc --model-file test/proc/twothread.model --binary ./twothread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char stored;	/* the byte the worker stored */
static unsigned char recvd;		/* 1 once the worker has stored a byte */

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cnd = PTHREAD_COND_INITIALIZER;
static unsigned char job;		/* the byte handed to the worker */
static int have_job;			/* 1 when a job waits for the worker */

/* emit the node's canonical state: [count=2][recvd][stored] on fd 3 */
static void
emit(void)
{
	unsigned char rec[3] = {2, recvd, stored};
	(void) !write(3, rec, sizeof rec);
}

/* the worker: wait for a job, store it (or a wrong byte under -DBUG),
 * emit, and loop.  The store is the far side of the thread handoff. */
static void *
worker(void *arg)
{
	(void) arg;
	for (;;)
	{
		pthread_mutex_lock(&mtx);
		while (!have_job)
			pthread_cond_wait(&cnd, &mtx);
		unsigned char v = job;
		have_job = 0;
		pthread_mutex_unlock(&mtx);
#ifdef BUG
		stored = 0;				/* drop the data across the handoff */
#else
		stored = v;
#endif
		recvd = 1;
		emit();
	}
	return NULL;
}

static int
listen_on(int port)
{
	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	int			one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (bind(ls, (void *) &a, sizeof a) != 0)
		_exit(2);
	listen(ls, 4);
	return ls;
}

static int
connect_to(int port)
{
	int			s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (connect(s, (void *) &a, sizeof a) != 0)
		_exit(3);
	return s;
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	emit();						/* initial state: recvd 0, stored 0 */

	if (id == 0)
	{
		/* client: connect to the server and send one request byte */
		int			s = connect_to(BASE + 1);
		unsigned char b = 'x';
		write(s, &b, 1);
		return 0;
	}

	/* server: start the worker, then the main thread reads requests and
	 * hands each byte to the worker over the condition variable */
	pthread_t	t;
	pthread_create(&t, NULL, worker, NULL);

	int			ls = listen_on(BASE + 1);
	int			cs = accept(ls, 0, 0);
	for (;;)
	{
		unsigned char b;
		ssize_t		n = read(cs, &b, 1);
		if (n != 1)
			break;
		pthread_mutex_lock(&mtx);
		job = b;
		have_job = 1;
		pthread_cond_signal(&cnd);
		pthread_mutex_unlock(&mtx);
	}
	return 0;
}
