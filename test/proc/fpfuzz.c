/*
 * fpfuzz.c: randomised tests of the process engine's world encoding
 * (src/cmc_proc.c, world_fp), complementing the named pairs of
 * fingerprint.c.  It builds random but consistent worlds (three nodes,
 * threads parked in every kind of call, connections with queued bytes,
 * a FIN or a reset, waiting dials, eventfds, epoll instances) from a
 * fixed seed, and checks two properties on each:
 *
 *   - a mutation of any one field a node's future depends on changes the
 *     encoding (merging the two worlds would drop one from the search);
 *   - a change the encoding is meant to hide (thread slot order, raw
 *     tids, which connection and bag slots hold what) leaves it the same.
 *
 * The engine source is included whole so the test reaches its statics.
 * Exits 0 when every check holds; prints the first failure of each kind
 * with its seed.
 */
#include "../../src/cmc_proc.c"

#define WORLDS 3000

static uint64_t rs;

static uint64_t
rnd(void)
{
	uint64_t	z = (rs += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static int
pick(int n)
{
	return (int) (rnd() % (uint64_t) n);
}

/* the encoding of the current world, copied out */
static size_t
encode(unsigned char *out)
{
	uint64_t	fp[2];

	world_fp(fp);
	memcpy(out, g_world, g_world_len);
	return g_world_len;
}

/* a new connection from node a to node b, accepted (fds on both ends) or
 * left waiting at b's listener, with a few bytes queued each way */
static void
add_conn(int a, int b, int accepted)
{
	int			c = conn_new(a, b);

	cfd_set(&N[a], N[a].next_fd++, b, c, 0);
	if (accepted)
	{
		conns[c].accepted = 1;
		cfd_set(&N[b], N[b].next_fd++, a, c, 1);
	}
	else
		N[b].pend[N[b].npend++] = c;
	for (int k = pick(3); k > 0; k--)
	{
		unsigned char m[4] = {(unsigned char) pick(256), (unsigned char) pick(256), 'm', 0};

		bag_add(c, accepted ? pick(2) : 1, m, 1 + pick(3), 0);
	}
	if (accepted && pick(4) == 0)
		bag_add(c, pick(2), NULL, 0, 1);	/* a FIN */
}

/* park thread t of node nd in a random kind of call, at a random point
 * of its code, sometimes holding an answer (BK_DONE) or held at a clone */
static void
park(Node *nd, Thread *t)
{
	t->nr = 200 + pick(3);
	t->ip = 0x401000 + 16 * (uint64_t) pick(4);
	t->sp = 0x7ff000 - 64 * (uint64_t) pick(4);
	t->args[0] = (uint64_t) pick(4);
	t->ctid_addr = pick(2) ? 0x9000 + 8 * (uint64_t) pick(3) : 0;
	switch (pick(7))
	{
		case 4:
			t->block_kind = BK_DONE;
			t->held_val = pick(3);
			t->held_err = pick(2) ? 0 : 11;
			t->held_len = pick(4);
			for (int k = 0; k < t->held_len; k++)
				t->held_data[k] = (unsigned char) pick(4);
			return;
		case 5:
			t->block_kind = BK_CLONED;
			return;
		case 6:
			t->block_kind = BK_NEWBORN;
			return;
	}
	switch (pick(4))
	{
		case 0:
			if (nd->nc > 0)
			{
				int			k = pick(nd->nc);

				t->block_kind = BK_RECV;
				t->block_peer = nd->cpeer[k];
				t->block_conn = nd->cconn[k];
				t->block_end = nd->cend[k];
				t->block_want = 1 + pick(8);
				break;
			}
			/* FALLTHROUGH */
		case 1:
			t->block_kind = BK_ACCEPT;
			t->block_peer = -2;
			break;
		case 2:
			t->block_kind = BK_FUTEX;
			t->futex_addr = 0x7000 + 8 * (uint64_t) pick(4);
			break;
		default:
			t->block_kind = BK_POLL;
			t->epoll_fd = -1;
			t->pollfd_n = 1 + pick(4);
			for (int k = 0; k < t->pollfd_n; k++)
			{
				t->pollfds[k] = nd->nc > 0 && pick(2) ? nd->cfd[pick(nd->nc)] : nd->listener_fd;
				t->pollevents[k] = pick(2) ? POLLIN : POLLIN | POLLOUT;
			}
			break;
	}
}

/* a random, consistent world */
static void
gen_world(void)
{
	memset(&M, 0, sizeof M);
	memset(N, 0, sizeof N);
	M.nodes = 3;
	reset_world();
	bud.timeouts = pick(3);
	bud.crashes = pick(2);
	bud.drops = pick(2);
	logical_ns += CLK_TICK * (uint64_t) pick(3);
	for (int i = 0; i < M.nodes; i++)
	{
		Node	   *nd = &N[i];

		nd->id = i;
		nd->alive = 1;
		nd->next_fd = 500;
		nd->listener_fd = nd->next_fd++;
		nd->timer_fd = -1;
		nd->canon_len = 1 + pick(3);
		for (int k = 0; k < nd->canon_len; k++)
			nd->canon[k] = (unsigned char) pick(4);
		nd->durable_len = pick(3);
		for (int k = 0; k < nd->durable_len; k++)
			nd->durable[k] = (unsigned char) pick(4);
	}
	for (int k = pick(4); k > 0; k--)
	{
		int			a = pick(3), b = (a + 1 + pick(2)) % 3;

		add_conn(a, b, pick(3) != 0);
	}
	for (int i = 0; i < M.nodes; i++)
	{
		Node	   *nd = &N[i];

		if (pick(3) == 0)
		{
			nd->efd[0] = nd->next_fd++;
			nd->efdval[0] = (uint64_t) pick(3);
			nd->nefd = 1;
		}
		nd->nth = 1 + pick(3);
		for (int t = 0; t < nd->nth; t++)
		{
			Thread	   *th = &nd->th[t];

			th->inuse = 1;
			th->tid = 1000 * (i + 1) + t;
			th->poll_woke_fd = -1;
			th->epoll_fd = -1;
			park(nd, th);
		}
	}
}

/* ---- mutations: each changes one field a future depends on ---------- */

static Node *
some_node(void)
{
	return &N[pick(M.nodes)];
}

static Thread *
some_thread(Node *nd)
{
	return &nd->th[pick(nd->nth)];
}

static int
m_canon(void)
{
	Node	   *nd = some_node();

	nd->canon[pick(nd->canon_len)] ^= 1 + pick(255);
	return 1;
}

static int
m_durable(void)
{
	Node	   *nd = some_node();

	nd->durable[nd->durable_len++] = 9;
	return 1;
}

static int
m_read_off(void)
{
	some_node()->durable_read_off++;
	return 1;
}

static int
m_alive(void)
{
	Node	   *nd = some_node();

	nd->alive = 0;
	nd->crashed = 1;
	return 1;
}

static int
m_block_kind(void)
{
	Thread	   *t = some_thread(some_node());

	t->block_kind = t->block_kind == BK_ACCEPT ? BK_FUTEX : BK_ACCEPT;
	t->futex_addr = 0x7000;
	t->block_peer = -2;
	return 1;
}

static int
m_want(void)
{
	Thread	   *t = some_thread(some_node());

	if (t->block_kind != BK_RECV)
		return 0;
	t->block_want++;
	return 1;
}

static int
m_recv_conn(void)
{
	Node	   *nd = some_node();
	Thread	   *t = some_thread(nd);

	if (t->block_kind != BK_RECV || nd->nc < 2)
		return 0;
	for (int k = 0; k < nd->nc; k++)
		if (nd->cconn[k] != t->block_conn || nd->cend[k] != t->block_end)
		{
			t->block_conn = nd->cconn[k];
			t->block_end = nd->cend[k];
			t->block_peer = nd->cpeer[k];
			return 1;
		}
	return 0;
}

static int
m_futex(void)
{
	Thread	   *t = some_thread(some_node());

	if (t->block_kind != BK_FUTEX)
		return 0;
	t->futex_addr += 0x100;
	return 1;
}

static int
m_poll_fd(void)
{
	Node	   *nd = some_node();
	Thread	   *t = some_thread(nd);

	if (t->block_kind != BK_POLL)
		return 0;
	int			k = pick(t->pollfd_n);

	t->pollfds[k] = t->pollfds[k] == nd->listener_fd ? -1 : nd->listener_fd;
	return 1;
}

static int
m_poll_events(void)
{
	Thread	   *t = some_thread(some_node());

	if (t->block_kind != BK_POLL)
		return 0;
	t->pollevents[pick(t->pollfd_n)] ^= POLLOUT;
	return 1;
}

static int
m_woke(void)
{
	Node	   *nd = some_node();
	Thread	   *t = some_thread(nd);

	if (nd->nc == 0 || t->poll_woke_fd >= 0)
		return 0;
	t->poll_woke_fd = nd->cfd[0];
	return 1;
}

static int
m_msg_byte(void)
{
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].len > 0 && conns[bag[i].conn].used)
		{
			bag[i].body[0] ^= 0x40;
			return 1;
		}
	return 0;
}

static int
m_msg_drop(void)
{
	for (int i = MAXMSG - 1; i >= 0; i--)
		if (bag[i].used)
		{
			bag[i].used = 0;
			return 1;
		}
	return 0;
}

static int
m_conn_flag(void)
{
	for (int c = 0; c < MAXCONNS; c++)
		if (conns[c].used && conns[c].accepted)
		{
			switch (pick(10))
			{
				case 9: conns[c].full[pick(2)] ^= 1; break;
				case 7: conns[c].shut_wr[pick(2)] ^= 1; break;
				case 8: conns[c].shut_rd[pick(2)] ^= 1; break;
				case 4: conns[c].connecting ^= 1; break;
				case 5: conns[c].nbdial ^= 1; break;
				case 6: conns[c].refused ^= 1; break;
				case 0: conns[c].reset ^= 1; break;
				case 1: conns[c].err[pick(2)] = ECONNRESET; break;
				case 2: conns[c].eof[pick(2)] ^= 1; break;
				default: conns[c].closed[pick(2)] ^= 1; break;
			}
			return 1;
		}
	return 0;
}

/* a socket option on a connection fd */
static int
m_sockopt(void)
{
	Node	   *nd = some_node();

	if (nd->nc == 0)
		return 0;
	nd->sockopt[nd->cfd[pick(nd->nc)] - 500] ^= SO_B_NODELAY;
	return 1;
}

/* a second number for a connection (dup) */
static int
m_dup(void)
{
	Node	   *nd = some_node();

	if (nd->nc == 0 || nd->next_fd - 500 >= MAXOWNED)
		return 0;
	nd->dupof[nd->next_fd++ - 500] = nd->cfd[pick(nd->nc)];
	return 1;
}

static int
m_nonblock(void)
{
	Node	   *nd = some_node();

	if (nd->nc == 0)
		return 0;
	nd->cnonblock[pick(nd->nc)] ^= 1;
	return 1;
}

static int
m_pend(void)
{
	Node	   *nd = some_node();

	if (nd->npend == 0)
		return 0;
	nd->npend--;
	return 1;
}

static int
m_efd(void)
{
	Node	   *nd = some_node();

	if (nd->nefd == 0)
		return 0;
	nd->efdval[0]++;
	return 1;
}

/* an eventfd's FD_CLOEXEC */
static int
m_efd_cloexec(void)
{
	Node	   *nd = some_node();

	if (nd->nefd == 0)
		return 0;
	nd->fdflag[nd->efd[0] - 500] ^= FD_CX;
	return 1;
}

/* a plain socket appears, or its O_NONBLOCK changes */
static int
m_plain(void)
{
	Node	   *nd = some_node();
	int			fd = nd->next_fd;

	if (fd - 500 >= MAXOWNED)
		return 0;
	nd->next_fd++;
	nd->fdflag[fd - 500] = FD_PLAIN | (pick(2) ? FD_NB : 0);
	return 1;
}

static int
m_budget(void)
{
	if (pick(2))
		bud.timeouts++;
	else
		bud.short_writes++;
	return 1;
}

static int
m_clock(void)
{
	logical_ns += CLK_TICK;
	return 1;
}

static int
m_reply(void)
{
	rep_src[nrep] = 1;
	rep_val[nrep] = 7;
	nrep++;
	return 1;
}

/* a newborn thread, not yet run */
static int
m_newborn(void)
{
	Node	   *nd = some_node();

	if (nd->nth >= MAXTHREAD)
		return 0;
	Thread	   *t = &nd->th[nd->nth++];

	memset(t, 0, sizeof *t);
	t->inuse = 1;
	t->tid = 98;
	t->poll_woke_fd = -1;
	t->epoll_fd = -1;
	t->block_kind = BK_NEWBORN;
	return 1;
}

/* a thread whose position is part of the world: one of a node with more
 * than one thread, or one holding an answer; NULL if there is none */
static Thread *
positioned_thread(void)
{
	Node	   *nd = some_node();
	Thread	   *t = some_thread(nd);

	if (node_multi(nd) || t->block_kind >= BK_DONE)
		return t;
	return NULL;
}

/* the point a thread is parked at: its call, an argument, its
 * instruction or stack pointer, or the word a join waits on */
static int
m_position(void)
{
	Thread	   *t = positioned_thread();

	if (t == NULL || t->block_kind == BK_NEWBORN)
		return 0;
	switch (pick(5))
	{
		case 0: t->nr++; break;
		case 1: t->args[pick(6)] ^= 1; break;
		case 2: t->ip += 16; break;
		case 3: t->sp -= 64; break;
		default: t->ctid_addr += 8; break;
	}
	return 1;
}

/* a held answer: its value, errno, or a byte it wrote */
static int
m_held(void)
{
	Thread	   *t = some_thread(some_node());

	if (t->block_kind != BK_DONE)
		return 0;
	switch (pick(3))
	{
		case 0: t->held_val++; break;
		case 1: t->held_err = t->held_err ? 0 : 11; break;
		default:
			if (t->held_len == 0)
				t->held_data[t->held_len++] = 1;
			else
				t->held_data[pick(t->held_len)] ^= 1;
			break;
	}
	return 1;
}

/* a parked thread becomes one held at a clone */
static int
m_cloned(void)
{
	Thread	   *t = some_thread(some_node());

	if (t->block_kind == BK_CLONED)
		return 0;
	t->block_kind = BK_CLONED;
	return 1;
}

static int
m_thread_count(void)
{
	Node	   *nd = some_node();

	if (nd->nth >= MAXTHREAD)
		return 0;
	Thread	   *t = &nd->th[nd->nth++];

	memset(t, 0, sizeof *t);
	t->inuse = 1;
	t->tid = 99;
	t->poll_woke_fd = -1;
	t->epoll_fd = -1;
	t->block_kind = BK_ACCEPT;
	t->block_peer = -2;
	return 1;
}

static int (*const mutations[]) (void) = {
	m_canon, m_durable, m_read_off, m_alive, m_block_kind, m_want,
	m_recv_conn, m_futex, m_poll_fd, m_poll_events, m_woke, m_msg_byte,
	m_msg_drop, m_conn_flag, m_nonblock, m_pend, m_efd, m_budget, m_clock,
	m_reply, m_newborn, m_thread_count, m_position, m_held, m_cloned, m_efd_cloexec, m_plain, m_sockopt, m_dup,
};
static const char *const mutation_names[] = {
	"canon", "durable", "read_off", "alive", "block_kind", "want",
	"recv_conn", "futex", "poll_fd", "poll_events", "woke", "msg_byte",
	"msg_drop", "conn_flag", "nonblock", "pend", "efd", "budget", "clock",
	"reply", "newborn", "thread_count", "position", "held", "cloned", "efd_cloexec", "plain", "sockopt", "dup",
};

/* ---- changes the encoding must hide ------------------------------- */

/* reverse the thread slots of a node, and give every thread a new tid */
static void
h_threads(void)
{
	Node	   *nd = some_node();

	for (int a = 0, b = nd->nth - 1; a < b; a++, b--)
	{
		Thread		t = nd->th[a];

		nd->th[a] = nd->th[b];
		nd->th[b] = t;
	}
	for (int t = 0; t < nd->nth; t++)
		nd->th[t].tid += 5000;
}

/* move every message to the other end of the bag array */
static void
h_bag(void)
{
	Msg			copy[MAXMSG];
	int			n = 0;

	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used)
			copy[n++] = bag[i];
	memset(bag, 0, sizeof bag);
	for (int i = 0; i < n; i++)
		bag[MAXMSG - 1 - i] = copy[i];
}

/* move connection slot c to the last free slot, and every reference */
static void
h_conn_slot(void)
{
	int			from = -1, to = -1;

	for (int c = 0; c < MAXCONNS; c++)
		if (conns[c].used)
		{
			from = c;
			break;
		}
	for (int c = MAXCONNS - 1; c >= 0; c--)
		if (!conns[c].used)
		{
			to = c;
			break;
		}
	if (from < 0 || to < 0)
		return;
	conns[to] = conns[from];
	memset(&conns[from], 0, sizeof conns[from]);
	for (int i = 0; i < MAXMSG; i++)
		if (bag[i].used && bag[i].conn == from)
			bag[i].conn = to;
	for (int i = 0; i < M.nodes; i++)
	{
		for (int k = 0; k < N[i].nc; k++)
			if (N[i].cconn[k] == from)
				N[i].cconn[k] = to;
		for (int k = 0; k < N[i].npend; k++)
			if (N[i].pend[k] == from)
				N[i].pend[k] = to;
		for (int t = 0; t < N[i].nth; t++)
			if (N[i].th[t].block_kind == BK_RECV && N[i].th[t].block_conn == from)
				N[i].th[t].block_conn = to;
	}
}

static void (*const hides[]) (void) = {h_threads, h_bag, h_conn_slot};
static const char *const hide_names[] = {"thread order and tids", "bag slots", "connection slots"};

int
main(void)
{
	static unsigned char a[sizeof g_world], b[sizeof g_world];
	int			nm = (int) (sizeof mutations / sizeof mutations[0]);
	int			nh = (int) (sizeof hides / sizeof hides[0]);
	int			fails = 0, applied = 0, hidden = 0;

	for (uint64_t w = 0; w < WORLDS; w++)
	{
		for (int m = 0; m < nm; m++)
		{
			rs = w * 7919;
			gen_world();
			size_t		na = encode(a);

			if (!mutations[m]())
				continue;
			applied++;
			size_t		nb = encode(b);

			if (na == nb && !memcmp(a, b, na))
			{
				if (fails++ < 8)
					printf("FAIL world %llu: a different %s encodes the same\n", (unsigned long long) w, mutation_names[m]);
			}
		}
		for (int h = 0; h < nh; h++)
		{
			rs = w * 7919;
			gen_world();
			size_t		na = encode(a);

			hides[h]();
			hidden++;
			size_t		nb = encode(b);

			if (na != nb || memcmp(a, b, na))
			{
				if (fails++ < 8)
					printf("FAIL world %llu: a change of %s alters the encoding\n", (unsigned long long) w, hide_names[h]);
			}
		}
	}
	printf("%s %d mutations and %d hidden changes over %d random worlds\n",
		   fails ? "FAIL" : "ok  ", applied, hidden, WORLDS);
	return fails != 0;
}
