/*
 * test/proc/conncrash.c: node 1 dials node 0 and sends one byte, then
 * waits.  Node 0 accepts and reports 1 if the connection ends with no
 * byte, which Linux allows when the dialler crashes between connect()
 * and write().
 *
 *   cc -O1 -no-pie -o conncrash test/proc/conncrash.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
static void emit(unsigned char v) { unsigned char r[2] = {1, v}; (void) !write(3, r, 2); }
int main(int argc, char **argv)
{
	int id = atoi(argv[1]);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	emit(0);
	if (id == 0) {
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		a.sin_port = htons(20000);
		bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4);
		int c = accept(ls, NULL, NULL);
		unsigned char b;
		if (c >= 0 && read(c, &b, 1) == 0) emit(1);
		for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	} else {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		a.sin_port = htons(20000);
		if (connect(s, (struct sockaddr *) &a, sizeof a) == 0) {
			unsigned char b = 'x';
			(void) !write(s, &b, 1);
			(void) !read(s, &b, 1);
		}
	}
	return 0;
}
