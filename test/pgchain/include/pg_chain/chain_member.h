/*-------------------------------------------------------------------------
 *
 * chain_member.h
 *	  The data-path protocol core of one chain member (PLAN sections 5.2,
 *	  5.3, 8.3), as a pure step function checked by libcmc.
 *
 * This is the static-projection core: the chain's members are nodes
 * 0..R-1 in order, node 0 the head and node R-1 the tail, and membership
 * does not change.  It includes the head's commit wait (5.2 steps 3-6):
 * a timeout, the noop compare-and-set, and the ambiguity wedge (5.5).
 * With reconfigs > 0 a configuration leader runs as node R and changes
 * the chain one epoch at a time (5.6): it removes a member it lost, adds
 * a restarted member back as repairing at the tail (8.2, 8.3), and
 * promotes it to tail once it holds what the sealed tail had.  The leader
 * stands in for the Paxos log.  Leases are not modelled yet.
 *
 * The core is the code the extension will run: the PostgreSQL glue turns
 * client commits, chain-log flushes and sender/receiver traffic into
 * events, and carries out the outputs (PLAN section 11, "How chainmc
 * stays honest").
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CHAIN_CHAIN_MEMBER_H
#define PG_CHAIN_CHAIN_MEMBER_H

#include <stdint.h>

#include "cmc.h"

#define CM_MAX_S		4		/* records per chain in one model instance */
#define CM_MAX_MEMBERS	CMC_MAX_NODES

/* message types; body = [type, epoch, a, b] */
enum
{
	CM_REC = 1,					/* a = s, b = value: a record, predecessor -> successor */
	CM_ACK,						/* a = s': every s <= s' is durable from here to the tail */
	CM_HELLO_P,					/* predecessor -> successor: report your state */
	CM_HELLO_S,					/* a = recv, b = ackable: successor's state */
	CM_SEAL,					/* leader -> survivor: stop taking writes in epoch */
	CM_SEALACK,					/* survivor -> leader */
	CM_ACTIVATE,				/* leader -> survivor: epoch e, a = new UPI bitmap */
	CM_REMOVED,					/* leader -> removed member */
	CM_HEARTBEAT,				/* member -> leader, once: lets a drop cut the
								 * pair, which the leader reads as a failure */
	CM_JOIN,					/* restarted member -> leader: re-add me (8.3) */
	CM_REPAIRDONE				/* repairing member -> leader: a = durable s */
};

typedef struct ChainMemberCfg
{
	int			members;		/* R, 1..CM_MAX_MEMBERS - 1 */
	int			reconfigs;		/* removals the leader may make; 0: no leader */
} ChainMemberCfg;

/*
 * Durable part, first in the state: the chain log.  log[s - 1] is the value
 * of s if bit s - 1 of present is set.  Whatever the core persists is
 * recorded, in any order: holes are the core's bug to show, not the
 * engine's to hide.  nlog is the highest s with no hole below it.
 */
typedef struct ChainMemberDurable
{
	uint8_t		present;
	uint8_t		nlog;
	uint8_t		log[CM_MAX_S];
	uint8_t		reloaded;		/* the log was emptied by repair (8.2) */
	uint8_t		pad[1];
} ChainMemberDurable;

/* a record whose transaction aborted before the sender took it (5.2 step 6) */
#define CM_NOOP 0
/* persist body [CM_TRUNCATE, 0]: empty the log (repair reloads it, 8.2) */
#define CM_TRUNCATE 0

/* client outcomes, in a reply's arg: s | value << 8 | outcome << 16 */
enum
{
	CM_COMMIT = 0,				/* the chain committed it */
	CM_ERROR,					/* definitely aborted: replaced by a noop */
	CM_AMBIGUOUS,				/* 08007: the chain may still commit it */
	CM_REFUSED					/* chain wedged, resolving: no s assigned */
};
#define CM_REPLY(s, v, o)	((uint32_t) (s) | (uint32_t) (v) << 8 | (uint32_t) (o) << 16)
#define CM_REPLY_S(a)		((a) & 0xffu)
#define CM_REPLY_V(a)		(((a) >> 8) & 0xffu)
#define CM_REPLY_O(a)		(((a) >> 16) & 0xffu)

typedef struct ChainMemberState
{
	ChainMemberDurable d;
	/* volatile: lost on a crash */
	uint8_t		wedged;			/* restarted: no further part (PLAN 8.3) */
	uint8_t		removed;		/* removed from the chain by the leader */
	uint8_t		epoch;			/* adopted epoch, from 1 */
	uint8_t		upi;			/* adopted UPI, bit i = member i; order = id order */
	uint8_t		rep;			/* adopted repairing set (at most one), after the UPI */
	uint8_t		sealed;			/* sealed in epoch: no new s */
	/* the leader (node members) only */
	uint8_t		l_phase;		/* 0 idle, 1 removing, 2 adding, 3 promoting */
	uint8_t		l_target;		/* member being removed, added or promoted */
	uint8_t		l_acks;			/* SEALACKs received, bitmap */
	uint8_t		l_nreconf;		/* epoch changes made */
	uint8_t		l_joins;		/* members that asked to rejoin */
	uint8_t		l_need;			/* promotion: highest recv the sealed UPI reported */
	uint8_t		l_repdone;		/* highest s the repairing member has durable, + 1;
								 * 0 = not heard from */
	uint8_t		next_s;			/* head: last s assigned */
	uint8_t		recv;			/* highest s received, contiguous */
	uint8_t		acked;			/* highest s acked by the successor */
	uint8_t		ack_sent;		/* highest s acked to the predecessor */
	uint8_t		sent;			/* head: highest s the sender has taken */
	uint8_t		answered;		/* head: bit s - 1 = s's client has its outcome */
	uint8_t		amb_s;			/* head: unresolved ambiguous s (5.5), or 0 */
	uint8_t		abort_wait;		/* head: bit s - 1 = s replaced by a noop, ERROR
								 * answered once the noop is durable */
	uint8_t		val[CM_MAX_S];	/* values of s <= recv; CM_NOOP for a noop */
	uint8_t		aborted_val[CM_MAX_S];	/* head: the value a noop replaced */
	uint8_t		epoch_first_s;	/* next_s when this epoch was adopted */
	uint8_t		hb_sent;		/* the heartbeat went out */
	uint8_t		repdone_sent;	/* repairing: last durable s reported, + 1 */
	uint8_t		lost_clients;	/* restarted: its clients' requests died with it */
	uint8_t		pad[2];
} ChainMemberState;

extern void chain_member_init(const void *cfg, int node, void *state);
extern void chain_member_step(const void *cfg, int node, void *state,
							  const CmcEvent *ev, CmcOut *out);
extern void chain_member_apply_persist(const void *cfg, int node, void *durable,
									   const uint8_t *body, uint16_t len);

#endif							/* PG_CHAIN_CHAIN_MEMBER_H */
