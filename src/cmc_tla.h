/*
 * cmc_tla.h: the TLA+ reader (cmc --spec FILE.tla --config FILE.cfg).
 * See cmc_tla.c.
 */
#ifndef CMC_TLA_H
#define CMC_TLA_H

#include <stddef.h>

/* Translate the TLA+ module at `tla`, with the TLC configuration at
 * `cfg`, to the .cml text it means.  Returns that text (malloc'd; the
 * caller frees it), or NULL with the reason in err: a construct outside
 * the subset cmc reads, named with its file and line, or an I/O error. */
char *cmc_tla_translate(const char *tla, const char *cfg, char *err, size_t errcap);

#endif
