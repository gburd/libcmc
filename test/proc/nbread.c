/*
 * test/proc/nbread.c: node 1 echoes one byte.  Node 0 dials it, sends
 * one byte, and then asks once, without waiting, whether the echo has
 * arrived; if it has (a "fast path"), it reports 1, else it waits for
 * the echo and reports 2.  On Linux the peer can answer before the
 * question, so 1 is reachable.  The mode, from the target args: read
 * (a non-blocking read), poll (poll with a zero timeout) or epoll
 * (epoll_wait with a zero timeout).  It reports before it reads on, since
 * the checker merges worlds on what a node reports.
 *
 *   cc -O1 -no-pie -o nbread test/proc/nbread.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static void emit(unsigned char v) { unsigned char r[2] = {1, v}; (void) !write(3, r, 2); }

/* has the echo arrived on s, asked without waiting? */
static int
arrived(int s, const char *mode)
{
	unsigned char b;
	if (strcmp(mode, "poll") == 0)
	{
		struct pollfd p = {s, POLLIN, 0};
		return poll(&p, 1, 0) == 1;
	}
	if (strcmp(mode, "epoll") == 0)
	{
		struct epoll_event ev = {EPOLLIN, {0}}, out;
		int ep = epoll_create1(0);
		epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev);
		return epoll_wait(ep, &out, 1, 0) == 1;
	}
	fcntl(s, F_SETFL, O_NONBLOCK);
	if (read(s, &b, 1) == 1) return 1;
	fcntl(s, F_SETFL, 0);
	return 0;
}

int main(int argc, char **argv)
{
	int id = atoi(argv[1]);
	const char *mode = argc > 2 ? argv[2] : "read";
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20001);
	emit(0);
	if (id == 1) {
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4);
		int c = accept(ls, NULL, NULL);
		unsigned char b;
		if (read(c, &b, 1) == 1) (void) !write(c, &b, 1);
		for (;;) { int x = accept(ls, NULL, NULL); if (x < 0) break; close(x); }
	} else {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		unsigned char b = 'p';
		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0) return 1;
		(void) !write(s, &b, 1);
		int ar = arrived(s, mode); if (ar) emit(1);
		else { (void) !read(s, &b, 1); emit(2); }
		(void) !read(s, &b, 1);
	}
	return 0;
}
