/*
 * cmc.c: the cmc engine.  Explicit-state BFS (TLC semantics), DFS with
 * a depth bound, seeded random walks and replay, over a world made of
 * opaque per-node core states plus an environment the engine owns.
 * Design: docs/research/01 4a.  Interface: cmc.h.
 *
 * World bytes (actor mode):
 *   node[0..nn-1]   core states, state_size bytes each (opaque)
 *   env header      budgets left, then per node: client_left, timers (u16),
 *                   pending-persist count, wakes (u8), resets (u8: bit p =
 *                   a CONN_RESET from peer p is pending)
 *   net             net_cap entries {dst, src, len, body[CMC_MSG_MAX], count}.
 *                   bag: sorted by (dst, src, len, body), equal messages
 *                   share an entry.  fifo: sorted by (dst, src), send order
 *                   within a pair, count 1; only a pair's first entry can be
 *                   delivered.  Unused entries zero
 *   persists        nn x persist_cap FIFO entries {len, body}
 *   replies         reply_cap entries {node, arg[4]} in emission order
 * padded with zeros to a multiple of 8.  Every part is canonical, so the
 * bytes are the encoding.  Direct mode (calibration cores) has one state
 * blob and no environment.
 */
#include "cmc.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CMC_CFG_MAX 256
#define CMC_MAX_PROPS 64

#define MSG_KEY (3 + CMC_MSG_MAX)
#define MSG_ENT (MSG_KEY + 1)
#define PERS_ENT (1 + CMC_MSG_MAX)
#define REP_ENT 5
#define HDR_FIXED 8
#define HDR_NODE 6

typedef struct Ent
{
	uint64_t	fp0,
				fp1,
				p0,
				p1;
	CmcAction	act;
	uint32_t	depth;
	uint32_t	flags;			/* 1 = used, 2 = root */
} Ent;

typedef struct Table
{
	Ent		   *e;
	uint64_t	cap;			/* power of two */
	uint64_t	n;
} Table;

typedef struct QChunk
{
	struct QChunk *next;
	size_t		head,
				tail;
	uint8_t		data[];
} QChunk;

typedef struct Queue
{
	QChunk	   *first,
			   *last;
	size_t		per;			/* worlds per chunk */
	size_t		ws;
	uint64_t	n;
} Queue;

struct CmcRun
{
	const CmcModel *m;
	const CmcCore *core;
	uint8_t		cfg[CMC_CFG_MAX];
	char		cfgstr[512];
	CmcOptions	o;
	bool		direct;
	int			nn;
	size_t		ss;
	size_t		off_hdr,
				off_msgs,
				off_pers,
				off_reps,
				ws;
	const CmcProp *inv[CMC_MAX_PROPS],
			   *actp[CMC_MAX_PROPS],
			   *wit[CMC_MAX_PROPS],
			   *qui[CMC_MAX_PROPS],
			   *ev[CMC_MAX_PROPS],
			   *sel[CMC_MAX_PROPS];	/* the named (not always) props */
	int			ninv,
				nactp,
				nwit,
				nqui,
				nev,
				nsel;
	int			wit_depth[CMC_MAX_PROPS];
	Table		t;
	/* scratch, sized per run (docs/research/02 M1) */
	uint8_t    *dur,			/* durable bytes before a step */
			   *detst,			/* determinism gate: state copy */
			   *qw;				/* quiescence probe world */
	CmcOut	   *deto;			/* determinism gate: outputs */
	/* bounded liveness (CMC_EVENTUALLY): fair-schedule buffers and a memo
	 * of states known to reach the goal (Ent.depth = fair steps to it) */
	Table		live;
	uint8_t    *fw,
			   *fw0;			/* fw0: spare world */
	CmcAction   *fsuf;			/* the fair suffix of the last failure */
	int			nfsuf;
	CmcAction   *facts;			/* enabled actions inside the fair run */
	uint64_t   *ffp;			/* round-start fingerprints of the fair run */
	int		   *fstep;			/* fair steps taken at each round start */
	int			fair_from;		/* of the last result, for traces */
	/* bound/gate error from the last apply */
	char		err[256];
};

/* ------------------------------------------------------------------ */
/* small helpers */

void
cmc_why(CmcWhy *why, const char *fmt,...)
{
	va_list		ap;

	if (!why)
		return;
	va_start(ap, fmt);
	vsnprintf(why->msg, sizeof(why->msg), fmt, ap);
	va_end(ap);
}

static double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void *
xmalloc(size_t n)
{
	void	   *p = malloc(n);

	if (!p)
	{
		fprintf(stderr, "cmc: out of memory (%zu bytes)\n", n);
		exit(3);
	}
	return p;
}

static void *
xcalloc(size_t n, size_t sz)
{
	void	   *p = calloc(n, sz);

	if (!p)
	{
		fprintf(stderr, "cmc: out of memory (%zu x %zu bytes)\n", n, sz);
		exit(3);
	}
	return p;
}

/* ------------------------------------------------------------------ */
/* fingerprint: two independent 64-bit hashes over 8-byte words */

static inline uint64_t
rotl64(uint64_t x, int r)
{
	return (x << r) | (x >> (64 - r));
}

static inline uint64_t
fmix64(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	k *= 0xc4ceb9fe1a85ec53ULL;
	k ^= k >> 33;
	return k;
}

static inline uint64_t
smix64(uint64_t z)
{
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static inline uint64_t
load64(const uint8_t *p)
{
	uint64_t	v;

	memcpy(&v, p, 8);
	return v;
}

/* n must be a multiple of 8 (worlds are padded). */
void
cmc_fingerprint(const uint8_t *p, size_t n, uint64_t fp[2])
{
	/* h1: murmur3-style word mix; h2: xor-rotate-multiply with splitmix
	 * finaliser.  Different constructions and constants. */
	uint64_t	h1 = 0x9368e53c2f6af274ULL ^ (n * 0x87c37b91114253d5ULL);
	uint64_t	h2 = 0x586dcd208f7cd3fdULL + n;

	for (size_t i = 0; i < n; i += 8)
	{
		uint64_t	k = load64(p + i);
		uint64_t	k1 = k * 0x87c37b91114253d5ULL;

		k1 = rotl64(k1, 31);
		k1 *= 0x4cf5ad432745937fULL;
		h1 ^= k1;
		h1 = rotl64(h1, 27) * 5 + 0x52dce729;

		h2 = rotl64(h2 ^ (k + 0x9e3779b97f4a7c15ULL), 23) * 0xd6e8feb86659fd93ULL;
		h2 += i;
	}
	fp[0] = fmix64(h1);
	fp[1] = smix64(h2);
	if (fp[0] == 0 && fp[1] == 0)
		fp[0] = 1;
}

/* ------------------------------------------------------------------ */
/* fingerprint table: open addressing, linear probing */

static void
table_init(Table *t, uint64_t cap)
{
	uint64_t	c = 1024;

	while (c < cap)
		c <<= 1;
	t->cap = c;
	t->n = 0;
	t->e = xcalloc(c, sizeof(Ent));
}

static void
table_free(Table *t)
{
	free(t->e);
	t->e = NULL;
}

static Ent *
table_find(const Table *t, uint64_t f0, uint64_t f1)
{
	uint64_t	mask = t->cap - 1;

	for (uint64_t i = f1 & mask;; i = (i + 1) & mask)
	{
		Ent		   *e = &t->e[i];

		if (!e->flags)
			return NULL;
		if (e->fp0 == f0 && e->fp1 == f1)
			return e;
	}
}

static void table_grow(Table *t);

/* Returns the entry; *isnew says whether it was inserted. */
static Ent *
table_insert(Table *t, uint64_t f0, uint64_t f1, bool *isnew)
{
	uint64_t	mask;

	if ((t->n + 1) * 4 > t->cap * 3)
		table_grow(t);
	mask = t->cap - 1;
	for (uint64_t i = f1 & mask;; i = (i + 1) & mask)
	{
		Ent		   *e = &t->e[i];

		if (!e->flags)
		{
			e->fp0 = f0;
			e->fp1 = f1;
			e->flags = 1;
			t->n++;
			*isnew = true;
			return e;
		}
		if (e->fp0 == f0 && e->fp1 == f1)
		{
			*isnew = false;
			return e;
		}
	}
}

static void
table_grow(Table *t)
{
	Table		nt;

	table_init(&nt, t->cap * 2);
	for (uint64_t i = 0; i < t->cap; i++)
	{
		Ent		   *e = &t->e[i];

		if (e->flags)
		{
			uint64_t	mask = nt.cap - 1;
			uint64_t	j = e->fp1 & mask;

			while (nt.e[j].flags)
				j = (j + 1) & mask;
			nt.e[j] = *e;
			nt.n++;
		}
	}
	free(t->e);
	*t = nt;
}

/* ------------------------------------------------------------------ */
/* FIFO of worlds */

static void
q_init(Queue *q, size_t ws)
{
	memset(q, 0, sizeof(*q));
	q->ws = ws;
	q->per = (1u << 22) / ws + 1;
}

static void
q_push(Queue *q, const uint8_t *w)
{
	QChunk	   *c = q->last;

	if (!c || c->tail == q->per)
	{
		c = xmalloc(sizeof(QChunk) + q->per * q->ws);
		c->next = NULL;
		c->head = c->tail = 0;
		if (q->last)
			q->last->next = c;
		else
			q->first = c;
		q->last = c;
	}
	memcpy(c->data + c->tail * q->ws, w, q->ws);
	c->tail++;
	q->n++;
}

static bool
q_pop(Queue *q, uint8_t *w)
{
	QChunk	   *c = q->first;

	if (!c || c->head == c->tail)
		return false;
	memcpy(w, c->data + c->head * q->ws, q->ws);
	c->head++;
	q->n--;
	if (c->head == c->tail && c->head == q->per)
	{
		q->first = c->next;
		if (!q->first)
			q->last = NULL;
		free(c);
	}
	return true;
}

static void
q_free(Queue *q)
{
	QChunk	   *c = q->first;

	while (c)
	{
		QChunk	   *n = c->next;

		free(c);
		c = n;
	}
	q->first = q->last = NULL;
}

/* ------------------------------------------------------------------ */
/* world accessors */

const void *
cmc_cfg(const CmcWorld *w)
{
	return w->run->cfg;
}

int
cmc_nnodes(const CmcWorld *w)
{
	return w->run->nn;
}

const void *
cmc_node(const CmcWorld *w, int node)
{
	return w->bytes + (size_t) node * w->run->ss;
}

size_t
cmc_world_size(const CmcRun *r)
{
	return r->ws;
}

#define HDR(r, w)		((w) + (r)->off_hdr)
#define NMSG(r, w)		(HDR(r, w)[0])
#define DROPS(r, w)		(HDR(r, w)[1])
#define DUPS(r, w)		(HDR(r, w)[2])
#define CRASHES(r, w)	(HDR(r, w)[3])
#define NREP(r, w)		(HDR(r, w)[4])
#define NODEH(r, w, n)	(HDR(r, w) + HDR_FIXED + (size_t) (n) * HDR_NODE)
#define CLIENTS(r, w, n) (NODEH(r, w, n)[0])
#define NPERS(r, w, n)	(NODEH(r, w, n)[3])
#define WAKES(r, w, n)	(NODEH(r, w, n)[4])
#define RESETS(r, w, n)	(NODEH(r, w, n)[5])
#define FIFO(r)			((r)->o.net == CMC_NET_FIFO)
#define MSG(r, w, i)	((w) + (r)->off_msgs + (size_t) (i) * MSG_ENT)
#define PERS(r, w, n, i) ((w) + (r)->off_pers + \
						  ((size_t) (n) * (r)->o.persist_cap + (i)) * PERS_ENT)
#define REP(r, w, i)	((w) + (r)->off_reps + (size_t) (i) * REP_ENT)

static inline uint16_t
get_timers(const CmcRun *r, const uint8_t *w, int n)
{
	const uint8_t *h = NODEH(r, w, n);

	return (uint16_t) (h[1] | (h[2] << 8));
}

static inline void
set_timers(const CmcRun *r, uint8_t *w, int n, uint16_t v)
{
	uint8_t    *h = NODEH(r, w, n);

	h[1] = (uint8_t) v;
	h[2] = (uint8_t) (v >> 8);
}

int
cmc_nreplies(const CmcWorld *w)
{
	if (w->run->direct)
		return 0;
	return NREP(w->run, w->bytes);
}

int
cmc_requests(const CmcWorld *w, int node)
{
	const CmcRun *r = w->run;

	if (r->direct || node < 0 || node >= r->nn ||
		(r->core->clients && !r->core->clients(r->cfg, node)))
		return 0;
	return r->o.client_budget - CLIENTS(r, w->bytes, node);
}

void
cmc_reply(const CmcWorld *w, int i, int *node, uint32_t *arg)
{
	const uint8_t *e = REP(w->run, w->bytes, i);

	*node = e[0];
	*arg = (uint32_t) e[1] | ((uint32_t) e[2] << 8) |
		((uint32_t) e[3] << 16) | ((uint32_t) e[4] << 24);
}

/* ------------------------------------------------------------------ */
/* run setup */

void
cmc_options_default(CmcOptions *o)
{
	memset(o, 0, sizeof(*o));
	o->search = CMC_BFS;
	o->max_depth = 100;
	o->walks = 1000;
	o->client_budget = 1;
	o->net_cap = 8;
	o->persist_cap = 2;
	o->reply_cap = 8;
	o->net = CMC_NET_FIFO;
	o->live_steps = 1000;
	o->progress_s = 0;
	o->out = stdout;
}

/* Parses "k=v,k=v" into buf (CMC_CFG_MAX bytes, zeroed first). */
static int
cfg_parse_into(const CmcCore *core, uint8_t *dst, const char *cfg, char *err, size_t errlen)
{
	char		buf[512];
	char	   *save = NULL;

	memset(dst, 0, CMC_CFG_MAX);
	core->cfg_default(dst);
	if (!cfg || !*cfg)
		return 0;
	snprintf(buf, sizeof(buf), "%s", cfg);
	for (char *kv = strtok_r(buf, ",", &save); kv; kv = strtok_r(NULL, ",", &save))
	{
		char	   *eq = strchr(kv, '=');

		if (!eq)
		{
			snprintf(err, errlen, "bad cfg item '%s' (want key=value)", kv);
			return -1;
		}
		*eq = 0;
		if (core->cfg_set(dst, kv, eq + 1) != 0)
		{
			snprintf(err, errlen, "bad cfg %s=%s for core %s", kv, eq + 1,
					 core->name);
			return -1;
		}
	}
	return 0;
}

static int
parse_cfg(CmcRun *r, const char *cfg, char *err, size_t errlen)
{
	snprintf(r->cfgstr, sizeof(r->cfgstr), "%s", cfg ? cfg : "");
	return cfg_parse_into(r->core, r->cfg, cfg, err, errlen);
}

static int
add_prop(CmcRun *r, const CmcProp *p)
{
	switch (p->kind)
	{
		case CMC_INV:
			r->inv[r->ninv++] = p;
			break;
		case CMC_ACTION_PROP:
			r->actp[r->nactp++] = p;
			break;
		case CMC_WITNESS:
			r->wit[r->nwit++] = p;
			break;
		case CMC_QUIESCENT:
			r->qui[r->nqui++] = p;
			break;
		case CMC_EVENTUALLY:
			r->ev[r->nev++] = p;
			break;
	}
	return 0;
}

static CmcRun *
run_reject(CmcRun *r, char *err, size_t errlen, const char *msg)
{
	snprintf(err, errlen, "%s", msg);
	cmc_run_free(r);
	return NULL;
}

CmcRun *
cmc_run_new(const CmcModel *m, const char *cfg, const char *const *props,
		   const CmcOptions *o, char *err, size_t errlen)
{
	CmcRun	   *r = xcalloc(1, sizeof(CmcRun));

	r->m = m;
	r->core = m->core;
	r->o = *o;
	if (!r->o.out)
		r->o.out = stdout;
	if (r->core->cfg_size > CMC_CFG_MAX)
		return run_reject(r, err, errlen, "core cfg too large");
	if (parse_cfg(r, cfg, err, errlen) != 0)
	{
		free(r);
		return NULL;
	}
	r->direct = r->core->direct_actions != NULL;
	r->nn = r->direct ? 1 : r->core->nnodes(r->cfg);
	if (r->nn < 1 || r->nn > CMC_MAX_NODES)
	{
		snprintf(err, errlen, "node count %d out of range", r->nn);
		free(r);
		return NULL;
	}
	r->ss = r->core->state_size;
	/* docs/research/02 M1: the durable part is checked in full, never clipped */
	if (r->core->durable_size > r->ss)
		return run_reject(r, err, errlen, "durable_size exceeds state_size");
	/* docs/research/02 m4: negative bounds are an error, not 255 */
	if (r->o.net_cap < 0 || r->o.reply_cap < 0 || r->o.persist_cap < 0 ||
		r->o.client_budget < 0 || r->o.drop_budget < 0 || r->o.dup_budget < 0 ||
		r->o.crash_budget < 0 || r->o.max_depth < 1 || r->o.live_steps < 1 ||
		r->o.time_limit < 0 || r->o.progress_s < 0)
		return run_reject(r, err, errlen,
						  "bounds must be >= 0 (depth and live steps >= 1)");
	if (r->o.net != CMC_NET_FIFO && r->o.net != CMC_NET_BAG)
		return run_reject(r, err, errlen, "unknown network model");
	r->off_hdr = (size_t) r->nn * r->ss;
	if (r->direct)
		r->ws = r->off_hdr;
	else
	{
		if (r->o.net_cap > 255 || r->o.reply_cap > 255 || r->o.persist_cap > 255 ||
			r->o.client_budget > 255 || r->o.drop_budget > 255 ||
			r->o.dup_budget > 255 || r->o.crash_budget > 255)
			return run_reject(r, err, errlen, "environment bounds must be <= 255");
		if (FIFO(r) && r->o.dup_budget > 0)
			return run_reject(r, err, errlen,
							  "a FIFO connection never duplicates: --dups needs --net bag");
		r->off_msgs = r->off_hdr + HDR_FIXED + (size_t) r->nn * HDR_NODE;
		r->off_pers = r->off_msgs + (size_t) r->o.net_cap * MSG_ENT;
		r->off_reps = r->off_pers + (size_t) r->nn * r->o.persist_cap * PERS_ENT;
		r->ws = r->off_reps + (size_t) r->o.reply_cap * REP_ENT;
	}
	r->ws = (r->ws + 7) & ~(size_t) 7;
	r->dur = xmalloc(r->ss + 1);
	r->detst = xmalloc(r->ss + 1);
	r->deto = xmalloc(sizeof(CmcOut));
	r->qw = xmalloc(r->ws);
	r->fw = xmalloc(r->ws);
	r->fw0 = xmalloc(r->ws);
	r->fsuf = xmalloc(sizeof(CmcAction) * ((size_t) r->o.live_steps + 1));
	r->facts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
	r->ffp = xmalloc(sizeof(uint64_t) * 2 * ((size_t) r->o.live_steps + 2));
	r->fstep = xmalloc(sizeof(int) * ((size_t) r->o.live_steps + 2));

	for (int j = 0; j < m->nalways; j++)
		add_prop(r, &m->props[j]);
	if (props)
	{
		for (int i = 0; props[i]; i++)
		{
			const CmcProp *p = NULL;

			for (int j = m->nalways; j < m->nprops; j++)
				if (strcmp(m->props[j].name, props[i]) == 0)
					p = &m->props[j];
			if (!p)
			{
				snprintf(err, errlen, "model %s has no property %s", m->name, props[i]);
				cmc_run_free(r);
				return NULL;
			}
			add_prop(r, p);
			r->sel[r->nsel++] = p;
		}
	}
	else
		for (int j = m->nalways; j < m->nprops; j++)
		{
			add_prop(r, &m->props[j]);
			r->sel[r->nsel++] = &m->props[j];
		}
	if (r->nev && r->direct)
		return run_reject(r, err, errlen, "CMC_EVENTUALLY needs actor mode");
	if (r->core->durable_size && !r->core->apply_persist)
		return run_reject(r, err, errlen,
						  "durable_size > 0 needs apply_persist (the engine owns durable)");
	return r;
}

void
cmc_run_free(CmcRun *r)
{
	if (!r)
		return;
	table_free(&r->t);
	table_free(&r->live);
	free(r->dur);
	free(r->detst);
	free(r->deto);
	free(r->qw);
	free(r->fw);
	free(r->fw0);
	free(r->fsuf);
	free(r->facts);
	free(r->ffp);
	free(r->fstep);
	free(r);
}

void
cmc_result_free(CmcResult *res)
{
	free(res->trace);
	res->trace = NULL;
}

/* ------------------------------------------------------------------ */
/* init, enabled actions, apply */

void
cmc_world_init(CmcRun *r, uint8_t *w)
{
	memset(w, 0, r->ws);
	for (int n = 0; n < r->nn; n++)
		r->core->init(r->cfg, n, w + (size_t) n * r->ss);
	if (!r->direct)
	{
		DROPS(r, w) = (uint8_t) r->o.drop_budget;
		DUPS(r, w) = (uint8_t) r->o.dup_budget;
		CRASHES(r, w) = (uint8_t) r->o.crash_budget;
		for (int n = 0; n < r->nn; n++)
			if (!r->core->clients || r->core->clients(r->cfg, n))
				CLIENTS(r, w, n) = (uint8_t) r->o.client_budget;
	}
}

/* FIFO: is net entry i the first of its (dst, src) channel? */
static inline bool
fifo_head(const CmcRun *r, const uint8_t *w, int i)
{
	const uint8_t *e = MSG(r, w, i);

	return i == 0 || MSG(r, w, i - 1)[0] != e[0] || MSG(r, w, i - 1)[1] != e[1];
}

/* index of the first entry of channel src->dst, or -1 */
static int
chan_find(const CmcRun *r, const uint8_t *w, int dst, int src)
{
	for (int i = 0; i < NMSG(r, w); i++)
	{
		const uint8_t *e = MSG(r, w, i);

		if (e[0] == dst && e[1] == src)
			return i;
		if (e[0] > dst || (e[0] == dst && e[1] > src))
			break;
	}
	return -1;
}

/* FIFO: may DELIVER/DROP use the head of channel src->dst?  Not while
 * dst still has to learn of a reset of that connection: the reset comes
 * before anything sent on the new one. */
static inline bool
fifo_open(const CmcRun *r, const uint8_t *w, int dst, int src)
{
	return !(RESETS(r, w, dst) & (1u << src));
}

/* FIFO: resets connection a-b: both directions are emptied and both ends
 * get CONN_RESET (a crashed end gets none: it is a new process). */
static void conn_reset(CmcRun *r, uint8_t *w, int a, int b, bool notify_a);

/*
 * Enabled actions.  Actor mode reads ONLY the environment part of the
 * world (what cores emitted, and budgets), never core state.
 */
int
cmc_enabled(CmcRun *r, const uint8_t *w, CmcAction *acts, int max)
{
	int			k = 0;

	if (r->direct)
	{
		int			n = r->core->direct_actions(r->cfg, w, acts, max);

		/* determinism gate: the action list must be a function of the bytes */
		if (r->o.det && n > 0)
		{
			CmcAction   *b = xmalloc(sizeof(CmcAction) * (size_t) max);
			int			m = r->core->direct_actions(r->cfg, w, b, max);

			if (m != n || memcmp(acts, b, sizeof(CmcAction) * (size_t) n) != 0)
				n = -2;
			free(b);
		}
		return n;
	}

#define PUSH(x) do { if (k >= max) return -1; acts[k++] = (x); } while (0)
	for (int i = 0; i < NMSG(r, w); i++)
	{
		const uint8_t *e = MSG(r, w, i);

		if (FIFO(r))
		{
			/* only a channel's head; one DROP per connection */
			if (!fifo_head(r, w, i) || !fifo_open(r, w, e[0], e[1]))
				continue;
			PUSH(CMC_ACT(CMC_A_DELIVER, e[0], e[1], 0, i));
			if (DROPS(r, w))
				PUSH(CMC_ACT(CMC_A_DROP, e[0], e[1], 0, i));
			continue;
		}
		PUSH(CMC_ACT(CMC_A_DELIVER, e[0], e[1], 0, i));
		if (DROPS(r, w))
			PUSH(CMC_ACT(CMC_A_DROP, e[0], e[1], 0, i));
		if (DUPS(r, w) && e[MSG_KEY] < 255)
			PUSH(CMC_ACT(CMC_A_DUP, e[0], e[1], 0, i));
	}
	for (int n = 0; n < r->nn; n++)
	{
		uint16_t	tm = get_timers(r, w, n);

		for (int b = 0; b < CMC_MAX_TIMERS; b++)
			if (tm & (1u << b))
				PUSH(CMC_ACT(CMC_A_FIRE, n, b, 0, 0));
		if (NPERS(r, w, n))
			PUSH(CMC_ACT(CMC_A_FLUSH, n, 0, 0, 0));
		for (int b = 0; b < CMC_MAX_WAKES; b++)
			if (WAKES(r, w, n) & (1u << b))
				PUSH(CMC_ACT(CMC_A_WAKE, n, b, 0, 0));
		for (int p = 0; p < r->nn; p++)
			if (RESETS(r, w, n) & (1u << p))
				PUSH(CMC_ACT(CMC_A_RESET, n, p, 0, 0));
		if (CLIENTS(r, w, n))
			PUSH(CMC_ACT(CMC_A_CLIENT, n, 0, 0, 0));
		if (CRASHES(r, w))
		{
			/* docs/research/02 B3: j of the k pending persists reached disk */
			int			kmax = r->core->durable_size ? NPERS(r, w, n) : 0;

			for (int j = 0; j <= kmax; j++)
				PUSH(CMC_ACT(CMC_A_CRASH, n, j, 0, 0));
		}
	}
#undef PUSH
	return k;
}

static void
msg_remove_one(CmcRun *r, uint8_t *w, int i)
{
	uint8_t    *e = MSG(r, w, i);

	if (--e[MSG_KEY] > 0)
		return;
	memmove(e, e + MSG_ENT, (size_t) (NMSG(r, w) - i - 1) * MSG_ENT);
	NMSG(r, w)--;
	memset(MSG(r, w, NMSG(r, w)), 0, MSG_ENT);
}

static int
msg_add(CmcRun *r, uint8_t *w, int dst, int src, const CmcOutput *o)
{
	uint8_t		key[MSG_KEY];
	int			nm = NMSG(r, w),
				i;

	memset(key, 0, sizeof(key));
	key[0] = (uint8_t) dst;
	key[1] = (uint8_t) src;
	key[2] = (uint8_t) o->len;
	memcpy(key + 3, o->body, o->len);
	if (FIFO(r))
	{
		/* append to channel src->dst: after every entry with (dst, src) <= */
		for (i = 0; i < nm; i++)
		{
			const uint8_t *e = MSG(r, w, i);

			if (e[0] > dst || (e[0] == dst && e[1] > src))
				break;
		}
		goto insert;
	}
	for (i = 0; i < nm; i++)
	{
		int			c = memcmp(MSG(r, w, i), key, MSG_KEY);

		if (c == 0)
		{
			uint8_t    *e = MSG(r, w, i);

			if (e[MSG_KEY] == 255)
			{
				snprintf(r->err, sizeof(r->err), "message count overflow");
				return -1;
			}
			e[MSG_KEY]++;
			return 0;
		}
		if (c > 0)
			break;
	}
insert:
	if (nm >= r->o.net_cap)
	{
		snprintf(r->err, sizeof(r->err),
				 "net_cap %d exceeded (raise --net-cap)", r->o.net_cap);
		return -1;
	}
	memmove(MSG(r, w, i + 1), MSG(r, w, i), (size_t) (nm - i) * MSG_ENT);
	memcpy(MSG(r, w, i), key, MSG_KEY);
	MSG(r, w, i)[MSG_KEY] = 1;
	NMSG(r, w)++;
	return 0;
}

/* FIFO: empties channel src->dst */
static void
chan_clear(CmcRun *r, uint8_t *w, int dst, int src)
{
	int			i;

	while ((i = chan_find(r, w, dst, src)) >= 0)
		msg_remove_one(r, w, i);
}

static void
conn_reset(CmcRun *r, uint8_t *w, int a, int b, bool notify_a)
{
	chan_clear(r, w, a, b);
	chan_clear(r, w, b, a);
	if (notify_a)
		RESETS(r, w, a) |= (uint8_t) (1u << b);
	RESETS(r, w, b) |= (uint8_t) (1u << a);
}

static int
process_outputs(CmcRun *r, uint8_t *w, int n, const CmcOut *out)
{
	for (int i = 0; i < out->n; i++)
	{
		const CmcOutput *o = &out->o[i];

		if (o->len > CMC_MSG_MAX)
		{
			snprintf(r->err, sizeof(r->err), "output body too long");
			return -1;
		}
		switch (o->kind)
		{
			case CMC_OUT_SEND:
				if (o->dst >= r->nn)
				{
					snprintf(r->err, sizeof(r->err), "SEND to bad node %d", o->dst);
					return -1;
				}
				/* fifo: a write on a connection whose reset n has not
				 * yet seen is lost (the old incarnation of the socket) */
				if (FIFO(r) && (RESETS(r, w, n) & (1u << o->dst)))
					break;
				if (msg_add(r, w, o->dst, n, o) != 0)
					return -1;
				break;
			case CMC_OUT_SET_TIMER:
			case CMC_OUT_CANCEL_TIMER:
				if (o->arg >= CMC_MAX_TIMERS)
				{
					snprintf(r->err, sizeof(r->err), "bad timer id %u", o->arg);
					return -1;
				}
				if (o->kind == CMC_OUT_SET_TIMER)
					set_timers(r, w, n, get_timers(r, w, n) | (uint16_t) (1u << o->arg));
				else
					set_timers(r, w, n, get_timers(r, w, n) & (uint16_t) ~(1u << o->arg));
				break;
			case CMC_OUT_PERSIST:
				{
					int			k = NPERS(r, w, n);
					uint8_t    *e;

					if (k >= r->o.persist_cap)
					{
						snprintf(r->err, sizeof(r->err),
								 "persist_cap %d exceeded (raise --persist-cap)",
								 r->o.persist_cap);
						return -1;
					}
					e = PERS(r, w, n, k);
					e[0] = (uint8_t) o->len;
					memset(e + 1, 0, CMC_MSG_MAX);
					memcpy(e + 1, o->body, o->len);
					NPERS(r, w, n)++;
					break;
				}
			case CMC_OUT_WAKE:
				if (o->arg >= CMC_MAX_WAKES)
				{
					snprintf(r->err, sizeof(r->err), "bad waiter id %u", o->arg);
					return -1;
				}
				WAKES(r, w, n) |= (uint8_t) (1u << o->arg);
				break;
			case CMC_OUT_REPLY:
				{
					uint8_t    *e;

					if (NREP(r, w) >= r->o.reply_cap)
					{
						snprintf(r->err, sizeof(r->err),
								 "reply_cap %d exceeded (raise --reply-cap)",
								 r->o.reply_cap);
						return -1;
					}
					e = REP(r, w, NREP(r, w));
					e[0] = (uint8_t) n;
					e[1] = (uint8_t) o->arg;
					e[2] = (uint8_t) (o->arg >> 8);
					e[3] = (uint8_t) (o->arg >> 16);
					e[4] = (uint8_t) (o->arg >> 24);
					NREP(r, w)++;
					break;
				}
			default:
				snprintf(r->err, sizeof(r->err), "unknown output kind %d", o->kind);
				return -1;
		}
	}
	return 0;
}

static bool
out_equal(const CmcOut *a, const CmcOut *b)
{
	if (a->n != b->n)
		return false;
	for (int i = 0; i < a->n; i++)
	{
		const CmcOutput *x = &a->o[i],
				   *y = &b->o[i];

		if (x->kind != y->kind || x->dst != y->dst || x->len != y->len ||
			x->arg != y->arg || x->len > CMC_MSG_MAX ||
			memcmp(x->body, y->body, x->len) != 0)
			return false;
	}
	return a->overflow == b->overflow;
}

static const char *
event_name(int kind)
{
	static const char *const nm[] = {"?", "RECV", "TIMER", "FLUSHED", "WAKE",
	"CLIENT", "RESTART", "ACTION", "CONN_RESET"};

	return kind >= 0 && kind <= CMC_EV_CONN_RESET ? nm[kind] : "?";
}

/*
 * Runs one core step on node n of world w.  Enforces the determinism gate
 * (if on), the output cap and the durable rule: step() never changes the
 * durable bytes, on any event, FLUSHED included (docs/research/02 B4).
 */
static int
core_step(CmcRun *r, uint8_t *w, int n, const CmcEvent *ev, CmcOut *out)
{
	uint8_t    *st = w + (size_t) n * r->ss;
	size_t		ds = r->core->durable_size;

	if (ds)
		memcpy(r->dur, st, ds);
	if (r->o.det)
	{
		CmcOut	   *o2 = r->deto;

		memcpy(r->detst, st, r->ss);
		memset(out, 0x00, sizeof(*out));
		memset(o2, 0xa5, sizeof(*o2));
		o2->n = 0;
		o2->overflow = 0;
		r->core->step(r->cfg, n, st, ev, out);
		r->core->step(r->cfg, n, r->detst, ev, o2);
		if (memcmp(r->detst, st, r->ss) != 0 || !out_equal(out, o2))
		{
			snprintf(r->err, sizeof(r->err),
					 "determinism gate: node %d step gave different %s on a repeat",
					 n, memcmp(r->detst, st, r->ss) != 0 ? "state" : "outputs");
			return -1;
		}
	}
	else
	{
		out->n = 0;
		out->overflow = 0;
		r->core->step(r->cfg, n, st, ev, out);
	}
	if (out->overflow)
	{
		snprintf(r->err, sizeof(r->err),
				 "core emitted more than CMC_OUT_MAX (%d) outputs (%d refused)",
				 CMC_OUT_MAX, out->overflow);
		return -1;
	}
	if (out->n < 0 || out->n > CMC_OUT_MAX)
	{
		snprintf(r->err, sizeof(r->err), "core emitted %d outputs", out->n);
		return -1;
	}
	if (ds && memcmp(r->dur, st, ds) != 0)
	{
		snprintf(r->err, sizeof(r->err),
				 "durable rule: node %d step changed durable state on %s"
				 " (only apply_persist may)", n, event_name(ev->kind));
		return -1;
	}
	return 0;
}

/*
 * Applies one persist record to node n's durable bytes through the core's
 * apply_persist (docs/research/02 B4).  It must touch only the durable part and
 * be deterministic.
 */
static int
persist_apply(CmcRun *r, uint8_t *w, int n, const uint8_t *body, uint16_t len)
{
	uint8_t    *st = w + (size_t) n * r->ss;
	size_t		ds = r->core->durable_size;

	if (!r->core->apply_persist)
		return 0;
	memcpy(r->detst, st, r->ss);
	r->core->apply_persist(r->cfg, n, st, body, len);
	if (memcmp(r->detst + ds, st + ds, r->ss - ds) != 0)
	{
		snprintf(r->err, sizeof(r->err),
				 "durable rule: node %d apply_persist changed volatile state", n);
		return -1;
	}
	if (r->o.det)
	{
		r->core->apply_persist(r->cfg, n, r->detst, body, len);
		if (memcmp(r->detst, st, ds) != 0)
		{
			snprintf(r->err, sizeof(r->err),
					 "determinism gate: node %d apply_persist differs on a repeat", n);
			return -1;
		}
	}
	return 0;
}

/* Applies action a to w in place.  Returns 0, or -1 with r->err set. */
int
cmc_apply(CmcRun *r, uint8_t *w, CmcAction a)
{
	CmcOut		out;
	CmcEvent		ev;
	uint8_t		body[CMC_MSG_MAX];
	int			n = CMC_ACT_A(a);
	unsigned	kind = CMC_ACT_KIND(a);

	r->err[0] = 0;
	memset(&ev, 0, sizeof(ev));
	if (r->direct)
	{
		ev.kind = CMC_EV_ACTION;
		ev.action = a;
		return core_step(r, w, 0, &ev, &out);
	}

	switch (kind)
	{
		case CMC_A_DELIVER:
		case CMC_A_DROP:
		case CMC_A_DUP:
			{
				int			i = (int) CMC_ACT_D(a);
				uint8_t    *e;

				if (i >= NMSG(r, w))
					goto bad;
				e = MSG(r, w, i);
				if (FIFO(r))
				{
					if (kind == CMC_A_DUP || !fifo_head(r, w, i) ||
						!fifo_open(r, w, e[0], e[1]))
						goto bad;
					if (kind == CMC_A_DROP)
					{
						if (!DROPS(r, w))
							goto bad;
						DROPS(r, w)--;
						conn_reset(r, w, e[0], e[1], true);
						return 0;
					}
				}
				if (kind == CMC_A_DUP)
				{
					if (!DUPS(r, w) || e[MSG_KEY] == 255)
						goto bad;
					e[MSG_KEY]++;
					DUPS(r, w)--;
					return 0;
				}
				n = e[0];
				ev.kind = CMC_EV_RECV;
				ev.src = e[1];
				ev.len = e[2];
				memcpy(body, e + 3, CMC_MSG_MAX);
				ev.body = body;
				msg_remove_one(r, w, i);
				if (kind == CMC_A_DROP)
				{
					if (!DROPS(r, w))
						goto bad;
					DROPS(r, w)--;
					return 0;
				}
				break;
			}
		case CMC_A_FIRE:
			{
				unsigned	b = CMC_ACT_B(a);
				uint16_t	tm;

				if (n >= r->nn || b >= CMC_MAX_TIMERS)
					goto bad;
				tm = get_timers(r, w, n);
				if (!(tm & (1u << b)))
					goto bad;
				set_timers(r, w, n, tm & (uint16_t) ~(1u << b));
				ev.kind = CMC_EV_TIMER;
				ev.arg = b;
				break;
			}
		case CMC_A_FLUSH:
			{
				uint8_t    *e;
				int			k;

				if (n >= r->nn || !(k = NPERS(r, w, n)))
					goto bad;
				e = PERS(r, w, n, 0);
				ev.kind = CMC_EV_FLUSHED;
				ev.len = e[0];
				memcpy(body, e + 1, CMC_MSG_MAX);
				ev.body = body;
				memmove(e, e + PERS_ENT, (size_t) (k - 1) * PERS_ENT);
				memset(PERS(r, w, n, k - 1), 0, PERS_ENT);
				NPERS(r, w, n)--;
				/* the engine writes the record (docs/research/02 B4); step then
				 * sees FLUSHED with durable read-only */
				if (persist_apply(r, w, n, body, ev.len) != 0)
					return -1;
				break;
			}
		case CMC_A_RESET:
			{
				unsigned	p = CMC_ACT_B(a);

				if (n >= r->nn || p >= (unsigned) r->nn ||
					!(RESETS(r, w, n) & (1u << p)))
					goto bad;
				RESETS(r, w, n) &= (uint8_t) ~(1u << p);
				ev.kind = CMC_EV_CONN_RESET;
				ev.src = (uint8_t) p;
				break;
			}
		case CMC_A_WAKE:
			{
				unsigned	b = CMC_ACT_B(a);

				if (n >= r->nn || b >= CMC_MAX_WAKES || !(WAKES(r, w, n) & (1u << b)))
					goto bad;
				WAKES(r, w, n) &= (uint8_t) ~(1u << b);
				ev.kind = CMC_EV_WAKE;
				ev.arg = b;
				break;
			}
		case CMC_A_CLIENT:
			if (n >= r->nn || !CLIENTS(r, w, n))
				goto bad;
			ev.kind = CMC_EV_CLIENT;
			ev.arg = (uint32_t) (r->o.client_budget - CLIENTS(r, w, n));
			CLIENTS(r, w, n)--;
			break;
		case CMC_A_CRASH:
			{
				size_t		ds = r->core->durable_size;
				uint8_t    *st = w + (size_t) n * r->ss;
				int			j = (int) CMC_ACT_B(a);

				if (n >= r->nn || !CRASHES(r, w) || j > NPERS(r, w, n) ||
					(j > 0 && !ds))
					goto bad;
				CRASHES(r, w)--;

				/*
				 * docs/research/02 B3: the first j pending records reached disk
				 * before the process died (XLogFlush returned, FLUSHED was
				 * never acted on).  apply_persist has no outputs; the dead
				 * process sends nothing.
				 */
				for (int k = 0; k < j; k++)
				{
					uint8_t    *pe = PERS(r, w, n, k);

					memcpy(body, pe + 1, CMC_MSG_MAX);
					if (persist_apply(r, w, n, body, pe[0]) != 0)
						return -1;
				}
				memset(st + ds, 0, r->ss - ds);
				set_timers(r, w, n, 0);
				WAKES(r, w, n) = 0;
				for (int k = 0; k < r->o.persist_cap; k++)
					memset(PERS(r, w, n, k), 0, PERS_ENT);
				NPERS(r, w, n) = 0;
				/* docs/research/02 B5: its connections die with it */
				if (FIFO(r))
				{
					RESETS(r, w, n) = 0;
					for (int p = 0; p < r->nn; p++)
						if (p != n)
							conn_reset(r, w, n, p, false);
				}
				ev.kind = CMC_EV_RESTART;
				break;
			}
		default:
			goto bad;
	}
	if (core_step(r, w, n, &ev, &out) != 0)
		return -1;
	return process_outputs(r, w, n, &out);

bad:
	snprintf(r->err, sizeof(r->err), "action %016" PRIx64 " is not enabled", a);
	return -1;
}

/* ------------------------------------------------------------------ */
/* properties */

static const CmcProp *
check_inv(CmcRun *r, const uint8_t *w, CmcWhy *why)
{
	CmcWorld		W = {r, w};

	for (int i = 0; i < r->ninv; i++)
	{
		why->msg[0] = 0;
		if (!r->inv[i]->state(&W, why))
			return r->inv[i];
	}
	return NULL;
}

static const CmcProp *
check_actp(CmcRun *r, const uint8_t *pre, CmcAction a, const uint8_t *post, CmcWhy *why)
{
	CmcWorld		P = {r, pre},
				Q = {r, post};

	for (int i = 0; i < r->nactp; i++)
	{
		why->msg[0] = 0;
		if (!r->actp[i]->action(&P, a, &Q, why))
			return r->actp[i];
	}
	return NULL;
}

static const CmcProp *
check_qui(CmcRun *r, const uint8_t *w, CmcWhy *why)
{
	CmcWorld		W = {r, w};

	for (int i = 0; i < r->nqui; i++)
	{
		why->msg[0] = 0;
		if (!r->qui[i]->state(&W, why))
			return r->qui[i];
	}
	return NULL;
}

/* Returns true when every witness has been reached. */
static bool
check_wit(CmcRun *r, const uint8_t *w, int depth)
{
	CmcWorld		W = {r, w};
	bool		all = true;

	for (int i = 0; i < r->nwit; i++)
	{
		if (!r->wit_depth[i] && r->wit[i]->state(&W, NULL))
			r->wit_depth[i] = depth;
		if (!r->wit_depth[i])
			all = false;
	}
	return all;
}

/*
 * CMC_QUIESCENT applies where nothing can happen, and also where the only
 * enabled actions are timer fires that each lead back to the same world
 * (a periodic timer that does nothing else: docs/research/02 B1).
 */
static bool
is_quiescent(CmcRun *r, const uint8_t *s, const CmcAction *acts, int na)
{
	if (na > 0 && r->direct)
		return false;
	for (int i = 0; i < na; i++)
	{
		if (CMC_ACT_KIND(acts[i]) != CMC_A_FIRE)
			return false;
		memcpy(r->qw, s, r->ws);
		if (cmc_apply(r, r->qw, acts[i]) != 0 || memcmp(r->qw, s, r->ws) != 0)
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/*
 * Bounded liveness (CMC_EVENTUALLY, docs/research/02 B1).
 *
 * From a state s the fair schedule runs: no faults and no new clients;
 * each round first drains everything the environment owes the cores
 * (every deliverable message in channel order, every pending persist,
 * wake and connection reset, in cmc_enabled order), then fires each timer
 * armed at that point once, draining after each.  It is deterministic and
 * every round is a function of the world at its start.  The check passes
 * when all CMC_EVENTUALLY properties hold together at some state within
 * live_steps steps; it fails when the schedule stops, when a round-start
 * world repeats (it cycles forever), or when the steps run out.
 *
 * Every state of the fair run is reachable (faults are optional), so a
 * failure from any reachable state is a real behaviour; checking every
 * state, not only those with budgets spent, is sound and needs no budget
 * bookkeeping.  Round starts that reach the goal are memoised (r->live,
 * Ent.depth = steps to the goal), which keeps the cost near one fair run
 * per round-start state.
 */

static bool
ev_goal(CmcRun *r, const uint8_t *w)
{
	CmcWorld		W = {r, w};

	for (int i = 0; i < r->nev; i++)
		if (!r->ev[i]->state(&W, NULL))
			return false;
	return true;
}

static bool
fair_kind(unsigned k)
{
	return k == CMC_A_DELIVER || k == CMC_A_FLUSH || k == CMC_A_WAKE || k == CMC_A_RESET;
}

/* one fair step on r->fw: 1 = goal reached, 0 = not, -1 = engine error,
 * -2 = out of steps */
static int
fair_step(CmcRun *r, CmcAction a)
{
	if (r->nfsuf >= r->o.live_steps)
		return -2;
	r->fsuf[r->nfsuf++] = a;
	if (cmc_apply(r, r->fw, a) != 0)
		return -1;
	return ev_goal(r, r->fw) ? 1 : 0;
}

/* drains r->fw; same returns as fair_step, 0 = drained */
static int
fair_drain(CmcRun *r)
{
	for (;;)
	{
		int			na = cmc_enabled(r, r->fw, r->facts, CMC_MAX_ACTIONS),
					i,
					rc;

		if (na < 0)
		{
			snprintf(r->err, sizeof(r->err), "too many enabled actions in the fair run");
			return -1;
		}
		for (i = 0; i < na && !fair_kind(CMC_ACT_KIND(r->facts[i])); i++)
			;
		if (i == na)
			return 0;
		if ((rc = fair_step(r, r->facts[i])) != 0)
			return rc;
	}
}

/*
 * Returns 0 (holds), 1 (violated: *badp, why; r->fsuf/nfsuf is the fair
 * suffix) or -1 (engine error in r->err; the suffix ends with the action
 * that failed).
 */
static int
check_ev(CmcRun *r, const uint8_t *s, CmcWhy *why, const CmcProp **badp)
{
	int			nr = 0,
				rc = 0;
	const char *how;
	char		howbuf[160];

	r->nfsuf = 0;
	memcpy(r->fw, s, r->ws);
	if (ev_goal(r, r->fw))
		return 0;
	for (;;)
	{
		uint64_t	fp[2];
		uint16_t	armed[CMC_MAX_NODES];
		int			start = r->nfsuf;
		Ent		   *e;

		cmc_fingerprint(r->fw, r->ws, fp);
		if ((e = table_find(&r->live, fp[0], fp[1])) &&
			r->nfsuf + (int) e->depth <= r->o.live_steps)
		{
			rc = 1;
			r->nfsuf += (int) e->depth;	/* only the count is used below */
			break;
		}
		for (int i = 0; i < nr; i++)
			if (r->ffp[2 * i] == fp[0] && r->ffp[2 * i + 1] == fp[1])
			{
				snprintf(howbuf, sizeof(howbuf),
						 "the fair schedule cycles (fair step %d repeats fair step %d)",
						 r->nfsuf, r->fstep[i]);
				how = howbuf;
				goto fail;
			}
		r->ffp[2 * nr] = fp[0];
		r->ffp[2 * nr + 1] = fp[1];
		r->fstep[nr++] = r->nfsuf;

		if ((rc = fair_drain(r)) != 0)
			goto stepped;
		for (int n = 0; n < r->nn; n++)
			armed[n] = get_timers(r, r->fw, n);
		for (int n = 0; n < r->nn && !rc; n++)
			for (int b = 0; b < CMC_MAX_TIMERS && !rc; b++)
				if ((armed[n] & (1u << b)) && (get_timers(r, r->fw, n) & (1u << b)))
				{
					if ((rc = fair_step(r, CMC_ACT(CMC_A_FIRE, n, b, 0, 0))) == 0)
						rc = fair_drain(r);
				}
		if (rc)
			goto stepped;
		if (r->nfsuf == start)
		{
			snprintf(howbuf, sizeof(howbuf),
					 "the fair schedule stops after %d steps (nothing to deliver, "
					 "flush, wake or fire)", r->nfsuf);
			how = howbuf;
			goto fail;
		}
		continue;
stepped:
		if (rc == 1)
			break;
		if (rc == -1)
			return -1;
		snprintf(howbuf, sizeof(howbuf), "not reached within %d fair steps (--live-steps)",
				 r->o.live_steps);
		how = howbuf;
		goto fail;
	}
	/* reached after r->nfsuf steps: memoise every round start */
	for (int i = 0; i < nr; i++)
	{
		bool		isnew;
		Ent		   *e = table_insert(&r->live, r->ffp[2 * i], r->ffp[2 * i + 1], &isnew);

		if (isnew)
			e->depth = (uint32_t) (r->nfsuf - r->fstep[i]);
	}
	r->nfsuf = 0;
	return 0;

fail:
	{
		CmcWorld		W = {r, r->fw};

		*badp = r->ev[0];
		for (int i = 0; i < r->nev; i++)
		{
			why->msg[0] = 0;
			if (!r->ev[i]->state(&W, why))
			{
				*badp = r->ev[i];
				break;
			}
		}
		{
			char		tmp[sizeof(why->msg)];
			size_t		l = strlen(how);

			memcpy(tmp, why->msg, sizeof(tmp));
			if (l > sizeof(why->msg) - 16)
				l = sizeof(why->msg) - 16;
			memcpy(why->msg, how, l);
			memcpy(why->msg + l, "; at its end: ", 15);
			l += 14;
			memcpy(why->msg + l, tmp, sizeof(why->msg) - l - 1);
			why->msg[sizeof(why->msg) - 1] = 0;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* traces */

void
cmc_describe_action(CmcRun *r, CmcAction a, FILE *f)
{
	if (r->direct)
	{
		if (r->core->describe_action)
			r->core->describe_action(r->cfg, a, f);
		else
			fprintf(f, "action %016" PRIx64, a);
		return;
	}
	switch (CMC_ACT_KIND(a))
	{
		case CMC_A_DELIVER:
			fprintf(f, "DELIVER %u->%u #%u", CMC_ACT_B(a), CMC_ACT_A(a), CMC_ACT_D(a));
			break;
		case CMC_A_DROP:
			if (FIFO(r))
				fprintf(f, "DROP %u->%u #%u (resets the connection)", CMC_ACT_B(a),
						CMC_ACT_A(a), CMC_ACT_D(a));
			else
				fprintf(f, "DROP %u->%u #%u", CMC_ACT_B(a), CMC_ACT_A(a), CMC_ACT_D(a));
			break;
		case CMC_A_DUP:
			fprintf(f, "DUP %u->%u #%u", CMC_ACT_B(a), CMC_ACT_A(a), CMC_ACT_D(a));
			break;
		case CMC_A_FIRE:
			fprintf(f, "FIRE node %u timer %u", CMC_ACT_A(a), CMC_ACT_B(a));
			break;
		case CMC_A_FLUSH:
			fprintf(f, "FLUSH node %u", CMC_ACT_A(a));
			break;
		case CMC_A_WAKE:
			fprintf(f, "WAKE node %u waiter %u", CMC_ACT_A(a), CMC_ACT_B(a));
			break;
		case CMC_A_CLIENT:
			fprintf(f, "CLIENT node %u", CMC_ACT_A(a));
			break;
		case CMC_A_CRASH:
			fprintf(f, "CRASH node %u", CMC_ACT_A(a));
			if (CMC_ACT_B(a))
				fprintf(f, " (%u pending persist%s reached disk)", CMC_ACT_B(a),
						CMC_ACT_B(a) > 1 ? "s" : "");
			break;
		case CMC_A_RESET:
			fprintf(f, "CONN_RESET node %u peer %u", CMC_ACT_A(a), CMC_ACT_B(a));
			break;
		default:
			fprintf(f, "action %016" PRIx64, a);
	}
}

static void
describe_world(CmcRun *r, const uint8_t *w, FILE *f)
{
	for (int n = 0; n < r->nn; n++)
	{
		if (!r->direct)
			fprintf(f, "  node %d: ", n);
		if (r->core->describe)
			r->core->describe(r->cfg, n, w + (size_t) n * r->ss, f);
		fprintf(f, "\n");
	}
	if (r->direct)
		return;
	fprintf(f, "  net:");
	for (int i = 0; i < NMSG(r, w); i++)
	{
		const uint8_t *e = MSG(r, w, i);

		fprintf(f, " [%u->%u x%u ", e[1], e[0], e[MSG_KEY]);
		if (r->core->describe_msg)
			r->core->describe_msg(r->cfg, e + 3, f);
		fprintf(f, "]");
	}
	fprintf(f, "\n  env: drops=%u dups=%u crashes=%u", DROPS(r, w), DUPS(r, w), CRASHES(r, w));
	for (int n = 0; n < r->nn; n++)
		fprintf(f, " | n%d clients=%u timers=%#x wakes=%#x persists=%u resets=%#x", n,
				CLIENTS(r, w, n), get_timers(r, w, n), WAKES(r, w, n), NPERS(r, w, n),
				RESETS(r, w, n));
	fprintf(f, "\n  replies:");
	for (int i = 0; i < NREP(r, w); i++)
	{
		CmcWorld		W = {r, w};
		int			nd;
		uint32_t	arg;

		cmc_reply(&W, i, &nd, &arg);
		fprintf(f, " (n%d,%u)", nd, arg);
	}
	fprintf(f, "\n");
}

void
cmc_print_trace(CmcRun *r, const CmcAction *acts, int n, FILE *f)
{
	uint8_t    *w = xmalloc(r->ws);

	cmc_world_init(r, w);
	fprintf(f, "State 1: <Initial predicate>\n");
	describe_world(r, w, f);
	for (int i = 0; i < n; i++)
	{
		if (r->fair_from && i + 1 == r->fair_from)
			fprintf(f, "\n--- from state %d on: the fair schedule (no faults, no new"
					" clients), which never reaches the goal ---\n", r->fair_from);
		fprintf(f, "\nState %d: <", i + 2);
		cmc_describe_action(r, acts[i], f);
		fprintf(f, ">\n");
		if (cmc_apply(r, w, acts[i]) != 0)
		{
			fprintf(f, "  (apply failed: %s)\n", r->err);
			break;
		}
		describe_world(r, w, f);
	}
	free(w);
}

int
cmc_write_replay(CmcRun *r, const char *path, const CmcAction *acts, int n)
{
	FILE	   *f = fopen(path, "w");

	if (!f)
		return -1;
	fprintf(f, "# cmc replay v2\nmodel %s\ncfg %s\n", r->m->name, r->cfgstr);
	if (!r->direct)
		fprintf(f, "env client=%d drop=%d dup=%d crash=%d net=%d persist=%d reply=%d"
				" mode=%s live=%d\n",
				r->o.client_budget, r->o.drop_budget, r->o.dup_budget,
				r->o.crash_budget, r->o.net_cap, r->o.persist_cap, r->o.reply_cap,
				r->o.net == CMC_NET_BAG ? "bag" : "fifo", r->o.live_steps);
	for (int i = 0; i < n; i++)
	{
		if (r->fair_from && i + 1 == r->fair_from)
			fprintf(f, "# fair schedule (bounded liveness) from here\n");
		fprintf(f, "act %016" PRIx64 " # ", acts[i]);
		cmc_describe_action(r, acts[i], f);
		fprintf(f, "\n");
	}
	return fclose(f);
}

/* Rebuild the action path from the fingerprint table. */
static int
trace_from_table(CmcRun *r, uint64_t f0, uint64_t f1, CmcAction **out)
{
	int			cap = 64,
				n = 0;
	CmcAction   *a = xmalloc(sizeof(CmcAction) * cap);

	for (;;)
	{
		Ent		   *e = table_find(&r->t, f0, f1);

		if (!e)
		{
			fprintf(stderr, "cmc: broken parent chain\n");
			exit(3);
		}
		if (e->flags & 2)
			break;
		if (n == cap)
		{
			cap *= 2;
			a = realloc(a, sizeof(CmcAction) * cap);
		}
		a[n++] = e->act;
		f0 = e->p0;
		f1 = e->p1;
	}
	for (int i = 0; i < n / 2; i++)
	{
		CmcAction	t = a[i];

		a[i] = a[n - 1 - i];
		a[n - 1 - i] = t;
	}
	*out = a;
	return n;
}

static void
set_violation(CmcResult *res, const CmcProp *p, const char *why)
{
	res->verdict = CMC_VIOLATION;
	res->prop = p ? p->name : "cmc:engine";
	snprintf(res->why, sizeof(res->why), "%s", why);
}

/* ------------------------------------------------------------------ */
/* BFS */

static void
progress(CmcRun *r, CmcResult *res, int level, uint64_t qn)
{
	fprintf(r->o.out, "Progress(%d): %" PRIu64 " states generated, %" PRIu64
			" distinct states found, %" PRIu64 " states left on queue.\n",
			level, res->generated, res->distinct, qn);
	fflush(r->o.out);
}

static void
run_bfs(CmcRun *r, CmcResult *res)
{
	Queue		q;
	uint8_t    *s = xmalloc(r->ws),
			   *t = xmalloc(r->ws);
	CmcAction   *acts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
	uint64_t	fp[2],
				pfp[2];
	CmcWhy		why;
	const CmcProp *bad;
	bool		isnew;
	Ent		   *e;
	double		t0 = now_s(),
				tlast = t0;
	uint64_t	level_left = 1,
				next_level = 0;
	int			level = 1;
	bool		all_wit;

	table_init(&r->t, 1u << 16);
	q_init(&q, r->ws);
	cmc_world_init(r, s);
	cmc_fingerprint(s, r->ws, fp);
	e = table_insert(&r->t, fp[0], fp[1], &isnew);
	e->flags |= 2;
	e->depth = 1;
	res->distinct = 1;
	res->generated = 1;
	res->depth = 1;
	if ((bad = check_inv(r, s, &why)))
	{
		set_violation(res, bad, why.msg);
		res->trace_len = 1;
		goto done;
	}
	all_wit = check_wit(r, s, 1);
	if (r->nwit && all_wit && r->o.stop_on_witness)
		goto done;
	q_push(&q, s);

	while (q_pop(&q, s))
	{
		int			na = cmc_enabled(r, s, acts, CMC_MAX_ACTIONS);

		cmc_fingerprint(s, r->ws, pfp);

		if (na < 0)
		{
			set_violation(res, NULL, na == -2 ?
						  "determinism gate: action list differs on a repeat" :
						  "too many enabled actions (raise CMC_MAX_ACTIONS)");
			goto fail_at_s;
		}
		if (r->nqui && is_quiescent(r, s, acts, na) && (bad = check_qui(r, s, &why)))
		{
			set_violation(res, bad, why.msg);
			goto fail_at_s;
		}
		if (r->nev)
		{
			int			rc = check_ev(r, s, &why, &bad);

			if (rc != 0)
			{
				set_violation(res, rc < 0 ? NULL : bad, rc < 0 ? r->err : why.msg);
				goto fail_fair;
			}
		}
		for (int i = 0; i < na; i++)
		{
			memcpy(t, s, r->ws);
			if (cmc_apply(r, t, acts[i]) != 0)
			{
				set_violation(res, NULL, r->err);
				cmc_fingerprint(s, r->ws, pfp);
				res->trace_len = trace_from_table(r, pfp[0], pfp[1], &res->trace) + 2;
				res->trace = realloc(res->trace, sizeof(CmcAction) * res->trace_len);
				res->trace[res->trace_len - 2] = acts[i];
				res->depth = level + 1;
				goto done;
			}
			res->cov_gen[CMC_ACT_KIND(acts[i])]++;
			res->cov_sample[CMC_ACT_KIND(acts[i])] = acts[i];
			res->generated++;
			if (r->nactp && (bad = check_actp(r, s, acts[i], t, &why)))
			{
				set_violation(res, bad, why.msg);
				cmc_fingerprint(s, r->ws, pfp);
				res->trace_len = trace_from_table(r, pfp[0], pfp[1], &res->trace) + 2;
				res->trace = realloc(res->trace, sizeof(CmcAction) * res->trace_len);
				res->trace[res->trace_len - 2] = acts[i];
				res->depth = level + 1;
				goto done;
			}
			cmc_fingerprint(t, r->ws, fp);
			e = table_insert(&r->t, fp[0], fp[1], &isnew);
			if (!isnew)
				continue;
			e->p0 = pfp[0];
			e->p1 = pfp[1];
			e->act = acts[i];
			e->depth = (uint32_t) (level + 1);
			res->distinct++;
			res->cov_new[CMC_ACT_KIND(acts[i])]++;
			if (level + 1 > res->depth)
				res->depth = level + 1;
			if ((bad = check_inv(r, t, &why)))
			{
				set_violation(res, bad, why.msg);
				res->trace_len = trace_from_table(r, fp[0], fp[1], &res->trace) + 1;
				goto done;
			}
			all_wit = check_wit(r, t, level + 1);
			if (r->nwit && all_wit && r->o.stop_on_witness)
				goto done;
			q_push(&q, t);
			next_level++;
		}
		if (--level_left == 0)
		{
			level_left = next_level;
			next_level = 0;
			level++;
		}
		if (r->o.max_states && res->distinct >= r->o.max_states)
		{
			res->verdict = CMC_INCOMPLETE;
			snprintf(res->why, sizeof(res->why), "max_states reached");
			goto done;
		}
		if ((res->distinct & 0x3fff) == 0)
		{
			double		tn = now_s();

			if (r->o.time_limit > 0 && tn - t0 > r->o.time_limit)
			{
				res->verdict = CMC_INCOMPLETE;
				snprintf(res->why, sizeof(res->why), "time limit reached");
				goto done;
			}
			if (r->o.progress_s && tn - tlast >= r->o.progress_s)
			{
				tlast = tn;
				progress(r, res, level, q.n);
			}
		}
	}
	goto done;

fail_at_s:
	cmc_fingerprint(s, r->ws, pfp);
	res->trace_len = trace_from_table(r, pfp[0], pfp[1], &res->trace) + 1;
	goto done;
fail_fair:
	/* the shortest prefix to s, then the fair suffix from s */
	cmc_fingerprint(s, r->ws, pfp);
	res->fair_from = trace_from_table(r, pfp[0], pfp[1], &res->trace) + 1;
	res->trace_len = res->fair_from + r->nfsuf;
	res->trace = realloc(res->trace, sizeof(CmcAction) * (size_t) res->trace_len);
	memcpy(res->trace + res->fair_from - 1, r->fsuf, sizeof(CmcAction) * (size_t) r->nfsuf);
done:
	res->left = q.n;
	q_free(&q);
	free(s);
	free(t);
	free(acts);
}

/* ------------------------------------------------------------------ */
/* DFS with a depth bound (stateful; re-explores a state reached again at a
 * shallower depth, so the bound does not hide states) */

typedef struct DFrame
{
	int			na,
				next;
	CmcAction   *acts;
} DFrame;

static void
run_dfs(CmcRun *r, CmcResult *res)
{
	int			D = r->o.max_depth;
	uint8_t    *worlds = xmalloc((size_t) (D + 1) * r->ws);
	DFrame	   *fr = xcalloc((size_t) D + 1, sizeof(DFrame));
	CmcAction   *path = xmalloc(sizeof(CmcAction) * (D + 1));
	uint64_t	fp[2];
	CmcWhy		why;
	const CmcProp *bad;
	bool		isnew,
				truncated = false;
	Ent		   *e;
	int			sp = 0;
	double		t0 = now_s();

	table_init(&r->t, 1u << 16);
	cmc_world_init(r, worlds);
	cmc_fingerprint(worlds, r->ws, fp);
	e = table_insert(&r->t, fp[0], fp[1], &isnew);
	e->flags |= 2;
	e->depth = 1;
	res->distinct = res->generated = 1;
	res->depth = 1;
	if ((bad = check_inv(r, worlds, &why)))
	{
		set_violation(res, bad, why.msg);
		res->trace_len = 1;
		goto done;
	}
	check_wit(r, worlds, 1);
	for (int i = 0; i <= D; i++)
		fr[i].acts = NULL;

	fr[0].acts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
	fr[0].na = cmc_enabled(r, worlds, fr[0].acts, CMC_MAX_ACTIONS);
	fr[0].next = 0;
	while (sp >= 0)
	{
		DFrame	   *f = &fr[sp];
		uint8_t    *s = worlds + (size_t) sp * r->ws,
				   *t;

		if (f->na < 0)
		{
			set_violation(res, NULL, "too many enabled actions");
			res->trace_len = sp + 1;
			goto copy_path;
		}
		if (f->next == 0 && r->nqui && is_quiescent(r, s, f->acts, f->na) &&
			(bad = check_qui(r, s, &why)))
		{
			set_violation(res, bad, why.msg);
			res->trace_len = sp + 1;
			goto copy_path;
		}
		if (f->next == 0 && r->nev && f->na >= 0)
		{
			int			rc = check_ev(r, s, &why, &bad);

			if (rc != 0)
			{
				set_violation(res, rc < 0 ? NULL : bad, rc < 0 ? r->err : why.msg);
				res->fair_from = sp + 1;
				res->trace_len = sp + 1 + r->nfsuf;
				res->trace = xmalloc(sizeof(CmcAction) * (size_t) (res->trace_len + 1));
				memcpy(res->trace, path, sizeof(CmcAction) * (size_t) sp);
				memcpy(res->trace + sp, r->fsuf, sizeof(CmcAction) * (size_t) r->nfsuf);
				goto done;
			}
		}
		if (f->next >= f->na)
		{
			sp--;
			continue;
		}
		if (sp + 1 >= D)
		{
			truncated = true;
			sp--;
			continue;
		}
		t = worlds + (size_t) (sp + 1) * r->ws;
		memcpy(t, s, r->ws);
		path[sp] = f->acts[f->next++];
		if (cmc_apply(r, t, path[sp]) != 0)
		{
			set_violation(res, NULL, r->err);
			res->trace_len = sp + 2;
			goto copy_path;
		}
		res->generated++;
		if (r->nactp && (bad = check_actp(r, s, path[sp], t, &why)))
		{
			set_violation(res, bad, why.msg);
			res->trace_len = sp + 2;
			goto copy_path;
		}
		cmc_fingerprint(t, r->ws, fp);
		e = table_insert(&r->t, fp[0], fp[1], &isnew);
		if (!isnew && e->depth <= (uint32_t) (sp + 2))
			continue;
		if (isnew)
		{
			res->distinct++;
			if ((bad = check_inv(r, t, &why)))
			{
				set_violation(res, bad, why.msg);
				res->trace_len = sp + 2;
				goto copy_path;
			}
			check_wit(r, t, sp + 2);
		}
		e->depth = (uint32_t) (sp + 2);
		if (sp + 2 > res->depth)
			res->depth = sp + 2;
		sp++;
		if (!fr[sp].acts)
			fr[sp].acts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
		fr[sp].na = cmc_enabled(r, t, fr[sp].acts, CMC_MAX_ACTIONS);
		fr[sp].next = 0;
		if (r->o.max_states && res->distinct >= r->o.max_states)
		{
			res->verdict = CMC_INCOMPLETE;
			snprintf(res->why, sizeof(res->why), "max_states reached");
			goto done;
		}
		if ((res->generated & 0xfff) == 0 && r->o.time_limit > 0 &&
			now_s() - t0 > r->o.time_limit)
		{
			res->verdict = CMC_INCOMPLETE;
			snprintf(res->why, sizeof(res->why), "time limit reached");
			goto done;
		}
	}
	if (truncated && res->verdict == CMC_PASS)
	{
		res->verdict = CMC_INCOMPLETE;
		snprintf(res->why, sizeof(res->why), "depth bound %d reached", D);
	}
	goto done;

copy_path:
	res->trace = xmalloc(sizeof(CmcAction) * (res->trace_len + 1));
	memcpy(res->trace, path, sizeof(CmcAction) * (res->trace_len - 1));
done:
	for (int i = 0; i <= D; i++)
		free(fr[i].acts);
	free(fr);
	free(worlds);
	free(path);
}

/* ------------------------------------------------------------------ */
/* random walks */

static uint64_t
rng_next(uint64_t s[4])
{
	/* xoshiro256** */
	uint64_t	result = rotl64(s[1] * 5, 7) * 9,
				t = s[1] << 17;

	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];
	s[2] ^= t;
	s[3] = rotl64(s[3], 45);
	return result;
}

static void
rng_seed(uint64_t s[4], uint64_t seed)
{
	for (int i = 0; i < 4; i++)
	{
		seed += 0x9e3779b97f4a7c15ULL;
		s[i] = smix64(seed);
	}
}

static void
run_simulate(CmcRun *r, CmcResult *res)
{
	int			D = r->o.max_depth;
	uint8_t    *s = xmalloc(r->ws),
			   *t = xmalloc(r->ws);
	CmcAction   *acts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
	CmcAction   *path = xmalloc(sizeof(CmcAction) * (D + 1));
	CmcWhy		why;
	const CmcProp *bad;
	double		t0 = now_s();

	for (uint64_t walk = 0; walk < r->o.walks; walk++)
	{
		uint64_t	rs[4];
		int			len = 0;
		bool		fair = false;

		rng_seed(rs, r->o.seed + walk);
		cmc_world_init(r, s);
		res->generated++;
		if ((bad = check_inv(r, s, &why)))
			goto viol;
		check_wit(r, s, 1);
		for (len = 0; len < D - 1; len++)
		{
			int			na = cmc_enabled(r, s, acts, CMC_MAX_ACTIONS);

			if (na < 0)
			{
				snprintf(why.msg, sizeof(why.msg), "too many enabled actions");
				bad = NULL;
				goto viol;
			}
			if (r->nqui && is_quiescent(r, s, acts, na) && (bad = check_qui(r, s, &why)))
				goto viol;
			if (r->nev)
			{
				int			rc = check_ev(r, s, &why, &bad);

				if (rc != 0)
				{
					if (rc < 0)
					{
						bad = NULL;
						snprintf(why.msg, sizeof(why.msg), "%s", r->err);
					}
					fair = true;
					goto viol;
				}
			}
			if (na == 0)
				break;
			path[len] = acts[rng_next(rs) % (uint64_t) na];
			memcpy(t, s, r->ws);
			if (cmc_apply(r, t, path[len]) != 0)
			{
				snprintf(why.msg, sizeof(why.msg), "%s", r->err);
				bad = NULL;
				len++;
				goto viol;
			}
			res->generated++;
			if (r->nactp && (bad = check_actp(r, s, path[len], t, &why)))
			{
				len++;
				goto viol;
			}
			memcpy(s, t, r->ws);
			if ((bad = check_inv(r, s, &why)))
			{
				len++;
				goto viol;
			}
			check_wit(r, s, len + 2);
		}
		if (len + 1 > res->depth)
			res->depth = len + 1;
		if (r->o.time_limit > 0 && now_s() - t0 > r->o.time_limit)
		{
			res->verdict = CMC_INCOMPLETE;
			snprintf(res->why, sizeof(res->why), "time limit after %" PRIu64 " walks", walk + 1);
			break;
		}
		continue;
viol:
		set_violation(res, bad, why.msg);
		snprintf(res->why + strlen(res->why), sizeof(res->why) - strlen(res->why),
				 " [walk %" PRIu64 ": --seed %" PRIu64 " --walks 1 reproduces it]",
				 walk, r->o.seed + walk);
		res->trace_len = len + 1;
		res->trace = xmalloc(sizeof(CmcAction) * (size_t) (len + 1 + (fair ? r->nfsuf : 0)));
		memcpy(res->trace, path, sizeof(CmcAction) * len);
		if (fair)
		{
			res->fair_from = len + 1;
			memcpy(res->trace + len, r->fsuf, sizeof(CmcAction) * (size_t) r->nfsuf);
			res->trace_len += r->nfsuf;
		}
		res->depth = len + 1;
		break;
	}
	free(s);
	free(t);
	free(acts);
	free(path);
}

/* ------------------------------------------------------------------ */
/* replay */

/*
 * docs/research/02 M4: a replay runs under the model, cfg and environment it
 * was recorded with.  Conflicts are refused, never replayed.
 */
static int
replay_check_header(CmcRun *r, FILE *f, char *why, size_t whylen)
{
	char		line[700],
				model[700] = "",
				cfg[700] = "";
	bool		have_model = false,
				have_cfg = false,
				have_env = false;
	CmcOptions	fo = r->o;
	uint8_t		fcfg[CMC_CFG_MAX];
	char		err[256];
	long		pos;

	if (!fgets(line, sizeof(line), f) || strcmp(line, "# cmc replay v2\n") != 0)
	{
		snprintf(why, whylen, "not a cmc replay v2 file");
		return -1;
	}
	for (;;)
	{
		size_t		l;

		pos = ftell(f);
		if (!fgets(line, sizeof(line), f))
			break;
		l = strlen(line);
		if (l && line[l - 1] == '\n')
			line[--l] = 0;
		if (!strncmp(line, "model ", 6))
		{
			snprintf(model, sizeof(model), "%s", line + 6);
			have_model = true;
		}
		else if (!strncmp(line, "cfg", 3) && (line[3] == ' ' || !line[3]))
		{
			snprintf(cfg, sizeof(cfg), "%s", line[3] ? line + 4 : "");
			have_cfg = true;
		}
		else if (!strncmp(line, "env ", 4))
		{
			char		net[8] = "";

			if (sscanf(line, "env client=%d drop=%d dup=%d crash=%d net=%d persist=%d"
					   " reply=%d mode=%7s live=%d", &fo.client_budget, &fo.drop_budget,
					   &fo.dup_budget, &fo.crash_budget, &fo.net_cap, &fo.persist_cap,
					   &fo.reply_cap, net, &fo.live_steps) != 9 ||
				(strcmp(net, "fifo") && strcmp(net, "bag")))
			{
				snprintf(why, whylen, "bad env line in replay file");
				return -1;
			}
			fo.net = strcmp(net, "bag") ? CMC_NET_FIFO : CMC_NET_BAG;
			have_env = true;
		}
		else
			break;
	}
	fseek(f, pos, SEEK_SET);
	if (!have_model || !have_cfg || (!r->direct && !have_env))
	{
		snprintf(why, whylen, "replay file lacks its model, cfg or env line");
		return -1;
	}
	if (strcmp(model, r->m->name) != 0)
	{
		snprintf(why, whylen, "replay file recorded with model %.200s, not %.200s",
				 model, r->m->name);
		return -1;
	}
	if (cfg_parse_into(r->core, fcfg, cfg, err, sizeof(err)) != 0 ||
		memcmp(fcfg, r->cfg, CMC_CFG_MAX) != 0)
	{
		snprintf(why, whylen, "replay file recorded with cfg \"%.200s\", not \"%.200s\"",
				 cfg, r->cfgstr);
		return -1;
	}
	if (!r->direct &&
		(fo.client_budget != r->o.client_budget || fo.drop_budget != r->o.drop_budget ||
		 fo.dup_budget != r->o.dup_budget || fo.crash_budget != r->o.crash_budget ||
		 fo.net_cap != r->o.net_cap || fo.persist_cap != r->o.persist_cap ||
		 fo.reply_cap != r->o.reply_cap || fo.net != r->o.net ||
		 (r->nev && fo.live_steps != r->o.live_steps)))
	{
		snprintf(why, whylen, "replay file recorded with different environment bounds"
				 " (client=%d drop=%d dup=%d crash=%d net-cap=%d persist-cap=%d"
				 " reply-cap=%d net=%s live-steps=%d)",
				 fo.client_budget, fo.drop_budget, fo.dup_budget, fo.crash_budget,
				 fo.net_cap, fo.persist_cap, fo.reply_cap,
				 fo.net == CMC_NET_BAG ? "bag" : "fifo", fo.live_steps);
		return -1;
	}
	return 0;
}

static void
run_replay(CmcRun *r, CmcResult *res)
{
	FILE	   *f = fopen(r->o.replay_in, "r");
	char		line[512];
	uint8_t    *s = xmalloc(r->ws),
			   *t = xmalloc(r->ws);
	CmcAction   *acts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
	CmcAction   *path = NULL;
	int			n = 0,
				cap = 0;
	CmcWhy		why;
	const CmcProp *bad = NULL;

	if (!f)
	{
		res->verdict = CMC_ERROR;
		snprintf(res->why, sizeof(res->why), "cannot open %s", r->o.replay_in);
		goto out;
	}
	if (replay_check_header(r, f, res->why, sizeof(res->why)) != 0)
	{
		res->verdict = CMC_ERROR;
		fclose(f);
		goto out;
	}
	while (fgets(line, sizeof(line), f))
	{
		unsigned long long v;

		if (sscanf(line, "act %llx", &v) == 1)
		{
			if (n == cap)
			{
				cap = cap ? cap * 2 : 64;
				path = realloc(path, sizeof(CmcAction) * cap);
			}
			path[n++] = (CmcAction) v;
		}
	}
	fclose(f);
	cmc_world_init(r, s);
	res->generated = 1;
	res->depth = 1;
	if ((bad = check_inv(r, s, &why)))
	{
		set_violation(res, bad, why.msg);
		res->trace_len = 1;
		goto out;
	}
	check_wit(r, s, 1);
	for (int i = 0; i < n; i++)
	{
		int			na = cmc_enabled(r, s, acts, CMC_MAX_ACTIONS),
					ok = 0;

		/* a liveness counterexample: the fair suffix follows this state */
		if (r->nev && check_ev(r, s, &why, &bad) != 0)
		{
			set_violation(res, bad, why.msg);
			res->fair_from = i + 1;
			res->trace_len = i + 1 + r->nfsuf;
			path = realloc(path, sizeof(CmcAction) * (size_t) (res->trace_len + 1));
			memcpy(path + i, r->fsuf, sizeof(CmcAction) * (size_t) r->nfsuf);
			break;
		}

		for (int j = 0; j < na; j++)
			if (acts[j] == path[i])
				ok = 1;
		if (!ok)
		{
			res->verdict = CMC_ERROR;
			snprintf(res->why, sizeof(res->why),
					 "replay step %d: action %016" PRIx64 " is not enabled", i + 1, path[i]);
			res->trace_len = i + 1;
			break;
		}
		memcpy(t, s, r->ws);
		if (cmc_apply(r, t, path[i]) != 0)
		{
			set_violation(res, NULL, r->err);
			res->trace_len = i + 2;
			break;
		}
		res->generated++;
		res->depth = i + 2;
		if (r->nactp && (bad = check_actp(r, s, path[i], t, &why)))
		{
			set_violation(res, bad, why.msg);
			res->trace_len = i + 2;
			break;
		}
		memcpy(s, t, r->ws);
		if ((bad = check_inv(r, s, &why)))
		{
			set_violation(res, bad, why.msg);
			res->trace_len = i + 2;
			break;
		}
		check_wit(r, s, i + 2);
	}
	if (res->verdict == CMC_PASS)
	{
		int			na = cmc_enabled(r, s, acts, CMC_MAX_ACTIONS);

		/* a quiescence violation shows only at the end of the trace */
		if (r->nqui && na >= 0 && is_quiescent(r, s, acts, na) &&
			(bad = check_qui(r, s, &why)))
			set_violation(res, bad, why.msg);
		else if (r->nev && check_ev(r, s, &why, &bad) != 0)
		{
			set_violation(res, bad, why.msg);
			res->fair_from = n + 1;
		}
		res->trace_len = n + 1;
	}
	res->trace = path;
	path = NULL;
out:
	free(path);
	free(s);
	free(t);
	free(acts);
}

/* ------------------------------------------------------------------ */

void
cmc_check(CmcRun *r, CmcResult *res)
{
	double		t0 = now_s();

	memset(res, 0, sizeof(*res));
	memset(r->wit_depth, 0, sizeof(r->wit_depth));
	table_free(&r->t);
	table_free(&r->live);
	table_init(&r->live, 1u << 10);
	r->fair_from = 0;
	switch (r->o.search)
	{
		case CMC_BFS:
			run_bfs(r, res);
			break;
		case CMC_DFS:
			run_dfs(r, res);
			break;
		case CMC_SIMULATE:
			run_simulate(r, res);
			break;
		case CMC_REPLAY:
			run_replay(r, res);
			break;
	}
	res->wall_s = now_s() - t0;
	res->nwitness = r->nwit < 16 ? r->nwit : 16;
	for (int i = 0; i < res->nwitness; i++)
	{
		res->witness_name[i] = r->wit[i]->name;
		res->witness_depth[i] = r->wit_depth[i];
	}
	/* docs/research/02 m3: the depth of a violation is that of the violating
	 * state (for liveness, the state the fair suffix starts from) */
	if (res->verdict == CMC_VIOLATION && res->trace_len > 0)
		res->depth = res->fair_from ? res->fair_from : res->trace_len;
	r->fair_from = res->fair_from;

	/*
	 * A witness never reached is a vacuity failure, in every search but a
	 * replay of one trace (docs/research/02 m1): BFS and a DFS that finished are
	 * complete; a simulation that never saw it proves nothing either.
	 */
	if (res->verdict == CMC_PASS && r->o.search != CMC_REPLAY)
		for (int i = 0; i < r->nwit; i++)
			if (!r->wit_depth[i])
			{
				res->verdict = CMC_VIOLATION;
				res->prop = r->wit[i]->name;
				snprintf(res->why, sizeof(res->why), "witness never reached in the %s (vacuous)",
						 r->o.search == CMC_SIMULATE ? "random walks" : "complete search");
				break;
			}
}
