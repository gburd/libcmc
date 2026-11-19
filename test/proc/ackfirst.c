/*
 * test/proc/ackfirst.c: one node reads WRITE(v) from the client, makes v
 * durable and acks it; with -DBUG it acks first.  There is no blocking
 * call between the two writes, so the bug is reachable only if a crash
 * can fall between two calls of one step, which Linux allows.
 *
 *   cc -O1 -no-pie -o ackfirst test/proc/ackfirst.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void)
{
	unsigned char z[2] = {1, 0};
	struct sockaddr_in a = {0};
	int ls = socket(AF_INET, SOCK_STREAM, 0), c;
	unsigned char m[2];
	(void) !write(3, z, 2);
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20000);
	bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 1);
	c = accept(ls, NULL, NULL);
	if (read(c, m, 2) == 2) {
		unsigned char ack[2] = {'A', m[1]}, rec[2] = {1, m[1]};
#ifdef BUG
		(void) !write(c, ack, 2);
		(void) !write(193, rec, 2);
#else
		(void) !write(193, rec, 2);
		(void) !write(c, ack, 2);
#endif
		(void) !write(3, rec, 2);
	}
	for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	return 0;
}
