/*
 * test/proc/cutconn.c: node 1 sleeps 1 s, then makes a blocking connect
 * to node 0 and reports 1 if it succeeded, 2 if it failed.  With the
 * link cut, Linux's connect waits, and fails with ETIMEDOUT unless the
 * link heals first; it cannot succeed across the cut.
 *
 *   cc -O1 -no-pie -o cutconn test/proc/cutconn.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static void emit(unsigned char v) { unsigned char r[2] = {1, v}; (void) !write(3, r, 2); }
int main(int argc, char **argv)
{
	int id = atoi(argv[1]);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20000);
	emit(0);
	if (id == 0) {
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4);
		for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; unsigned char b; (void) !read(x, &b, 1); }
	} else {
		struct timespec t = {1, 0};
		nanosleep(&t, NULL);
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(s, (struct sockaddr *) &a, sizeof a) == 0) emit(1); else emit(2);
		unsigned char b; (void) !read(s, &b, 1);
	}
	return 0;
}
