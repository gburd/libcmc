/*
 * test/proc/memrace.c: memory interleavings between two threads (plan
 * 2.1, item 3).  The mode, from the target args, picks the program:
 *
 *   unlocked   two workers each add 1 to a shared counter with a plain
 *              load and store, no lock and no system call between them.
 *              Linux can interleave the two loads before either store
 *              and lose an update; an engine whose choice points are
 *              system calls and locks never does.  Instrumented, the
 *              race runtime reports the data race.
 *   locked     the same increment under a pthread mutex: no race, and
 *              the count is always 2.
 *   seqcst     a handoff through a seq_cst atomic flag: the reader sees
 *              the payload the writer stored before setting the flag.
 *   relaxed    the same handoff with relaxed atomics: no happens-before
 *              edge orders the payload, so the writer's store and the
 *              reader's load are a data race, reported.
 *   relaxedadd two workers add 1 with a relaxed atomic fetch_add, and
 *              main reads the count after joining: no race, but the
 *              search runs only sequentially consistent orders, so a
 *              pass that used a relaxed atomic is not a verdict.
 *
 * Canonical state fd 3: [phase][value].  phase 2 once both workers have
 * finished and main has joined them; value is the count, or what the
 * reader saw.  proc_gate writes one model per mode.
 *
 *   cc -O1 -no-pie -fno-pie [-fsanitize=thread -c ... libcmc-race.a] \
 *      -o memrace test/proc/memrace.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int	counter;
static int	payload;
static atomic_int flag;
static int	seen;
static const char *mode = "unlocked";

static void
emit(int phase, int value)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) value};

	(void) !write(3, rec, sizeof rec);
}

static void *
adder(void *arg)
{
	(void) arg;
	if (strcmp(mode, "locked") == 0)
		pthread_mutex_lock(&mu);
	int			v = counter;

	counter = v + 1;
	if (strcmp(mode, "locked") == 0)
		pthread_mutex_unlock(&mu);
	return NULL;
}

static atomic_int rcount;

static void *
radder(void *arg)
{
	(void) arg;
	atomic_fetch_add_explicit(&rcount, 1, memory_order_relaxed);
	return NULL;
}

static void *
writer(void *arg)
{
	(void) arg;
	payload = 7;
	if (strcmp(mode, "relaxed") == 0)
		atomic_store_explicit(&flag, 1, memory_order_relaxed);
	else
		atomic_store(&flag, 1);
	return NULL;
}

static void *
reader(void *arg)
{
	(void) arg;
	int			f = strcmp(mode, "relaxed") == 0 ?
		atomic_load_explicit(&flag, memory_order_relaxed) : atomic_load(&flag);

	seen = f ? payload : 7;
	return NULL;
}

int
main(int argc, char **argv)
{
	pthread_t	a, b;
	int			handoff;

	if (argc > 2)
		mode = argv[2];
	handoff = strcmp(mode, "seqcst") == 0 || strcmp(mode, "relaxed") == 0;
	int			radd = strcmp(mode, "relaxedadd") == 0;

	emit(0, 0);
	pthread_create(&a, NULL, radd ? radder : handoff ? writer : adder, NULL);
	pthread_create(&b, NULL, radd ? radder : handoff ? reader : adder, NULL);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	emit(2, radd ? atomic_load(&rcount) : handoff ? seen : counter);
	return 0;
}
