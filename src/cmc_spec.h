/*
 * cmc_spec.h: the abstract-model front end (cmc --spec FILE.cml) and the
 * refinement hooks it exposes to the refinement checker (cmc --refine).
 * See cmc_spec.c and docs/plans/tlaplus-scope.md (Milestones B and C).
 */
#ifndef CMC_SPEC_H
#define CMC_SPEC_H

#include <stddef.h>

/* Model-check the .cml specification at `path` on its own.  `defs` are
 * the -D tags given (to activate [TAG]-gated actions).  Returns an exit
 * status: 0 no violation, 1 a violation, 2 incomplete, 3 a model error. */
int cmc_spec_main(const char *path, char **defs, int ndefs);

/* ---- refinement support (Milestone C) ------------------------------
 * The refinement checker runs an implementation and, at each reached
 * state, asks the abstract model whether the implementation's state --
 * mapped to the abstract variables by a binding -- satisfies the
 * abstract invariants.  The binding's right-hand sides are evaluated
 * over the implementation's OBSERVABLES, supplied through this accessor
 * (set by the refinement driver; the abstract evaluator calls it when an
 * expression mentions node[i] / node[i].field / nodes / replies). */
typedef struct {
	int nnodes;
	/* byte k of node i's observed canonical state, or -1 if out of range */
	long (*node_byte)(void *ctx, int i, int k);
	int  (*node_len)(void *ctx, int i);
	/* the reply log */
	int  nrepl;
	long (*repl_src)(void *ctx, int r);
	long (*repl_val)(void *ctx, int r);
	void *ctx;
} CmcObs;

/* Load a .cml spec (with its -D tags) for refinement; returns 0 ok, else
 * nonzero.  Then load the binding file (.bind) that maps the abstract
 * variables to observable expressions; returns 0 ok. */
int cmc_refine_load_spec(const char *path, char **defs, int ndefs);
int cmc_refine_load_bind(const char *path, char *err, size_t errcap);

/* At one reached implementation state, with observables `obs`, build the
 * abstract state via the binding and check the abstract invariants.
 * Returns the index of the first violated invariant (its name in
 * *name), -1 if all hold, -2 on an evaluation error (message in err). */
int cmc_refine_check(const CmcObs *obs, const char **name, char *err, size_t errcap);

/* Milestone C2: action-level refinement with stuttering.  cmc_refine_step
 * checks, in addition to the invariant (so it subsumes cmc_refine_check),
 * that the step from the previous reached implementation state to this
 * one maps under the binding to either a stutter (the abstraction is
 * unchanged) or a real abstract action step.  Returns -1 if fine, an
 * invariant index (>=0, name in *name) on an invariant violation, -3 if
 * the step matches no abstract action and is not a stutter (*name a
 * reason), -2 on an evaluation error (message in err).  Call
 * cmc_refine_reset() at the start of each walk/path to forget the
 * previous state. */
int cmc_refine_step(const CmcObs *obs, const char **name, char *err, size_t errcap);
void cmc_refine_reset(void);

/* Milestone D: translate the .cml spec to TLA+ on stdout (for TLC,
 * Apalache, TLAPS).  Returns 0 ok, 3 on a model error. */
int cmc_emit_tla(const char *path, char **defs, int ndefs);

#endif
