/*
 * fingerprint.c: a whitebox test of the process engine's world encoding
 * (src/cmc_proc.c, world_fp).  Each pair below is two worlds whose
 * futures differ; the engine dedups on the encoding, so the pair must
 * encode differently or the search would explore one and silently skip
 * the other.  A final pair differs only in a raw tid, which no future
 * depends on, and must encode the same.  The test compares the 128-bit
 * fingerprints world_fp() returns, which follow the encoding.  Exits 0
 * when every check holds.
 *
 * The engine source is included whole so the test reaches its statics.
 */
#include "../../src/cmc_proc.c"

static int	failed;

/* one live node, one live thread parked in futex on word 0x1000 */
static void
init_world(void)
{
	memset(&M, 0, sizeof M);
	memset(N, 0, sizeof N);
	M.nodes = 2;
	reset_world();
	N[0].alive = 1;
	N[0].listener_fd = 500;
	N[0].timer_fd = -1;
	N[0].nth = 1;
	N[0].th[0].inuse = 1;
	N[0].th[0].tid = 41;
	N[0].th[0].epoll_fd = -1;
	N[0].th[0].poll_woke_fd = -1;
	N[0].th[0].block_kind = BK_FUTEX;
	N[0].th[0].futex_addr = 0x1000;
}

static uint64_t before[2];

static void
snap(void)
{
	world_fp(before);
}

static void
verdict(int want_same, const char *what)
{
	uint64_t	after[2];

	world_fp(after);
	int			eq = after[0] == before[0] && after[1] == before[1];
	int			ok = want_same ? eq : !eq;

	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failed = 1;
}

static void differs(const char *what) { verdict(0, what); }
static void same(const char *what) { verdict(1, what); }

static void
poll_on(int n, int listener_at)
{
	Thread	   *th = &N[0].th[0];

	th->block_kind = BK_POLL;
	th->pollfd_n = n;
	for (int k = 0; k < n; k++)
	{
		th->pollfds[k] = k == listener_at ? 500 : -1;
		th->pollevents[k] = POLLIN;
	}
}

int
main(void)
{
	init_world();
	snap();
	N[0].durable_len = 1;
	N[0].durable[0] = 99;
	differs("a different durable record");

	init_world();
	snap();
	N[0].th[0].futex_addr = 0x2000;
	differs("the sole waiter on a different futex word");

	init_world();
	N[0].nep = 1;
	N[0].ep[0].nit = 1;
	N[0].ep[0].fd[0] = 500;
	N[0].ep[0].events[0] = EPOLLIN;
	N[0].ep[0].data[0] = 7;
	snap();
	N[0].ep[0].data[0] = 8;
	differs("an epoll registration with different user data");

	init_world();
	poll_on(6, -1);
	snap();
	poll_on(6, 5);
	differs("a poll set that differs only in its sixth entry");

	init_world();
	N[0].th[0].block_kind = BK_RECV;
	N[0].th[0].block_peer = 1;
	N[0].th[0].block_want = 3;
	snap();
	N[0].th[0].block_want = 2;
	differs("a recv waiting for a different length");

	init_world();
	snap();
	N[0].durable_read_off = 1;
	differs("a durable record already read back");

	init_world();
	snap();
	N[0].th[0].tid = 97;
	same("a different raw tid");

	/* the seen set keys on the bytes: two encodings forced onto one
	 * fingerprint (a collision) are two states, and a repeat is not new */
	uint64_t	fp[2] = {0x1234, 0x5678};
	int			first = mark(fp, (const unsigned char *) "abc", 3);
	int			other = mark(fp, (const unsigned char *) "abd", 3);
	int			again = mark(fp, (const unsigned char *) "abc", 3);

	printf("%s a fingerprint collision keeps both worlds\n", first && other && !again ? "ok  " : "FAIL");
	if (!(first && other && !again))
		failed = 1;
	return failed;
}
