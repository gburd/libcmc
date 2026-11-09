/*
 * cmc_proc.h: the process front end (cmc_proc.c), internal to libcmc.
 * cmc_main() calls cmc_proc_main() for cmc --model-file.
 */
#ifndef CMC_PROC_H
#define CMC_PROC_H

#include <stddef.h>
#include <stdint.h>

typedef struct CmcProcOptions
{
	const char *model_file;
	const char *binary;			/* NULL: the model file's [target] binary */
	int			simulate;		/* 0 breadth-first, 1 random walks */
	int			fast;			/* snapshot-DFS instead of replay (--fast) */
	uint64_t	seed, walks;
	long		depth;			/* -1: the model file's, else unbounded */
	size_t		max_states;		/* 0: no limit */
	double		time_limit;		/* seconds; 0: no limit */
	int			crashes, drops, timeouts;	/* -1: the model file's budget */
} CmcProcOptions;

int			cmc_proc_main(const CmcProcOptions *po);

#endif
