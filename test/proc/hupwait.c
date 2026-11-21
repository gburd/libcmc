/*
 * test/proc/hupwait.c: node 0 dials node 1 and shuts its own sending
 * side; node 1 reads the EOF and closes.  Node 0 then waits in a poll
 * that asks for no events, which only a hangup ends: once node 1's FIN
 * arrives both sides are shut and Linux reports POLLHUP (16).  Node 0
 * reports that poll's revents, the next poll's, its read (100 + n) and
 * a last poll's.  Natively: 16, 21, 100, 21.
 *
 *   cc -O1 -no-pie -o hupwait test/proc/hupwait.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
static void emit4(unsigned char a, unsigned char b, unsigned char c, unsigned char d)
{ unsigned char r[5] = {4, a, b, c, d}; (void) !write(3, r, 5); }
int main(int argc, char **argv)
{
	int id = atoi(argv[1]);
	signal(SIGPIPE, SIG_IGN);
	emit4(0, 0, 0, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20001);
	if (id == 1) {
		int one = 1, ls = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4);
		int x = accept(ls, NULL, NULL); unsigned char b; (void) !read(x, &b, 1);
		close(x);
		for (;;) { int y = accept(ls, NULL, NULL); if (y < 0) break; close(y); }
		return 0;
	}
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(s, (struct sockaddr *) &a, sizeof a) != 0) return 1;
	shutdown(s, SHUT_WR);
	struct pollfd p = {s, 0, 0}; poll(&p, 1, -1); unsigned char p1 = p.revents;
	p.events = POLLIN | POLLOUT; poll(&p, 1, 0); unsigned char p2 = p.revents;
	char b; ssize_t k = read(s, &b, 1);
	poll(&p, 1, 0);
	emit4(p1, p2, k >= 0 ? 100 + k : errno, p.revents);
	return 0;
}
