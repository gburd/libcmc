/*
 * toy_core.c: a lock server and two clients, cmc's toy core for the
 * engine's own tests (a toy core with a planted bug must be caught).  Unlike the HC calibration core it is a real message-passing
 * core: the engine enables actions only from its SEND / SET_TIMER outputs
 * and from the CLIENT budget.
 *
 * Node 0 is the server {holder, queue}; nodes 1, 2 are clients with a
 * phase: 0 idle, 1 waiting, 2 in the critical section, 3 done.
 *   client  CLIENT        -> phase 1, SEND REQ to 0
 *   server  RECV REQ(c)   -> free: holder = c, SEND GRANT; else queue c
 *   client  RECV GRANT    -> phase 2, SET_TIMER 0
 *   client  TIMER 0       -> phase 3, SEND REL to 0, REPLY
 *   server  RECV REL      -> holder = 0; queue non-empty: grant the first
 *
 * Planted bugs (compile with -DMC_TOY_BUG=n):
 *   1  the server grants on REQ without checking holder   -> Mutex
 *   2  the server forgets the queued client on REL (a lost wake: the
 *      client waits forever)                              -> AllReplied
 *   3  a client leaving the critical section goes back to idle instead
 *      of done                                            -> PhaseMonotone
 *
 * Exact reachable state count of the correct core with one request per
 * client, derived by hand (test_cmc.c checks it): each client is in one of
 * a idle, b REQ in flight, c queued, d GRANT in flight, e in CS, f REL in
 * flight, g done; server state and the network follow from the pair.
 * Allowed pairs: {a,b,g}^2 = 9, one of {d,e,f} with one of {a,b,g} = 18,
 * one queued while the other holds = 6.  Total 33; the deepest is (g,g)
 * after 10 steps, so the TLC-style depth is 11.
 */
#include "toy.h"

#include <string.h>

static void
send(CmcOut *out, int dst, uint8_t kind)
{
	CmcOutput   *o = cmc_out_push(out, CMC_OUT_SEND);

	o->dst = (uint8_t) dst;
	o->len = 1;
	o->body[0] = kind;
}

static void
emit(CmcOut *out, uint8_t kind, uint32_t arg)
{
	cmc_out_push(out, kind)->arg = arg;
}

void
toy_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	ToyState   *s = sv;

	(void) cfg;
	out->n = 0;
	if (node == 0)
	{
		if (ev->kind != CMC_EV_RECV || ev->len != 1)
			return;
		if (ev->body[0] == M_REQ)
		{
#if CMC_TOY_BUG == 1
			s->holder = ev->src;
			send(out, ev->src, M_GRANT);
#else
			if (s->holder == 0)
			{
				s->holder = ev->src;
				send(out, ev->src, M_GRANT);
			}
			else if (s->nq < 2)
				s->q[s->nq++] = ev->src;
#endif
		}
		else if (ev->body[0] == M_REL && s->holder == ev->src)
		{
			s->holder = 0;
#if CMC_TOY_BUG == 2
			s->nq = 0;
			memset(s->q, 0, sizeof(s->q));
#else
			if (s->nq > 0)
			{
				s->holder = s->q[0];
				s->q[0] = s->q[1];
				s->q[1] = 0;
				s->nq--;
				send(out, s->holder, M_GRANT);
			}
#endif
		}
		return;
	}
	switch (ev->kind)
	{
		case CMC_EV_CLIENT:
			if (s->phase == 0)
			{
				s->phase = 1;
				send(out, 0, M_REQ);
			}
			break;
		case CMC_EV_RECV:
			if (ev->len == 1 && ev->body[0] == M_GRANT && s->phase == 1)
			{
				s->phase = 2;
				emit(out, CMC_OUT_SET_TIMER, 0);
			}
			break;
		case CMC_EV_TIMER:
			if (s->phase == 2)
			{
#if CMC_TOY_BUG == 3
				s->phase = 0;
#else
				s->phase = 3;
#endif
				send(out, 0, M_REL);
				emit(out, CMC_OUT_REPLY, (uint32_t) node);
			}
			break;
		default:
			break;
	}
}

void
toy_init(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, sizeof(ToyState));
}
