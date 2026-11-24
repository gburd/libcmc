/*-------------------------------------------------------------------------
 *
 * paxos.h
 *	  The configuration log's consensus core (PLAN section 7.2, research/03
 *	  section 3), as a pure step function checked by libcmc.
 *
 * Multi-Paxos over a fixed set of voters: nodes 0..V-1 are acceptors, and
 * the first C of them are also candidates (proposers that may lead).  A
 * leader runs phase 1 once for every slot, then phase 2 slot by slot.
 * Ballots are (round, node), slots and ballots are separate numbers
 * (research/03 section 3.8), and every promise or accepted reply is sent
 * only after the acceptor's state is durable.  A client submits a log
 * entry at a candidate and is told it was chosen (COMMITTED), may have
 * been chosen (AMBIGUOUS: the leader was preempted after phase 2 began),
 * or was never proposed (REFUSED).
 *
 * Voter-set changes (PLAN 7.2 V1-V3, research/03 3.6): a log value can be
 * a VoterCfg, a new member bitmap, and governs every slot after its own.
 * The configuration of slot k is therefore a function of the values chosen
 * at slots below k, and a leader proposes at slot k only once it has
 * learned every slot below it (alpha = 1 with no pipelining at all, which
 * a log written a few times a minute does not miss).  Phase 1 is one
 * PREPARE to every node; before
 * proposing at slot k the leader needs a q1 of slot k's configuration,
 * tallied over that configuration's members only (V2), and recovers slot
 * k from their promises; phase 2 for slot k goes to that configuration
 * (V3).  The initial configuration is nodes 0..V-1 with the cfg's q1 and
 * q2; any other uses a majority of its members for both.  Raft's rule V4
 * (commit an entry in your own term before a change) is not needed: two
 * leaders can only disagree on slot k's configuration by disagreeing on
 * a chosen value below k, which Paxos rules out.
 * shortcut: counting quorums, not src/quorum/ expressions; the glue will
 * call qexpr_is_quorum() where this calls is_q().  Retiring an old
 * configuration and catching up a new voter (V5) are the glue's.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CHAIN_PAXOS_H
#define PG_CHAIN_PAXOS_H

#include <stdint.h>

#include "cmc.h"

#define PX_MAX_SLOTS	3
#define PX_MAX_NODES	5
#define PX_NOOP			0xfe	/* the value a leader fills a gap with */

/* messages; body = [type, ballot, ...] */
enum
{
	PX_PREPARE = 1,				/* phase 1a, all slots */
	PX_PROMISE,					/* phase 1b: acc_b[S], acc_v[S] follow */
	PX_ACCEPT,					/* phase 2a: slot, value */
	PX_ACCEPTED,				/* phase 2b: slot, value */
	PX_NACK,					/* ballot rejected: the acceptor's promise */
	PX_COMMIT					/* leader -> learners: slot, value is chosen */
};

/* persist records */
enum
{
	PX_P_PROMISE = 1,			/* [type, ballot] */
	PX_P_ACCEPT,				/* [type, ballot, slot, value] */
	PX_P_BALLOT					/* [type, ballot]: a candidate's own ballot */
};

/* client outcomes; a reply's arg is PX_REPLY(slot, value, outcome, ballot) */
enum
{
	PX_COMMITTED = 0,
	PX_AMBIGUOUS,
	PX_REFUSED
};
#define PX_REPLY(s, v, o, n) \
	((uint32_t) (s) | (uint32_t) (v) << 8 | (uint32_t) (o) << 16 | (uint32_t) (n) << 24)
#define PX_REPLY_S(a)		((a) & 0xffu)
#define PX_REPLY_V(a)		(((a) >> 8) & 0xffu)
#define PX_REPLY_O(a)		(((a) >> 16) & 0xffu)
#define PX_REPLY_B(a)		(((a) >> 24) & 0xffu)

/* ballot = round << 3 | node; round >= 1, so 0 means "none" */
#define PX_BALLOT(r, n)		((uint8_t) ((r) << 3 | (n)))
#define PX_ROUND(b)			((b) >> 3)
#define PX_NODE(b)			((b) & 7)

/* a VoterCfg value: 0x80 | the new member bitmap */
#define PX_VCFG(mask)		((uint8_t) (0x80 | (mask)))
#define PX_IS_VCFG(v)		((v) != PX_NOOP && ((v) & 0x80))
#define PX_VCFG_MASK(v)		((uint8_t) ((v) & 0x7f))

/* a client's value: unique per (node, request) */
#define PX_VALUE(n, req)	((uint8_t) (1 + (n) * 4 + (req)))
#define PX_VALUE_NODE(v)	(((v) - 1) / 4)

/* proposer phases */
typedef enum PaxosPhase
{
	PX_PH_FOLLOWER = 0,
	PX_PH_BALLOT,				/* waiting for its new ballot to be durable */
	PX_PH_PREPARE,				/* phase 1, before the first proposal */
	PX_PH_LEAD					/* has proposed; drives the log slot by slot */
} PaxosPhase;

typedef struct PaxosCfg
{
	int			nodes;			/* all nodes, voters or not, V..PX_MAX_NODES */
	int			voters;			/* V: the initial configuration is nodes 0..V-1 */
	int			candidates;		/* C, 1..V: nodes 0..C-1 may lead */
	int			change;			/* node 0's first request is VoterCfg(change); 0 = none */
	int			change2;		/* node 1's first request is VoterCfg(change2) */
	int			q1;				/* phase-1 quorum size of the initial configuration */
	int			q2;				/* its phase-2 quorum size; q1 + q2 > V */
	int			slots;			/* log length, 1..PX_MAX_SLOTS */
	int			max_round;		/* highest ballot round a candidate tries */
} PaxosCfg;

/*
 * Durable part, first in the state: the acceptor, and the highest ballot
 * this node has campaigned with, so a restarted candidate never reuses
 * one (pgchain.acceptor in PLAN 7.2).
 */
typedef struct PaxosDurable
{
	uint8_t		promised;		/* highest ballot promised */
	uint8_t		last_ballot;	/* highest ballot campaigned with */
	uint8_t		acc_b[PX_MAX_SLOTS];	/* ballot of the accepted value, 0 = none */
	uint8_t		acc_v[PX_MAX_SLOTS];
} PaxosDurable;

typedef struct PaxosState
{
	PaxosDurable d;
	/* volatile: lost on a crash */
	uint8_t		prom_v;			/* acceptor: highest ballot promised, durable or not */
	uint8_t		phase;			/* proposer: PaxosPhase */
	uint8_t		ballot;			/* proposer: its current ballot */
	uint8_t		max_round;		/* highest round seen in a NACK */
	uint8_t		p1;				/* promises for ballot, bitmap */
	uint8_t		gave_up;		/* out of rounds: refuses clients */
	uint8_t		pb[PX_MAX_NODES][PX_MAX_SLOTS];	/* phase 1: what each promise
												 * reported, ballot ... */
	uint8_t		pv[PX_MAX_NODES][PX_MAX_SLOTS];	/* ... and value, per slot */
	uint8_t		prop_v[PX_MAX_SLOTS];	/* phase 2: value proposed at ballot */
	uint8_t		p2[PX_MAX_SLOTS];	/* phase 2: accepteds, bitmap */
	uint8_t		learned[PX_MAX_SLOTS];	/* learner: chosen value, 0 = unknown */
	uint8_t		pending;		/* client value awaiting an outcome, 0 = none */
	uint8_t		pending_slot;	/* its slot at the current ballot, or PX_NO_SLOT */
	uint8_t		pad[3];
} PaxosState;

#define PX_NO_SLOT 0xff

/*
 * The configuration governing slot k, given the values chosen at slots
 * 0..k-1 (log[j] = 0 if unknown): members bitmap and quorum sizes.  The
 * core, the model's properties and the glue all use this one function.
 */
extern void paxos_config_at(const PaxosCfg *cfg, const uint8_t *log, int k,
							uint8_t *members, int *q1, int *q2);

extern void paxos_init(const void *cfg, int node, void *state);
extern void paxos_step(const void *cfg, int node, void *state,
					   const CmcEvent *ev, CmcOut *out);
extern void paxos_apply_persist(const void *cfg, int node, void *durable,
								const uint8_t *body, uint16_t len);

#endif							/* PG_CHAIN_PAXOS_H */
