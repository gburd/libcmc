/*
 * counter.c: the guide's threaded node.  A worker thread counts the
 * requests the main thread hands it; both read and write `served`.  The
 * guide checks it with the race runtime, which reports the unlocked
 * build's data race.
 *
 *   cc -O1 -fno-pie -fsanitize=thread -c counter.c
 *   cc -no-pie -o counter counter.o libcmc-race.a -lpthread
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <unistd.h>

#ifndef COUNTER_LOCKED
#define COUNTER_LOCKED 1
#endif

static void
emit(int phase, int served)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) served};

	(void) !write(3, rec, sizeof rec);
}

/* !region count */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int	served;
static const int locked = COUNTER_LOCKED;	/* 0 in the bug build */

static void
count_one(void)
{
	if (locked)
		pthread_mutex_lock(&mu);
	served = served + 1;
	if (locked)
		pthread_mutex_unlock(&mu);
}
/* !endregion count */

static void *
worker(void *arg)
{
	(void) arg;
	count_one();
	return NULL;
}

int
main(void)
{
	pthread_t	t;

	emit(0, 0);
	pthread_create(&t, NULL, worker, NULL);
	count_one();
	pthread_join(t, NULL);
	emit(2, served);
	return 0;
}
