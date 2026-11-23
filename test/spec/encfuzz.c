/*
 * encfuzz.c: randomised tests of the abstract checker's value encoding
 * (src/cmc_spec.c), complementing test/spec/distinct.cml.  It builds
 * random pairs of values of one nested type (functions of records that
 * hold sequences of records), over small domains so equal pairs are
 * common, encodes both with the checker's own constructors, and checks:
 *
 *   - the encodings are byte-equal exactly when the two values are equal
 *     as trees (a value differing in one field, element or length never
 *     merges with another, and equal values always do);
 *   - enc_eq(), which the evaluator's == uses, agrees;
 *   - a symmetry permutation and its inverse give back the same bytes.
 *
 * The checker source is included whole so the test reaches its statics.
 * Exits 0 when every check holds; prints the first failure and its seed.
 */
#include "../../src/cmc_spec.c"

#define PAIRS 20000

static uint64_t rs;
static uint64_t
rnd(void)
{
	uint64_t z = (rs += 0x9e3779b97f4a7c15ULL);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/* The test's own value tree: a leaf integer, or n children with keys. */
typedef struct Tr { int tag; long i; int n; long key[4]; struct Tr *kid[4]; } Tr;
static Tr pool[PAIRS * 64]; static int npool;

/* A random tree of the type
 *   [K -> [a : 0 .. 2, b : Seq([c : 0 .. 1], 2)]]
 * where K is one of {0, 1} or {0, 1, 2}, at depth d of that type. */
static Tr *
mk(int d)
{
	Tr *t = &pool[npool++];
	memset(t, 0, sizeof *t);
	switch (d) {
	case 0:		/* the function */
		t->tag = T_FUN; t->n = 2 + (int) (rnd() % 2);
		for (int j = 0; j < t->n; j++) { t->key[j] = j; t->kid[j] = mk(1); }
		break;
	case 1:		/* the record [a, b] */
		t->tag = T_REC; t->n = 2;
		t->key[0] = 0; t->kid[0] = &pool[npool++];
		*t->kid[0] = (Tr) { .tag = T_INT, .i = (long) (rnd() % 3) };
		t->key[1] = 1; t->kid[1] = mk(2);
		break;
	case 2:		/* the sequence */
		t->tag = T_SEQ; t->n = (int) (rnd() % 3);
		for (int j = 0; j < t->n; j++) t->kid[j] = mk(3);
		break;
	default:	/* the record [c] */
		t->tag = T_REC; t->n = 1; t->key[0] = 2; t->kid[0] = &pool[npool++];
		*t->kid[0] = (Tr) { .tag = T_INT, .i = (long) (rnd() % 2) };
	}
	return t;
}
/* a deep copy of t that changes its target-th integer leaf (counting
 * with *at), or, when the target falls on a sequence, drops that
 * sequence's last element */
static Tr *
near(const Tr *t, int *at, int target)
{
	Tr *c = &pool[npool++];
	*c = *t;
	int here = (*at)++;
	if (t->tag == T_INT) { if (here == target) c->i = (c->i + 1) % 2; return c; }
	if (t->tag == T_SEQ && here == target && c->n > 0) c->n--;
	for (int j = 0; j < c->n; j++) c->kid[j] = near(t->kid[j], at, target);
	return c;
}
static int
tr_eq(const Tr *a, const Tr *b)
{
	if (a->tag != b->tag) return 0;
	if (a->tag == T_INT) return a->i == b->i;
	if (a->n != b->n) return 0;
	for (int j = 0; j < a->n; j++)
		if (a->key[j] != b->key[j] || !tr_eq(a->kid[j], b->kid[j])) return 0;
	return 1;
}
/* t encoded by the checker's constructors, into the arena */
static Value
enc(Pst *s, const Tr *t)
{
	if (t->tag == T_INT) return vint(t->i);
	Value kid[4];
	for (int j = 0; j < t->n; j++) kid[j] = enc(s, t->kid[j]);
	return mk_comp(s, t->tag, t->n, t->tag == T_SEQ ? NULL : t->key, kid);
}

int
main(void)
{
	int bad = 0, equal = 0;
	S.nvar = 1;
	g_symn = 2; g_symel[0] = 0; g_symel[1] = 1;	/* swap 0 and 1 */
	for (int p = 0; p < PAIRS && !bad; p++) {
		uint64_t seed = rs;
		npool = 0; ar_release(0);
		Tr *a = mk(0), *b = rnd() % 2 ? mk(0) : a;
		if (b == a && rnd() % 2) {	/* a near copy: one leaf or one length */
			int at = 0;
			b = near(a, &at, (int) (rnd() % 24));
		}
		Pst s = {0}; s.ok = 1;
		Value va = enc(&s, a), vb = enc(&s, b);
		State sa, sb, sp, sq; char err[128];
		if (!s.ok || st_pack(&sa, &va, err, sizeof err) || st_pack(&sb, &vb, err, sizeof err)) {
			printf("encfuzz: seed %llu: cannot build: %s\n", (unsigned long long) seed, s.err); return 1;
		}
		int want = tr_eq(a, b), bytes = sa.nw == sb.nw && !memcmp(sa.w, sb.w, (size_t) sa.nw * sizeof sa.w[0]);
		int ta, tb, eq = enc_eq(sa.w, sb.w, &ta, &tb);
		equal += want;
		if (bytes != want || eq != want) {
			printf("encfuzz: seed %llu: trees %s, bytes %s, enc_eq %d\n", (unsigned long long) seed,
			       want ? "equal" : "differ", bytes ? "equal" : "differ", eq);
			bad = 1;
		}
		int perm[2] = {1, 0};
		sym_apply(&sa, &sp, perm); sym_apply(&sp, &sq, perm);
		if (sq.nw != sa.nw || memcmp(sq.w, sa.w, (size_t) sa.nw * sizeof sa.w[0])) {
			printf("encfuzz: seed %llu: a permutation twice is not the identity\n", (unsigned long long) seed);
			bad = 1;
		}
	}
	if (!bad) printf("encfuzz: ok: %d pairs, %d equal\n", PAIRS, equal);
	return bad;
}
