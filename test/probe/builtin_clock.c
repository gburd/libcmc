/*
 * probe/builtin_clock.c: docs/research/02 m2.  The same clock read through a
 * compiler builtin (no "asm" in the source), plus a read through an
 * absolute address.  Only the instruction scan can see these.
 */
#include "cmc.h"

uint64_t	probe_tsc(void);
uint64_t	probe_peek(void);

uint64_t
probe_tsc(void)
{
#if defined(__x86_64__) || defined(__i386__)
	return __builtin_ia32_rdtsc();
#else
	return 0;
#endif
}

uint64_t
probe_peek(void)
{
	return *(volatile uint64_t *) 0x7fff0000;
}
