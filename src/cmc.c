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

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#define CMC_CFG_MAX 256
#define CMC_MAX_WORKERS 256
#define CMC_STR_(x) #x
#define CMC_STR(x) CMC_STR_(x)
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
				fp1;
	uint32_t	depth;
	uint32_t	flags;			/* 1 = used */
} Ent;

typedef struct Table
{
	Ent		   *e;
	uint64_t	cap;			/* power of two */
	uint64_t	n;
} Table;

#define REC_ROOT UINT64_MAX
#define REC_BITS 40
#define REC_MASK ((UINT64_C(1) << REC_BITS) - 1)
#define NSHARD 256

typedef struct Rec
{
	uint64_t	fp0,
				fp1,
				parent;			/* record number, REC_ROOT for the initial state */
	CmcAction	act;			/* the action from the parent */
} Rec;

/* One shard of the state store: the records whose fingerprint falls in
 * it, and their index.  A record's number is local << 8 | shard. */
typedef struct Shard
{
	Rec		   *rec;
	uint64_t	n,				/* records */
				rcap;			/* records allocated */
	uint64_t   *slot;			/* 0 = empty, else tag << REC_BITS | (local + 1) */
	uint64_t	cap;			/* slots, a power of two */
} Shard;

typedef struct Store
{
	Shard	   *sh;				/* NSHARD shards */
} Store;

/* A chunk of BFS queue entries {record number, world}.  A chunk whose
 * data has been spilled has data == NULL and its entries in file. */
typedef struct QChunk
{
	struct QChunk *next;
	size_t		head,
				tail;
	uint8_t    *data;
	char	   *file;
} QChunk;

typedef struct Queue
{
	QChunk	   *first,
			   *last;
	size_t		per;			/* entries per chunk */
	size_t		esz;			/* bytes per entry: 8 + world size */
	uint64_t	n;
	uint64_t	mem,			/* chunk bytes resident */
				budget;			/* spill beyond this; 0 = never */
	const char *dir;			/* where spill files go */
	uint64_t	nfile;			/* spill files written, for unique names */
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
			   *ev[CMC_MAX_PROPS];
	int			ninv,
				nactp,
				nwit,
				nqui,
				nev;
	int			wit_depth[CMC_MAX_PROPS];
	Table		t;			/* DFS: depth per state */
	Store		st;				/* BFS: records and parents */
	/* scratch, sized per run (docs/research/02 M1) */
	uint8_t    *dur,			/* durable bytes before a step */
			   *detst,			/* determinism gate: state copy */
			   *qw;				/* quiescence probe world */
	CmcOut	   *deto;			/* determinism gate: outputs */
	/* bounded liveness (CMC_EVENTUALLY): fair-schedule buffers and a memo
	 * of states known to reach the goal (Ent.depth = fair steps to it) */
	Table		live;
	uint8_t    *fw,
			   *fw0;			/* fw0: the world at a drain generation's start */
	CmcAction   *fsuf;			/* the fair suffix of the last failure */
	int			nfsuf;
	CmcAction   *facts;			/* actions enabled at a generation's start */
	uint64_t   *ffp,			/* round-start fingerprints of the fair run */
			   *dfp;			/* generation-start fingerprints of a drain */
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
xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
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
/*
 * The BFS state store: records (fingerprint, parent record, action) and
 * an index of them by fingerprint, split into NSHARD shards by the low
 * byte of fp0 so that parallel workers can insert into disjoint shards.
 * An index slot holds a local record number and 24 bits of the
 * fingerprint, so most probes that miss never touch a record.  Growing a
 * shard's index frees the old one first and rebuilds it from the records,
 * so unlike Table two copies of the index are never resident at once;
 * only one shard grows at a time per worker, so the transient is a
 * fraction of the store.  A trace is the parent chain from a record back
 * to the root.
 */

static inline Rec *
store_rec(const Store *st, uint64_t k)
{
	return &st->sh[k % NSHARD].rec[k / NSHARD];
}

static void
shard_index(Shard *h, uint64_t cap)
{
	uint64_t	mask = cap - 1;

	free(h->slot);
	h->slot = xcalloc(cap, sizeof(uint64_t));
	h->cap = cap;
	for (uint64_t k = 0; k < h->n; k++)
	{
		const Rec  *e = &h->rec[k];
		uint64_t	i = (e->fp1 >> 8) & mask;

		while (h->slot[i])
			i = (i + 1) & mask;
		h->slot[i] = (e->fp1 >> REC_BITS) << REC_BITS | (k + 1);
	}
}

static void
store_init(Store *st)
{
	st->sh = xcalloc(NSHARD, sizeof(Shard));
}

static void
store_free(Store *st)
{
	if (st->sh)
		for (int i = 0; i < NSHARD; i++)
		{
			free(st->sh[i].rec);
			free(st->sh[i].slot);
		}
	free(st->sh);
	st->sh = NULL;
}

/* Returns the slot of shard h for fingerprint (f0, f1): the one holding
 * it, or the empty one where it would go. */
static uint64_t
shard_probe(const Shard *h, uint64_t f0, uint64_t f1)
{
	uint64_t	mask = h->cap - 1,
				tag = (f1 >> REC_BITS) << REC_BITS;

	for (uint64_t i = (f1 >> 8) & mask;; i = (i + 1) & mask)
	{
		uint64_t	v = h->slot[i];
		const Rec  *e;

		if (!v)
			return i;
		if ((v & ~REC_MASK) != tag)
			continue;
		e = &h->rec[(v & REC_MASK) - 1];
		if (e->fp0 == f0 && e->fp1 == f1)
			return i;
	}
}

/*
 * Returns the record number of fingerprint (f0, f1), adding it with
 * parent and act when it is new (*isnew).  Only the worker that owns
 * shard f0 % NSHARD may call this for it.  Note that a slot numbers
 * 2^40 records per shard, 32 TiB of them, so memory runs out (an exit 3
 * from xrealloc()) long before the numbers do.
 */
static uint64_t
store_add(Store *st, uint64_t f0, uint64_t f1, uint64_t parent, CmcAction act, bool *isnew)
{
	Shard	   *h = &st->sh[f0 % NSHARD];
	uint64_t	i;
	Rec		   *e;

	if ((h->n + 1) * 4 > h->cap * 3)
		shard_index(h, h->cap ? h->cap * 2 : 256);
	i = shard_probe(h, f0, f1);
	*isnew = h->slot[i] == 0;
	if (!*isnew)
		return ((h->slot[i] & REC_MASK) - 1) * NSHARD + f0 % NSHARD;
	if (h->n == h->rcap)
	{
		h->rcap = h->rcap ? h->rcap + h->rcap / 2 : 1024;
		h->rec = xrealloc(h->rec, sizeof(Rec) * h->rcap);
	}
	e = &h->rec[h->n];
	e->fp0 = f0;
	e->fp1 = f1;
	e->parent = parent;
	e->act = act;
	h->slot[i] = (f1 >> REC_BITS) << REC_BITS | (h->n + 1);
	return h->n++ * NSHARD + f0 % NSHARD;
}

/* ------------------------------------------------------------------ */
/* FIFO of worlds */

/* Where queue chunks spill: $CMC_SCRATCH, else $TMPDIR, else NULL. */
static const char *
spill_dir(void)
{
	const char *d = getenv("CMC_SCRATCH");

	if (!d || !*d)
		d = getenv("TMPDIR");
	return d && *d ? d : NULL;
}

static void
q_init(Queue *q, size_t ws, uint64_t budget)
{
	memset(q, 0, sizeof(*q));
	q->esz = 8 + ws;
	/* a chunk is never larger than the budget, so a small one spills */
	q->per = (budget && budget < (1u << 22) ? budget : (1u << 22)) / q->esz + 1;
	q->budget = budget;
	q->dir = spill_dir();
}

static _Noreturn void
q_die(const char *what, const char *file)
{
	fprintf(stderr, "cmc: cannot %s BFS queue spill file %s: %s\n", what, file, strerror(errno));
	exit(3);
}

/* Writes full chunk c to a file of its own and frees its data. */
static void
q_spill(Queue *q, QChunk *c)
{
	size_t		len = strlen(q->dir) + 64;
	FILE	   *f;

	c->file = xmalloc(len);
	snprintf(c->file, len, "%s/cmc-queue-%ld-%" PRIu64, q->dir, (long) getpid(), q->nfile++);
	if (!(f = fopen(c->file, "wb")))
		q_die("create", c->file);
	if (fwrite(c->data, q->esz, c->tail, f) != c->tail || fclose(f) != 0)
		q_die("write", c->file);
	free(c->data);
	c->data = NULL;
	q->mem -= q->per * q->esz;
}

/* Reads spilled chunk c back and removes its file. */
static void
q_load(Queue *q, QChunk *c)
{
	FILE	   *f = fopen(c->file, "rb");

	if (!f)
		q_die("open", c->file);
	c->data = xmalloc(q->per * q->esz);
	if (fread(c->data, q->esz, c->tail, f) != c->tail)
		q_die("read", c->file);
	fclose(f);
	remove(c->file);
	free(c->file);
	c->file = NULL;
	q->mem += q->per * q->esz;
}

static void
q_push(Queue *q, uint64_t rec, const uint8_t *w)
{
	QChunk	   *c = q->last;
	uint8_t    *e;

	if (!c || c->tail == q->per)
	{
		/* a full chunk between the two ends waits on disk when over budget */
		if (c && c != q->first && q->budget && q->mem > q->budget)
			q_spill(q, c);
		c = xcalloc(1, sizeof(QChunk));
		c->data = xmalloc(q->per * q->esz);
		q->mem += q->per * q->esz;
		if (q->last)
			q->last->next = c;
		else
			q->first = c;
		q->last = c;
	}
	e = c->data + c->tail * q->esz;
	memcpy(e, &rec, 8);
	memcpy(e + 8, w, q->esz - 8);
	c->tail++;
	q->n++;
}

static bool
q_pop(Queue *q, uint64_t *rec, uint8_t *w)
{
	QChunk	   *c = q->first;
	const uint8_t *e;

	if (!c || c->head == c->tail)
		return false;
	if (!c->data)
		q_load(q, c);
	e = c->data + c->head * q->esz;
	memcpy(rec, e, 8);
	memcpy(w, e + 8, q->esz - 8);
	c->head++;
	q->n--;
	if (c->head == c->tail && c->head == q->per)
	{
		q->first = c->next;
		if (!q->first)
			q->last = NULL;
		free(c->data);
		free(c);
		q->mem -= q->per * q->esz;
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

		if (c->file)
		{
			remove(c->file);
			free(c->file);
		}
		free(c->data);
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

/*
 * Files property p under its kind.  Returns 0, or -1 with the reason in err
 * when p has no known kind (it would never be checked) or its kind already
 * holds CMC_MAX_PROPS properties.  Both are checked before anything is
 * written.
 */
static int
add_prop(CmcRun *r, const CmcProp *p, char *err, size_t errlen)
{
	const CmcProp **arr;
	int		   *n;

	switch (p->kind)
	{
		case CMC_INV:
			arr = r->inv;
			n = &r->ninv;
			break;
		case CMC_ACTION_PROP:
			arr = r->actp;
			n = &r->nactp;
			break;
		case CMC_WITNESS:
			arr = r->wit;
			n = &r->nwit;
			break;
		case CMC_QUIESCENT:
			arr = r->qui;
			n = &r->nqui;
			break;
		case CMC_EVENTUALLY:
			arr = r->ev;
			n = &r->nev;
			break;
		default:
			snprintf(err, errlen, "property %s has unknown kind %d", p->name, (int) p->kind);
			return -1;
	}
	if (*n >= CMC_MAX_PROPS)
	{
		snprintf(err, errlen, "too many properties of one kind (at most %d): %s",
				 CMC_MAX_PROPS, p->name);
		return -1;
	}
	arr[(*n)++] = p;
	return 0;
}

/*
 * The scratch buffers cmc_enabled(), cmc_apply() and the fair schedule
 * write, and the liveness memo.  Each BFS worker has its own set, in a
 * copy of the run (bfs_clone()); everything else in a run is read-only
 * during a search.
 */
static void
scratch_alloc(CmcRun *r)
{
	r->dur = xmalloc(r->ss + 1);
	r->detst = xmalloc(r->ss + 1);
	r->deto = xmalloc(sizeof(CmcOut));
	r->qw = xmalloc(r->ws);
	r->fw = xmalloc(r->ws);
	r->fw0 = xmalloc(r->ws);
	r->fsuf = xmalloc(sizeof(CmcAction) * ((size_t) r->o.live_steps + 1));
	r->facts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
	r->ffp = xmalloc(sizeof(uint64_t) * 2 * ((size_t) r->o.live_steps + 2));
	r->dfp = xmalloc(sizeof(uint64_t) * 2 * ((size_t) r->o.live_steps + 2));
	r->fstep = xmalloc(sizeof(int) * ((size_t) r->o.live_steps + 2));
}

static void
scratch_free(CmcRun *r)
{
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
	free(r->dfp);
	free(r->fstep);
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
	/* cmc_check() would run no search and report a pass */
	if (r->o.search < CMC_BFS || r->o.search > CMC_REPLAY)
		return run_reject(r, err, errlen, "unknown search");
	if (r->o.search == CMC_REPLAY && !r->o.replay_in)
		return run_reject(r, err, errlen, "a replay needs replay_in, the file to replay");
	if (r->o.workers < 0 || r->o.workers > CMC_MAX_WORKERS)
		return run_reject(r, err, errlen, "workers must be from 0 to " CMC_STR(CMC_MAX_WORKERS));
	if (r->o.workers > 1 && r->o.search != CMC_BFS)
		return run_reject(r, err, errlen, "more than one worker needs breadth-first search");
	if (r->o.queue_mem && r->o.search != CMC_BFS)
		return run_reject(r, err, errlen, "a queue memory budget needs breadth-first search");
	if (r->o.queue_mem && !spill_dir())
		return run_reject(r, err, errlen,
						  "a queue memory budget needs CMC_SCRATCH or TMPDIR for its files");
	/* no walk looks at no state, which must not read as a pass */
	if (r->o.search == CMC_SIMULATE && r->o.walks < 1)
		return run_reject(r, err, errlen, "a simulation needs at least one walk");
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
	scratch_alloc(r);

	for (int j = 0; j < m->nalways; j++)
		if (add_prop(r, &m->props[j], err, errlen) != 0)
			goto bad_prop;
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
			if (add_prop(r, p, err, errlen) != 0)
				goto bad_prop;
		}
	}
	else
		for (int j = m->nalways; j < m->nprops; j++)
			if (add_prop(r, &m->props[j], err, errlen) != 0)
				goto bad_prop;
	if (r->nev && r->direct)
		return run_reject(r, err, errlen, "CMC_EVENTUALLY needs actor mode");
	if (r->core->durable_size && !r->core->apply_persist)
		return run_reject(r, err, errlen,
						  "durable_size > 0 needs apply_persist (the engine owns durable)");
	return r;

bad_prop:
	cmc_run_free(r);
	return NULL;
}

void
cmc_run_free(CmcRun *r)
{
	if (!r)
		return;
	table_free(&r->t);
	store_free(&r->st);
	scratch_free(r);
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

				if (i >= NMSG(r, w) || (kind == CMC_A_DROP && !DROPS(r, w)))
					goto bad;
				e = MSG(r, w, i);
				if (FIFO(r))
				{
					if (kind == CMC_A_DUP || !fifo_head(r, w, i) ||
						!fifo_open(r, w, e[0], e[1]))
						goto bad;
					if (kind == CMC_A_DROP)
					{
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
				/* what the node sent itself never left the process, so it dies
				 * with the process in either network */
				chan_clear(r, w, n, n);
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
 * (every deliverable message, every pending persist, wake and connection
 * reset), then fires each timer armed at that point once, draining after
 * each, so messages arrive before timeouts.  A drain runs in generations
 * (fair_drain), so no channel waits behind another.  The world is finite
 * and each generation a function of the world at its start, so a drain
 * that would never empty comes back to a generation-start world, and ends
 * there: cores that keep messaging each other cannot postpone an armed
 * timer forever.  It is deterministic and every round is a function of
 * the world at its start.  The check passes
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

/*
 * Returns delivery a, enabled on r->fw0, pointed at the same message in
 * r->fw, where its index may have moved as other messages came and went.
 * Only its own delivery removes a message during a drain; were it gone,
 * the index would be past the last entry and cmc_apply() would refuse it.
 */
static CmcAction
fair_redirect(CmcRun *r, CmcAction a)
{
	const uint8_t *key = MSG(r, r->fw0, CMC_ACT_D(a));
	int			j;

	for (j = 0; j < NMSG(r, r->fw); j++)
		if (memcmp(MSG(r, r->fw, j), key, MSG_KEY) == 0)
			break;
	return CMC_ACT(CMC_A_DELIVER, key[0], key[1], 0, j);
}

/*
 * Drains r->fw in generations.  A generation takes once each, in
 * cmc_enabled order, the deliveries, flushes, wakes and resets enabled at
 * its start; what they enable waits for the next generation, so a
 * message is never postponed behind an endless stream on another channel.
 * The drain ends when a generation has nothing to take, or when a
 * generation's start world repeats an earlier one in this drain: the
 * cores then keep each other busy forever, and stopping is what lets the
 * timers that follow fire.  Same returns as fair_step, 0 = drained.
 */
static int
fair_drain(CmcRun *r)
{
	for (int ng = 0;; ng++)
	{
		uint64_t	fp[2];
		int			na;
		bool		took = false;

		/* every generation but the last takes a step, so ng <= live_steps */
		cmc_fingerprint(r->fw, r->ws, fp);
		for (int g = 0; g < ng; g++)
			if (r->dfp[2 * g] == fp[0] && r->dfp[2 * g + 1] == fp[1])
				return 0;
		r->dfp[2 * ng] = fp[0];
		r->dfp[2 * ng + 1] = fp[1];

		memcpy(r->fw0, r->fw, r->ws);
		na = cmc_enabled(r, r->fw0, r->facts, CMC_MAX_ACTIONS);
		if (na < 0)
		{
			snprintf(r->err, sizeof(r->err), "too many enabled actions in the fair run");
			return -1;
		}
		for (int i = 0; i < na; i++)
		{
			CmcAction	a = r->facts[i];
			int			rc;

			if (!fair_kind(CMC_ACT_KIND(a)))
				continue;
			if (CMC_ACT_KIND(a) == CMC_A_DELIVER)
				a = fair_redirect(r, a);
			if ((rc = fair_step(r, a)) != 0)
				return rc;
			took = true;
		}
		if (!took)
			return 0;
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

/* Rebuilds the action path to record rec from the parent chain. */
static int
trace_from_store(const CmcRun *r, uint64_t rec, CmcAction **out)
{
	int			cap = 64,
				n = 0;
	CmcAction   *a = xmalloc(sizeof(CmcAction) * cap);

	while (store_rec(&r->st, rec)->parent != REC_ROOT)
	{
		const Rec  *e = store_rec(&r->st, rec);

		if (n == cap)
		{
			cap *= 2;
			a = xrealloc(a, sizeof(CmcAction) * cap);
		}
		a[n++] = e->act;
		rec = e->parent;
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

/* The engine error for a negative cmc_enabled() result, the same in every
 * search and in replay. */
static const char *
enabled_error(int na)
{
	return na == -2 ? "determinism gate: action list differs on a repeat" :
		"too many enabled actions (raise CMC_MAX_ACTIONS)";
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
/* ------------------------------------------------------------------ */
/*
 * BFS, in batches (TLC semantics with any number of workers).
 *
 * The master pops a batch of states off the queue, a prefix of it, and
 * the workers expand it in four phases separated by barriers:
 *
 *   1. expand: workers claim states and compute each one's enabled
 *      actions, quiescence and liveness checks, successors, action
 *      properties and successor fingerprints, stopping a state at its
 *      first event (an error or a violation);
 *   2. insert: each worker inserts, into the store shards it owns, the
 *      successors of every state of the batch in batch order, so each
 *      fingerprint gets the parent and the "new" flag that a one-worker
 *      search would give it;
 *   3. check: each worker checks the invariants and the witnesses not yet
 *      reached on the new successors it generated;
 *   4. merge: the master walks the batch in order and does what a
 *      one-worker search does after each step: counts, coverage, depth,
 *      witness depths, the queue, and the first event in (state, action)
 *      order, which ends the search.
 *
 * Phase 4 sees exactly the sequence of a one-worker BFS, so the counts,
 * the depth, the witness depths and the counterexample are the same for
 * every worker count; work done past the first event is thrown away.
 * Workers share the run read-only and write only their own copy's scratch
 * (bfs_clone()), so the cores' step functions and the properties must be
 * pure functions of their arguments, as cmc(7) requires.
 */

enum
{
	BEV_ENABLED = 1,			/* cmc_enabled() failed at the state */
	BEV_QUI,					/* a quiescence property failed at it */
	BEV_FAIR,					/* the fair schedule from it failed */
	BEV_APPLY,					/* an action of it failed to apply */
	BEV_ACTP,					/* an action of it broke an action property */
	BEV_INV						/* a new successor broke an invariant */
};

typedef struct BEvent
{
	int			kind;
	CmcAction	act;			/* BEV_APPLY, BEV_ACTP */
	const CmcProp *prop;		/* NULL = an engine error */
	char		why[sizeof(((CmcWhy *) 0)->msg)];
	CmcAction   *fsuf;			/* BEV_FAIR: the fair suffix */
	int			nfsuf;
} BEvent;

typedef struct Succ
{
	CmcAction	act;
	uint64_t	fp0,
				fp1,
				rec;			/* its record (phase 2) */
	uint64_t	wit;			/* witnesses it reaches (phase 3) */
	uint32_t	j;				/* the batch state it came from */
	int			evi;			/* BEV_INV event, or -1 */
	bool		isnew;
} Succ;

typedef struct BState
{
	uint64_t	rec;
	uint32_t	beg,
				end;			/* its successors in worker w's list */
	int			w;
	int			evi;			/* its event in worker w's list, or -1 */
} BState;

typedef struct Bfs Bfs;

typedef struct Worker
{
	Bfs		   *b;
	CmcRun	   *r;				/* worker 0: the run itself */
	int			id;
	Succ	   *succ;
	uint8_t    *wbuf;			/* the successor worlds */
	uint32_t	nsucc,
				csucc;
	BEvent	   *ev;
	int			nev,
				cev;
	CmcAction   *acts;
	thrd_t		th;
} Worker;

struct Bfs
{
	CmcRun	   *r;
	int			nw;
	Worker	   *w;
	BState	   *bs;
	uint8_t    *bw;				/* the batch's worlds */
	uint32_t	nb,
				cap;			/* states in the batch, and room */
	atomic_uint next;			/* phase 1: the next state to claim */
	uint64_t	witdone;		/* witnesses reached before the batch */
	mtx_t		mu;
	cnd_t		cv;
	unsigned	gen;
	int			arrived;
	bool		quit;
};

typedef struct BLevel
{
	uint64_t	left,			/* states of this level still to expand */
				next;			/* states found for the next level */
	int			level;
} BLevel;

#define BFS_CLAIM 8				/* states a worker claims at a time */
#define BFS_PER_WORKER 256		/* batch states per worker */

/* Returns a new event slot of worker k. */
static BEvent *
bfs_event(Worker *k, int kind, const CmcProp *p, const char *why)
{
	BEvent	   *e;

	if (k->nev == k->cev)
	{
		k->cev = k->cev ? 2 * k->cev : 16;
		k->ev = xrealloc(k->ev, sizeof(BEvent) * (size_t) k->cev);
	}
	e = &k->ev[k->nev++];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->prop = p;
	snprintf(e->why, sizeof(e->why), "%s", why);
	return e;
}

/* Returns room for one more successor of worker k, and its world. */
static Succ *
bfs_succ(Worker *k, uint8_t **w)
{
	size_t		ws = k->r->ws;

	if (k->nsucc == k->csucc)
	{
		k->csucc = k->csucc ? 2 * k->csucc : 1024;
		k->succ = xrealloc(k->succ, sizeof(Succ) * k->csucc);
		k->wbuf = xrealloc(k->wbuf, ws * k->csucc);
	}
	*w = k->wbuf + ws * k->nsucc;
	return &k->succ[k->nsucc];
}

/*
 * Phase 1 for batch state j: what a one-worker search does from popping
 * it to inserting its successors, up to the first event, which ends the
 * state (an event after it could never be reported).
 */
static void
bfs_expand(Worker *k, uint32_t j)
{
	CmcRun	   *r = k->r;
	BState	   *bs = &k->b->bs[j];
	const uint8_t *s = k->b->bw + r->ws * j;
	CmcWhy		why;
	const CmcProp *bad;
	int			na = cmc_enabled(r, s, k->acts, CMC_MAX_ACTIONS);

	bs->w = k->id;
	bs->beg = bs->end = k->nsucc;
	bs->evi = -1;
	if (na < 0)
	{
		bs->evi = k->nev;
		bfs_event(k, BEV_ENABLED, NULL, enabled_error(na));
		return;
	}
	if (r->nqui && is_quiescent(r, s, k->acts, na) && (bad = check_qui(r, s, &why)))
	{
		bs->evi = k->nev;
		bfs_event(k, BEV_QUI, bad, why.msg);
		return;
	}
	if (r->nev)
	{
		int			rc = check_ev(r, s, &why, &bad);

		if (rc != 0)
		{
			BEvent	   *e;

			bs->evi = k->nev;
			e = bfs_event(k, BEV_FAIR, rc < 0 ? NULL : bad, rc < 0 ? r->err : why.msg);
			e->nfsuf = r->nfsuf;
			e->fsuf = xmalloc(sizeof(CmcAction) * ((size_t) r->nfsuf + 1));
			memcpy(e->fsuf, r->fsuf, sizeof(CmcAction) * (size_t) r->nfsuf);
			return;
		}
	}
	for (int i = 0; i < na; i++)
	{
		uint8_t    *t;
		Succ	   *u = bfs_succ(k, &t);
		uint64_t	fp[2];

		memcpy(t, s, r->ws);
		if (cmc_apply(r, t, k->acts[i]) != 0)
		{
			bs->evi = k->nev;
			bfs_event(k, BEV_APPLY, NULL, r->err)->act = k->acts[i];
			return;
		}
		if (r->nactp && (bad = check_actp(r, s, k->acts[i], t, &why)))
		{
			bs->evi = k->nev;
			bfs_event(k, BEV_ACTP, bad, why.msg)->act = k->acts[i];
			return;
		}
		cmc_fingerprint(t, r->ws, fp);
		u->act = k->acts[i];
		u->fp0 = fp[0];
		u->fp1 = fp[1];
		u->j = j;
		u->evi = -1;
		u->wit = 0;
		u->isnew = false;
		bs->end = ++k->nsucc;
	}
}

/* Phase 2: worker k inserts the batch's successors that fall in its
 * shards, in batch order. */
static void
bfs_insert(Worker *k)
{
	Bfs		   *b = k->b;

	for (uint32_t j = 0; j < b->nb; j++)
	{
		const BState *bs = &b->bs[j];
		Worker	   *o = &b->w[bs->w];

		for (uint32_t i = bs->beg; i < bs->end; i++)
		{
			Succ	   *u = &o->succ[i];

			if ((int) (u->fp0 % NSHARD % (uint64_t) b->nw) != k->id)
				continue;
			u->rec = store_add(&b->r->st, u->fp0, u->fp1, bs->rec, u->act, &u->isnew);
		}
	}
}

/* Phase 3: worker k checks the invariants, and the witnesses not reached
 * before the batch, on the new successors it generated. */
static void
bfs_check(Worker *k)
{
	CmcRun	   *r = k->r;

	for (uint32_t i = 0; i < k->nsucc; i++)
	{
		Succ	   *u = &k->succ[i];
		CmcWorld	W = {r, k->wbuf + r->ws * i};
		CmcWhy		why;
		const CmcProp *bad;

		if (!u->isnew)
			continue;
		if ((bad = check_inv(r, W.bytes, &why)))
		{
			u->evi = k->nev;
			bfs_event(k, BEV_INV, bad, why.msg);
			continue;
		}
		for (int w = 0; w < r->nwit; w++)
			if (!(k->b->witdone & (UINT64_C(1) << w)) && r->wit[w]->state(&W, NULL))
				u->wit |= UINT64_C(1) << w;
	}
}

/* Waits until every worker has arrived; worker 0 returns first-in-line
 * only after all have. */
static void
bfs_barrier(Bfs *b)
{
	unsigned	g;

	if (b->nw == 1)
		return;
	mtx_lock(&b->mu);
	g = b->gen;
	if (++b->arrived == b->nw)
	{
		b->arrived = 0;
		b->gen++;
		cnd_broadcast(&b->cv);
	}
	else
		while (g == b->gen)
			cnd_wait(&b->cv, &b->mu);
	mtx_unlock(&b->mu);
}

/* Phases 1 to 3 of one batch, run by every worker. */
static void
bfs_phases(Worker *k)
{
	Bfs		   *b = k->b;

	k->nsucc = 0;
	for (int e = 0; e < k->nev; e++)
		free(k->ev[e].fsuf);
	k->nev = 0;
	for (;;)
	{
		uint32_t	j = atomic_fetch_add(&b->next, BFS_CLAIM);

		if (j >= b->nb)
			break;
		for (uint32_t i = j; i < j + BFS_CLAIM && i < b->nb; i++)
			bfs_expand(k, i);
	}
	bfs_barrier(b);
	bfs_insert(k);
	bfs_barrier(b);
	bfs_check(k);
}

/* A worker thread other than 0: a batch per round, until quit. */
static int
bfs_thread(void *arg)
{
	Worker	   *k = arg;
	Bfs		   *b = k->b;

	for (;;)
	{
		bfs_barrier(b);			/* the master has filled the batch */
		if (b->quit)
			return 0;
		bfs_phases(k);
		bfs_barrier(b);			/* the batch is checked */
	}
}

/* A copy of run r for a worker: shared read-only state, its own scratch. */
static CmcRun *
bfs_clone(CmcRun *r)
{
	CmcRun	   *c = xmalloc(sizeof(CmcRun));

	*c = *r;
	memset(&c->t, 0, sizeof(c->t));
	memset(&c->st, 0, sizeof(c->st));
	scratch_alloc(c);
	table_init(&c->live, 1u << 10);
	return c;
}

static bool
bfs_all_wit(const CmcRun *r)
{
	for (int i = 0; i < r->nwit; i++)
		if (!r->wit_depth[i])
			return false;
	return true;
}

/* Starts the workers of b; worker 0 is the master, on run r itself. */
static void
bfs_start(Bfs *b, CmcRun *r)
{
	memset(b, 0, sizeof(*b));
	b->r = r;
	b->nw = r->o.workers > 1 ? r->o.workers : 1;
	b->cap = (uint32_t) b->nw * BFS_PER_WORKER;
	b->bs = xmalloc(sizeof(BState) * b->cap);
	b->bw = xmalloc(r->ws * b->cap);
	b->w = xcalloc((size_t) b->nw, sizeof(Worker));
	if (b->nw > 1 && (mtx_init(&b->mu, mtx_plain) != thrd_success ||
					  cnd_init(&b->cv) != thrd_success))
	{
		fprintf(stderr, "cmc: cannot set up the BFS workers\n");
		exit(3);
	}
	for (int i = 0; i < b->nw; i++)
	{
		Worker	   *k = &b->w[i];

		k->b = b;
		k->id = i;
		k->r = i == 0 ? r : bfs_clone(r);
		k->acts = xmalloc(sizeof(CmcAction) * CMC_MAX_ACTIONS);
		if (i > 0 && thrd_create(&k->th, bfs_thread, k) != thrd_success)
		{
			fprintf(stderr, "cmc: cannot start BFS worker %d\n", i);
			exit(3);
		}
	}
}

static void
bfs_stop(Bfs *b)
{
	b->quit = true;
	bfs_barrier(b);
	for (int i = 0; i < b->nw; i++)
	{
		Worker	   *k = &b->w[i];

		if (i > 0)
		{
			thrd_join(k->th, NULL);
			scratch_free(k->r);
			free(k->r);
		}
		for (int e = 0; e < k->nev; e++)
			free(k->ev[e].fsuf);
		free(k->ev);
		free(k->succ);
		free(k->wbuf);
		free(k->acts);
	}
	if (b->nw > 1)
	{
		mtx_destroy(&b->mu);
		cnd_destroy(&b->cv);
	}
	free(b->w);
	free(b->bs);
	free(b->bw);
}

/* Fills the next batch from the queue; returns its size. */
static uint32_t
bfs_fill(Bfs *b, Queue *q)
{
	uint32_t	n = 0;

	while (n < b->cap && q_pop(q, &b->bs[n].rec, b->bw + b->r->ws * n))
		n++;
	b->nb = n;
	atomic_store(&b->next, 0);
	b->witdone = 0;
	for (int w = 0; w < b->r->nwit; w++)
		if (b->r->wit_depth[w])
			b->witdone |= UINT64_C(1) << w;
	return n;
}

/* The search-limit checks a one-worker search makes after each state. */
static bool
bfs_limits(CmcRun *r, CmcResult *res, double t0, double *tlast, BLevel *lv, uint64_t qn)
{
	if (r->o.max_states && res->distinct >= r->o.max_states)
	{
		res->verdict = CMC_INCOMPLETE;
		snprintf(res->why, sizeof(res->why), "max_states reached");
		return true;
	}
	if ((res->distinct & 0x3fff) == 0)
	{
		double		tn = now_s();

		if (r->o.time_limit > 0 && tn - t0 > r->o.time_limit)
		{
			res->verdict = CMC_INCOMPLETE;
			snprintf(res->why, sizeof(res->why), "time limit reached");
			return true;
		}
		if (r->o.progress_s && tn - *tlast >= r->o.progress_s)
		{
			*tlast = tn;
			progress(r, res, lv->level, qn);
		}
	}
	return false;
}

/* The trace to record rec plus extra actions, into res. */
static void
bfs_trace(CmcRun *r, CmcResult *res, uint64_t rec, const CmcAction *extra, int nextra)
{
	int			n = trace_from_store(r, rec, &res->trace);

	res->trace = xrealloc(res->trace, sizeof(CmcAction) * ((size_t) (n + nextra) + 1));
	memcpy(res->trace + n, extra, sizeof(CmcAction) * (size_t) nextra);
	res->trace_len = n + nextra + 1;
}

/* Reports state j's event e (phase 4), at BFS level 'level'. */
static void
bfs_report(CmcRun *r, CmcResult *res, const BState *bs, const BEvent *e, int level)
{
	set_violation(res, e->prop, e->why);
	if (e->kind == BEV_ACTP)
	{
		/* its successor was generated before the property looked at it */
		res->cov_gen[CMC_ACT_KIND(e->act)]++;
		res->cov_sample[CMC_ACT_KIND(e->act)] = e->act;
		res->generated++;
	}
	if (e->kind == BEV_APPLY || e->kind == BEV_ACTP)
	{
		bfs_trace(r, res, bs->rec, &e->act, 1);
		res->depth = level + 1;
	}
	else if (e->kind == BEV_FAIR)
	{
		bfs_trace(r, res, bs->rec, e->fsuf, e->nfsuf);
		res->fair_from = res->trace_len - e->nfsuf;
	}
	else
		bfs_trace(r, res, bs->rec, NULL, 0);
}

/*
 * Phase 4 for batch state j.  Returns 1 when the search ends at it (res
 * set), 0 otherwise.
 */
static int
bfs_merge(Bfs *b, uint32_t j, Queue *q, CmcResult *res, BLevel *lv)
{
	CmcRun	   *r = b->r;
	const BState *bs = &b->bs[j];
	Worker	   *k = &b->w[bs->w];

	for (uint32_t i = bs->beg; i < bs->end; i++)
	{
		const Succ *u = &k->succ[i];
		unsigned	kind = CMC_ACT_KIND(u->act);

		res->cov_gen[kind]++;
		res->cov_sample[kind] = u->act;
		res->generated++;
		if (!u->isnew)
			continue;
		res->distinct++;
		res->cov_new[kind]++;
		if (lv->level + 1 > res->depth)
			res->depth = lv->level + 1;
		if (u->evi >= 0)
		{
			set_violation(res, k->ev[u->evi].prop, k->ev[u->evi].why);
			bfs_trace(r, res, u->rec, NULL, 0);
			return 1;
		}
		for (int w = 0; w < r->nwit; w++)
			if ((u->wit & (UINT64_C(1) << w)) && !r->wit_depth[w])
				r->wit_depth[w] = lv->level + 1;
		if (r->nwit && r->o.stop_on_witness && bfs_all_wit(r))
			return 2;
		q_push(q, u->rec, k->wbuf + r->ws * i);
		lv->next++;
	}
	if (bs->evi >= 0)
	{
		bfs_report(r, res, bs, &k->ev[bs->evi], lv->level);
		return 1;
	}
	if (--lv->left == 0)
	{
		lv->left = lv->next;
		lv->next = 0;
		lv->level++;
	}
	return 0;
}

static void
run_bfs(CmcRun *r, CmcResult *res)
{
	Queue		q;
	Bfs			b;
	uint8_t    *s = xmalloc(r->ws);
	uint64_t	fp[2],
				rec;
	CmcWhy		why;
	const CmcProp *bad;
	bool		isnew;
	double		t0 = now_s(),
				tlast = t0;
	BLevel		lv = {1, 0, 1};
	int			rc = 0;
	uint64_t	left = 0;

	store_init(&r->st);
	q_init(&q, r->ws, r->o.queue_mem);
	cmc_world_init(r, s);
	cmc_fingerprint(s, r->ws, fp);
	rec = store_add(&r->st, fp[0], fp[1], REC_ROOT, 0, &isnew);
	res->distinct = 1;
	res->generated = 1;
	res->depth = 1;
	if ((bad = check_inv(r, s, &why)))
	{
		set_violation(res, bad, why.msg);
		res->trace_len = 1;
		goto out;
	}
	if (check_wit(r, s, 1) && r->nwit && r->o.stop_on_witness)
		goto stopped;
	q_push(&q, rec, s);

	bfs_start(&b, r);
	while (rc == 0 && bfs_fill(&b, &q) > 0)
	{
		bfs_barrier(&b);		/* the batch is ready */
		bfs_phases(&b.w[0]);
		bfs_barrier(&b);		/* every worker has checked */
		for (uint32_t j = 0; j < b.nb && rc == 0; j++)
		{
			rc = bfs_merge(&b, j, &q, res, &lv);
			if (rc == 0 && bfs_limits(r, res, t0, &tlast, &lv, q.n + b.nb - j - 1))
				rc = 3;
			/* the batch states after j were never popped, in one-worker terms */
			if (rc)
				left = b.nb - j - 1;
		}
	}
	bfs_stop(&b);
	if (rc != 2)
		goto out;
stopped:
	/* the rest of the graph was not searched, so this is no pass */
	res->verdict = CMC_INCOMPLETE;
	snprintf(res->why, sizeof(res->why), "stopped once every witness was reached");
out:
	res->left = q.n + left;
	q_free(&q);
	free(s);
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
			set_violation(res, NULL, enabled_error(f->na));
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
		if (f->next == 0 && r->nev)
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
		for (len = 0;; len++)
		{
			int			na = cmc_enabled(r, s, acts, CMC_MAX_ACTIONS);

			if (na < 0)
			{
				snprintf(why.msg, sizeof(why.msg), "%s", enabled_error(na));
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
			/* the walk ends here only after this state, its last, was checked */
			if (na == 0 || len == D - 1)
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
	for (int i = 0;; i++)
	{
		int			na = cmc_enabled(r, s, acts, CMC_MAX_ACTIONS),
					ok = 0,
					rc;

		/* each state, the last included, is checked as BFS checks it */
		if (na < 0)
		{
			set_violation(res, NULL, enabled_error(na));
			res->trace_len = i + 1;
			break;
		}
		if (r->nqui && is_quiescent(r, s, acts, na) && (bad = check_qui(r, s, &why)))
		{
			set_violation(res, bad, why.msg);
			res->trace_len = i + 1;
			break;
		}
		/* a liveness counterexample: the fair suffix follows this state */
		if (r->nev && (rc = check_ev(r, s, &why, &bad)) != 0)
		{
			set_violation(res, rc < 0 ? NULL : bad, rc < 0 ? r->err : why.msg);
			res->fair_from = i + 1;
			res->trace_len = i + 1 + r->nfsuf;
			path = realloc(path, sizeof(CmcAction) * (size_t) (res->trace_len + 1));
			memcpy(path + i, r->fsuf, sizeof(CmcAction) * (size_t) r->nfsuf);
			break;
		}
		if (i == n)
		{
			res->trace_len = n + 1;
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
