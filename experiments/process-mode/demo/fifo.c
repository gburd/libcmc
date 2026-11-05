/*
 * fifo probe for process-mode M7 (experiments/process-mode).
 *
 * An engine probe: node 0 writes A then B on one TCP connection; node 1
 * reads twice.  TCP delivers a connection's bytes in order, so node 1
 * can never read B first.  `never_b_first` is true of every real run; a
 * checker that may deliver a connection's messages out of order reports
 * a violation that cannot happen.
 *
 * Canonical state on fd 3: [count=2][first][second], 0 = nothing yet.
 *
 *   cc -O1 -no-pie -o fifo demo/fifo.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

static unsigned char got[2];

static void
emit(void)
{
	unsigned char rec[3] = {2, got[0], got[1]};
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
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	emit();
	if (id == 0)
	{
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);
		if (connect(s, (void *) &a, sizeof a) != 0)
			return 3;
		unsigned char ma[2] = {'M', 1}, mb[2] = {'M', 2};
		(void) !write(s, ma, 2);
		(void) !write(s, mb, 2);
		return 0;
	}
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = addr(BASE + 1);
	bind(ls, (void *) &a, sizeof a);
	listen(ls, 4);
	int c = accept(ls, 0, 0);
	for (int i = 0; i < 2; i++)
	{
		unsigned char m[2];
		if (read(c, m, 2) != 2)
			break;
		got[i] = m[1];
		emit();
	}
	return 0;
}
