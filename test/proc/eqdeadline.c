/*
 * test/proc/eqdeadline.c: two nodes whose timers fall due at one
 * instant.  n0 listens, n1 dials it, and both sleep 1 s.  Then n0
 * writes "a" and makes a non-blocking read; n1 makes a non-blocking read
 * and writes "y".  Each reports 1 if its read got a byte, 2 if EAGAIN.
 * On Linux both reads can get a byte: n0 writes, n1 wakes and reads it
 * and answers, and n0 reads the answer.  While n0 is stopped inside its
 * step the clocks are frozen, but n1's deadline is already due then and
 * must still be able to fire.  SPIN delays n0 between its calls, for a
 * native run.
 *
 *   cc -O1 -no-pie -o eqdeadline test/proc/eqdeadline.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static void emit(unsigned char v) { unsigned char r[2] = {1, v}; (void) !write(3, r, 2); }
static void spin(void) { if (getenv("SPIN")) { volatile long z = 0; for (long q = 0; q < 300000000; q++) z += q; } }
int main(int argc, char **argv)
{
	int id = atoi(argv[1]);
	signal(SIGPIPE, SIG_IGN);
	emit(0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(20000);
	struct timespec t = {1, 0};
	char b;
	int s;
	if (id == 0) {
		int one = 1, ls = socket(AF_INET, SOCK_STREAM, 0);
		setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		bind(ls, (struct sockaddr *) &a, sizeof a); listen(ls, 4);
		s = accept(ls, NULL, NULL);
	} else {
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0) return 1;
	}
	fcntl(s, F_SETFL, O_NONBLOCK);
	nanosleep(&t, NULL);
	if (id == 0) {
		(void) !write(s, "a", 1);
		spin();
		ssize_t k = read(s, &b, 1);
		emit(k == 1 ? 1 : 2);
	} else {
		ssize_t k = read(s, &b, 1);
		(void) !write(s, "y", 1);
		emit(k == 1 ? 1 : 2);
	}
	usleep(500000);
	return 0;
}
