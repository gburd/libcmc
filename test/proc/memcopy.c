/*
 * test/proc/memcopy.c: a data race made by a call to memcpy, memset or
 * memmove from instrumented code.  The instrumentation turns a plain
 * access into a call the race runtime sees, but gcc leaves these three
 * as calls to the C library, and with _FORTIFY_SOURCE (the default on
 * most distributions) both gcc and clang call __memcpy_chk and its
 * siblings instead.  The runtime must see those calls too.
 *
 * The mode, from the target args: a worker fills a shared buffer with
 * memcpy (cpy), memset (set) or memmove (move), while main reads the
 * buffer's first byte with a plain load.  Nothing orders the two, so
 * each is a data race.  In cpylock both hold a mutex: no race.  The
 * string and format modes fill it the same way with strcpy (scpy),
 * strncpy (sncpy), strcat (scat), stpcpy (spcpy), sprintf (spr),
 * snprintf (snpr) and vsnprintf (vsnpr).  Their source is a writable
 * array, so no compiler expands the call inline.
 *
 * Built with -DCHK the calls are the fortified __*_chk forms; without,
 * the plain ones.  The length comes from a volatile, so no compiler
 * inlines the call.
 *
 * Canonical state fd 3: [phase][value], phase 2 once main has joined.
 *
 *   cc -O1 -fno-pie [-DCHK] -fsanitize=thread -c test/proc/memcopy.c
 *   cc -no-pie -o memcopy memcopy.o libcmc-race.a -lpthread
 */
#undef _FORTIFY_SOURCE
#ifdef CHK
#define _FORTIFY_SOURCE 2
#endif
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char buf[32];
static const char src[32] = "abcdefghijklmnopqrstuvwxyz";
static char wsrc[32] = "abcdefghijklmnop";
static volatile size_t len = 16;
static const char *mode = "cpy";
static int	locked;

static void
emit(int phase, int value)
{
	unsigned char rec[3] = {2, (unsigned char) phase, (unsigned char) value};

	(void) !write(3, rec, sizeof rec);
}

static void
vfill(const char *fmt, ...)
{
	va_list		ap;

	va_start(ap, fmt);
	(void) vsnprintf(buf, len, fmt, ap);
	va_end(ap);
}

/* the string and format modes; 0 when mode is none of them */
static int
strfill(void)
{
	if (strcmp(mode, "scpy") == 0)
		strcpy(buf, wsrc);
	else if (strcmp(mode, "sncpy") == 0)
		strncpy(buf, wsrc, len);
	else if (strcmp(mode, "scat") == 0)
		strcat(buf, wsrc);
	else if (strcmp(mode, "spcpy") == 0)
		(void) stpcpy(buf, wsrc);
	else if (strcmp(mode, "spr") == 0)
		(void) sprintf(buf, "%s", wsrc);
	else if (strcmp(mode, "snpr") == 0)
		(void) snprintf(buf, len, "%s", wsrc);
	else if (strcmp(mode, "vsnpr") == 0)
		vfill("%s", wsrc);
	else
		return 0;
	return 1;
}

static void *
worker(void *arg)
{
	(void) arg;
	if (locked)
		pthread_mutex_lock(&mu);
	if (strfill())
		;
	else if (strcmp(mode, "set") == 0)
		memset(buf, 'x', len);
	else if (strcmp(mode, "move") == 0)
		memmove(buf, buf + 1, len);
	else
		memcpy(buf, src, len);
	if (locked)
		pthread_mutex_unlock(&mu);
	return NULL;
}

int
main(int argc, char **argv)
{
	pthread_t	t;
	int			seen;

	if (argc > 2)
		mode = argv[2];
	locked = strcmp(mode, "cpylock") == 0;
	emit(0, 0);
	pthread_create(&t, NULL, worker, NULL);
	if (locked)
		pthread_mutex_lock(&mu);
	seen = buf[0];
	if (locked)
		pthread_mutex_unlock(&mu);
	pthread_join(t, NULL);
	emit(2, seen == 0 || seen == 'a' || seen == 'x' ? 1 : 0);
	return 0;
}
