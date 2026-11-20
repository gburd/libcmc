/*
 * test/proc/ackfirst.c: one node reads WRITE(v) from the client, makes v
 * durable and acks it; with -DBUG it acks first.  There is no blocking
 * call between the two writes, so the bug is reachable only if a crash
 * can fall between two calls of one step, which Linux allows.  With
 * -DWRITEV it makes the record durable with writev(), and reports 9
 * instead of the value while it runs, so a crashed node whose record is
 * durable and whose ack is not sent is a state the model can name.
 *
 *   cc -O1 -no-pie -o ackfirst test/proc/ackfirst.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
#elif defined(WRITEV)
		struct iovec v = {rec, 2};
		unsigned char nine[2] = {1, 9};
		(void) !write(3, nine, 2);
		(void) !writev(193, &v, 1);
		(void) !write(c, ack, 2);
#else
		(void) !write(193, rec, 2);
		(void) !write(c, ack, 2);
#endif
#ifndef WRITEV
		(void) !write(3, rec, 2);
#endif
	}
	for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	return 0;
}
