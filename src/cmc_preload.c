/*
 * cmc_preload.c: the library every node of cmc --model-file runs under,
 * LD_PRELOADed by the process front end (cmc_proc.c).  It makes a node's
 * run a pure function of the actions the checker chose:
 *
 *   - a deterministic bump allocator: malloc/calloc/realloc carve
 *     sequentially from an arena mapped MAP_FIXED at a fixed base, so the
 *     Nth allocation lands at the same address every run, whatever ASLR
 *     does.  free() is a no-op: a bounded run never needs the memory
 *     back, and a freelist would reintroduce order effects.
 *   - clock_gettime, gettimeofday and getrandom through a real syscall.
 *     glibc serves the clock from the vDSO, which seccomp never sees; as
 *     syscalls, the checker answers them with its logical clock (it
 *     advances only when the search fires a timeout) and a fixed random
 *     pattern, so a replay reproduces the same times.
 *
 * Its constructor announces it to the checker with a clock_gettime on
 * the clock id CMC_PRELOAD_MAGIC, which no real clock uses; a node that
 * never announces it is refused, rather than checked against the real
 * clock.
 *
 * The functions below replace libc's, so they take libc's prototypes.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ARENA_BASE ((void *) 0x600000000000ULL)
#define ARENA_SIZE (64UL * 1024 * 1024)
#define ALIGN      16UL

/* exported at fixed, nm-findable symbols */
extern void *cmc_arena;
extern size_t cmc_arena_bump, cmc_arena_size;
void	   *cmc_arena = ARENA_BASE;
size_t		cmc_arena_bump = 0;
size_t		cmc_arena_size = ARENA_SIZE;

static int	ready;

static void
arena_init(void)
{
	if (ready)
		return;
	void	   *p = mmap(ARENA_BASE, ARENA_SIZE, PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p != ARENA_BASE)
	{
		static const char msg[] = "cmc-preload: cannot map the arena at its fixed base\n";

		(void) !write(2, msg, sizeof msg - 1);
		_exit(97);
	}
	ready = 1;
}

#define CMC_PRELOAD_MAGIC 0x434d43	/* "CMC"; cmc_proc.c has the same */

__attribute__((constructor))
static void
ctor(void)
{
	struct timespec ts;

	arena_init();
	(void) syscall(SYS_clock_gettime, CMC_PRELOAD_MAGIC, &ts);
}

static void *
bump(size_t n)
{
	arena_init();
	size_t		off = (cmc_arena_bump + (ALIGN - 1)) & ~(ALIGN - 1);
	if (off + n > ARENA_SIZE)
		return NULL;
	cmc_arena_bump = off + n;
	return (unsigned char *) ARENA_BASE + off;
}

void *
malloc(size_t n)
{
	return bump(n ? n : 1);
}

void
free(void *p)
{
	(void) p;					/* no-op: see the header comment */
}

void *
calloc(size_t a, size_t b)
{
	size_t		n = a * b;
	void	   *p = bump(n ? n : 1);
	/* the arena is freshly mmap'd zero and never reused, so it is already
	 * zero; memset anyway to honour the contract exactly */
	if (p)
		memset(p, 0, n);
	return p;
}

void *
realloc(void *old, size_t n)
{
	void	   *p = bump(n ? n : 1);
	if (p && old)
		memcpy(p, old, n);		/* bounded run: copying n may over-read the
								 * old block, but our blocks are arena-resident
								 * and the arena is readable, so it is safe */
	return p;
}

/*
 * M5b -- absolute-time and randomness shims.  glibc serves clock_gettime
 * and gettimeofday from the vDSO, so seccomp never sees them; forcing
 * them through a real syscall() makes them visible to the checker, which
 * answers with a LOGICAL clock it advances only at timeout events (so
 * stateless replay stays deterministic).  getrandom is routed the same
 * way so the value a target reads is engine-chosen, not real entropy.
 * These override the libc symbols because this .so is LD_PRELOAD'd first.
 */
int
clock_gettime(clockid_t id, struct timespec *ts)
{
	return syscall(SYS_clock_gettime, id, ts);
}

int
gettimeofday(struct timeval *restrict tv, void *restrict tz)
{
	return syscall(SYS_gettimeofday, tv, tz);
}

ssize_t
getrandom(void *buf, size_t len, unsigned int flags)
{
	return syscall(SYS_getrandom, buf, len, flags);
}
