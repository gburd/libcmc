/*
 * test/proc/threadpos.c: thread positions the world encoding cannot
 * tell apart (soundness plan 3.8).  A thread's record names the call it
 * is parked in (the call, its arguments, its instruction and stack
 * pointers), but two different points of a thread's run can share all
 * of them, and memory between two calls is not in the world at all.
 * Worlds that differ only there must not be merged.  Each mode emits a
 * state the program can reach, which the model forbids.
 *
 *   loop   a worker makes the same call twice from one call site (a
 *          loop), then emits 1.  The two iterations park at the same
 *          point with the same answer.
 *   seq    the main thread runs one thread and joins it, then runs a
 *          second, which emits 1.  glibc gives the second thread the
 *          first one's stack, so the two births look alike.
 *   order  two workers each make a call, update a shared variable s =
 *          s * 10 + id, and make a second call.  Which worker updates
 *          first is a search choice; the main thread joins both and
 *          emits s, 12 or 21.
 *   spin   a worker makes the same call forever, and the main thread
 *          joins it.  Without merging the search never ends, so only a
 *          bound stops it, and the result is INCOMPLETE.
 *   spin2  two workers do the same, so every world has two successors
 *          and the tree is wide rather than deep: a breadth-first search
 *          reaches a million worlds near depth 20, which keeps each
 *          replay short (docs/measurements/2026-11-17-tree-cap.md).
 *
 * The calls are state-hook writes of an unchanged record, so they are
 * visible operations that change nothing the world holds.
 *
 * Canonical state fd 3: [phase][value]; phase 1 with the mode's result.
 *
 *   cc -O1 -no-pie -o threadpos test/proc/threadpos.c -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static volatile int s;

static void
emit(int phase, int value)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) value};

	(void) !write(3, rec, sizeof rec);
}

static void *
loop(void *arg)
{
	(void) arg;
	for (volatile int i = 0; i < 2; i++)
		emit(0, 0);
	emit(1, 1);
	return NULL;
}

static void *
spin(void *arg)
{
	(void) arg;
	for (;;)
		emit(0, 0);
}

static void *
nothing(void *arg)
{
	return arg;
}

static void *
second(void *arg)
{
	(void) arg;
	emit(1, 1);
	return NULL;
}

static void *
step(void *arg)
{
	int			id = (int) (long) arg;

	emit(0, 0);
	s = s * 10 + id;
	emit(0, 0);
	return NULL;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 2 ? argv[2] : "";
	pthread_t	t, u;

	emit(0, 0);
	if (!strcmp(mode, "loop"))
	{
		pthread_create(&t, NULL, loop, NULL);
		pthread_join(t, NULL);
	}
	if (!strcmp(mode, "spin"))
	{
		pthread_create(&t, NULL, spin, NULL);
		pthread_join(t, NULL);
	}
	if (!strcmp(mode, "spin2"))
	{
		pthread_create(&t, NULL, spin, NULL);
		pthread_create(&u, NULL, spin, NULL);
		pthread_join(t, NULL);
	}
	if (!strcmp(mode, "seq"))
	{
		pthread_create(&t, NULL, nothing, NULL);
		pthread_join(t, NULL);
		pthread_create(&t, NULL, second, NULL);
		pthread_join(t, NULL);
	}
	if (!strcmp(mode, "order"))
	{
		pthread_create(&t, NULL, step, (void *) 1L);
		pthread_create(&u, NULL, step, (void *) 2L);
		pthread_join(t, NULL);
		pthread_join(u, NULL);
		emit(1, s);
	}
	return 0;
}
