/*
 * test/proc/rstmid.c: node 0 dials node 1 and writes "0", "a" and "b"
 * with no blocking call between.  Node 1 reads one byte and closes,
 * leaving bytes unread, so its stack sends an RST.  On Linux the RST
 * can arrive between node 0's writes: the write of "a" succeeds and
 * the write of "b" fails (node 0 reports 1).
 *
 *   cc -O1 -no-pie -o rstmid test/proc/rstmid.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
static void emit(unsigned char v) { unsigned char r[2] = {1, v}; (void) !write(3, r, 2); }
int main(int argc, char **argv)
{
	int id = atoi(argv[1]);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20001);
	signal(SIGPIPE, SIG_IGN);
	emit(0);
	if (id == 1) {
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4);
		int x = accept(ls, NULL, NULL); unsigned char b; (void) !read(x, &b, 1); close(x);
		for (;;) { int y = accept(ls, NULL, NULL); if (y < 0) break; close(y); }
	} else {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0) return 1;
		ssize_t w0 = write(s, "0", 1); (void) w0;
		ssize_t w1 = write(s, "a", 1);
		ssize_t w2 = write(s, "b", 1);
		emit(w1 == 1 && w2 < 0 ? 1 : w1 < 0 ? 2 : 3);
		unsigned char b; (void) !read(s, &b, 1);
	}
	return 0;
}
