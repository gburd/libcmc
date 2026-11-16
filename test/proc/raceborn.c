/*
 * test/proc/raceborn.c: a new thread's first system call and its
 * parent's next one can come in either order (soundness plan 3.8).
 *
 * Node 1 dials node 0, then starts a thread that sends 'B' on the
 * connection, sends 'A' itself, and joins the thread.  The protocol says
 * 'A' comes first.  Under -DBUG the parent sends 'A' after
 * pthread_create, so the new thread can send first; the correct build
 * sends 'A' before it creates the thread.  Node 0 reads two bytes and
 * emits them in the order they arrived.
 *
 * The kernel lets a new thread and its parent run at once, so an engine
 * that lets them both run to their first checked call races them for
 * real.  The checker holds both at the clone's return (ptrace stops)
 * and lets the search choose which runs first, so the 'B' first order is
 * found every time.
 *
 * Canonical state fd 3, node 0: [first][second], 0 until read.
 *
 *   cc -O1 -no-pie -o raceborn     test/proc/raceborn.c -lpthread
 *   cc -O1 -no-pie -DBUG -o raceborn-bug test/proc/raceborn.c -lpthread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static int conn;

static void
emit(unsigned char first, unsigned char second)
{
	unsigned char rec[3] = {2, first, second};
	(void) !write(3, rec, sizeof rec);
}

static void *
sender(void *arg)
{
	unsigned char b = 'B';

	(void) arg;
	(void) !write(conn, &b, 1);
	return NULL;
}

static void
node0(void)
{
	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	unsigned char x = 0, y = 0;

	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(BASE);
	if (bind(ls, (void *) &a, sizeof a) != 0)
		_exit(2);
	listen(ls, 4);
	int			cs = accept(ls, 0, 0);
	if (read(cs, &x, 1) != 1)
		_exit(4);
	emit(x, 0);
	if (read(cs, &y, 1) != 1)
		_exit(4);
	emit(x, y);
	for (;;)
		if (read(cs, &y, 1) <= 0)
			break;
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	unsigned char b = 'A';
	pthread_t	t;

	emit(0, 0);
	if (id == 0)
	{
		node0();
		return 0;
	}
	conn = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(BASE);
	if (connect(conn, (void *) &a, sizeof a) != 0)
		_exit(3);
#ifndef BUG
	(void) !write(conn, &b, 1);
#endif
	pthread_create(&t, NULL, sender, NULL);
#ifdef BUG
	(void) !write(conn, &b, 1);
#endif
	pthread_join(t, NULL);
	/* stay up, parked in a read the peer never answers, so the end is a
	 * quiescent point and not an exit that closes the connection */
	(void) !read(conn, &b, 1);
	return 0;
}
