/*
 * test/proc/twothread_join.c: a two-threaded server whose worker does one
 * job and exits, and whose main thread joins it.  The companion to
 * twothread.c (docs/plans/threads.md, Tier 3 ask 8); it exercises the
 * thread-exit and pthread_join path that the long-lived worker in
 * twothread.c does not.
 *
 * Node 1 is the server.  Its main thread accepts one connection, reads one
 * request byte, hands it to the worker over the condition variable, then
 * calls pthread_join to wait for the worker.  The worker takes the job,
 * stores it, emits, and returns (exits the thread).  The join is a futex
 * wait on the worker's clear-tid word, which the kernel's
 * CLONE_CHILD_CLEARTID zeroes and wakes on the worker's exit; the checker
 * models that wake in its SYS_exit handler.  Node 0 is a client that sends
 * one byte ('x' = 120).
 *
 * Canonical state fd 3, node 1: [recvd][stored byte].  The invariant is
 * that once the worker has stored a byte, it is the byte the client sent.
 *
 *   cc -O1 -no-pie -o twothread_join test/proc/twothread_join.c -lpthread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char stored;	/* the byte the worker stored */
static unsigned char recvd;		/* 1 once the worker has stored a byte */

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cnd = PTHREAD_COND_INITIALIZER;
static unsigned char job;
static int have_job;

static void
emit(void)
{
	unsigned char rec[3] = {2, recvd, stored};
	(void) !write(3, rec, sizeof rec);
}

/* the worker: take one job, store it, emit, then return (exit the
 * thread).  Its exit wakes the main thread's pthread_join. */
static void *
worker(void *arg)
{
	(void) arg;
	pthread_mutex_lock(&mtx);
	while (!have_job)
		pthread_cond_wait(&cnd, &mtx);
	unsigned char v = job;
	pthread_mutex_unlock(&mtx);
	stored = v;
	recvd = 1;
	emit();
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
		int			s = connect_to(BASE + 1);
		unsigned char b = 'x';
		write(s, &b, 1);
		return 0;
	}

	pthread_t	t;
	pthread_create(&t, NULL, worker, NULL);

	int			ls = listen_on(BASE + 1);
	int			cs = accept(ls, 0, 0);
	unsigned char b;
	if (read(cs, &b, 1) == 1)
	{
		pthread_mutex_lock(&mtx);
		job = b;
		have_job = 1;
		pthread_cond_signal(&cnd);
		pthread_mutex_unlock(&mtx);
	}
	pthread_join(t, NULL);		/* wait for the worker's exit (futex on its tid) */
	for (;;)
	{
		if (read(cs, &b, 1) != 1)
			break;
	}
	return 0;
}
