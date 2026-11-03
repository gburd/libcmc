/*
 * probe/asm_clock.c: docs/research/02 m2.  A lease check that reads the cycle
 * counter with inline asm.  nm -u sees nothing; the purity gate must
 * reject it from the instructions and from the source.
 */
#include "cmc.h"

uint8_t		probe_lease_ok(const uint8_t *s);

uint8_t
probe_lease_ok(const uint8_t *s)
{
	uint32_t	lo = 0,
				hi = 0;

#if defined(__x86_64__) || defined(__i386__)
	__asm__ volatile ("rdtsc":"=a" (lo), "=d"(hi));
#endif
	return (((uint64_t) hi << 32) | lo) < s[0];
}
