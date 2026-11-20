/*
 * test/proc/midmerge.c: node 0 accepts node 1, dials node 2, polls both
 * and remembers whose byte came first.  It then writes to both peers
 * (crash points while a crash is in the budget) and only then reports
 * which came first on fd 3.  Two histories that differ in that memory
 * reach the writes with the same hook record; a search that merged
 * worlds held mid-step on the hook would explore one of them only, and
 * a crash budget would hide a violation reachable without it.
 *
 *   cc -O1 -no-pie -o midmerge test/proc/midmerge.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
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
		int c[2]; c[0] = accept(ls, NULL, NULL);
		struct sockaddr_in b2 = a; b2.sin_port = htons(20002);
		c[1] = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(c[1], (struct sockaddr *) &b2, sizeof b2) != 0) return 1;
		unsigned char first = 0, b; int got = 0, done[2] = {0, 0};
		while (got < 2) {
			struct pollfd p[2] = {{done[0] ? -1 : c[0], POLLIN, 0}, {done[1] ? -1 : c[1], POLLIN, 0}};
			if (poll(p, 2, -1) <= 0) return 1;
			for (int k = 0; k < 2; k++)
				if (p[k].revents & POLLIN) {
					if (read(c[k], &b, 1) != 1) return 1;
					if (!first) first = b;
					done[k] = 1; got++;
					break;
				}
		}
		(void) !write(c[0], "d", 1);
		(void) !write(c[1], "d", 1);
		emit(first);
		for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	} else if (id == 2) {
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		a.sin_port = htons(20002);
		bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4);
		int s = accept(ls, NULL, NULL);
		unsigned char b = 2;
		(void) !write(s, &b, 1);
		(void) !read(s, &b, 1);
		for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	} else {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		unsigned char b = (unsigned char) id;
		if (connect(s, (struct sockaddr *) &a, sizeof a) == 0) {
			(void) !write(s, &b, 1);
			(void) !read(s, &b, 1);
		}
	}
	return 0;
}
