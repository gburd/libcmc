/*
 * test/proc/atomic128.c: a handoff through a 16-byte atomic, the kind a
 * lock-free list uses for a pointer and its generation count.  Built
 * with -mcx16, the instrumentation turns each such operation into a
 * __tsan_atomic128_* call, which the race runtime must provide.  The
 * mode, from the target args, picks the program:
 *
 *   seqcst   the writer stores the payload, then publishes a 16-byte
 *            {generation, value} pair with a seq_cst store; the reader
 *            loads the pair and, if it is set, reads the payload.  No
 *            race, and the reader sees 7.
 *   relaxed  the same with relaxed atomics: nothing orders the payload,
 *            so the writer's store and the reader's load are a race.
 *   rmw      two workers each add 1 to both halves of the pair with one
 *            fetch_add, so a carry would show; after the joins main
 *            swaps the expected {2, 2} for {2, 5} with a compare-exchange
 *            and reports 2 * 10 + 5 = 25.
 *
 * Canonical state fd 3: [phase][value], phase 2 once main has joined.
 *
 *   cc -O1 -fno-pie -mcx16 -fsanitize=thread -c test/proc/atomic128.c
 *   cc -no-pie -o atomic128 atomic128.o libcmc-race.a -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <unistd.h>

typedef unsigned __int128 Pair;	/* generation in the high half */

static Pair pair;
static int	payload;
static int	seen;
static const char *mode = "seqcst";

static void
emit(int phase, int value)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) value};

	(void) !write(3, rec, sizeof rec);
}

static int
weak(void)
{
	return strcmp(mode, "relaxed") == 0 ? __ATOMIC_RELAXED : __ATOMIC_SEQ_CST;
}

static void *
writer(void *arg)
{
	(void) arg;
	payload = 7;
	__atomic_store_n(&pair, ((Pair) 1 << 64) | 1, weak());
	return NULL;
}

static void *
reader(void *arg)
{
	(void) arg;
	Pair		p = __atomic_load_n(&pair, weak());

	seen = p ? payload : 7;
	return NULL;
}

static void *
bumper(void *arg)
{
	(void) arg;
	__atomic_fetch_add(&pair, ((Pair) 1 << 64) | 1, __ATOMIC_SEQ_CST);
	return NULL;
}

int
main(int argc, char **argv)
{
	pthread_t	a, b;
	int			rmw;

	if (argc > 2)
		mode = argv[2];
	rmw = strcmp(mode, "rmw") == 0;
	emit(0, 0);
	pthread_create(&a, NULL, rmw ? bumper : writer, NULL);
	pthread_create(&b, NULL, rmw ? bumper : reader, NULL);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	if (rmw)
	{
		Pair		want = ((Pair) 2 << 64) | 2;

		if (__atomic_compare_exchange_n(&pair, &want, ((Pair) 2 << 64) | 5, 0,
										__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		{
			Pair		p = __atomic_load_n(&pair, __ATOMIC_SEQ_CST);

			seen = (int) (p >> 64) * 10 + (int) (unsigned long) p;
		}
		else
			seen = 0;
	}
	emit(2, seen);
	return 0;
}
