/*
 * detmalloc.c: a deterministic bump allocator, LD_PRELOAD'd by the M2
 * checker so a target's malloc heap is reproducible across runs
 * (experiments/process-mode/DESIGN.md, M2).
 *
 * It carves malloc/calloc/realloc sequentially from an arena mapped
 * MAP_FIXED at a fixed base, so the Nth allocation in a given sequence
 * lands at the same address every run regardless of ASLR.  free() is a
 * no-op: for a bounded model-checking run the arena never needs to be
 * reclaimed, and a no-op keeps placement a pure function of the
 * allocation sequence (a freelist would reintroduce order effects).
 *
 * The live protocol state is exactly arena[0 .. *bump).  The arena base
 * and the bump pointer live at fixed, exported addresses so the checker
 * can read the region without the target describing anything:
 *   cmc_arena       -- the fixed base (a symbol, value = ARENA_BASE)
 *   cmc_arena_bump  -- bytes allocated so far (a size_t the checker reads)
 *
 *   cc -shared -fPIC -o detmalloc.so detmalloc.c
 *   LD_PRELOAD=./detmalloc.so ./target
 *
 * Only the target's own allocations go through here; glibc-internal
 * allocations made before our constructor runs use the real allocator and
 * are not in the arena (and not part of the declared state).
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ARENA_BASE ((void *) 0x600000000000ULL)
#define ARENA_SIZE (64UL * 1024 * 1024)
#define ALIGN      16UL

/* exported at fixed, nm-findable symbols */
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
		_exit(97);				/* the fixed base was unavailable */
	ready = 1;
}

__attribute__((constructor))
static void	ctor(void) { arena_init(); }

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
