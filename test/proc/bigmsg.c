/*
 * bigmsg probe for process-mode M7 (test/proc).
 *
 * An engine probe: node 0 sends one 65-byte write; the checker's message
 * limit is 64 bytes.  The M6 engine kept the first 8 bytes and dropped
 * the rest without a word; the right verdict is an engine error (exit 1)
 * naming the limit, because a truncated message is a different protocol.
 *
 *   cc -O1 -no-pie -o bigmsg test/proc/bigmsg.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define BASE 20000

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	unsigned char rec[2] = {1, 0};
	(void) !write(3, rec, 2);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(BASE + 1);
	if (id == 0)
	{
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(s, (void *) &a, sizeof a) != 0)
			return 3;
		unsigned char b[65] = {0};
		(void) !write(s, b, sizeof b);
		return 0;
	}
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	bind(ls, (void *) &a, sizeof a);
	listen(ls, 4);
	int c = accept(ls, 0, 0);
	unsigned char m[65];
	(void) !read(c, m, sizeof m);
	return 0;
}
