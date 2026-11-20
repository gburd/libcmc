/*
 * shortw.c: a non-blocking writer and a full send buffer.  Node 0 dials
 * node 1, makes the socket non-blocking, and sends a one-byte header and
 * then a four-byte body.  A correct writer loops on send(), waiting in
 * poll() for POLLOUT after EAGAIN and sending the rest after a short
 * write, as an event loop must.  Built with -DBUG it sends the body once
 * and ignores the count, which loses bytes only when the buffer fills,
 * so cmc catches it only with a short_writes budget.  The [target] arg
 * picks the call that sends: send (the default), writev or sendmsg, each
 * of two pieces.
 *
 * Node 1 reads until end of stream and records what arrived:
 * [done][count][bytes...] on fd 3, done 1 once the stream ended.
 *
 *   cc -O1 -no-pie -o shortw test/proc/shortw.c
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define BASE 20000

static void
emit(int done, const unsigned char *b, int n)
{
	unsigned char out[16] = {0};

	out[0] = (unsigned char) (n + 2);
	out[1] = (unsigned char) done;
	out[2] = (unsigned char) n;
	memcpy(out + 3, b, (size_t) n);
	(void) !write(3, out, (size_t) n + 3);
}

static const char *how = "send";	/* the [target] arg: send, writev or sendmsg */

/* one send of n bytes of b by the call how names */
static ssize_t
send1(int s, const char *b, int n)
{
	struct iovec iov[2] = {{(void *) b, (size_t) n / 2}, {(void *) (b + n / 2), (size_t) (n - n / 2)}};
	struct msghdr mh = {.msg_iov = iov, .msg_iovlen = 2};

	if (!strcmp(how, "writev")) return writev(s, iov, 2);
	if (!strcmp(how, "sendmsg")) return sendmsg(s, &mh, MSG_NOSIGNAL);
	return send(s, b, (size_t) n, MSG_NOSIGNAL);
}

/* send all of b, as an event loop does on a non-blocking socket */
static void
send_all(int s, const char *b, int n)
{
#ifdef BUG
	(void) !send1(s, b, n);
#else
	while (n > 0)
	{
		ssize_t		k = send1(s, b, n);

		if (k < 0 && errno == EAGAIN)
		{
			struct pollfd p = {s, POLLOUT, 0};

			(void) poll(&p, 1, -1);
			continue;
		}
		if (k < 0)
			return;
		b += k;
		n -= (int) k;
	}
#endif
}

int
main(int argc, char **argv)
{
	int			id = argc > 1 ? atoi(argv[1]) : 0;
	struct sockaddr_in a;
	unsigned char got[8];
	int			n = 0, one = 1;

	if (argc > 2) how = argv[2];

	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(BASE + 1);
	if (id == 0)
	{
		int			s = socket(AF_INET, SOCK_STREAM, 0);

		if (connect(s, (struct sockaddr *) &a, sizeof a) != 0)
			return 1;
		fcntl(s, F_SETFL, O_NONBLOCK);
		send_all(s, "H", 1);
		send_all(s, "abcd", 4);
		close(s);
		return 0;
	}
	int			ls = socket(AF_INET, SOCK_STREAM, 0);

	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	bind(ls, (struct sockaddr *) &a, sizeof a);
	listen(ls, 1);
	emit(0, got, 0);
	int			c = accept(ls, NULL, NULL);

	for (;;)
	{
		ssize_t		k = read(c, got + n, sizeof got - (size_t) n);

		if (k <= 0)
			break;
		n += (int) k;
		emit(0, got, n);
	}
	emit(1, got, n);
	return 0;
}
