/*
 * toy.h: private state of the toy lock-server core.  Included by
 * toy_core.c (the pure step code) and toy_model.c (cfg, descriptions,
 * properties), never by the engine.
 */
#ifndef CMC_TOY_H
#define CMC_TOY_H

#include "cmc.h"

enum
{
	M_REQ = 1, M_GRANT, M_REL
};

typedef struct ToyState
{
	uint8_t		phase;			/* clients */
	uint8_t		holder;			/* server */
	uint8_t		nq;
	uint8_t		q[2];
	uint8_t		pad[3];
} ToyState;

typedef struct ToyCfg
{
	int			clients;
} ToyCfg;

/* one copy of the step code per CMC_TOY_BUG variant links into one binary */
#ifndef CMC_TOY_BUG
#define CMC_TOY_BUG 0
#endif
#define TOY_CAT2(a, b) a##b
#define TOY_CAT(a, b) TOY_CAT2(a, b)
#define toy_step TOY_CAT(toy_step_v, CMC_TOY_BUG)
#define toy_init TOY_CAT(toy_init_v, CMC_TOY_BUG)

void		toy_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out);
void		toy_init(const void *cfg, int node, void *sv);

#endif							/* CMC_TOY_H */
