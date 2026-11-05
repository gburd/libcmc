/*
 * demo/list.c: an ordinary one-connection TCP server whose protocol state
 * is a malloc'd ordered linked set (experiments/process-mode/DESIGN.md,
 * M2).  Written as it ships: blocking sockets, plain malloc, no libcmc
 * header, no step function.  The client inserts keys; the server keeps
 * them sorted and acknowledges.
 *
 * Protocol, one byte each way:
 *   client -> k (1..9)   : insert key k into the set, then acknowledge
 *   server -> 'A'        : done
 *
 * Insert order does not change the logical set: insert 1 then 2 and
 * insert 2 then 1 both give {1,2}.  But the two orders allocate the two
 * nodes in opposite arena slots, so the raw heap bytes differ -- the wall
 * M2 demonstrates.
 *
 * Build switches:
 *   -DBUG    an insert whose key is smaller than the current head
 *            overwrites the head's key in place instead of linking a new
 *            node, dropping the old head.  Insert 2 then insert 1 loses
 *            2.  The invariant "every inserted key is still present"
 *            catches it.
 *   -DCANON  after each insert, emit the keys in sorted order on fd 3, a
 *            declared side channel.  The checker hashes that instead of
 *            the raw arena, so the two insert orders canonicalise to the
 *            same bytes and the search merges them.  ~8 lines; the
 *            protocol code is unchanged.
 *
 *   cc -O1 -no-pie -o list     demo/list.c
 *   cc -O1 -no-pie -DCANON -o list-canon demo/list.c
 *   cc -O1 -no-pie -DCANON -DBUG -o list-bug demo/list.c
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct node
{
	int			key;
	struct node *next;
};

static struct node *head;		/* ascending by key */

static void
insert(int k)
{
#ifdef BUG
	/* The bug: an insert whose key is smaller than the current head
	 * overwrites the head's key in place instead of linking a new node,
	 * dropping the key that was there.  insert 2 then insert 1 loses 2. */
	if (head && k < head->key)
	{
		head->key = k;
		return;
	}
#endif
	struct node **pp = &head;
	while (*pp && (*pp)->key < k)
		pp = &(*pp)->next;
	if (*pp && (*pp)->key == k)
		return;					/* already present */
	struct node *n = malloc(sizeof *n);
	n->key = k;
	n->next = *pp;
	*pp = n;
}

#ifdef CANON
/* The opt-in canonicaliser: after each change, emit the logical state
 * (keys, ascending) on fd 3, a declared side channel the checker reads.
 * This encoding does not depend on malloc order, so two insert orders
 * emit the same bytes and the checker merges them.  ~8 lines, and the
 * protocol code above is unchanged. */
static void
cmc_canon_emit(void)
{
	unsigned char buf[32];
	size_t n = 1;				/* buf[0] = count, filled below */
	for (struct node *p = head; p && n < sizeof buf; p = p->next)
		buf[n++] = (unsigned char) p->key;
	buf[0] = (unsigned char) (n - 1);
	(void) !write(3, buf, n);	/* one self-delimiting record: [count][keys...] */
}
#endif

int
main(int argc, char **argv)
{
	int			port = argc > 1 ? atoi(argv[1]) : 0;
	int			ls = socket(AF_INET, SOCK_STREAM, 0);
	int			one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short) port);
	if (bind(ls, (void *) &a, sizeof a) != 0)
		return 2;
	listen(ls, 1);
	int			cs = accept(ls, 0, 0);
	if (cs < 0)
		return 3;

	for (;;)
	{
		unsigned char m;
		ssize_t		r = read(cs, &m, 1);
		if (r <= 0)
			break;
		if (m < 1 || m > 9)
			continue;
		insert(m);
#ifdef CANON
		cmc_canon_emit();
#endif
		unsigned char ack = 'A';
		if (write(cs, &ack, 1) != 1)
			break;
	}
	return 0;
}
