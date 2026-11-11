/*
 * cmc.h: the interface between cmc (an explicit-state model checker)
 * and the protocol cores it checks.  docs/research/01 4a.
 *
 * A core is a vtable over a fixed-size, pointer-free per-node state that
 * the engine treats as opaque bytes.  The engine owns the environment
 * (network bag, armed timers, pending persists and wakes, fault budgets)
 * and enables an action ONLY from something a core emitted (SEND,
 * SET_TIMER, PERSIST, WAKE) or from an environment budget (CLIENT, DROP,
 * DUP, CRASH).  It never reads a core's state to decide what can run:
 * that is the "omniscient oracle" that hid 8 lost-wake bugs in libxtc
 * (docs/research/01 1.2).  Invariants may read core state; they are never
 * consulted when computing enabled actions.
 *
 * Calibration exception (docs/research/01 4b.1): a shared-register TLA+ model
 * such as HummingConsensus has no messages.  Such a core sets
 * direct_actions; the world is then node 0's state alone, the action set
 * is whatever direct_actions returns (the TLA+ actions), and an action is
 * applied with step(CMC_EV_ACTION).  Only calibration cores may do this.
 *
 * The engine owns the durable part of a node's state (docs/research/02 B4): a
 * core changes it only through apply_persist(), which the engine calls on
 * FLUSH (and, for the records that reached disk, on CRASH).  step() must
 * leave the durable bytes alone on every event, FLUSHED included; the
 * engine checks this with memcmp.
 */
#ifndef CMC_H
#define CMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The release (semantic versioning).  The release workflows require the
 * tag to be v followed by exactly this string. */
#define CMC_VERSION		"0.5.0"

#define CMC_MAX_NODES	8
#define CMC_MSG_MAX		32		/* bytes per message body / persist record */
#define CMC_OUT_MAX		16		/* outputs per step */
#define CMC_MAX_ACTIONS	2048	/* enabled actions per state */
#define CMC_MAX_TIMERS	16
#define CMC_MAX_WAKES	8

/* An action, packed so the fingerprint table can store it.  The top byte
 * is the kind: env kinds below, or a core-defined kind in direct mode. */
typedef uint64_t CmcAction;

#define CMC_ACT(kind, a, b, c, d) \
	(((uint64_t)(kind) << 56) | ((uint64_t)(uint8_t)(a) << 48) | \
	 ((uint64_t)(uint8_t)(b) << 40) | ((uint64_t)(uint8_t)(c) << 32) | \
	 (uint64_t)(uint32_t)(d))
#define CMC_ACT_KIND(x)	((unsigned)((x) >> 56))
#define CMC_ACT_A(x)		((unsigned)(((x) >> 48) & 0xff))
#define CMC_ACT_B(x)		((unsigned)(((x) >> 40) & 0xff))
#define CMC_ACT_C(x)		((unsigned)(((x) >> 32) & 0xff))
#define CMC_ACT_D(x)		((uint32_t)(x))

/* Environment action kinds (actor mode).  a = node, b/c/d per kind. */
enum
{
	CMC_A_DELIVER = 1,			/* a = dst, b = src, d = index in net */
	CMC_A_DROP,					/* d = index in net (budget); fifo: resets the
								 * connection a-b */
	CMC_A_DUP,					/* d = index in net (budget; bag only) */
	CMC_A_FIRE,					/* a = node, b = timer id */
	CMC_A_FLUSH,					/* a = node: oldest pending persist completes */
	CMC_A_WAKE,					/* a = node, b = waiter id */
	CMC_A_CLIENT,				/* a = node (budget) */
	CMC_A_CRASH,					/* a = node, b = j (budget): the first j pending
								 * persists reached disk, then volatile is
								 * lost and the node RESTARTs */
	CMC_A_RESET					/* a = node, b = peer (fifo): node learns that
								 * its connection to peer was reset */
};

/* Events into a core. */
typedef enum CmcEventKind
{
	CMC_EV_RECV = 1,				/* src, body: a message from src */
	CMC_EV_TIMER,				/* arg = timer id */
	CMC_EV_FLUSHED,				/* body = the record that is now durable */
	CMC_EV_WAKE,					/* arg = waiter id */
	CMC_EV_CLIENT,				/* arg = request number at this node */
	CMC_EV_RESTART,				/* volatile state was zeroed */
	CMC_EV_ACTION,				/* direct mode only: action = the TLA+ action */
	CMC_EV_CONN_RESET			/* src = peer: the connection to src was reset
								 * (fifo); messages in flight on it are gone */
} CmcEventKind;

typedef struct CmcEvent
{
	uint8_t		kind;
	uint8_t		src;
	uint16_t	len;
	uint32_t	arg;
	CmcAction	action;
	const uint8_t *body;
} CmcEvent;

/* Outputs from a core. */
typedef enum CmcOutKind
{
	CMC_OUT_SEND = 1,			/* dst, body */
	CMC_OUT_SET_TIMER,			/* arg = timer id */
	CMC_OUT_CANCEL_TIMER,		/* arg = timer id */
	CMC_OUT_PERSIST,				/* body = record; FLUSHED delivers it later */
	CMC_OUT_WAKE,				/* arg = waiter id; WAKE delivered later */
	CMC_OUT_REPLY				/* arg = client-visible result (ghost log) */
} CmcOutKind;

typedef struct CmcOutput
{
	uint8_t		kind;
	uint8_t		dst;
	uint16_t	len;
	uint32_t	arg;
	uint8_t		body[CMC_MSG_MAX];
} CmcOutput;

typedef struct CmcOut
{
	int			n;
	int			overflow;		/* pushes refused past CMC_OUT_MAX */
	CmcOutput	o[CMC_OUT_MAX];
	CmcOutput	sink;			/* where a refused push writes */
} CmcOut;

/*
 * Cores emit only through cmc_out_push() (a grep gate bans indexing
 * out->o in core sources).  Past CMC_OUT_MAX it returns a scratch entry and
 * counts the overflow, which the engine reports as a violation instead of
 * corrupting memory (docs/research/02 M3).
 */
static inline CmcOutput *
cmc_out_push(CmcOut *out, uint8_t kind)
{
	CmcOutput   *o;

	if (out->n < CMC_OUT_MAX)
		o = &out->o[out->n++];
	else
	{
		out->overflow++;
		o = &out->sink;
	}
	memset(o, 0, sizeof(*o));
	o->kind = kind;
	return o;
}

/* The core vtable.  step() must be total and deterministic, read no
 * clock, randomness, globals or shared memory, and not allocate.  The
 * first durable_size bytes of a node's state are its durable part: only
 * apply_persist() changes them, and they survive CMC_A_CRASH. */
typedef struct CmcCore
{
	const char *name;
	size_t		state_size;		/* bytes per node */
	size_t		durable_size;	/* leading bytes that survive a crash */
	size_t		cfg_size;
	void		(*cfg_default) (void *cfg);
	int			(*cfg_set) (void *cfg, const char *key, const char *val);
	int			(*nnodes) (const void *cfg);
	void		(*init) (const void *cfg, int node, void *state);
	void		(*step) (const void *cfg, int node, void *state,
						 const CmcEvent *ev, CmcOut *out);
	/* applies one PERSIST record to the durable bytes; pure, no outputs.
	 * Required for a core that emits CMC_OUT_PERSIST. */
	void		(*apply_persist) (const void *cfg, int node, void *durable,
								  const uint8_t *body, uint16_t len);
	/* optional, for traces */
	void		(*describe) (const void *cfg, int node, const void *state, FILE *f);
	void		(*describe_msg) (const void *cfg, const uint8_t *body, FILE *f);
	/* calibration exception only; see the header comment */
	int			(*direct_actions) (const void *cfg, const void *state,
								   CmcAction *acts, int max);
	void		(*describe_action) (const void *cfg, CmcAction a, FILE *f);
	/* optional: does this node take CLIENT events (from cfg, never state)?
	 * NULL = every node does. */
	int			(*clients) (const void *cfg, int node);
} CmcCore;

/* ---- what properties see ------------------------------------------- */

typedef struct CmcRun CmcRun;

typedef struct CmcWorld
{
	const CmcRun *run;
	const uint8_t *bytes;
} CmcWorld;

typedef struct CmcWhy
{
	char		msg[512];
} CmcWhy;

const void *cmc_cfg(const CmcWorld *w);
int			cmc_nnodes(const CmcWorld *w);
const void *cmc_node(const CmcWorld *w, int node);
/* ghost client log (actor mode): replies emitted so far */
int			cmc_nreplies(const CmcWorld *w);
/* CLIENT events delivered to node so far (environment, not core state) */
int			cmc_requests(const CmcWorld *w, int node);
void		cmc_reply(const CmcWorld *w, int i, int *node, uint32_t *arg);
void		cmc_why(CmcWhy *why, const char *fmt,...)
			__attribute__((format(printf, 2, 3)));

typedef enum CmcPropKind
{
	CMC_INV = 1,					/* must hold in every reachable state */
	CMC_ACTION_PROP,				/* must hold on every transition */
	CMC_WITNESS,					/* must hold in SOME reachable state */
	CMC_QUIESCENT,				/* must hold in every state with no action
								 * (or whose only actions are timer fires
								 * that lead back to the same state) */
	CMC_EVENTUALLY				/* bounded liveness (actor mode): from every
								 * reachable state the fair schedule (no
								 * faults, no new clients) must reach a state
								 * where it holds within live_steps steps */
} CmcPropKind;

typedef struct CmcProp
{
	CmcPropKind	kind;
	const char *name;
	bool		(*state) (const CmcWorld *w, CmcWhy *why);
	bool		(*action) (const CmcWorld *pre, CmcAction a,
						   const CmcWorld *post, CmcWhy *why);
} CmcProp;

#define CMC_INVARIANT(n, fn)		{CMC_INV, #n, fn, NULL}
#define CMC_ACTION_PROPERTY(n, fn) {CMC_ACTION_PROP, #n, NULL, fn}
#define CMC_WITNESS_PROP(n, fn)	{CMC_WITNESS, #n, fn, NULL}
#define CMC_QUIESCENT_PROP(n, fn) {CMC_QUIESCENT, #n, fn, NULL}
#define CMC_EVENTUALLY_PROP(n, fn) {CMC_EVENTUALLY, #n, fn, NULL}

/* A model = a core plus the properties written against it. */
typedef struct CmcModel
{
	const char *name;
	const CmcCore *core;
	const CmcProp *props;
	int			nprops;
	int			nalways;		/* the first nalways props are always checked */
} CmcModel;

/* ---- the engine ------------------------------------------------------ */

typedef enum CmcSearch
{
	CMC_BFS = 1, CMC_DFS, CMC_SIMULATE, CMC_REPLAY
} CmcSearch;

/* Network model (actor mode).  FIFO: one ordered channel per (src, dst)
 * pair, as TCP/libpq; a crash or a DROP resets connections and the peers
 * get CMC_EV_CONN_RESET; no DUP.  BAG: an unordered multiset with DUP, for
 * transports without FIFO (a superset of FIFO behaviour for safety, so a
 * bag counterexample may be spurious for a FIFO protocol). */
typedef enum CmcNet
{
	CMC_NET_FIFO = 0, CMC_NET_BAG
} CmcNet;

typedef struct CmcOptions
{
	CmcSearch	search;
	int			max_depth;		/* DFS bound / simulate walk length */
	uint64_t	seed;
	uint64_t	walks;			/* simulate: number of walks */
	uint64_t	max_states;		/* 0 = unbounded; reaching it = INCOMPLETE */
	double		time_limit;		/* seconds; 0 = none */
	bool		det;			/* determinism gate */
	bool		stop_on_witness;	/* stop when every witness is reached */
	bool		quiet;			/* no trace or progress printing */
	bool		coverage;		/* print per-action-kind counts (TLC -coverage) */
	int			progress_s;		/* progress line period, 0 = off */
	const char *trace_out;		/* write a replay file for a counterexample */
	const char *replay_in;
	/* environment budgets (actor mode) */
	int			client_budget;	/* CLIENT events per node */
	int			drop_budget;
	int			dup_budget;
	int			crash_budget;
	int			net_cap;		/* messages in flight */
	int			persist_cap;	/* pending persists per node */
	int			reply_cap;
	CmcNet		net;
	int			live_steps;		/* CMC_EVENTUALLY bound K (fair steps) */
	FILE	   *out;
} CmcOptions;

typedef enum CmcVerdict
{
	CMC_PASS = 0, CMC_VIOLATION = 1, CMC_INCOMPLETE = 2, CMC_ERROR = 3
} CmcVerdict;

typedef struct CmcResult
{
	CmcVerdict	verdict;
	const char *prop;			/* violated property, or NULL */
	char		why[512];
	uint64_t	distinct;
	uint64_t	generated;
	uint64_t	left;			/* states left on the queue */
	int			depth;			/* TLC sense: longest path, init = 1 */
	int			trace_len;		/* states in the counterexample */
	int			fair_from;		/* CMC_EVENTUALLY: state where the fair suffix
								 * starts (1-based), 0 = none */
	CmcAction   *trace;			/* trace_len - 1 actions (malloc'd) */
	int			nwitness;
	const char *witness_name[16];
	int			witness_depth[16];	/* 0 = not reached */
	/* per action kind (top byte): states generated and new distinct states */
	uint64_t	cov_gen[256];
	uint64_t	cov_new[256];
	CmcAction	cov_sample[256];
	double		wall_s;
} CmcResult;

void		cmc_options_default(CmcOptions *o);
/* Builds a run for model m with cfg "k=v,k=v" and the properties named in
 * props (NULL-terminated; NULL = every property of the model). */
CmcRun	   *cmc_run_new(const CmcModel *m, const char *cfg, const char *const *props,
					   const CmcOptions *o, char *err, size_t errlen);
void		cmc_run_free(CmcRun *r);
size_t		cmc_world_size(const CmcRun *r);
void		cmc_check(CmcRun *r, CmcResult *res);
void		cmc_result_free(CmcResult *res);
void		cmc_describe_action(CmcRun *r, CmcAction a, FILE *f);
void		cmc_print_trace(CmcRun *r, const CmcAction *acts, int n, FILE *f);
int			cmc_write_replay(CmcRun *r, const char *path, const CmcAction *acts, int n);

/* lower level, used by tests */
void		cmc_world_init(CmcRun *r, uint8_t *w);
int			cmc_enabled(CmcRun *r, const uint8_t *w, CmcAction *acts, int max);
int			cmc_apply(CmcRun *r, uint8_t *w, CmcAction a);
void		cmc_fingerprint(const uint8_t *p, size_t n, uint64_t fp[2]);

/* ---- the command-line driver (src/cmc_cli.c) ------------------------- */

typedef struct CmcModelEntry
{
	const char *name;			/* the --model name */
	const CmcModel *m;
} CmcModelEntry;

/* Parses TLC-style options, runs the check and prints TLC-style output.
 * Returns the CmcVerdict as the exit status. */
int			cmc_main(int argc, char **argv, const CmcModelEntry *models, size_t nmodels);

#endif							/* CMC_H */
