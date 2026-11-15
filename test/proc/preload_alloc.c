/*
 * preload_alloc.c: the preload's allocator (src/cmc_preload.c) under its
 * own names, checked directly.  calloc's size product and the arena
 * bound must not wrap, so an impossible request fails with NULL instead
 * of returning memory already in use; realloc copies the old block's
 * size, not the new one, so growing a block never copies its neighbour.
 * Exits 0 when every check holds.
 */
#define malloc pa_malloc
#define calloc pa_calloc
#define realloc pa_realloc
#define free pa_free
#define posix_memalign pa_posix_memalign
#define aligned_alloc pa_aligned_alloc
#define memalign pa_memalign
#define clock_gettime pa_clock_gettime
#define gettimeofday pa_gettimeofday
#define time pa_time
#define getrandom pa_getrandom
#include "../../src/cmc_preload.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

#include <stdio.h>

static int	failed;

static void
check(int ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failed = 1;
}

int
main(void)
{
	volatile size_t huge = SIZE_MAX;

	check(pa_calloc(huge / 2 + 1, 2) == NULL, "calloc whose product overflows is NULL");
	check(pa_malloc(huge) == NULL, "malloc(SIZE_MAX) is NULL");
	check(pa_malloc(huge - 8) == NULL, "malloc near SIZE_MAX is NULL");

	unsigned char *a = pa_malloc(16), *b = pa_malloc(64);
	memset(a, 'A', 16);
	memset(b, 'B', 64);
	unsigned char *c = pa_realloc(a, 64);
	int		copied = c != NULL;
	for (int i = 0; copied && i < 16; i++)
		copied = c[i] == 'A';
	int		clean = c != NULL;
	for (int i = 16; clean && i < 64; i++)
		clean = c[i] == 0;
	check(copied, "realloc keeps the old block's bytes");
	check(clean, "realloc copies only the old block's size");
	check(pa_realloc(c, 8) != NULL && c[0] == 'A', "realloc to a smaller size keeps its prefix");
	return failed;
}
