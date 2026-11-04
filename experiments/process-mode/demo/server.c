/*
 * demo/server.c: an ordinary one-connection TCP server, written the way
 * it would ship -- blocking sockets, a real write()+fsync(), no libcmc
 * header and no step function.  The checker (../checker.c) runs THIS
 * binary unmodified and model-checks it by intercepting its syscalls.
 *
 * Protocol, one byte each way on a connection:
 *   client -> 'W' v   : store value v durably, then acknowledge
 *   server -> 'A' v   : v is acknowledged
 *
 * Correct order: write the value to the store file, fsync it, THEN send
 * the ACK.  Built with -DBUG the ACK goes out before the fsync, so a
 * crash after the ACK but before the flush loses an acknowledged write
 * -- the bug process mode must catch.
 *
 *   cc -O1 -o server demo/server.c
 *   cc -O1 -DBUG -o server-bug demo/server.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int			port = argc > 1 ? atoi(argv[1]) : 0;
	const char *store = argc > 2 ? argv[2] : "store.dat";

	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	int			one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (bind(ls, (void *) &a, sizeof a) != 0)
		return 2;
	socklen_t al = sizeof a;
	getsockname(ls, (void *) &a, &al);
	/* tell the parent which port we got */
	dprintf(1, "port %d\n", ntohs(a.sin_port));
	listen(ls, 1);

	int			cs = accept(ls, 0, 0);
	if (cs < 0)
		return 3;

	for (;;)
	{
		unsigned char m[2];
		ssize_t		n = read(cs, m, 2);
		if (n <= 0)
			break;
		if (m[0] != 'W')
			continue;

		int			fd = open(store, O_WRONLY | O_CREAT, 0600);
		unsigned char rec = m[1];
		pwrite(fd, &rec, 1, 0);

		unsigned char ack[2] = {'A', m[1]};
#ifdef BUG
		/* acknowledge before the write is durable */
		write(cs, ack, 2);
		fsync(fd);
#else
		fsync(fd);
		write(cs, ack, 2);
#endif
		close(fd);
	}
	return 0;
}
