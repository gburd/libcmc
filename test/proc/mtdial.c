/*
 * test/proc/mtdial.c: node 1 starts a second thread, sleeps a second
 * (so node 0's host may crash), and makes a blocking dial to node 0.
 * A host that is down answers no SYN, so the dial times out
 * (ETIMEDOUT) or, had the host stayed up, connects; it is never
 * refused.  Node 1 reports 1 for ETIMEDOUT, 2 for a connection, 3 for
 * ECONNREFUSED and 4 for any other error as node[1][0].
 *
 *   cc -O1 -no-pie -o mtdial test/proc/mtdial.c -lpthread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void
emit(unsigned char v)
{
	unsigned char r[2] = {1, v};

	(void) !write(3, r, 2);
}

static void *
idle(void *x)
{
	return x;
}

int
main(int argc, char **argv)
{
	int			id = atoi(argv[1]), x, fd, r;
	struct sockaddr_in a;
	struct timespec s = {1, 0};
	pthread_t	t;
	char		b;

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(20000);
	emit(0);
	if (id == 0)
	{
		int			ls = socket(AF_INET, SOCK_STREAM, 0);

		bind(ls, (struct sockaddr *) &a, sizeof a);
		listen(ls, 4);
		while ((x = accept(ls, NULL, NULL)) >= 0)
			(void) !read(x, &b, 1);
		return 0;
	}
	pthread_create(&t, NULL, idle, NULL);
	nanosleep(&s, NULL);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	r = connect(fd, (struct sockaddr *) &a, sizeof a);
	emit(r == 0 ? 2 : errno == ETIMEDOUT ? 1 : errno == ECONNREFUSED ? 3 : 4);
	pthread_join(t, NULL);
	return 0;
}
