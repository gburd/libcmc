/*
 * cmc_spec.h: the abstract-model front end (cmc --spec FILE.cml).
 * See cmc_spec.c and docs/plans/tlaplus-scope.md (Milestone B).
 */
#ifndef CMC_SPEC_H
#define CMC_SPEC_H

/* Model-check the .cml specification at `path` on its own.  `defs` are
 * the -D tags given (to activate [TAG]-gated actions).  Returns an exit
 * status: 0 no violation, 1 a violation, 2 incomplete, 3 a model error. */
int cmc_spec_main(const char *path, char **defs, int ndefs);

#endif
