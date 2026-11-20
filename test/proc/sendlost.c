/*
 * test/proc/sendlost.c: node 0 (primary) accepts the client, reads
 * WRITE(v), dials node 1 (backup), sends REPL(v) and acks the client.
 * Node 1 reports v if REPL(v) arrived, 2 if the connection ended with
 * nothing.  The primary acks once REPL(v) is in its send queue, not
 * once the backup has it, so a host crash right after the ack can lose
 * REPL(v): the backup then sees only a dead peer.
 *
 *   cc -O1 -no-pie -o sendlost test/proc/sendlost.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
static void emit(unsigned char v) { unsigned char r[2] = {1, v}; (void) !write(3, r, 2); }
static int lsn(int port) { struct sockaddr_in a = {0}; int ls = socket(AF_INET, SOCK_STREAM, 0);
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
	bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4); return ls; }
int main(int argc, char **argv)
{
	int id = atoi(argv[1]);
	emit(0);
	if (id == 1) {
		int ls = lsn(20001), c = accept(ls, NULL, NULL);
		unsigned char m[2];
		ssize_t r = read(c, m, 2);
		if (r == 2) emit(m[1]); else if (r <= 0) emit(2);
		for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	} else {
		int ls = lsn(20000), c = accept(ls, NULL, NULL);
		unsigned char m[2];
		if (c >= 0 && read(c, m, 2) == 2) {
			struct sockaddr_in a = {0};
			a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20001);
			int s = socket(AF_INET, SOCK_STREAM, 0);
			if (connect(s, (struct sockaddr *) &a, sizeof a) == 0) {
				unsigned char rp[2] = {'R', m[1]}, ack[2] = {'A', m[1]};
				(void) !write(s, rp, 2);
				(void) !write(c, ack, 2);
				(void) !read(s, rp, 1);
			}
		}
		for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	}
	return 0;
}
