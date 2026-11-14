/*
 * test/proc/guarded.c: a two-node pair that keeps its state hook in the
 * production build but guards it, so the same binary is safe to ship and
 * checkable.  It emits on CMC_STATE_FD only when the checker sets it, and
 * exercises the preload's time() and posix_memalign overrides so a run is
 * deterministic (same state count twice).
 *
 * Node 0 reads one client WRITE(v), replicates it to node 1, and after
 * node 1 acks, stores and acks the client.  Both emit their stored value.
 * time() and an aligned buffer feed the value's low bits so a
 * non-deterministic clock or heap address would change the reported
 * state; under the preload they do not.
 *
 *   cc -O1 -no-pie -o guarded test/proc/guarded.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BASE 20000

static unsigned char value;

/* emit the stored value, but only under the checker (CMC_STATE_FD set),
 * so a production build never writes to a stray descriptor 3 */
static void
emit(void)
{
	const char *sf = getenv("CMC_STATE_FD");
	if (!sf)
		return;
	int fd = atoi(sf);
	unsigned char rec[2] = {1, value};
	(void) !write(fd, rec, 2);
}

static struct sockaddr_in
addr(int port)
{
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	return a;
}

/* Touch the preload's deterministic time() and aligned allocator and
 * check they are the preload's, not libc's: posix_memalign must return a
 * pointer in the fixed arena (base 0x600000000000), and time() must read
 * the logical clock (a small epoch, not a ~1.7e9 wall-clock value).  If
 * either comes from libc the address is ASLR'd or the clock is real and
 * replay is not deterministic, so the node exits and the checker reports
 * it never reported state -- a visible failure, not a silent pass. */
static void
determinism_probe(void)
{
	void *p = NULL;
	if (posix_memalign(&p, 64, 128) != 0 || p == NULL)
		_exit(4);
	if ((unsigned long) p < 0x600000000000UL || (unsigned long) p >= 0x600004000000UL)
		_exit(5);				/* not from the preload's arena */
	if (((unsigned long) p & 63UL) != 0)
		_exit(6);				/* not 64-byte aligned */
	memset(p, 0, 128);
	if ((unsigned long) time(NULL) >= 1600000000UL)
		_exit(7);				/* a real wall clock, not the logical one */
	free(p);
}

int
main(int argc, char **argv)
{
	int id = argc > 1 ? atoi(argv[1]) : 0;
	determinism_probe();
	emit();

	if (id == 1)
	{
		int ls = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a = addr(BASE + 1);
		bind(ls, (void *) &a, sizeof a);
		listen(ls, 4);
		int cs = accept(ls, 0, 0);
		unsigned char m[2];
		if (read(cs, m, 2) == 2 && m[0] == 'R')
		{
			value = m[1];
			emit();
			unsigned char ack[2] = {'A', m[1]};
			(void) !write(cs, ack, 2);
		}
		return 0;
	}

	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a0 = addr(BASE + 0);
	bind(ls, (void *) &a0, sizeof a0);
	listen(ls, 4);
	int client = accept(ls, 0, 0);
	unsigned char m[2];
	if (read(client, m, 2) != 2 || m[0] != 'W')
		return 0;
	unsigned char v = m[1];

	int s = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a1 = addr(BASE + 1);
	if (connect(s, (void *) &a1, sizeof a1) != 0)
		return 3;
	unsigned char repl[2] = {'R', v};
	(void) !write(s, repl, 2);
	unsigned char ba[2];
	if (read(s, ba, 2) == 2 && ba[0] == 'A')
	{
		value = v;
		emit();
		unsigned char cack[2] = {'A', v};
		(void) !write(client, cack, 2);
	}
	return 0;
}
