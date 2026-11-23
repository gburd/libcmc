/*
 * cmc_spec.c: the abstract-model front end.  `cmc --spec FILE.cml`
 * model-checks a declarative specification on its own -- no
 * implementation -- the way TLC checks a TLA+ module.  This is Milestone
 * B of docs/plans/tlaplus-scope.md: the reusable abstract model that
 * (later, Milestone C) an implementation is checked to refine, and that
 * (Milestone D) translates to TLA+ for TLC/Apalache/TLAPS.
 *
 * The language is small and explicit -- a decidable, SMT-encodable
 * fragment -- not full TLA+:
 *
 *   module NAME
 *   const N = 3                       # a finite parameter
 *   var x : 0 .. N                    # a variable, with a finite domain
 *   var pc : [0 .. N-1 -> 0 .. 2]     # a function (finite map) variable
 *   init x == 0 and pc == [i in 0 .. N-1 -> 0]
 *   action inc:                       # a guarded, possibly parametric action
 *     when x < N                      # guard (boolean over the vars)
 *     x' = x + 1                      # assignment(s) to primed variables
 *   action vote(i in 0 .. N-1):       # a parameter ranges over a finite set
 *     when ...
 *     pc' = [pc except [i] = 1]
 *   invariant safe: EXPR              # checked in every reachable state
 *   var r : [a : 0 .. 3, b : 0 .. 1]  # a record variable
 *   var log : Seq([idx : 1 .. 2, val : 0 .. 1], 4)   # at most 4 elements
 *   def Next(i) = (i + 1) mod N       # a named operator, no recursion
 *
 * Values are bounded integers, booleans, finite sets of integers,
 * functions from integers to values, records and bounded sequences
 * (tuples are sequences).  A world is the tuple of variable values,
 * each in one canonical encoding, so equal states have equal bytes; the
 * search is breadth-first, so a violation is reported with the shortest
 * action sequence, in TLC's wording.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmc_spec.h"

/* ---- limits (bounded, so a state is a fixed size of bytes) ---------- */
#define SP_MAXVAR 16
#define SP_MAXACT 32
#define SP_MAXINV 16
#define SP_MAXCONST 16
#define SP_MAXPARAM 4			/* bound parameters per action */
#define SP_SETMAX 24			/* elements in a set value */
#define SP_MAPMAX 12			/* entries in a map value */
#define SP_NAME 32
#define SP_EXPR 512
#define SP_MAXDEF 32			/* def operators */
#define SP_MAXFLD 32			/* distinct record field names */
#define SP_MAXTY 64			/* nodes in the variables' type trees */
#define SP_STATEW 1024			/* words in a state's encoding */

/* ---- the value model ------------------------------------------------
 * Every value has one canonical encoding as a run of words, so two
 * values are equal exactly when their encodings are, and a state (the
 * concatenation of its variables' encodings) can be hashed and compared
 * as bytes.  A scalar is two words, [tag, value].  A compound value is
 * [tag, len, n, ...], len counting every word including these three:
 *   T_SET  n integers, sorted and unique
 *   T_FUN  n pairs (key, child), keys integers in increasing order
 *   T_REC  n pairs (field id, child), field ids increasing
 *   T_SEQ  n children, in order
 * where a child is itself an encoding.  Each encoding is self-delimiting
 * (its tag gives its length), so a concatenation reads back one way.
 *
 * A Value is a view: a scalar carries its tag and value; a compound
 * value points at its encoding, which lives in a state or in the
 * evaluation arena (ar_*), never in the Value itself. */
enum { T_INT, T_BOOL, T_SET, T_FUN, T_REC, T_SEQ };
typedef struct {
	int tag;
	long i;						/* T_INT / T_BOOL (0/1) */
	const long *w;				/* compound: the encoding (w[0] == tag) */
} Value;

static Value vint(long x){ Value v={0}; v.tag=T_INT; v.i=x; return v; }
static Value vbool(int b){ Value v={0}; v.tag=T_BOOL; v.i=b?1:0; return v; }
static int scalar_tag(long t){ return t==T_INT || t==T_BOOL; }
/* the length in words of the encoding at w */
static size_t enc_len(const long *w){ return scalar_tag(w[0]) ? 2 : (size_t)w[1]; }
/* the length in words of v's encoding */
static size_t val_len(const Value *v){ return scalar_tag(v->tag) ? 2 : (size_t)v->w[1]; }
/* the Value that views the encoding at w */
static Value enc_view(const long *w){
	Value v={0}; v.tag=(int)w[0];
	if(scalar_tag(w[0])) v.i=w[1]; else v.w=w;
	return v;
}
/* write v's encoding at dst (val_len(v) words) */
static void enc_put(long *dst, const Value *v){
	if(scalar_tag(v->tag)){ dst[0]=v->tag; dst[1]=v->i; }
	else memcpy(dst,v->w,val_len(v)*sizeof *dst);
}
/* the element count of a compound value, and a set's j-th element */
static int val_n(const Value *v){ return (int)v->w[2]; }
static long set_e(const Value *v, int j){ return v->w[3+j]; }

/* the evaluation arena: compound values built while evaluating an
 * expression.  It only grows during one evaluation; a caller that has
 * copied out what it needs (into a state, or a scalar) releases back to
 * the mark it took, so a value built below that mark stays valid. */
#define AR_WORDS (1<<20)
static long AR[AR_WORDS];
static size_t ar_n;
static size_t ar_mark(void){ return ar_n; }
static void ar_release(size_t m){ ar_n=m; }

/* canonical order so equal sets compare equal */
static int lcmp(const void *a, const void *b){ long x=*(const long*)a,y=*(const long*)b; return x<y?-1:x>y?1:0; }
static int set_canon(long *e, int n){
	/* sort, then drop duplicates */
	qsort(e, (size_t)n, sizeof e[0], lcmp);
	int w=0; for(int r=0;r<n;r++){ if(w==0||e[r]!=e[w-1]) e[w++]=e[r]; }
	return w;
}

/* ---- the model ------------------------------------------------------ */
typedef struct { char name[SP_NAME]; long val; } Const;
/* A variable's declared type, as a tree of Ty nodes:
 *   TY_DOM  an integer in the set dom (`0 .. N`, `{0, 2}`)
 *   TY_FUN  [dom -> sub]: a function on exactly the integer set dom
 *   TY_REC  [f : T, ...]: a record with exactly the fields fid[]
 *   TY_SEQ  Seq(sub, max): a sequence of at most max elements */
enum { TY_DOM, TY_FUN, TY_REC, TY_SEQ };
typedef struct {
	int kind;
	char dom[SP_EXPR];			/* TY_DOM: the set; TY_FUN: the key set */
	int sub;				/* TY_FUN: the range; TY_SEQ: the element */
	long max;				/* TY_SEQ: the length bound */
	int nf; int fid[SP_MAXFLD]; int fty[SP_MAXFLD];	/* TY_REC, by field id */
} Ty;
typedef struct { char name[SP_NAME]; int ty; } Var;
/* def NAME(p, ...) = body: a named operator.  Its body may use only the
 * defs declared before it, so no def can reach itself. */
typedef struct { char name[SP_NAME]; int np; char pname[SP_MAXPARAM][SP_NAME]; char body[SP_EXPR]; } Def;
typedef struct {
	char name[SP_NAME];
	int nparam; char pname[SP_MAXPARAM][SP_NAME]; char pdom[SP_MAXPARAM][SP_EXPR];
	char guard[SP_EXPR];
	int nasgn; char lhs[SP_MAXVAR][SP_NAME]; char rhs[SP_MAXVAR][SP_EXPR];
	char tag[SP_NAME];			/* a [TAG] gate: action active only if -D TAG */
} Action;
typedef struct { char name[SP_NAME]; char expr[SP_EXPR]; } SpInv;
typedef struct { char name[SP_NAME]; int kind; char a[SP_EXPR]; char b[SP_EXPR]; } SpProp;	/* kind: 0 eventually(a), 1 a~>b, 2 always [a]_vars */

static struct {
	char modname[SP_NAME];
	Const cst[SP_MAXCONST]; int ncst;
	Var var[SP_MAXVAR]; int nvar;
	char init[SP_EXPR];
	Action act[SP_MAXACT]; int nact;
	SpInv inv[SP_MAXINV]; int ninv;
	char define[SP_MAXCONST][SP_NAME]; int ndef;	/* -D tags given */
	char fname[SP_MAXCONST][SP_NAME]; int foff[SP_MAXCONST]; int nfield;	/* binding [fields] */
	char symmetry[SP_EXPR]; int has_sym;	/* a set of interchangeable values */
	SpProp prop[SP_MAXINV]; int nprop;	/* liveness properties */
	int fair_weak;						/* `fairness weak|strong`: every action, 1 WF, 2 SF */
	char fairname[SP_MAXACT][SP_NAME]; int fairk[SP_MAXACT]; int nfair;	/* `fair weak|strong A` lines */
	int afair[SP_MAXACT];				/* fairness per action, from the fair lines: 0, 1 WF, 2 SF */
	int deadlock_off;					/* `deadlock = off`: no deadlock check */
	SpInv con[SP_MAXINV]; int ncon;		/* state constraints (TLC's CONSTRAINT) */
	SpInv acon[SP_MAXINV]; int nacon;	/* action constraints (ACTION_CONSTRAINT) */
	Ty ty[SP_MAXTY]; int nty;			/* the variables' types */
	Def def[SP_MAXDEF]; int ndefs;		/* def operators, in order */
	char fld[SP_MAXFLD][SP_NAME]; int nfld;	/* record field names, by id */
} S;

/* ---- evaluation context: constants + variable values + bound names - */
#define SP_MAXBIND 8			/* names bound at once: parameters, quantifiers */
typedef struct Env {
	const Value *var;			/* current value of each variable, by index */
	const Value *nxt;			/* an action constraint: the successor's values, read as x' */
	char pname[SP_MAXBIND][SP_NAME]; long pval[SP_MAXBIND]; int np;	/* bound names */
	/* a def's parameter is bound by name, as TLA+ substitutes it: the
	 * argument's text, evaluated in the caller's environment at each use */
	const char *thunk[SP_MAXBIND]; const struct Env *tenv[SP_MAXBIND];
	const CmcObs *obs;			/* refinement: observable accessor, or NULL */
	const int *assigned;			/* init: which vars are set so far, or NULL (all) */
	const Value *at;			/* inside an except value: what @ names */
	int ndefvis;				/* the defs this expression may call: def[0 .. ndefvis) */
} Env;

/* a fresh environment over the variable values var (NULL: none), in
 * which every def may be called */
static Env env_new(const Value *var){
	static const Value none[SP_MAXVAR];	/* every variable 0, where none is set */
	Env e; memset(&e,0,sizeof e); e.var=var?var:none; e.ndefvis=S.ndefs;
	return e;
}

static long cst_lookup(const char *name, int *found){
	for(int i=0;i<S.ncst;i++) if(!strcmp(S.cst[i].name,name)){ *found=1; return S.cst[i].val; }
	*found=0; return 0;
}
static int var_index(const char *name){ for(int i=0;i<S.nvar;i++) if(!strcmp(S.var[i].name,name)) return i; return -1; }
static int def_index(const char *name){ for(int i=0;i<S.ndefs;i++) if(!strcmp(S.def[i].name,name)) return i; return -1; }

/* the id of record field name, registering it if new; -1 when the
 * table is full.  Ids order a record's fields canonically. */
static int fld_find(const char *name){ for(int i=0;i<S.nfld;i++) if(!strcmp(S.fld[i],name)) return i; return -1; }
static int fld_id(const char *name){
	for(int i=0;i<S.nfld;i++) if(!strcmp(S.fld[i],name)) return i;
	if(S.nfld>=SP_MAXFLD) return -1;
	snprintf(S.fld[S.nfld],SP_NAME,"%s",name);
	return S.nfld++;
}

/* =====================================================================
 * expression evaluator over Value.  Grammar (low -> high precedence):
 *   or  ->  and  ->  not  ->  cmp  ->  add (+ - \o)  ->  mul  ->  postfix
 *   ->  atom
 * plus quantifiers  (all|some|count) V in SET : EXPR, the function,
 * record, sequence and except forms, the sequence operators, and calls
 * of def operators.  A postfix is f[k] or r.field, after any atom.
 * Returns a Value; sets *ok=0 on an evaluation/parse error.
 * ===================================================================== */
/* dead > 0 while the parser reads an operand that and/or short-circuits
 * past (TLC evaluates left to right and never evaluates it).  Such an
 * operand is still parsed, and a syntax error in it is still an error,
 * but its value is ignored and evaluation errors in it (a wrong type,
 * division by zero, an application outside the domain, an overflow)
 * are not raised: `x > 0 and y / x == 2` is FALSE at x = 0. */
/* lastmod and lastand say whether the term sp_mul() or sp_and() just
 * read was a `mod` or held an `and`, so that the caller can refuse the
 * mixes TLA+ refuses (see sp_mul()). */
typedef struct { const char *p; Env *env; int ok; char err[128]; int dead; int lastmod, lastand; } Pst;

static void sp_skip(Pst *s){ while(*s->p && isspace((unsigned char)*s->p)) s->p++; }
static int sp_word(Pst *s, const char *w){
	sp_skip(s); size_t n=strlen(w);
	if(!strncmp(s->p,w,n) && !isalnum((unsigned char)s->p[n]) && s->p[n]!='_'){ s->p+=n; return 1; }
	return 0;
}
static Value sp_or(Pst *s);
static void fail(Pst *s, const char *m){ if(s->ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",m); } }

/* an evaluation error: fails s and returns 1, or, in an operand that
 * is not evaluated, returns 0 so the caller parses on */
static int efail(Pst *s, const char *m){ if(s->dead) return 0; fail(s,m); return 1; }

static const char *tag_name(int t){
	static const char *nm[]={"an integer","a boolean","a set","a function","a record","a sequence"};
	return nm[t];
}

/* An operand of the wrong type is a model error, as TLC reports it.
 * Read as its integer field instead, `not votes` held in every state and
 * `x < {1}` compared x with 0.  need() returns 1 when v has type tag, and
 * otherwise fails s, naming where the value was used.  In an operand
 * that is not evaluated it never fails, and makes v a placeholder. */
static const long EMPTY[6][3]={{0},{0},{T_SET,3,0},{T_FUN,3,0},{T_REC,3,0},{T_SEQ,3,0}};
static int need(Pst *s, Value *v, int tag, const char *where){
	if(v->tag==tag) return 1;
	if(s->dead){
		/* an operand that is not evaluated: a placeholder of the type
		 * the caller reads, so no caller touches a missing encoding */
		Value d={0}; d.tag=tag; if(!scalar_tag(tag)) d.w=EMPTY[tag];
		*v=d; return 1;
	}
	if(s->ok){ s->ok=0; snprintf(s->err,sizeof s->err,"expected %s in %s, not %s",tag_name(tag),where,tag_name(v->tag)); }
	return 0;
}

/* Equality of two encodings: 1 equal, 0 not, or -1 when they hold
 * values of different types at one position (TLC refuses TRUE = 1, and
 * a record compared with a function).  Two functions on different keys,
 * records of different fields and sequences of different lengths are
 * unequal, as in TLC, without comparing their entries.  *ta and *tb get
 * the two tags that differ. */
static int enc_eq(const long *a, const long *b, int *ta, int *tb){
	if(a[0]!=b[0]){ *ta=(int)a[0]; *tb=(int)b[0]; return -1; }
	if(scalar_tag(a[0])) return a[1]==b[1];
	if(a[0]==T_SET){ if(a[1]!=b[1]) return 0; return !memcmp(a,b,(size_t)a[1]*sizeof *a); }
	long n=a[2]; if(n!=b[2]) return 0;
	const long *pa=a+3, *pb=b+3;
	int keyed=a[0]!=T_SEQ, eq=1;
	if(keyed){ /* the key sets first: unequal keys decide, as in TLC */
		const long *qa=pa, *qb=pb;
		for(long j=0;j<n;j++){ if(qa[0]!=qb[0]) return 0; qa+=1+enc_len(qa+1); qb+=1+enc_len(qb+1); }
	}
	for(long j=0;j<n;j++){
		if(keyed){ pa++; pb++; }
		int r=enc_eq(pa,pb,ta,tb); if(r<0) return -1; if(!r) eq=0;
		pa+=enc_len(pa); pb+=enc_len(pb);
	}
	return eq;
}
static int val_eq_raw(const Value *a, const Value *b, int *ta, int *tb){
	long ea[2], eb[2];
	const long *pa=a->w, *pb=b->w;
	if(scalar_tag(a->tag)){ ea[0]=a->tag; ea[1]=a->i; pa=ea; }
	if(scalar_tag(b->tag)){ eb[0]=b->tag; eb[1]=b->i; pb=eb; }
	return enc_eq(pa,pb,ta,tb);
}
/* a == b for the evaluator: the verdict, or a model error naming the two
 * types when they differ at some position */
static int sp_val_eq(Pst *s, const Value *a, const Value *b, const char *op){
	if(s->dead) return 0;
	int ta, tb, r=val_eq_raw(a,b,&ta,&tb);
	if(r>=0) return r;
	if(s->ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s compares %s with %s",op,tag_name(ta),tag_name(tb)); }
	return 0;
}

/* ---- building values in the arena ----------------------------------- */
/* reserve n words of the arena, or fail s */
static long *ar_alloc(Pst *s, size_t n){
	if(ar_n+n>AR_WORDS){ fail(s,"values too large for the evaluation arena"); return NULL; }
	long *p=AR+ar_n; ar_n+=n; return p;
}
/* the set of the n integers e[] (sorted and deduplicated here) */
static Value mk_set(Pst *s, long *e, int n){
	Value z={0}; z.tag=T_SET;
	n=set_canon(e,n);
	long *w=ar_alloc(s,(size_t)n+3); if(!w) return z;
	w[0]=T_SET; w[1]=n+3; w[2]=n; memcpy(w+3,e,(size_t)n*sizeof *e);
	z.w=w; return z;
}
/* a function (T_FUN), record (T_REC) or sequence (T_SEQ) of the n
 * children kid[], with keys key[] (integers or field ids, increasing;
 * NULL for a sequence) */
static Value mk_comp(Pst *s, int tag, int n, const long *key, const Value *kid){
	Value z={0}; z.tag=tag;
	size_t len=3; for(int j=0;j<n;j++) len+=(key?1:0)+val_len(&kid[j]);
	long *w=ar_alloc(s,len); if(!w) return z;
	w[0]=tag; w[1]=(long)len; w[2]=n;
	long *p=w+3;
	for(int j=0;j<n;j++){ if(key) *p++=key[j]; enc_put(p,&kid[j]); p+=val_len(&kid[j]); }
	z.w=w; return z;
}
/* the children of compound v into key[] (if not NULL) and kid[] (each a
 * view into v's encoding); returns the count */
static int comp_kids(const Value *v, long *key, Value *kid){
	int n=val_n(v); const long *p=v->w+3;
	for(int j=0;j<n;j++){
		if(v->tag!=T_SEQ){ if(key) key[j]=*p; p++; }
		kid[j]=enc_view(p); p+=enc_len(p);
	}
	return n;
}
/* the empty value of tag (a set, function or sequence) */
static Value mk_empty(Pst *s, int tag){ return tag==T_SET ? mk_set(s,NULL,0) : mk_comp(s,tag,0,NULL,NULL); }

/* read an identifier */
static int sp_ident(Pst *s, char *out, int cap){
	sp_skip(s); int n=0;
	while((isalnum((unsigned char)*s->p)||*s->p=='_') && n<cap-1) out[n++]=*s->p++;
	out[n]=0; return n;
}
/* consume the character c (after blanks), or fail with msg */
static int sp_expect(Pst *s, char c, const char *msg){
	sp_skip(s); if(*s->p!=c){ fail(s,msg); return 0; }
	s->p++; return 1;
}

/* a SET value literal or range: {a,b,c} or LO .. HI */
static Value sp_setspec(Pst *s);
static int env_bind(Pst *s, Env *ne, const char *name, long val);
static Value sp_add(Pst *s);
static Value sp_range(Pst *s);
static Value sp_post(Pst *s);

/* evaluate the operand of a sub-parse that starts at body with the
 * environment ne, copying its error back to s; *endp gets where it
 * stopped */
static Value sp_sub(Pst *s, const char *body, Env *ne, int dead, const char **endp){
	Pst sub=*s; sub.p=body; sub.env=ne; sub.dead+=dead;
	Value v=sp_or(&sub);
	if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); }
	*endp=sub.p; return v;
}

/* [i in SET -> EXPR], after "[i in": the function on SET */
static Value sp_funlit(Pst *s, const char *id){
	Value z={0};
	Value dom=sp_setspec(s); if(!s->ok) return z;
	sp_skip(s); if(!(s->p[0]=='-'&&s->p[1]=='>')){ fail(s,"expected -> in function literal"); return z; } s->p+=2;
	const char *body=s->p, *endp=body;
	int n=s->dead ? 0 : val_n(&dom);
	long key[SP_MAPMAX]; Value kid[SP_MAPMAX];
	if(n>SP_MAPMAX){ fail(s,"function too large"); return z; }
	if(n==0){
		/* no element to bind: parse the body once, unevaluated */
		Env ne=*s->env; if(!env_bind(s,&ne,id,0)) return z;
		(void)sp_sub(s,body,&ne,1,&endp); if(!s->ok) return z;
	}
	for(int e=0;e<n;e++){
		Env ne=*s->env; if(!env_bind(s,&ne,id,set_e(&dom,e))) return z;
		key[e]=set_e(&dom,e); kid[e]=sp_sub(s,body,&ne,0,&endp); if(!s->ok) return z;
	}
	s->p=endp;
	if(!sp_expect(s,']',"expected ]")) return z;
	if(s->dead) return z;
	return mk_comp(s,T_FUN,n,key,kid);
}

/* [a |-> E, b |-> E], after "[a": a record, its fields in id order */
static Value sp_reclit(Pst *s, const char *first){
	Value z={0}; long key[SP_MAXFLD]; Value kid[SP_MAXFLD]; int n=0;
	char id[SP_NAME]; snprintf(id,sizeof id,"%s",first);
	for(;;){
		sp_skip(s); if(strncmp(s->p,"|->",3)){ fail(s,"expected |-> in record literal"); return z; } s->p+=3;
		int f=fld_id(id); if(f<0){ fail(s,"too many record field names"); return z; }
		for(int j=0;j<n;j++) if(key[j]==f){ fail(s,"a record field is given twice"); return z; }
		if(n>=SP_MAXFLD){ fail(s,"record too large"); return z; }
		Value v=sp_or(s); if(!s->ok) return z;
		/* insert in field-id order */
		int j=n; while(j>0 && key[j-1]>f){ key[j]=key[j-1]; kid[j]=kid[j-1]; j--; }
		key[j]=f; kid[j]=v; n++;
		sp_skip(s); if(*s->p!=',') break;
		s->p++; if(!sp_ident(s,id,sizeof id)){ fail(s,"expected a field name in record literal"); return z; }
	}
	if(!sp_expect(s,']',"expected ] to close record")) return z;
	if(s->dead) return z;
	return mk_comp(s,T_REC,n,key,kid);
}

/* <<E, ...>>, after "<<": a sequence (a tuple is one) */
static Value sp_seqlit(Pst *s){
	Value z={0}; Value kid[SP_MAPMAX]; int n=0;
	sp_skip(s);
	if(strncmp(s->p,">>",2)){
		for(;;){
			Value v=sp_or(s); if(!s->ok) return z;
			if(n>=SP_MAPMAX){ if(efail(s,"sequence too large")) return z; } else kid[n++]=v;
			sp_skip(s); if(*s->p!=',') break;
			s->p++;
		}
	}
	sp_skip(s); if(strncmp(s->p,">>",2)){ fail(s,"expected >> to close a sequence"); return z; } s->p+=2;
	if(s->dead) return z;
	return mk_comp(s,T_SEQ,n,NULL,kid);
}

/* ---- except: [B except P = V, ...] ----------------------------------
 * Each clause replaces the part of B that the selector path P names
 * (TLA+'s [B EXCEPT !P = V]): P is an optional `!`, then one or more
 * selectors, [k] for a function key or sequence position, .f for a
 * record field.  In V, `@` names the part being replaced.  A path
 * outside B (a key not in a function's domain, a position past a
 * sequence's end, a field the record lacks) is a model error: TLC
 * leaves the value unchanged there and warns, which would read a typo
 * as a step that changes nothing. */
#define SP_PATH 4
typedef struct { int field; long k; } Sel;	/* field: k is a field id, else a key or position */

/* the index of selector sel among compound v's n children (keys key[]),
 * or -1 */
static int sel_find(const Value *v, const Sel *sel, const long *key, int n){
	if(v->tag==T_SEQ) return !sel->field && sel->k>=1 && sel->k<=n ? (int)sel->k-1 : -1;
	if((v->tag==T_REC)!=(sel->field!=0)) return -1;
	for(int j=0;j<n;j++) if(key[j]==sel->k) return j;
	return -1;
}
/* the part of v at path[0 .. np), into *out; 0, or -1 with s failed */
static int path_get(Pst *s, Value v, const Sel *path, int np, Value *out){
	for(int d=0;d<np;d++){
		if(scalar_tag(v.tag) || v.tag==T_SET){ efail(s,"except on a value that is not a function, record or sequence"); return -1; }
		long key[SP_MAXFLD+SP_MAPMAX]; Value kid[SP_MAXFLD+SP_MAPMAX];
		int n=comp_kids(&v,key,kid), j=sel_find(&v,&path[d],key,n);
		if(j<0){ efail(s,"except outside the domain of its value"); return -1; }
		v=kid[j];
	}
	*out=v; return 0;
}
/* v with the part at path[0 .. np) replaced by nv, a new value in the
 * arena.  NB: path_get() has checked that the path lies inside v. */
static Value path_set(Pst *s, const Value *v, const Sel *path, int np, const Value *nv){
	if(np==0) return *nv;
	long key[SP_MAXFLD+SP_MAPMAX]; Value kid[SP_MAXFLD+SP_MAPMAX];
	int n=comp_kids(v,key,kid), j=sel_find(v,&path[0],key,n);
	kid[j]=path_set(s,&kid[j],path+1,np-1,nv);
	return mk_comp(s,v->tag,n,v->tag==T_SEQ?NULL:key,kid);
}
/* one selector path, up to its `=` */
static int sp_path(Pst *s, Sel *path){
	int np=0;
	sp_skip(s); if(*s->p=='!') s->p++;
	for(;;){
		sp_skip(s);
		if(*s->p!='[' && *s->p!='.') break;
		if(np>=SP_PATH){ fail(s,"an except path is too deep"); return -1; }
		if(*s->p=='['){
			s->p++; Value k=sp_or(s); if(!s->ok || !need(s,&k,T_INT,"a function key")) return -1;
			if(!sp_expect(s,']',"expected ]")) return -1;
			path[np].field=0; path[np].k=k.i;
		} else {
			s->p++; char f[SP_NAME]; if(!sp_ident(s,f,sizeof f)){ fail(s,"expected a field name after ."); return -1; }
			int id=fld_find(f); path[np].field=1; path[np].k=id;
			if(id<0 && efail(s,"except outside the domain of its value")) return -1;
		}
		np++;
	}
	if(np==0){ fail(s,"expected [ or . in except"); return -1; }
	return np;
}
/* the except clauses after "[B except", applied to base */
static Value sp_except(Pst *s, Value f){
	Value z={0};
	for(;;){
		Sel path[SP_PATH]; int np=sp_path(s,path); if(np<0) return z;
		if(!sp_expect(s,'=',"expected = in except")) return z;
		Value old={0}; int live=!s->dead && path_get(s,f,path,np,&old)==0;
		if(!s->ok) return z;
		static const Value at_dead={T_INT,0,NULL};
		Env ne=*s->env; ne.at=live?&old:&at_dead;
		const char *endp; Value vv=sp_sub(s,s->p,&ne,live?0:1,&endp); if(!s->ok) return z;
		s->p=endp;
		if(live){ f=path_set(s,&f,path,np,&vv); if(!s->ok) return z; }
		sp_skip(s); if(*s->p==','){ s->p++; continue; } break;
	}
	if(!sp_expect(s,']',"expected ] to close except")) return z;
	return f;
}

/* ---- sequence operators, as in TLA+'s Sequences module --------------
 * Positions count from 1.  Head and Tail of the empty sequence, an
 * index past the end, and SubSeq bounds outside 1 .. Len are model
 * errors, as TLC reports them.  A sequence's length bound belongs to
 * the variable that holds it: a value longer than its variable's Seq(T,
 * N) is refused when it is stored (var_in_type()), never cut. */
static Value seq_slice(Pst *s, const Value *q, int from, int to){
	Value kid[SP_MAPMAX]; int n=comp_kids(q,NULL,kid);
	(void)n;
	return mk_comp(s,T_SEQ,to-from,NULL,kid+from);
}
static int sp_args(Pst *s, Value *a, int n, const char *fn){
	if(!sp_expect(s,'(',"expected ( after a sequence operator")) return 0;
	for(int i=0;i<n;i++){
		a[i]=sp_or(s); if(!s->ok) return 0;
		if(i+1<n && !sp_expect(s,',',"expected , between arguments")) return 0;
	}
	if(!sp_expect(s,')',"expected ) after the arguments")) return 0;
	(void)fn; return 1;
}
/* NAME(args) for a sequence operator NAME; *hit says whether id is one */
static Value sp_seqop(Pst *s, const char *id, int *hit){
	static const char *ops[]={"Len","Head","Tail","Append","SubSeq"};
	static const int nargs[]={1,1,1,2,3};
	Value z={0}; int op=-1;
	for(int i=0;i<5;i++) if(!strcmp(id,ops[i])) op=i;
	*hit=op>=0; if(op<0) return z;
	Value a[3]; if(!sp_args(s,a,nargs[op],id)) return z;
	if(!need(s,&a[0],T_SEQ,id)) return z;
	if(s->dead) return op==0 ? vint(0) : op==1 ? z : a[0];
	int n=val_n(&a[0]);
	switch(op){
	case 0: return vint(n);
	case 1: { if(n==0){ fail(s,"Head of the empty sequence"); return z; }
		Value kid[SP_MAPMAX]; comp_kids(&a[0],NULL,kid); return kid[0]; }
	case 2: if(n==0){ fail(s,"Tail of the empty sequence"); return z; }
		return seq_slice(s,&a[0],1,n);
	case 3: { if(n>=SP_MAPMAX){ fail(s,"sequence too large"); return z; }
		Value kid[SP_MAPMAX]; comp_kids(&a[0],NULL,kid); kid[n]=a[1];
		return mk_comp(s,T_SEQ,n+1,NULL,kid); }
	default:
		if(!need(s,&a[1],T_INT,"SubSeq") || !need(s,&a[2],T_INT,"SubSeq")) return z;
		if(a[1].i>a[2].i) return mk_empty(s,T_SEQ);
		if(a[1].i<1 || a[2].i>n){ fail(s,"SubSeq outside the sequence"); return z; }
		return seq_slice(s,&a[0],(int)a[1].i-1,(int)a[2].i);
	}
}
/* s \o t: the concatenation of two sequences */
static Value seq_cat(Pst *s, const Value *a, const Value *b){
	Value z={0}; Value kid[2*SP_MAPMAX];
	int na=comp_kids(a,NULL,kid), nb=comp_kids(b,NULL,kid+na);
	if(na+nb>SP_MAPMAX){ fail(s,"sequence too large"); return z; }
	return mk_comp(s,T_SEQ,na+nb,NULL,kid);
}

/* ---- def calls -------------------------------------------------------
 * NAME or NAME(args) for def d.  As in TLA+, the body is the operator's
 * definition with each parameter replaced by its argument: here each
 * parameter is bound to the argument's text, evaluated in the caller's
 * environment where the body uses it, so `def safe(a) = d == 0 or a`
 * evaluates a only when d != 0.  The body sees the constants, the
 * variables, its parameters and the defs before it (env.ndefvis), and
 * nothing bound at the call site. */
static Value sp_call(Pst *s, int d){
	Value z={0}; const Def *D=&S.def[d];
	Env ne=env_new(s->env->var); ne.obs=s->env->obs; ne.assigned=s->env->assigned; ne.ndefvis=d;
	if(D->np){
		if(!sp_expect(s,'(',"expected ( after an operator with parameters")) return z;
		for(int i=0;i<D->np;i++){
			sp_skip(s);
			snprintf(ne.pname[i],SP_NAME,"%s",D->pname[i]); ne.thunk[i]=s->p; ne.tenv[i]=s->env;
			/* parse the argument for its extent only */
			Pst sub=*s; sub.dead++; (void)sp_or(&sub);
			if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); return z; }
			s->p=sub.p;
			if(i+1<D->np && !sp_expect(s,',',"expected , between arguments")) return z;
		}
		if(!sp_expect(s,')',"expected ) after the arguments")) return z;
		ne.np=D->np;
	}
	/* defs_check() has parsed the body whole at load */
	const char *endp; return sp_sub(s,D->body,&ne,0,&endp);
}

/* the name id, bound by a quantifier, a literal, an action or a def:
 * its value, and 1; or 0 when id is not bound */
static int sp_bound(Pst *s, const char *id, Value *out){
	const Env *e=s->env;
	for(int i=0;i<e->np;i++){
		if(strcmp(id,e->pname[i])) continue;
		if(!e->thunk[i]){ *out=vint(e->pval[i]); return 1; }
		const char *endp; *out=sp_sub(s,e->thunk[i],(Env*)e->tenv[i],0,&endp);
		return 1;
	}
	return 0;
}

/* a refinement observable (nodes, replies, node[i]..., r.src, r.val);
 * *hit says whether id is one */
static Value sp_obs(Pst *s, const char *id, int *hit){
	const CmcObs *o=s->env->obs; Value z={0}; long e[SP_SETMAX]; int n=0;
	*hit=1;
	if(!strcmp(id,"nodes")){ for(int i=0;i<o->nnodes && n<SP_SETMAX;i++) e[n++]=i; return mk_set(s,e,n); }
	if(!strcmp(id,"replies")){ for(int r=0;r<o->nrepl && n<SP_SETMAX;r++) e[n++]=r; return mk_set(s,e,n); }
	if(!strcmp(id,"node")){
		if(!sp_expect(s,'[',"expected [ after node")) return z;
		Value iv=sp_or(s); if(!s->ok || !need(s,&iv,T_INT,"node[]")) return z;
		if(!sp_expect(s,']',"expected ]")) return z;
		int i=(int)iv.i; long k=-1;
		sp_skip(s);
		if(*s->p=='['){ s->p++; Value kv=sp_or(s); if(!s->ok || !need(s,&kv,T_INT,"node[][]")) return z; if(!sp_expect(s,']',"expected ]")) return z; k=kv.i; }
		else if(*s->p=='.'){ s->p++; char fn[SP_NAME]; sp_ident(s,fn,sizeof fn); int fi=-1; for(int j=0;j<S.nfield;j++) if(!strcmp(S.fname[j],fn)) fi=j; if(fi<0){ fail(s,"unknown field"); return z; } k=S.foff[fi]; }
		else { fail(s,"node[i] needs [k] or .field"); return z; }
		if(s->dead) return vint(0);	/* i may be a placeholder */
		return vint(o->node_byte(o->ctx,i,(int)k));
	}
	if(!strcmp(id,"r") && *s->p=='.'){
		/* r.src / r.val when ranging over replies (param r bound to index) */
		s->p++; char fn[SP_NAME]; sp_ident(s,fn,sizeof fn);
		long ri=0; for(int i=0;i<s->env->np;i++) if(!strcmp("r",s->env->pname[i])) ri=s->env->pval[i];
		if(s->dead) return vint(0);
		if(!strcmp(fn,"src")) return vint(o->repl_src(o->ctx,(int)ri));
		if(!strcmp(fn,"val")) return vint(o->repl_val(o->ctx,(int)ri));
		fail(s,"reply has .src and .val"); return z;
	}
	*hit=0; return z;
}

/* "[" forms, after the "[": a function literal [i in S -> E], a record
 * literal [a |-> E, ...], or an update [B except ...] */
static Value sp_bracket(Pst *s){
	Value z={0};
	sp_skip(s);
	const char *save=s->p;
	char id[SP_NAME];
	if(sp_ident(s,id,sizeof id)){
		if(sp_word(s,"in")) return sp_funlit(s,id);
		sp_skip(s); if(!strncmp(s->p,"|->",3)) return sp_reclit(s,id);
	}
	s->p=save;
	Value base=sp_post(s); if(!s->ok) return z;
	if(!sp_word(s,"except")){ fail(s,"expected 'in', '|->' or 'except' in [..]"); return z; }
	return sp_except(s,base);
}

/* {a, b, ...}, after the "{": a set of integers */
static Value sp_setlit(Pst *s){
	Value z={0}; long e[SP_SETMAX]; int n=0;
	sp_skip(s);
	if(*s->p!='}'){
		for(;;){ Value ev=sp_or(s); if(!s->ok || !need(s,&ev,T_INT,"a set")) return z; if(n>=SP_SETMAX){ if(efail(s,"set too large")) return z; } else e[n++]=ev.i; sp_skip(s); if(*s->p==','){ s->p++; continue; } break; }
	}
	if(!sp_expect(s,'}',"expected }")) return z;
	return mk_set(s,e,n);
}

/* atom: integer, TRUE/FALSE, @, a bound name, const, variable, def
 * call, sequence operator, ( EXPR ), and the [..], {..} and <<..>>
 * literals */
static Value sp_atom(Pst *s){
	sp_skip(s);
	Value z={0};
	if(*s->p=='('){ s->p++; Value v=sp_or(s); if(!s->ok || !sp_expect(s,')',"expected )")) return z; return v; }
	if(*s->p=='['){ s->p++; return sp_bracket(s); }
	if(*s->p=='{'){ s->p++; return sp_setlit(s); }
	if(s->p[0]=='<' && s->p[1]=='<'){ s->p+=2; return sp_seqlit(s); }
	if(*s->p=='@'){
		s->p++;
		if(!s->env->at){ fail(s,"@ outside an except value"); return z; }
		return *s->env->at;
	}
	if(isdigit((unsigned char)*s->p)){
		errno=0; long x=strtol(s->p,(char**)&s->p,10);
		if(errno==ERANGE){ fail(s,"integer literal overflows a long"); return z; }
		return vint(x); }
	/* identifier-led: keywords, const, var, param, def, operator */
	char id[SP_NAME]; int n=sp_ident(s,id,sizeof id);
	if(n==0){ fail(s,"expected an expression"); return z; }
	if(!strcmp(id,"TRUE")) return vbool(1);
	if(!strcmp(id,"FALSE")) return vbool(0);
	int hit;
	if(s->env->obs){ Value v=sp_obs(s,id,&hit); if(hit) return v; }
	{ Value v; if(sp_bound(s,id,&v)) return v; }
	{ int f; long c=cst_lookup(id,&f); if(f) return vint(c); }
	/* A primed variable has no next-state value here: guards, right-hand
	 * sides and invariants are all evaluated against the current state,
	 * so reading x' as x would silently change the spec's meaning (and
	 * the emitted TLA+ keeps the prime).  Refuse it, except in an action
	 * constraint, which is evaluated over a step and reads x' from the
	 * successor. */
	int vi=var_index(id);
	if(*s->p=='\''){
		if(!s->env->nxt || vi<0){ fail(s,"a primed variable can only be assigned (x' = E), not read"); return z; }
		s->p++;
		return s->env->nxt[vi];
	}
	if(vi>=0){
		if(s->env->assigned && !s->env->assigned[vi]){ fail(s,"init reads a variable before assigning it"); return z; }
		return s->env->var[vi];
	}
	int d=def_index(id);
	if(d>=0){
		if(d>=s->env->ndefvis){ fail(s,"a def may only use the defs declared before it (no recursion)"); return z; }
		return sp_call(s,d);
	}
	{ Value v=sp_seqop(s,id,&hit); if(hit) return v; }
	fail(s,"unknown identifier"); return z;
}

/* postfix: atom, then any number of f[k] (a function at key k, or a
 * sequence at position k) and r.field */
static Value sp_post(Pst *s){
	Value v=sp_atom(s);
	for(;;){
		if(!s->ok) return v;
		/* no blank before a selector: `x .. y` is a range, not x.y */
		if(*s->p=='.' && (isalpha((unsigned char)s->p[1]) || s->p[1]=='_')){
			s->p++; char f[SP_NAME]; sp_ident(s,f,sizeof f);
			if(!need(s,&v,T_REC,"a field access")) return v;
			if(s->dead){ v=vint(0); continue; }
			long key[SP_MAXFLD]; Value kid[SP_MAXFLD]; int n=comp_kids(&v,key,kid), id=fld_find(f), j=-1;
			for(int i=0;i<n;i++) if(key[i]==id) j=i;
			if(j<0){ fail(s,"a record has no such field"); return v; }
			v=kid[j]; continue;
		}
		sp_skip(s);
		if(*s->p!='[') return v;
		s->p++;
		Value kk=sp_or(s); if(!s->ok || !need(s,&kk,T_INT,"a function key")) return v;
		if(!sp_expect(s,']',"expected ] in application")) return v;
		if(s->dead){ v=vint(0); continue; }
		if(v.tag!=T_FUN && v.tag!=T_SEQ){ efail(s,"applied a non-function"); return v; }
		long key[SP_MAPMAX]; Value kid[SP_MAPMAX]; int n=comp_kids(&v,key,kid), j=-1;
		if(v.tag==T_SEQ){ if(kk.i>=1 && kk.i<=n) j=(int)kk.i-1; }
		else for(int i=0;i<n;i++) if(key[i]==kk.i) j=i;
		if(j<0){ fail(s,v.tag==T_SEQ?"a sequence index outside 1 .. Len":"function applied outside its domain"); return v; }
		v=kid[j];
	}
}

/* env with one more bound name, or an error when SP_MAXBIND names are
 * already bound (silently skipping the binding would let the body read
 * an outer value of the same name) */
static int env_bind(Pst *s, Env *ne, const char *name, long val){
	if(ne->np>=SP_MAXBIND){ fail(s,"too many nested bound names"); return 0; }
	/* TLA+ forbids redeclaring a name in scope (SANY rejects it); here an
	 * inner binding would be shadowed by the outer one, not the reverse */
	int f; (void)cst_lookup(name,&f);
	for(int i=0;i<ne->np;i++) if(!strcmp(ne->pname[i],name)) f=1;
	if(f || var_index(name)>=0 || def_index(name)>=0){ fail(s,"a bound name redeclares a name already in scope"); return 0; }
	snprintf(ne->pname[ne->np],SP_NAME,"%s",name); ne->pval[ne->np]=val; ne->thunk[ne->np]=NULL; ne->np++;
	return 1;
}

/* a range LO .. HI (inclusive, bounds arithmetic expressions), or the
 * term LO alone: `..` binds below + and -, as in TLA+ */
static Value sp_range(Pst *s){
	Value lo=sp_add(s); if(!s->ok) return lo; sp_skip(s);
	if(!(s->p[0]=='.'&&s->p[1]=='.')) return lo;
	s->p+=2; Value hi=sp_add(s); if(!s->ok) return hi;
	if(!need(s,&lo,T_INT,"..") || !need(s,&hi,T_INT,"..")) return lo;
	if(s->dead) return mk_empty(s,T_SET);
	long e[SP_SETMAX]; int n=0;
	for(long x=lo.i;x<=hi.i;x++){ if(n>=SP_SETMAX){ fail(s,"range too large"); return lo; } e[n++]=x; }
	return mk_set(s,e,n);
}
/* a set used in domains and quantifiers: {..}, LO .. HI, or any
 * set-valued term (a def, a variable of set type) */
static Value sp_setspec(Pst *s){
	sp_skip(s);
	if(*s->p=='{') return sp_atom(s);
	Value v=sp_range(s); if(!s->ok) return v;
	if(v.tag!=T_SET){ if(efail(s,"expected a set (LO .. HI or {..}), not a scalar")) return v; need(s,&v,T_SET,"a set"); }
	return v;
}

/* arithmetic is checked: an overflow is a model error, never a wrapped
 * value (signed overflow is also undefined in C).  `/` and `mod` are
 * refused on negative operands, where C's truncating division differs
 * from TLA+'s \div and %, so the emitted module would mean something
 * else. */
/*
 * Precedence follows TLA+ wherever cmc accepts a form, and the forms
 * TLA+ refuses are refused: SANY gives \div no associativity and %
 * a level of its own, between + and *, with none.  So a `/` or a `mod`
 * stands alone in its chain of * / mod, and a `mod` beside + or - needs
 * parentheses (sp_add()).  Read with C's rules, `x mod 3 * 2` meant
 * (x mod 3) * 2 to cmc and x % 6 to TLC.
 */
static Value sp_mul(Pst *s){
	Value a=sp_post(s);
	int nop=0, odd=0, lm=0;	/* operators read; one was / or mod; the last was mod */
	for(;;){ sp_skip(s);
		int ismul=*s->p=='*', isdiv=*s->p=='/', ismod=!ismul && !isdiv && sp_word(s,"mod");
		if(!ismul && !isdiv && !ismod) break;
		if(nop && (odd || !ismul)){ fail(s,"/ or mod next to * / mod needs parentheses, as in TLA+"); return a; }
		nop++; odd|=!ismul; lm=ismod;
		if(ismul){ s->p++; Value b=sp_post(s); long r; if(!need(s,&a,T_INT,"*") || !need(s,&b,T_INT,"*")) return a; if(__builtin_mul_overflow(a.i,b.i,&r) && efail(s,"integer overflow in *")) return a; a=vint(r); }
		else if(isdiv){ s->p++; Value b=sp_post(s); if(!need(s,&a,T_INT,"/") || !need(s,&b,T_INT,"/")) return a; if(b.i==0 && efail(s,"divide by zero")) return a; if((a.i<0||b.i<0) && efail(s,"/ of a negative number is not supported")) return a; a=vint(b.i>0&&a.i>=0?a.i/b.i:0); }
		else { Value b=sp_post(s); if(!need(s,&a,T_INT,"mod") || !need(s,&b,T_INT,"mod")) return a; if(b.i==0 && efail(s,"mod by zero")) return a; if((a.i<0||b.i<0) && efail(s,"mod of a negative number is not supported")) return a; a=vint(b.i>0&&a.i>=0?a.i%b.i:0); }
	}
	s->lastmod=lm;
	return a;
}
/* `mod` binds below + and - in TLA+, with no relative precedence, so a
 * mod term next to + or - needs parentheses (SANY refuses it). */
static Value sp_add(Pst *s){
	Value a=sp_mul(s); int nop=0, mods=s->lastmod;
	for(;;){ sp_skip(s);
		int isplus=*s->p=='+', isminus=*s->p=='-'&&s->p[1]!='>';
		if(isplus||isminus) nop++;
		if(isplus){ s->p++; Value b=sp_mul(s); mods|=s->lastmod; long r; if(!need(s,&a,T_INT,"+") || !need(s,&b,T_INT,"+")) return a; if(__builtin_add_overflow(a.i,b.i,&r) && efail(s,"integer overflow in +")) return a; a=vint(r); }
		else if(isminus){ s->p++; Value b=sp_mul(s); mods|=s->lastmod; long r; if(!need(s,&a,T_INT,"-") || !need(s,&b,T_INT,"-")) return a; if(__builtin_sub_overflow(a.i,b.i,&r) && efail(s,"integer overflow in -")) return a; a=vint(r); }
		else if(s->p[0]=='\\' && s->p[1]=='o' && !isalnum((unsigned char)s->p[2])){ s->p+=2; Value b=sp_mul(s); if(!need(s,&a,T_SEQ,"\\o") || !need(s,&b,T_SEQ,"\\o")) return a; if(!s->dead) a=seq_cat(s,&a,&b); }
		else break;
		if(nop && mods){ fail(s,"mod next to + or - needs parentheses, as in TLA+"); return a; }
	}
	s->lastmod=0;
	return a;
}

/* quantifiers and membership: (all|some|count) V in SET : EXPR, A in SET */
static Value sp_quant(Pst *s){
	sp_skip(s);
	int isall=sp_word(s,"all"), issome=0, iscount=0;
	if(!isall){ issome=sp_word(s,"some"); if(!issome) iscount=sp_word(s,"count"); }
	if(isall||issome||iscount){
		const char *q=isall?"all":issome?"some":"count";
		char var[SP_NAME]; sp_ident(s,var,sizeof var);
		if(!sp_word(s,"in")){ fail(s,"expected 'in' in quantifier"); return vbool(0); }
		Value set=sp_setspec(s); if(!s->ok) return vbool(0);
		if(!sp_expect(s,':',"expected ':' in quantifier")) return vbool(0);
		const char *body=s->p; long cnt=0; int res=isall?1:0; const char *endp=body;
		int n=s->dead ? 0 : val_n(&set);	/* dead: parse the body once, below */
		for(int e=0;e<n;e++){
			Env ne=*s->env; if(!env_bind(s,&ne,var,set_e(&set,e))) return vbool(0);
			Value bv=sp_sub(s,body,&ne,0,&endp); if(!s->ok) return vbool(0);
			if(!need(s,&bv,T_BOOL,q)) return vbool(0);
			if(isall && !bv.i) res=0;
			if(issome && bv.i) res=1;
			if(iscount && bv.i) cnt++;
		}
		if(n==0){ /* still parse the body once, unevaluated, so a broken body is caught */
			Env ne=*s->env; if(!env_bind(s,&ne,var,0)) return vbool(0);
			Value bv=sp_sub(s,body,&ne,1,&endp); if(!s->ok) return vbool(0);
			(void)bv;
		}
		s->p=endp;
		return iscount?vint(cnt):vbool(res);
	}
	/* a comparison or arithmetic term, possibly `A in SET` */
	Value a=sp_range(s); sp_skip(s);
	if(sp_word(s,"in")){ if(!need(s,&a,T_INT,"in")) return vbool(0); Value set=sp_setspec(s); if(!s->ok) return vbool(0); int in=0; for(int i=0;!s->dead && i<val_n(&set);i++) if(set_e(&set,i)==a.i) in=1; return vbool(in); }
	const char *op=s->p;
	if(!strncmp(op,"==",2)){ s->p+=2; Value b=sp_range(s); if(!s->ok) return vbool(0); return vbool(sp_val_eq(s,&a,&b,"==")); }
	if(!strncmp(op,"!=",2)){ s->p+=2; Value b=sp_range(s); if(!s->ok) return vbool(0); return vbool(!sp_val_eq(s,&a,&b,"!=")); }
	/* an order: <= and >= before < and >, which they begin with; << and
	 * >> delimit a sequence */
	int len=!strncmp(op,"<=",2)||!strncmp(op,">=",2)?2:(*op=='<'&&op[1]!='<')||(*op=='>'&&op[1]!='>')?1:0;
	if(len){
		char o[3]={op[0],len==2?op[1]:0,0};
		s->p+=len; Value b=sp_add(s);
		if(!need(s,&a,T_INT,o) || !need(s,&b,T_INT,o)) return vbool(0);
		return vbool(o[0]=='<' ? (len==2 ? a.i<=b.i : a.i<b.i) : (len==2 ? a.i>=b.i : a.i>b.i));
	}
	return a;			/* a bare arithmetic/boolean value */
}
static Value sp_not(Pst *s){ sp_skip(s); if(sp_word(s,"not")){ Value v=sp_not(s); need(s,&v,T_BOOL,"not"); return vbool(!v.i); } return sp_quant(s); }
/* and/or evaluate left to right and short-circuit, as TLC does: once
 * the left operand decides the result, the right one is parsed but not
 * evaluated (see Pst.dead) */
static Value sp_and(Pst *s){
	Value a=sp_not(s); int nand=0;
	while(sp_word(s,"and")){ nand++;
		if(!need(s,&a,T_BOOL,"and")) return a;
		int skip=!a.i; s->dead+=skip;
		Value b=sp_not(s);
		s->dead-=skip;
		if(!skip && need(s,&b,T_BOOL,"and")) a=b;
	}
	s->lastand=nand>0;
	return a;
}
/* TLA+ gives /\ and \/ no relative precedence: SANY refuses
 * `a \/ b /\ c`, so `a or b and c` needs parentheses here too. */
static Value sp_or(Pst *s){
	Value a=sp_and(s); int ands=s->lastand;
	while(sp_word(s,"or")){
		if(ands){ fail(s,"and next to or needs parentheses, as in TLA+"); return a; }
		if(!need(s,&a,T_BOOL,"or")) return a;
		int skip=a.i!=0; s->dead+=skip;
		Value b=sp_and(s);
		s->dead-=skip;
		if(s->lastand){ fail(s,"and next to or needs parentheses, as in TLA+"); return a; }
		if(!skip && need(s,&b,T_BOOL,"or")) a=b;
	}
	return a;
}

/* evaluate EXPR to a Value in env; returns 1 ok, 0 error (msg in err) */
static int sp_eval(const char *expr, Env *env, Value *out, char *errbuf, size_t errcap){
	Pst s={0}; s.p=expr; s.env=env; s.ok=1;
	Value v=sp_or(&s);
	sp_skip(&s);
	if(s.ok && *s.p){ s.ok=0; snprintf(s.err,sizeof s.err,"trailing text: %s",s.p); }
	if(!s.ok){ if(errbuf) snprintf(errbuf,errcap,"%s",s.err); return 0; }
	*out=v; return 1;
}

/* evaluate the boolean expression expr (an invariant, a guard, a goal,
 * named by what) to *holds; returns 1 ok, 0 error (msg in err) */
static int sp_eval_bool(const char *expr, Env *env, int *holds, const char *what, char *errbuf, size_t errcap){
	Value v;
	if(!sp_eval(expr,env,&v,errbuf,errcap)) return 0;
	if(v.tag!=T_BOOL){ if(errbuf) snprintf(errbuf,errcap,"expected a boolean in %s, not %s",what,tag_name(v.tag)); return 0; }
	*holds=(int)v.i; return 1;
}

/* evaluate a SET-valued expression (a range LO..HI or {..} literal) to a
 * Value in env; returns 1 ok, 0 error */
static int sp_eval_set(const char *expr, Env *env, Value *out, char *errbuf, size_t errcap){
	Pst s={0}; s.p=expr; s.env=env; s.ok=1;
	Value v=sp_setspec(&s);
	sp_skip(&s);
	if(s.ok && *s.p){ s.ok=0; snprintf(s.err,sizeof s.err,"trailing text: %s",s.p); }
	if(!s.ok){ if(errbuf) snprintf(errbuf,errcap,"%s",s.err); return 0; }
	*out=v; return 1;
}

/* =====================================================================
 * the .cml parser
 * ===================================================================== */
static char *sp_trim(char *s){ while(*s&&isspace((unsigned char)*s))s++; char*e=s+strlen(s); while(e>s&&isspace((unsigned char)e[-1]))*--e=0; return s; }

/* evaluate a constant-only integer expression at parse time (for bounds
 * like N-1): reuse sp_eval with an env that has no variables */
static long sp_const_eval(const char *expr, int *ok){
	Env env=env_new(NULL);
	Value v; char e[128];
	if(!sp_eval(expr,&env,&v,e,sizeof e) || v.tag!=T_INT){ *ok=0; return 0; }
	*ok=1; return v.i;
}

static int sp_load_into(const char *path);
static int g_symok;		/* symmetry reduction is active (sym_init) */

/* copy src into dst[cap], or report that it does not fit.  A model whose
 * text does not fit a buffer is an error, never a silently shorter
 * expression (a truncated `A and FALSE` would read as `A`). */
static int fits(char *dst, size_t cap, const char *src, const char *path, int ln, const char *what){
	if((size_t)snprintf(dst,cap,"%s",src)<cap) return 1;
	fprintf(stderr,"cmc: %s:%d: %s is too long (limit %zu bytes)\n",path,ln,what,cap-1);
	return 0;
}
#define CPY(dst,src,what) do{ if(!fits((dst),sizeof(dst),(src),path,ln,(what))) goto bad; }while(0)

/* the first occurrence of tok in t outside brackets, or NULL */
static char *top_find(char *t, const char *tok){
	int d=0; size_t n=strlen(tok);
	for(char *p=t;*p;p++){
		if(d==0 && !strncmp(p,tok,n)) return p;
		if(*p=='('||*p=='['||*p=='{') d++;
		else if(*p==')'||*p==']'||*p=='}') d--;
	}
	return NULL;
}
/* A variable's type, parsed from t (which it may write into) into a new
 * Ty node: [K -> T], [a : T, ...], Seq(T, N), or a set of integers.
 * Returns the node's index, or -1 after reporting why. */
static int ty_parse(char *t, const char *path, int ln){
	t=sp_trim(t);
	if(S.nty>=SP_MAXTY){ fprintf(stderr,"cmc: %s:%d: the variables' types are too large\n",path,ln); return -1; }
	int ti=S.nty++; Ty *T=&S.ty[ti]; memset(T,0,sizeof *T);
	size_t n=strlen(t);
	if(!strncmp(t,"Seq(",4)){
		if(t[n-1]!=')'){ fprintf(stderr,"cmc: %s:%d: Seq type needs )\n",path,ln); return -1; }
		t[n-1]=0; char *c=strrchr(t+4,',');
		if(!c){ fprintf(stderr,"cmc: %s:%d: Seq type needs a maximum length: Seq(T, N)\n",path,ln); return -1; }
		*c=0; int ok; long mx=sp_const_eval(c+1,&ok);
		if(!ok || mx<0 || mx>SP_MAPMAX){ fprintf(stderr,"cmc: %s:%d: a Seq length bound must be a constant from 0 to %d\n",path,ln,SP_MAPMAX); return -1; }
		T->kind=TY_SEQ; T->max=mx;
		int sub=ty_parse(t+4,path,ln); if(sub<0) return -1;
		S.ty[ti].sub=sub; return ti;
	}
	if(t[0]!='['){ T->kind=TY_DOM; return fits(T->dom,sizeof T->dom,t,path,ln,"var domain")?ti:-1; }
	char *rb=strrchr(t,']');
	char *arrow=top_find(t+1,"->");
	if(arrow){
		if(!rb){ fprintf(stderr,"cmc: %s:%d: function var needs ]\n",path,ln); return -1; }
		*arrow=0; *rb=0; T->kind=TY_FUN;
		char *d=sp_trim(t+1);
		if(!fits(T->dom,sizeof T->dom,d,path,ln,"function domain")) return -1;
		char *r=sp_trim(arrow+2);
		if(!r[0]){ fprintf(stderr,"cmc: %s:%d: function var needs a range\n",path,ln); return -1; }
		if(strlen(r)>=SP_EXPR){ fprintf(stderr,"cmc: %s:%d: function range is too long (limit %d bytes)\n",path,ln,SP_EXPR-1); return -1; }
		int sub=ty_parse(r,path,ln); if(sub<0) return -1;
		S.ty[ti].sub=sub; return ti;
	}
	if(!top_find(t+1,":")){ fprintf(stderr,"cmc: %s:%d: function var needs ->\n",path,ln); return -1; }
	if(!rb){ fprintf(stderr,"cmc: %s:%d: record var needs ]\n",path,ln); return -1; }
	/* [a : T, b : T]: one field per top-level comma */
	*rb=0; T->kind=TY_REC;
	char *p=t+1;
	while(p){
		char *c=top_find(p,","); if(c) *c=0;
		char *col=top_find(p,":");
		if(!col){ fprintf(stderr,"cmc: %s:%d: a record field needs name : type\n",path,ln); return -1; }
		*col=0; char *fn=sp_trim(p);
		int id=fld_id(fn); if(id<0){ fprintf(stderr,"cmc: %s:%d: too many record field names\n",path,ln); return -1; }
		Ty *R=&S.ty[ti];
		for(int j=0;j<R->nf;j++) if(R->fid[j]==id){ fprintf(stderr,"cmc: %s:%d: record field %s is declared twice\n",path,ln,fn); return -1; }
		if(R->nf>=SP_MAXFLD){ fprintf(stderr,"cmc: %s:%d: too many record fields\n",path,ln); return -1; }
		int sub=ty_parse(col+1,path,ln); if(sub<0) return -1;
		/* keep the fields in id order, the order a record value has */
		R=&S.ty[ti]; int j=R->nf;
		while(j>0 && R->fid[j-1]>id){ R->fid[j]=R->fid[j-1]; R->fty[j]=R->fty[j-1]; j--; }
		R->fid[j]=id; R->fty[j]=sub; R->nf++;
		p=c?c+1:NULL;
	}
	return ti;
}

/* def NAME(p, ...) = EXPR (or ==, as TLA+ writes it), the text after
 * "def ", into a new Def; 0, or -1 after reporting why */
static int def_parse(char *t, const char *path, int ln){
	if(S.ndefs>=SP_MAXDEF){ fprintf(stderr,"cmc: %s:%d: too many defs\n",path,ln); return -1; }
	char *eq=strchr(t,'=');
	if(!eq){ fprintf(stderr,"cmc: %s:%d: def needs = EXPR\n",path,ln); return -1; }
	*eq=0; char *body=eq+1; if(*body=='=') body++;
	Def *D=&S.def[S.ndefs]; memset(D,0,sizeof *D);
	char *head=sp_trim(t), *lp=strchr(head,'(');
	if(lp){
		char *rp=strrchr(lp,')');
		if(!rp || *sp_trim(rp+1)){ fprintf(stderr,"cmc: %s:%d: def parameters need )\n",path,ln); return -1; }
		*lp=0; *rp=0;
		char *save=NULL;
		for(char *p=strtok_r(lp+1,",",&save); p; p=strtok_r(NULL,",",&save)){
			if(D->np>=SP_MAXPARAM){ fprintf(stderr,"cmc: %s:%d: too many def parameters\n",path,ln); return -1; }
			if(!fits(D->pname[D->np],SP_NAME,sp_trim(p),path,ln,"def parameter")) return -1;
			D->np++;
		}
		if(D->np==0){ fprintf(stderr,"cmc: %s:%d: a def with () needs a parameter\n",path,ln); return -1; }
	}
	if(!fits(D->name,SP_NAME,sp_trim(head),path,ln,"def name")) return -1;
	if(!fits(D->body,SP_EXPR,sp_trim(body),path,ln,"def body")) return -1;
	int f; (void)cst_lookup(D->name,&f);
	if(!D->name[0] || f || var_index(D->name)>=0 || def_index(D->name)>=0){ fprintf(stderr,"cmc: %s:%d: def %s redeclares a name or names nothing\n",path,ln,D->name); return -1; }
	S.ndefs++;
	return 0;
}
/* Parse each def's body once, unevaluated, with its parameters bound:
 * a name it cannot resolve (a later def, so itself too) and a syntax
 * error are load errors, never a surprise in the middle of a search */
static int defs_check(const char *path){
	for(int d=0;d<S.ndefs;d++){
		const Def *D=&S.def[d];
		Env env=env_new(NULL); env.ndefvis=d;
		Pst s={0}; s.p=D->body; s.env=&env; s.ok=1; s.dead=1;
		for(int i=0;i<D->np && s.ok;i++) env_bind(&s,&env,D->pname[i],0);
		size_t m=ar_mark();
		(void)sp_or(&s); sp_skip(&s);
		if(s.ok && *s.p) fail(&s,"trailing text");
		ar_release(m);
		if(!s.ok){ fprintf(stderr,"cmc: %s: def %s: %s\n",path,D->name,s.err); return -1; }
	}
	return 0;
}

static int sp_load(const char *path){
	memset(&S.modname,0,sizeof S.modname);
	S.ncst=S.nvar=S.nact=S.ninv=0; S.init[0]=0; S.has_sym=0; S.nprop=0; S.fair_weak=0; S.nfair=0;
	S.deadlock_off=0; S.ncon=S.nacon=0;
	S.nty=S.ndefs=S.nfld=0;
	g_symok=0;	/* no symmetry carries over from a previously loaded spec */
	if(sp_load_into(path)) return -1;
	if(!S.modname[0]){ fprintf(stderr,"cmc: %s: no module declaration\n",path); return -1; }
	if(S.nvar==0){ fprintf(stderr,"cmc: %s: no variables\n",path); return -1; }
	if(!S.init[0]){ fprintf(stderr,"cmc: %s: no init\n",path); return -1; }
	if(defs_check(path)) return -1;
	if(S.ninv==0 && S.nprop==0){ fprintf(stderr,"cmc: %s: no invariant or property\n",path); return -1; }
	/* a fair line names an action of the spec; on one that --define
	 * leaves out of Next it is vacuous, as WF or SF of a disabled action */
	memset(S.afair,0,sizeof S.afair);
	for(int i=0;i<S.nfair;i++){
		int a=0; while(a<S.nact && strcmp(S.act[a].name,S.fairname[i])) a++;
		if(a==S.nact){ fprintf(stderr,"cmc: %s: fair %s names no action\n",path,S.fairname[i]); return -1; }
		if(S.fairk[i]>S.afair[a]) S.afair[a]=S.fairk[i];
	}
	/* a --define that gates nothing is a typo: checking without the
	 * planted bug it meant to turn on would read as a pass */
	for(int i=0;i<S.ndef;i++){
		int used=0; for(int a=0;a<S.nact;a++) if(!strcmp(S.act[a].tag,S.define[i])) used=1;
		if(!used){ fprintf(stderr,"cmc: --define %s names no [%s] action in %s\n",S.define[i],S.define[i],path); return -1; }
	}
	return 0;
}

/* parse one .cml file, APPENDING its declarations to S (so an `extends`
 * base is merged in before the extending module's own additions).
 * `extends FILE` loads FILE first, resolved relative to this file. */
static int sp_load_into(const char *path){
	FILE *f=fopen(path,"r");
	if(!f){ fprintf(stderr,"cmc: cannot open %s\n",path); return -1; }
	char line[1024]; int ln=0; Action *cur=NULL; int in_action=0;
	while(fgets(line,sizeof line,f)){
		ln++;
		if(!strchr(line,'\n') && !feof(f)){ fprintf(stderr,"cmc: %s:%d: line is too long (limit %zu bytes)\n",path,ln,sizeof line-2); goto bad; }
		char *h=strchr(line,'#'); if(h)*h=0;
		char *s=sp_trim(line);
		if(!*s) continue;
		if(!strncmp(s,"module ",7)){ if(!S.modname[0]) CPY(S.modname,sp_trim(s+7),"module name"); in_action=0; continue; }
		if(!strncmp(s,"extends ",8) || !strncmp(s,"import ",7)){
			const char *base=sp_trim(s+(s[0]=='e'?8:7));
			/* resolve relative to this file's directory; add .cml if absent */
			char bp[1024]; const char *sl=strrchr(path,'/');
			if(base[0]=='/'||!sl) CPY(bp,base,"extends path");
			else if((size_t)snprintf(bp,sizeof bp,"%.*s/%s",(int)(sl-path),path,base)>=sizeof bp){ fprintf(stderr,"cmc: %s:%d: extends path is too long\n",path,ln); goto bad; }
			if(!strstr(bp,".cml")){ if(strlen(bp)+4>=sizeof bp){ fprintf(stderr,"cmc: %s:%d: extends path is too long\n",path,ln); goto bad; } strcat(bp,".cml"); }
			if(sp_load_into(bp)){ fprintf(stderr,"cmc: %s:%d: cannot extend %s\n",path,ln,base); goto bad; }
			in_action=0; continue;
		}
		if(!strncmp(s,"const ",6)){
			char *rest=sp_trim(s+6); char *eq=strchr(rest,'='); if(!eq){ fprintf(stderr,"cmc: %s:%d: const needs =\n",path,ln); goto bad; }
			*eq=0; char *nm=sp_trim(rest), *vv=sp_trim(eq+1);
			/* No override, even of an extended module's const: that
			 * module's load-time values (a derived const, a Seq bound)
			 * already used the first value.  SANY refuses it too. */
			{ int dup; (void)cst_lookup(nm,&dup); if(dup){ fprintf(stderr,"cmc: %s:%d: const %s is declared twice\n",path,ln,nm); goto bad; } }
			if(S.ncst>=SP_MAXCONST){ fprintf(stderr,"cmc: too many consts\n"); goto bad; }
			int ok; long val=sp_const_eval(vv,&ok); if(!ok){ fprintf(stderr,"cmc: %s:%d: const %s: bad value\n",path,ln,nm); goto bad; }
			CPY(S.cst[S.ncst].name,nm,"const name"); S.cst[S.ncst].val=val; S.ncst++; in_action=0; continue;
		}
		if(!strncmp(s,"var ",4)){
			char *rest=sp_trim(s+4); char *col=strchr(rest,':'); if(!col){ fprintf(stderr,"cmc: %s:%d: var needs :\n",path,ln); goto bad; }
			*col=0; char *nm=sp_trim(rest), *dom=sp_trim(col+1);
			if(S.nvar>=SP_MAXVAR){ fprintf(stderr,"cmc: too many vars\n"); goto bad; }
			Var *V=&S.var[S.nvar];
			CPY(V->name,nm,"var name");
			V->ty=ty_parse(dom,path,ln); if(V->ty<0) goto bad;
			S.nvar++; in_action=0; continue;
		}
		if(!strncmp(s,"def ",4)){ if(def_parse(s+4,path,ln)) goto bad; in_action=0; continue; }
		if(!strncmp(s,"fairness ",9) || !strncmp(s,"fair ",5)){
			/* `fairness weak|strong`: every action; `fair weak|strong A`:
			 * one action, by name, resolved once every action is read */
			int all=s[4]=='n'; char *v=sp_trim(s+(all?9:5)); int k=0;
			if(!strncmp(v,"weak",4) && (all?!v[4]:v[4]==' ')) k=1;
			else if(!strncmp(v,"strong",6) && (all?!v[6]:v[6]==' ')) k=2;
			if(!k){ fprintf(stderr,"cmc: %s:%d: %s\n",path,ln,all?"fairness is weak or strong":"fair needs weak or strong and an action name"); goto bad; }
			if(all){ if(k>S.fair_weak) S.fair_weak=k; in_action=0; continue; }
			if(S.nfair>=SP_MAXACT){ fprintf(stderr,"cmc: too many fair lines\n"); goto bad; }
			CPY(S.fairname[S.nfair],sp_trim(v+(k==1?4:6)),"fair action name"); S.fairk[S.nfair++]=k;
			in_action=0; continue;
		}
		if(!strncmp(s,"property",8)){
			char *rest=sp_trim(s+8); char *col=strchr(rest,':'); if(!col){ fprintf(stderr,"cmc: %s:%d: property needs name:\n",path,ln); goto bad; }
			*col=0; if(S.nprop>=SP_MAXINV){ fprintf(stderr,"cmc: too many properties\n"); goto bad; }
			SpProp *P=&S.prop[S.nprop]; CPY(P->name,sp_trim(rest),"property name"); char *body=sp_trim(col+1);
			char *lt=strstr(body,"~>");
			if(lt){ *lt=0; P->kind=1; CPY(P->a,sp_trim(body),"property"); CPY(P->b,sp_trim(lt+2),"property"); }
			else if(!strncmp(body,"eventually",10)){ P->kind=0; CPY(P->a,sp_trim(body+10),"property"); P->b[0]=0; }
			else if(!strncmp(body,"always",6) && isspace((unsigned char)body[6])){
				/* always [E]_vars: TLA+'s [][E]_vars, E over x and x' */
				char *a=sp_trim(body+6); size_t n=strlen(a);
				if(a[0]!='[' || n<8 || strcmp(a+n-6,"]_vars")){ fprintf(stderr,"cmc: %s:%d: an action property is 'always [E]_vars'\n",path,ln); goto bad; }
				a[n-6]=0; P->kind=2; CPY(P->a,sp_trim(a+1),"property"); P->b[0]=0;
			}
			else { fprintf(stderr,"cmc: %s:%d: property must be 'eventually E', 'A ~> B' or 'always [E]_vars'\n",path,ln); goto bad; }
			S.nprop++; in_action=0; continue;
		}
		if(!strncmp(s,"symmetry ",9)){ CPY(S.symmetry,sp_trim(s+9),"symmetry set"); S.has_sym=1; in_action=0; continue; }
		if(!strncmp(s,"init ",5)){ const char *ic=sp_trim(s+5);
			if(S.init[0]){ if(strlen(S.init)+5+strlen(ic)>=sizeof S.init){ fprintf(stderr,"cmc: %s:%d: init is too long (limit %zu bytes)\n",path,ln,sizeof S.init-1); goto bad; } strcat(S.init," and "); strcat(S.init,ic); }
			else CPY(S.init,ic,"init");
			in_action=0; continue; }
		if(!strncmp(s,"action ",7)){
			if(S.nact>=SP_MAXACT){ fprintf(stderr,"cmc: too many actions\n"); goto bad; }
			cur=&S.act[S.nact++]; memset(cur,0,sizeof *cur);
			char *rest=sp_trim(s+7);
			/* NAME, then an optional (params) and an optional [TAG], then
			 * the colon.  The tag is only the bracket after the parameter
			 * list, so a domain such as 0 .. f[i] is never read as one. */
			char *cl=strrchr(rest,':');
			if(cl) *cl=0;
			rest=sp_trim(rest);
			char *p=rest;
			while(isalnum((unsigned char)*p)||*p=='_') p++;
			char *ne=p, *params=NULL, *tag=NULL;
			while(*p==' '||*p=='\t') p++;
			if(*p=='('){
				int d=0; char *q=p;
				for(;*q;q++){ if(*q=='('||*q=='['||*q=='{') d++; else if(*q==')'||*q==']'||*q=='}'){ if(--d==0) break; } }
				if(*q!=')'){ fprintf(stderr,"cmc: %s:%d: action parameters need )\n",path,ln); goto bad; }
				*q=0; params=p+1; p=q+1;
				while(*p==' '||*p=='\t') p++;
			}
			if(*p=='['){
				char *te=strchr(p,']');
				if(!te){ fprintf(stderr,"cmc: %s:%d: action tag needs ]\n",path,ln); goto bad; }
				*te=0; tag=sp_trim(p+1); p=te+1;
				while(*p==' '||*p=='\t') p++;
				for(char *c=tag;*c;c++) if(!isalnum((unsigned char)*c)&&*c!='_'){ fprintf(stderr,"cmc: %s:%d: action tag is not a name: [%s]\n",path,ln,tag); goto bad; }
			}
			if(ne==rest || *p){ fprintf(stderr,"cmc: %s:%d: cannot parse action header: %s\n",path,ln,rest); goto bad; }
			*ne=0;
			CPY(cur->name,rest,"action name");
			if(tag) CPY(cur->tag,tag,"action tag");
			if(params){
				/* parse  p in SET, q in SET */
				char *save=NULL; for(char *t=strtok_r(params,",",&save); t; t=strtok_r(NULL,",",&save)){
					char *in=strstr(t," in "); if(!in){ fprintf(stderr,"cmc: %s:%d: action param needs 'in'\n",path,ln); goto bad; }
					*in=0; if(cur->nparam>=SP_MAXPARAM){ fprintf(stderr,"cmc: too many params\n"); goto bad; }
					CPY(cur->pname[cur->nparam],sp_trim(t),"parameter name"); CPY(cur->pdom[cur->nparam],sp_trim(in+4),"parameter domain"); cur->nparam++;
				}
			}
			in_action=1; continue;
		}
		if(!strncmp(s,"when ",5)){
			if(!in_action || !cur){ fprintf(stderr,"cmc: %s:%d: when outside action\n",path,ln); goto bad; }
			/* each further when line is one more conjunct, as a TLA+ /\ line */
			const char *g=sp_trim(s+5);
			if(!cur->guard[0]) CPY(cur->guard,g,"guard");
			else { char both[2*SP_EXPR+16]; snprintf(both,sizeof both,"(%s) and (%s)",cur->guard,g); CPY(cur->guard,both,"guard"); }
			continue;
		}
		if(!strncmp(s,"deadlock",8) && (s[8]==' '||s[8]=='=')){
			char *v=sp_trim(s+8);
			if(*v!='='){ fprintf(stderr,"cmc: %s:%d: deadlock needs = on or = off\n",path,ln); goto bad; }
			v=sp_trim(v+1);
			if(!strcmp(v,"off")) S.deadlock_off=1;
			else if(!strcmp(v,"on")) S.deadlock_off=0;
			else { fprintf(stderr,"cmc: %s:%d: deadlock is on or off, not %s\n",path,ln,v); goto bad; }
			in_action=0; continue;
		}
		if(!strncmp(s,"constraint ",11) || !strncmp(s,"action_constraint ",18)){
			int act=s[0]=='a'; const char *kw=act?"action_constraint":"constraint";
			SpInv *tab=act?S.acon:S.con; int *n=act?&S.nacon:&S.ncon;
			char *rest=sp_trim(s+strlen(kw)); char *col=strchr(rest,':');
			if(!col){ fprintf(stderr,"cmc: %s:%d: %s needs name:\n",path,ln,kw); goto bad; }
			*col=0; if(*n>=SP_MAXINV){ fprintf(stderr,"cmc: too many %ss\n",kw); goto bad; }
			CPY(tab[*n].name,sp_trim(rest),"constraint name"); CPY(tab[*n].expr,sp_trim(col+1),"constraint"); (*n)++;
			in_action=0; continue;
		}
		if(!strncmp(s,"invariant",9)){
			char *rest=sp_trim(s+9); char *col=strchr(rest,':'); if(!col){ fprintf(stderr,"cmc: %s:%d: invariant needs name:\n",path,ln); goto bad; }
			*col=0; if(S.ninv>=SP_MAXINV){ fprintf(stderr,"cmc: too many invariants\n"); goto bad; }
			CPY(S.inv[S.ninv].name,sp_trim(rest),"invariant name"); CPY(S.inv[S.ninv].expr,sp_trim(col+1),"invariant"); S.ninv++; in_action=0; continue;
		}
		/* otherwise: an assignment  lhs' = rhs  inside an action */
		if(in_action && cur){
			char *pr=strstr(s,"'"); char *eq=strchr(s,'=');
			if(pr && eq && pr<eq){
				*pr=0; char *lhs=sp_trim(s); char *rhs=sp_trim(eq+1);
				if(cur->nasgn>=SP_MAXVAR){ fprintf(stderr,"cmc: too many assignments\n"); goto bad; }
				/* x' = 1 together with x' = 0 is FALSE in TLA+; refuse it */
				for(int i=0;i<cur->nasgn;i++) if(!strcmp(cur->lhs[i],lhs)){ fprintf(stderr,"cmc: %s:%d: %s' is assigned twice in action %s\n",path,ln,lhs,cur->name); goto bad; }
				CPY(cur->lhs[cur->nasgn],lhs,"assignment target"); CPY(cur->rhs[cur->nasgn],rhs,"assignment"); cur->nasgn++;
				continue;
			}
		}
		fprintf(stderr,"cmc: %s:%d: cannot parse: %s\n",path,ln,s); goto bad;
	}
	fclose(f);
	return 0;
bad: fclose(f); return -1;
}

/* is action a gated out by a [TAG] not given with --define? */
static int act_active(const Action *a){
	if(!a->tag[0]) return 1;
	for(int i=0;i<S.ndef;i++) if(!strcmp(S.define[i],a->tag)) return 1;
	return 0;
}

/* =====================================================================
 * explicit-state BFS over the tuple of variable values
 * ===================================================================== */
/* A state is the concatenation of its variables' encodings (see the
 * value model), canonical by construction, so two states are the same
 * exactly when their words are.  nw counts the words in use; the rest
 * of w[] is never read. */
typedef struct { int nw; long w[SP_STATEW]; } State;

/* the views of st's variables, into vv[] */
static void st_views(const State *st, Value *vv){
	const long *p=st->w;
	for(int i=0;i<S.nvar;i++){ vv[i]=enc_view(p); p+=enc_len(p); }
}
/* the state of the variable values vv[]; 0, or -1 when it does not fit */
static int st_pack(State *st, const Value *vv, char *err, size_t errcap){
	size_t o=0;
	for(int i=0;i<S.nvar;i++){
		size_t n=val_len(&vv[i]);
		if(o+n>SP_STATEW){ snprintf(err,errcap,"a state needs more than %d words",SP_STATEW); return -1; }
		enc_put(st->w+o,&vv[i]); o+=n;
	}
	st->nw=(int)o; return 0;
}
/* the bytes of a state, for hashing and comparison */
static size_t state_bytes(const State *st, unsigned char *buf, size_t cap){
	size_t n=(size_t)st->nw*sizeof st->w[0];
	memcpy(buf,st->w,n<cap?n:cap);
	return n;
}
static uint64_t fnv(const unsigned char *b, size_t n){ uint64_t h=1469598103934665603ULL; for(size_t i=0;i<n;i++){ h^=b[i]; h*=1099511628211ULL; } return h; }

/* ---- symmetry reduction (Milestone E) -----------------------------
 * When `symmetry S` names a set of interchangeable values, two states
 * that differ only by a permutation of S's elements are equivalent.
 * Canonicalise a state to the lexicographically-least serialisation over
 * all permutations of S, and dedup on that.  Sound when the spec really
 * is symmetric under permuting S (the user's declared obligation, as in
 * TLC); the gate checks it empirically -- same verdict with symmetry on
 * and off, strictly fewer states.  A permutation remaps every integer
 * that is a member of S, wherever it appears: integer values, set
 * elements, the keys and values of functions, record field values and
 * sequence elements (never a record's field names or a sequence's
 * positions, which are not values of the model). */
static Value g_symset;
static long g_symel[8]; static int g_symn;
/* Evaluate the symmetry set once; returns 0, or -1 with the reason in
 * err when it cannot be used (one it cannot read, or of more than 8
 * values, whose 9! permutations a state would be canonicalised over).
 * A set of fewer than two values reduces nothing and is accepted. */
static int sym_init(char *err, size_t errcap){
	g_symok=0; if(!S.has_sym) return 0;
	Env env=env_new(NULL);
	char e[128];
	if(!sp_eval_set(S.symmetry,&env,&g_symset,e,sizeof e)){ snprintf(err,errcap,"symmetry %.80s: %s",S.symmetry,e); return -1; }
	if(val_n(&g_symset)>8){ snprintf(err,errcap,"symmetry %.80s: at most 8 values, not %d",S.symmetry,val_n(&g_symset)); return -1; }
	g_symn=val_n(&g_symset); for(int i=0;i<g_symn;i++) g_symel[i]=set_e(&g_symset,i);
	g_symok=g_symn>=2;
	return 0;
}
/* remap x under a permutation perm[] of the symmetry set (perm[i] is the
 * image of g_symel[i]); non-members pass through */
static long sym_map(long x, const int *perm){
	for(int i=0;i<g_symn;i++) if(g_symel[i]==x) return g_symel[perm[i]];
	return x;
}
/* the encoding at in under the permutation perm, written at out (the
 * same length); a function's entries are re-sorted by their new keys */
static void enc_sym(const long *in, long *out, const int *perm){
	size_t len=enc_len(in);
	if(in[0]==T_BOOL){ memcpy(out,in,2*sizeof *in); return; }
	if(in[0]==T_INT){ out[0]=T_INT; out[1]=sym_map(in[1],perm); return; }
	out[0]=in[0]; out[1]=in[1]; out[2]=in[2];
	int n=(int)in[2];
	if(in[0]==T_SET){
		for(int j=0;j<n;j++) out[3+j]=sym_map(in[3+j],perm);
		(void)set_canon(out+3,n); return;
	}
	if(in[0]!=T_FUN){
		/* a record keeps its field ids, a sequence its positions */
		const long *p=in+3; long *q=out+3;
		for(int j=0;j<n;j++){
			if(in[0]==T_REC){ *q++=*p++; }
			enc_sym(p,q,perm); q+=enc_len(p); p+=enc_len(p);
		}
		return;
	}
	/* a function: remap each entry, then emit them in key order */
	const long *ent[SP_MAPMAX]; long nk[SP_MAPMAX];
	const long *p=in+3;
	for(int j=0;j<n;j++){ ent[j]=p; nk[j]=sym_map(p[0],perm); p+=1+enc_len(p+1); }
	long *q=out+3;
	for(int done=0;done<n;done++){
		int m=-1;
		for(int j=0;j<n;j++) if(ent[j] && (m<0 || nk[j]<nk[m])) m=j;
		*q++=nk[m]; enc_sym(ent[m]+1,q,perm); q+=enc_len(ent[m]+1); ent[m]=NULL;
	}
	(void)len;
}
static void sym_apply(const State *in, State *out, const int *perm){
	out->nw=in->nw;
	const long *p=in->w; long *q=out->w;
	for(int vi=0;vi<S.nvar;vi++){ enc_sym(p,q,perm); q+=enc_len(p); p+=enc_len(p); }
}
/* The canonical serialisation of a state: its bytes, or under symmetry
 * the lexicographically least serialisation over every permutation of
 * the symmetry set, so all states in one orbit serialise alike.  Writes
 * to out (cap bytes) and returns the length. */
static void sym_perms(int *perm, int k, int n, const State *st, unsigned char *best, size_t *bestn, size_t cap, int *first){
	if(k==n){
		State p; sym_apply(st,&p,perm); unsigned char bb[65536]; size_t nb=state_bytes(&p,bb,sizeof bb);
		if(*first || nb<*bestn || (nb==*bestn && memcmp(bb,best,nb)<0)){ memcpy(best,bb,nb<cap?nb:cap); *bestn=nb; *first=0; }
		return;
	}
	for(int i=k;i<n;i++){ int t=perm[k]; perm[k]=perm[i]; perm[i]=t; sym_perms(perm,k+1,n,st,best,bestn,cap,first); t=perm[k]; perm[k]=perm[i]; perm[i]=t; }
}
static size_t state_canon(const State *st, unsigned char *out, size_t cap){
	if(!g_symok) return state_bytes(st,out,cap);
	int perm[8]; for(int i=0;i<g_symn;i++) perm[i]=i; size_t n=0; int first=1;
	sym_perms(perm,0,g_symn,st,out,&n,cap,&first); return n;
}
/* the fingerprint of a state's canonical serialisation: an index only,
 * since two states are the same exactly when those bytes are */
static uint64_t state_fp(const State *st){
	unsigned char bb[65536]; size_t nb=state_canon(st,bb,sizeof bb); return fnv(bb,nb);
}

/* exact equality of two abstract states: the canonical serialisation,
 * compared byte for byte.  C2 matching and the liveness graph use this,
 * never state_fp() alone, so neither symmetry canonicalisation nor a
 * hash collision can make two different states match. */
static int state_eq(const State *a, const State *b){
	static unsigned char ba[65536], bb[65536];
	size_t na=state_bytes(a,ba,sizeof ba), nb=state_bytes(b,bb,sizeof bb);
	return na==nb && na<=sizeof ba && !memcmp(ba,bb,na);
}

/* set the initial state from the `init` predicate (a conjunction of
 * `var == EXPR`).  Returns 0 ok. */
static int in_domains(const Value *vv, char *err, size_t errcap);
static int sp_init_state(State *st, char *err, size_t errcap){
	Value vv[SP_MAXVAR]; for(int i=0;i<S.nvar;i++) vv[i]=vint(0);
	size_t m=ar_mark(); int rc=-1;
	/* split init on " and " at top level (init conjuncts are var == expr) */
	int set[SP_MAXVAR]={0};
	char work[SP_EXPR]; snprintf(work,sizeof work,"%s",S.init);
	char *p=work;
	while(*p){
		char *nx=strstr(p," and "); if(nx)*nx=0;
		char *conj=sp_trim(p);
		char *eq=strstr(conj,"==");
		if(!eq){ snprintf(err,errcap,"init conjunct is not var == expr: %s",conj); goto out; }
		*eq=0; char *lhs=sp_trim(conj); char *rhs=sp_trim(eq+2);
		int vi=var_index(lhs); if(vi<0){ snprintf(err,errcap,"init assigns unknown var %s",lhs); goto out; }
		if(set[vi]){ snprintf(err,errcap,"init assigns %s twice",lhs); goto out; }
		Env env=env_new(vv); env.assigned=set;
		Value v; if(!sp_eval(rhs,&env,&v,err,errcap)) goto out;
		vv[vi]=v; set[vi]=1;
		if(!nx) break;
		p=nx+5;
	}
	for(int i=0;i<S.nvar;i++) if(!set[i]){ snprintf(err,errcap,"init does not set %s",S.var[i].name); goto out; }
	if(in_domains(vv,err,errcap) || st_pack(st,vv,err,errcap)) goto out;
	rc=0;
out:	ar_release(m);
	return rc;
}

static int set_has(const Value *set, long x){
	for(int j=0;j<val_n(set);j++) if(set_e(set,j)==x) return 1;
	return 0;
}

/* is x a member of the domain expression dom?  A range `LO .. HI` is
 * tested by its bounds, since a value range (0 .. 99) can exceed
 * SP_SETMAX; any other form is evaluated as a set.  Returns 1 member,
 * 0 not, -1 on an error (message in err). */
static int dom_has(const char *dom, long x, char *err, size_t errcap){
	Env env=env_new(NULL);
	const char *dd=strstr(dom,"..");
	size_t m=ar_mark(); int rc=-1;
	if(dd && !strchr(dom,'{')){
		char lo[SP_EXPR]; snprintf(lo,sizeof lo,"%.*s",(int)(dd-dom),dom);
		Value l,h; if(!sp_eval(lo,&env,&l,err,errcap) || !sp_eval(dd+2,&env,&h,err,errcap)) goto out;
		if(l.tag!=T_INT || h.tag!=T_INT){ snprintf(err,errcap,"the bounds of %s are not integers",dom); goto out; }
		rc=x>=l.i && x<=h.i; goto out;
	}
	Value set; if(!sp_eval_set(dom,&env,&set,err,errcap)) goto out;	/* a set, or an error */
	rc=set_has(&set,x);
out:	ar_release(m);
	return rc;
}

/* Does v have type ty?  A TY_DOM value is an integer in its set; a
 * TY_FUN value a function whose keys are exactly its key set, each
 * value of the range type; a TY_REC value a record of exactly its
 * fields; a TY_SEQ value a sequence of at most its bound, each element
 * of the element type.  name names the part, as `f[2].a`, in err.  The
 * checker enumerates states, not declarations, so a value outside its
 * type is a different model than the one written (and than the emitted
 * TLA+'s type annotations allow): a model error, never a silently
 * explored state, and a sequence past its bound is never cut.  Returns
 * 0 if in, -1 (message in err) if not or on an evaluation error. */
static int val_in_type(int ty, const Value *v, char *name, char *err, size_t errcap){
	const Ty *T=&S.ty[ty];
	static const int want[]={T_INT,T_FUN,T_REC,T_SEQ};
	if(v->tag!=want[T->kind] && T->kind==TY_FUN){ snprintf(err,errcap,"%s is not a function on %s",name,T->dom); return -1; }
	if(v->tag!=want[T->kind]){ snprintf(err,errcap,"%s is %s, not %s",name,tag_name(v->tag),tag_name(want[T->kind])); return -1; }
	if(T->kind==TY_DOM){
		int in=dom_has(T->dom,v->i,err,errcap); if(in<0) return -1;
		/* a part of a variable lies in a range, a whole one in a domain */
		const char *what=strpbrk(name,"[.") ? "range" : "domain";
		if(!in){ snprintf(err,errcap,"%s = %ld is outside its %s %s",name,v->i,what,T->dom); return -1; }
		return 0;
	}
	long key[SP_MAXFLD+SP_MAPMAX]; Value kid[SP_MAXFLD+SP_MAPMAX];
	int n=comp_kids(v,key,kid);
	size_t nl=strlen(name);
	if(T->kind==TY_FUN){
		Env env=env_new(NULL); size_t m=ar_mark(); Value keys; int ok=1;
		if(!sp_eval_set(T->dom,&env,&keys,err,errcap)) return -1;
		if(n!=val_n(&keys)) ok=0;
		for(int j=0;ok && j<n;j++) if(key[j]!=set_e(&keys,j)) ok=0;
		ar_release(m);
		if(!ok){ snprintf(err,errcap,"%s is not a function on %s",name,T->dom); return -1; }
	}
	else if(T->kind==TY_REC){
		int ok=n==T->nf;
		for(int j=0;ok && j<n;j++) if(key[j]!=T->fid[j]) ok=0;
		if(!ok){ snprintf(err,errcap,"%s does not have exactly the fields of its record type",name); return -1; }
	}
	else if(n>T->max){ snprintf(err,errcap,"%s has length %d, past its bound %ld",name,n,T->max); return -1; }
	for(int j=0;j<n;j++){
		/* name the part: f[k], r.a, s[i] */
		if(T->kind==TY_REC) snprintf(name+nl,128-nl,".%s",S.fld[key[j]]);
		else snprintf(name+nl,128-nl,"[%ld]",T->kind==TY_SEQ?(long)j+1:key[j]);
		int sub=T->kind==TY_REC ? T->fty[j] : T->sub;
		if(val_in_type(sub,&kid[j],name,err,errcap)) return -1;
		name[nl]=0;
	}
	return 0;
}
static int var_in_domain(int vi, const Value *v, char *err, size_t errcap){
	char name[128]; snprintf(name,sizeof name,"%s",S.var[vi].name);
	return val_in_type(S.var[vi].ty,v,name,err,errcap);
}
static int in_domains(const Value *vv, char *err, size_t errcap){
	for(int vi=0;vi<S.nvar;vi++) if(var_in_domain(vi,&vv[vi],err,errcap)) return -1;
	return 0;
}

/* apply action a with bound params (in env) to st, producing *out;
 * returns 1 if the guard held and the step was taken, 0 if disabled,
 * -1 on an error.  Every successor lies in its variables' types. */
static int sp_step(const Action *a, Env *env, const State *st, State *out, char *err, size_t errcap){
	Value cur[SP_MAXVAR], nxt[SP_MAXVAR]; st_views(st,cur);
	size_t m=ar_mark(); int rc=-1;
	env->var=cur;
	if(a->guard[0]){ int g; if(!sp_eval_bool(a->guard,env,&g,"a guard",err,errcap)) goto out; if(!g){ rc=0; goto out; } }
	memcpy(nxt,cur,sizeof cur);
	/* evaluate all RHS against the CURRENT state (simultaneous assignment) */
	for(int i=0;i<a->nasgn;i++){
		int vi=var_index(a->lhs[i]); if(vi<0){ snprintf(err,errcap,"assign to unknown var %s",a->lhs[i]); goto out; }
		Value v; if(!sp_eval(a->rhs[i],env,&v,err,errcap)) goto out;
		nxt[vi]=v;
	}
	if(in_domains(nxt,err,errcap) || st_pack(out,nxt,err,errcap)) goto out;
	rc=1;
out:	ar_release(m);
	return rc;
}

/* recursively bind an action's parameters over their finite domains and
 * invoke the body for each combination */
static void bind_params(const Action *a, Env *env, int pi, const State *st,
	State *succ, int *nsucc, int cap, char *err, size_t errcap, int *rc){
	if(*rc<0) return;
	if(pi==a->nparam){
		static State out;	/* not on the stack: a state is SP_STATEW words */
		int r=sp_step(a,env,st,&out,err,errcap);
		if(r<0){ *rc=-1; return; }
		if(r==1){ if(*nsucc<cap) succ[(*nsucc)++]=out; else { snprintf(err,errcap,"too many successors"); *rc=-1; } }
		return;
	}
	/* evaluate the param domain in the current state */
	Value cur[SP_MAXVAR]; st_views(st,cur);
	Value dom; char derr[128]; Env denv=*env; denv.var=cur;
	size_t m=ar_mark();
	if(!sp_eval_set(a->pdom[pi],&denv,&dom,derr,sizeof derr)){ ar_release(m); snprintf(err,errcap,"param domain: %s",derr); *rc=-1; return; }
	long el[SP_SETMAX]; int n=val_n(&dom); for(int k=0;k<n;k++) el[k]=set_e(&dom,k);
	ar_release(m);
	for(int k=0;k<n;k++){
		Env e3=*env; if(e3.np>=SP_MAXPARAM){ snprintf(err,errcap,"action %s: too many parameters",a->name); *rc=-1; return; }
		{ int f; (void)cst_lookup(a->pname[pi],&f); for(int i=0;i<e3.np;i++) if(!strcmp(e3.pname[i],a->pname[pi])) f=1;
		  if(f || var_index(a->pname[pi])>=0 || def_index(a->pname[pi])>=0){ snprintf(err,errcap,"action %s: parameter %s redeclares a name already in scope",a->name,a->pname[pi]); *rc=-1; return; } }
		snprintf(e3.pname[e3.np],SP_NAME,"%s",a->pname[pi]); e3.pval[e3.np]=el[k]; e3.thunk[e3.np]=NULL; e3.np++;
		bind_params(a,&e3,pi+1,st,succ,nsucc,cap,err,errcap,rc);
		if(*rc<0) return;
	}
}

/* check every invariant in state st; return the index of the first that
 * fails, -1 if all hold, -2 on an evaluation error (msg in err) */
static int sp_check(const State *st, char *err, size_t errcap){
	Value vv[SP_MAXVAR]; st_views(st,vv);
	Env env=env_new(vv); size_t m=ar_mark(); int rc=-1;
	for(int i=0;i<S.ninv && rc==-1;i++){ int h; if(!sp_eval_bool(S.inv[i].expr,&env,&h,"an invariant",err,errcap)) rc=-2; else if(!h) rc=i; }
	ar_release(m);
	return rc;
}

/* BFS state store (open-addressed hash of fingerprints) + a queue of
 * states with their shortest path (action descriptions) */
#define SP_MAXSTATES 2000000
/* The seen set: an open-addressing table indexed by fingerprint whose
 * entries hold each state's canonical bytes in a pool, compared on a
 * fingerprint match, so a hash collision cannot drop a state.  Sized for
 * SP_MAXSTATES at half load, so it never grows. */
typedef struct { uint64_t h; size_t off; uint32_t len; } SeenEnt;
static SeenEnt *seen; static size_t seen_cap, seen_n;
static unsigned char *seen_pool; static size_t seen_pool_cap, seen_pool_used;
static int seen_insert(const State *st){
	unsigned char bb[65536]; size_t nb=state_canon(st,bb,sizeof bb);
	uint64_t h=fnv(bb,nb); if(!h) h=1;
	size_t m=seen_cap-1, i=h&m;
	for(;;){
		if(seen[i].h==0) break;
		if(seen[i].h==h && seen[i].len==nb && !memcmp(seen_pool+seen[i].off,bb,nb)) return 0;
		i=(i+1)&m;
	}
	if(seen_pool_used+nb>seen_pool_cap){
		size_t nc=seen_pool_cap?seen_pool_cap*2:(size_t)1<<20; while(seen_pool_used+nb>nc) nc*=2;
		unsigned char *np=realloc(seen_pool,nc); if(!np){ fprintf(stderr,"cmc: out of memory for the seen set\n"); exit(1); }
		seen_pool=np; seen_pool_cap=nc;
	}
	memcpy(seen_pool+seen_pool_used,bb,nb);
	seen[i].h=h; seen[i].off=seen_pool_used; seen[i].len=(uint32_t)nb; seen_pool_used+=nb; seen_n++;
	return 1;
}

/* a queued state, holding only the words its state uses */
typedef struct QN { struct QN *prev; char step[64]; int depth; int nw; long w[]; } QN;
static QN *qn_new(const State *st){
	QN *n=calloc(1,sizeof *n+(size_t)st->nw*sizeof st->w[0]);
	if(!n){ fprintf(stderr,"cmc: out of memory\n"); exit(1); }
	n->nw=st->nw; memcpy(n->w,st->w,(size_t)st->nw*sizeof st->w[0]);
	return n;
}
static void qn_state(const QN *n, State *st){ st->nw=n->nw; memcpy(st->w,n->w,(size_t)n->nw*sizeof st->w[0]); }

/* the indices 0 .. n-1 of a record's fields fid[] in the order of their
 * names, into ord[]: Apalache writes a record's fields sorted by name */
static void fld_order(const long *fid, int n, int *ord){
	for(int j=0;j<n;j++){
		int k=j;
		while(k>0 && strcmp(S.fld[fid[ord[k-1]]],S.fld[fid[j]])>0){ ord[k]=ord[k-1]; k--; }
		ord[k]=j;
	}
}

/* the views of the variables of the queued state n, into vv[] */
static void qn_views(const QN *n, Value *vv){
	const long *p=n->w;
	for(int i=0;i<S.nvar;i++){ vv[i]=enc_view(p); p+=enc_len(p); }
}

/* One value of a state in a trace file.  In ITF (Apalache's ADR-015)
 * an integer is {"#bigint": "N"}, as Apalache writes every integer, a
 * function is {"#map": [[k, v], ...]}, a record an object of its fields
 * and a sequence an array.  In plain JSON an integer is a number, a
 * function an object keyed by its arguments, a record an object of its
 * fields and a sequence an array.  A state's variables have passed
 * in_domains(), so each is an integer or one of these, nested. */
static void trace_value(FILE *o, const Value *v, int itf){
	if(v->tag==T_INT){ fprintf(o,itf?"{\"#bigint\":\"%ld\"}":"%ld",v->i); return; }
	long key[SP_MAXFLD+SP_MAPMAX]; Value kid[SP_MAXFLD+SP_MAPMAX]; int n=comp_kids(v,key,kid);
	if(v->tag==T_SEQ) fputc('[',o);
	else fputs(v->tag==T_FUN && itf ? "{\"#map\":[" : "{",o);
	int ord[SP_MAXFLD+SP_MAPMAX];
	if(v->tag==T_REC) fld_order(key,n,ord);
	else for(int j=0;j<n;j++) ord[j]=j;
	for(int i=0;i<n;i++){
		int j=ord[i];
		if(i) fputc(',',o);
		if(v->tag==T_REC) fprintf(o,"\"%s\":",S.fld[key[j]]);
		else if(v->tag==T_FUN && itf) fprintf(o,"[{\"#bigint\":\"%ld\"},",key[j]);
		else if(v->tag==T_FUN) fprintf(o,"\"%ld\":",key[j]);
		trace_value(o,&kid[j],itf);
		if(v->tag==T_FUN && itf) fputc(']',o);
	}
	if(v->tag==T_SEQ) fputc(']',o);
	else fputs(v->tag==T_FUN && itf ? "]}" : "}",o);
}

/* The type of Ty node ti as Apalache writes it in an ITF file's
 * varTypes: Int, (Int -> T), Seq(T), and a record as { a: T, b: T },
 * its fields sorted by name, into buf.  Returns buf. */
static const char *ty_itf(int ti, char *buf, size_t cap){
	const Ty *T=&S.ty[ti]; char sub[1024];
	if(T->kind==TY_DOM) snprintf(buf,cap,"Int");
	else if(T->kind==TY_FUN) snprintf(buf,cap,"(Int -> %s)",ty_itf(T->sub,sub,sizeof sub));
	else if(T->kind==TY_SEQ) snprintf(buf,cap,"Seq(%s)",ty_itf(T->sub,sub,sizeof sub));
	else {
		long fid[SP_MAXFLD]; int ord[SP_MAXFLD];
		for(int j=0;j<T->nf;j++) fid[j]=T->fid[j];
		fld_order(fid,T->nf,ord);
		size_t n=(size_t)snprintf(buf,cap,"{ ");
		for(int j=0;j<T->nf && n<cap;j++)
			n+=(size_t)snprintf(buf+n,cap-n,"%s%s: %s",j?", ":"",S.fld[fid[ord[j]]],ty_itf(T->fty[ord[j]],sub,sizeof sub));
		if(n<cap) snprintf(buf+n,cap-n," }");
	}
	return buf;
}

/* Write a counterexample of n states to `file`, one state per line, as
 * ITF (fmt "itf") or as a JSON array (fmt "json").  State i has the
 * words w[i] and, past the first, was reached by action act[i].  A
 * lasso (loop >= 0) goes on from its last state back to state loop by
 * action loopact, or stutters there forever when loopact is NULL.  ITF
 * says so as ADR-015 does, with "loop": loop, and names loopact in the
 * header's #meta; JSON repeats state loop as a last entry with the
 * action and "back_to": loop.  The ITF header follows what Apalache
 * writes for the module cmc --emit-tla makes of this spec: the
 * constants as "params", repeated in every state, and each variable's
 * type in "varTypes".  Each state's #meta also names the action that
 * led to it, which Apalache leaves out.  Returns 0, or -1 if the file
 * could not be written. */
static int spec_write_trace(const char *file, const char *fmt, const long *const *w, const char *const *act,
		int n, int loop, const char *loopact){
	char tb[1024];
	int itf=!strcmp(fmt,"itf"), i;
	FILE *o=fopen(file,"w");
	if(!o) return -1;
	if(itf){
		fprintf(o,"{\"#meta\":{\"format\":\"ITF\",\"format-description\":\"https://apalache-mc.org/docs/adr/015adr-trace.html\","
			"\"description\":\"Created by cmc --spec from %s\",",S.modname);
		if(loop>=0) fprintf(o,"\"loopAction\":\"%s\",",loopact?loopact:"Stuttering");
		fprintf(o,"\"varTypes\":{");
		for(int vi=0;vi<S.nvar;vi++) fprintf(o,"%s\"%s\":\"%s\"",vi?",":"",S.var[vi].name,ty_itf(S.var[vi].ty,tb,sizeof tb));
		fprintf(o,"}},\n\"params\":[");
		for(int c=0;c<S.ncst;c++) fprintf(o,"%s\"%s\"",c?",":"",S.cst[c].name);
		fprintf(o,"],\n\"vars\":[");
		for(int vi=0;vi<S.nvar;vi++) fprintf(o,"%s\"%s\"",vi?",":"",S.var[vi].name);
		fprintf(o,"],\n\"states\":[\n");
	}
	else fprintf(o,"[\n");
	int last=n+(loop>=0 && !itf);	/* JSON repeats the loop state */
	for(i=0;i<last;i++){
		int k=i<n?i:loop;
		Value vv[SP_MAXVAR]; const long *p=w[k];
		for(int vi=0;vi<S.nvar;vi++){ vv[vi]=enc_view(p); p+=enc_len(p); }
		if(itf){
			fprintf(o,"{\"#meta\":{\"index\":%d",i);
			if(i) fprintf(o,",\"action\":\"%s\"",act[i]);
			fprintf(o,"}");
			for(int c=0;c<S.ncst;c++) fprintf(o,",\"%s\":{\"#bigint\":\"%ld\"}",S.cst[c].name,S.cst[c].val);
		}
		else if(i==n) fprintf(o,"{\"index\":%d,\"action\":\"%s\",\"back_to\":%d,\"vars\":{",i,loopact?loopact:"Stuttering",loop);
		else if(i) fprintf(o,"{\"index\":%d,\"action\":\"%s\",\"vars\":{",i,act[i]);
		else fprintf(o,"{\"index\":0,\"action\":null,\"vars\":{");
		for(int vi=0;vi<S.nvar;vi++){
			fprintf(o,"%s\"%s\":",vi||itf?",":"",S.var[vi].name);
			trace_value(o,&vv[vi],itf);
		}
		fprintf(o,"%s%s\n",itf?"}":"}}",i+1<last?",":"");
	}
	if(itf && loop>=0) fprintf(o,"],\n\"loop\":%d}\n",loop);
	else fprintf(o,"%s\n",itf?"]}":"]");
	return fclose(o)?-1:0;
}

/* Is the step from `from` to `to` inside the model, as TLC's CONSTRAINT
 * and ACTION_CONSTRAINT decide it?  Every state constraint must hold in
 * `to`, and, unless `from` is NULL (an initial state), every action
 * constraint over the pair, with x' read from `to`.  Sets *in; returns
 * 0, or -1 on an evaluation error (message in err). */
static int sp_in_model(const State *from, const State *to, int *in, char *err, size_t errcap){
	Value cur[SP_MAXVAR], nxt[SP_MAXVAR]; st_views(to,nxt);
	Env env=env_new(nxt); size_t m=ar_mark(); int rc=0;
	*in=1;
	for(int i=0;i<S.ncon && *in && !rc;i++)
		if(!sp_eval_bool(S.con[i].expr,&env,in,"a constraint",err,errcap)) rc=-1;
	if(from){
		st_views(from,cur); env.var=cur; env.nxt=nxt;
		for(int i=0;i<S.nacon && *in && !rc;i++)
			if(!sp_eval_bool(S.acon[i].expr,&env,in,"an action constraint",err,errcap)) rc=-1;
	}
	ar_release(m);
	return rc;
}

/* print v on stdout: an integer, TRUE or FALSE, {..}, a function as
 * [k:v,...], a record as [a |-> v, ...], a sequence as <<..>> */
static void val_print(const Value *v){
	if(v->tag==T_INT){ printf("%ld",v->i); return; }
	if(v->tag==T_BOOL){ printf("%s",v->i?"TRUE":"FALSE"); return; }
	if(v->tag==T_SET){ printf("{"); for(int j=0;j<val_n(v);j++) printf("%s%ld",j?", ":"",set_e(v,j)); printf("}"); return; }
	long key[SP_MAXFLD+SP_MAPMAX]; Value kid[SP_MAXFLD+SP_MAPMAX]; int n=comp_kids(v,key,kid);
	printf(v->tag==T_SEQ?"<<":"[");
	for(int j=0;j<n;j++){
		if(v->tag==T_FUN) printf("%s%ld:",j?",":"",key[j]);
		else if(v->tag==T_REC) printf("%s%s |-> ",j?", ":"",S.fld[key[j]]);
		else if(j) printf(", ");
		val_print(&kid[j]);
	}
	printf(v->tag==T_SEQ?">>":"]");
}

/* Where cmc_spec_main() was asked to write a counterexample, if
 * anywhere (--trace-out), and in which format. */
static const char *g_trace_out, *g_trace_fmt;

/* Print the shortest behaviour from the initial state to `cur` and the
 * variable values there, after the caller's "Error: ..." line, and write
 * it to the --trace-out file if one was named.  Returns 0, or 3 if that
 * file could not be written. */
static int print_trace(const QN *cur){
	int cn=0; for(const QN *x=cur; x; x=x->prev) cn++;
	printf("Error: The behavior up to this point is (%d actions):\n", cn-1);
	const char **steps = malloc((size_t)cn * sizeof *steps); int si=0;
	if(!steps){ fprintf(stderr,"cmc: out of memory\n"); exit(1); }
	for(const QN *x=cur; x; x=x->prev) steps[si++]=x->step;
	for(int i=cn-2;i>=0;i--) printf("    %d. %s\n", cn-1-i, steps[i]);
	free(steps);
	printf("  state:");
	Value vv[SP_MAXVAR]; qn_views(cur,vv);
	for(int vi=0;vi<S.nvar;vi++){ printf(" %s=",S.var[vi].name); val_print(&vv[vi]); }
	printf("\n");
	if(!g_trace_out) return 0;
	const long **w=malloc((size_t)cn*2*sizeof *w);
	if(!w){ fprintf(stderr,"cmc: out of memory\n"); exit(1); }
	const char **act=(const char **)(void *)(w+cn);
	si=cn; for(const QN *x=cur; x; x=x->prev){ si--; w[si]=x->w; act[si]=x->step; }
	int wr=spec_write_trace(g_trace_out,g_trace_fmt,w,act,cn,-1,NULL);
	free(w);
	if(wr){ fprintf(stderr,"cmc: could not write %s: %s\n",g_trace_out,strerror(errno)); return 3; }
	printf("Trace written to %s (%s, %d states).\n",g_trace_out,g_trace_fmt,cn);
	return 0;
}

/* Check the invariants in the state of `x`, newly generated, and report
 * a violation.  Returns 0 if they hold, else the exit status (1 or 3). */
/* The part of the symmetry obligation a single state can show.  For
 * each transposition of the symmetry set's first value with another
 * (together they generate every permutation), the image of st must lie
 * in the variables' domains, and every invariant and constraint must
 * give it the value st gets.  Otherwise the reduction would fold st
 * onto a state the model tells apart, and could hide a violation (a
 * counter over 0 .. 2 under symmetry 1 .. 2 folds x = 2 onto x = 1).
 * Returns 0, or -1 with the reason in err.  An asymmetric guard is not
 * seen here; it stays the model's obligation, as in TLC. */
static int sym_check(const State *st, char *err, size_t errcap){
	Value a[SP_MAXVAR], b[SP_MAXVAR]; st_views(st,a);
	char e[128]; size_t m=ar_mark(); int rc=0;
	for(int t=1;t<g_symn && !rc;t++){
		int perm[8]; for(int i=0;i<g_symn;i++) perm[i]=i;
		perm[0]=t; perm[t]=0;
		static State img; sym_apply(st,&img,perm); st_views(&img,b);
		if(in_domains(b,e,sizeof e)){ snprintf(err,errcap,"the model is not symmetric under %.80s: swapping %ld and %ld gives a state outside it (%.40s)",S.symmetry,g_symel[0],g_symel[t],e); rc=-1; break; }
		for(int k=0;k<S.ninv+S.ncon && !rc;k++){
			const SpInv *I=k<S.ninv?&S.inv[k]:&S.con[k-S.ninv];
			Env ea=env_new(a), eb=env_new(b); int ha=0, hb=0;
			int oka=sp_eval_bool(I->expr,&ea,&ha,"an invariant",e,sizeof e);
			int okb=sp_eval_bool(I->expr,&eb,&hb,"an invariant",e,sizeof e);
			if(oka!=okb || ha!=hb){ snprintf(err,errcap,"the model is not symmetric under %.80s: %s %.40s tells a state from its image under swapping %ld and %ld",S.symmetry,k<S.ninv?"invariant":"constraint",I->name,g_symel[0],g_symel[t]); rc=-1; }
		}
	}
	ar_release(m);
	return rc;
}

static int check_new(const QN *x){
	char err[256];
	static State st; qn_state(x,&st);
	if(g_symok && sym_check(&st,err,sizeof err)){ fprintf(stderr,"cmc: %s\n",err); return 3; }
	int bad=sp_check(&st,err,sizeof err);
	if(bad==-2){ fprintf(stderr,"cmc: invariant evaluation error: %s\n",err); return 3; }
	if(bad<0) return 0;
	printf("Error: Invariant %s is violated.\n", S.inv[bad].name);
	return print_trace(x) ? 3 : 1;
}

/* Check every action property (`always [E]_vars`) on the step from
 * `from` to `to`, the state of `x`, as TLC checks an implied action on each
 * transition it generates, inside the model or not, to a new state or
 * a seen one.  A step that changes no variable is a stutter, which
 * [E]_vars allows.  Returns 0 if they hold, else the exit status (1 for
 * a violation, after its trace; 3 on an evaluation error). */
static int check_step(const State *from, const State *to, const QN *x){
	if(state_eq(from,to)) return 0;
	char err[256]; Value cur[SP_MAXVAR], nxt[SP_MAXVAR];
	st_views(from,cur); st_views(to,nxt);
	Env env=env_new(cur); env.nxt=nxt;
	size_t m=ar_mark(); int rc=0;
	for(int i=0;i<S.nprop && !rc;i++){
		if(S.prop[i].kind!=2) continue;
		int h; if(!sp_eval_bool(S.prop[i].a,&env,&h,"an action property",err,sizeof err)){ fprintf(stderr,"cmc: %s\n",err); rc=3; break; }
		if(h) continue;
		printf("Error: Action property %s is violated.\n",S.prop[i].name);
		rc=print_trace(x) ? 3 : 1;
	}
	ar_release(m);
	return rc;
}

/* =====================================================================
 * liveness (Milestone F): temporal properties on the abstract model,
 * with TLA+'s meaning for Spec == Init /\ [][Next]_vars /\ Fairness.
 *
 * A behaviour may stutter at any state, so a run can stop moving
 * anywhere unless fairness forbids it.  `fairness weak` adds WF_vars(A)
 * for every active action A, which forbids a run that from some point
 * on has <<A>>_vars continuously enabled and never takes it.  An action
 * step that leaves every variable unchanged is a stutter, not an A
 * step, so it neither enables A nor discharges A's fairness.
 *
 * After the safety search, rebuild the reachable graph with
 * non-stuttering, action-labelled edges and, per state, the actions
 * with a non-stuttering successor.  For a goal G, let M be the ~G
 * states.  A run that stays in M forever ends inside one SCC C of the M
 * subgraph, and a fair run covering C exists iff every action is
 * disabled at some state of C or labels an edge inside C.  (A single
 * state with no internal edge qualifies when every action is disabled
 * there, and always without fairness.)  Call the states of such SCCs
 * the bad core, and the M states that reach the core through M the bad
 * region:
 *   eventually P : violated iff init lies in the bad region of ~P.
 *   A ~> B       : violated iff a reachable A state lies in the bad
 *                  region of ~B.  The path to the core avoids B, since
 *                  reaching B discharges the obligation.
 * Symmetry reduction is refused here: a permuted successor would fold
 * into a self-loop and read as a stutter.  The spec gate checks each
 * verdict against TLC on the emitted module.
 * ===================================================================== */
#define LV_MAX 200000
#define LV_SUCC 4096
static struct {
	long *pool; size_t pused, pcap;		/* the states' words, back to back */
	size_t *off; int *nw; int n, cap;	/* state i: nw[i] words at pool+off[i]; 0 is init */
	int *head;				/* first out-edge of each state, -1 none */
	int *to, *act, *next; int ne, ecap;	/* out-edges, as linked lists */
	int *rhead, *rsrc;			/* in-edges, CSR: rsrc[rhead[v] .. rhead[v+1]) */
	unsigned char *en;			/* en[i*nact+a]: a has a non-stuttering successor at i */
	int *slot; size_t mcap;			/* open-addressed index of states, -1 empty */
} lv;

static void lv_free(void){
	free(lv.pool); free(lv.off); free(lv.nw); free(lv.head); free(lv.to); free(lv.act); free(lv.next);
	free(lv.rhead); free(lv.rsrc); free(lv.en); free(lv.slot);
	memset(&lv,0,sizeof lv);
}

/* the index of state s, adding it if new (*added says which).  Lookup
 * compares states exactly, so a hash collision cannot merge two. */
static void lv_get(int i, State *st){ st->nw=lv.nw[i]; memcpy(st->w,lv.pool+lv.off[i],(size_t)lv.nw[i]*sizeof st->w[0]); }
static int lv_find_or_add(const State *s, int *added, char *err, size_t errcap){
	size_t m=lv.mcap-1, i=(size_t)state_fp(s)&m;
	while(lv.slot[i]!=-1){
		int j=lv.slot[i];
		if(lv.nw[j]==s->nw && !memcmp(lv.pool+lv.off[j],s->w,(size_t)s->nw*sizeof s->w[0])){ *added=0; return j; }
		i=(i+1)&m;
	}
	if(lv.n>=LV_MAX){ snprintf(err,errcap,"more than %d states",LV_MAX); return -1; }
	if(lv.n==lv.cap){
		int nc=lv.cap?lv.cap*2:1024; if(nc>LV_MAX) nc=LV_MAX;
		size_t *no=realloc(lv.off,sizeof(size_t)*(size_t)nc); if(!no) goto oom; lv.off=no;
		int *nn=realloc(lv.nw,sizeof(int)*(size_t)nc); if(!nn) goto oom; lv.nw=nn;
		int *nh=realloc(lv.head,sizeof(int)*(size_t)nc); if(!nh) goto oom; lv.head=nh;
		lv.cap=nc;
	}
	if(lv.pused+(size_t)s->nw>lv.pcap){
		size_t nc=lv.pcap?lv.pcap*2:(size_t)1<<16; while(lv.pused+(size_t)s->nw>nc) nc*=2;
		long *np=realloc(lv.pool,nc*sizeof *np); if(!np) goto oom; lv.pool=np; lv.pcap=nc;
	}
	memcpy(lv.pool+lv.pused,s->w,(size_t)s->nw*sizeof s->w[0]);
	lv.off[lv.n]=lv.pused; lv.nw[lv.n]=s->nw; lv.pused+=(size_t)s->nw;
	lv.head[lv.n]=-1; lv.slot[i]=lv.n; *added=1;
	return lv.n++;
oom:	snprintf(err,errcap,"out of memory"); return -1;
}

static int lv_add_edge(int from, int to, int act, char *err, size_t errcap){
	if(lv.ne==lv.ecap){
		int nc=lv.ecap?lv.ecap*2:4096;
		int *a=realloc(lv.to,sizeof(int)*(size_t)nc); if(!a) goto oom; lv.to=a;
		int *b=realloc(lv.act,sizeof(int)*(size_t)nc); if(!b) goto oom; lv.act=b;
		int *c=realloc(lv.next,sizeof(int)*(size_t)nc); if(!c) goto oom; lv.next=c;
		lv.ecap=nc;
	}
	lv.to[lv.ne]=to; lv.act[lv.ne]=act; lv.next[lv.ne]=lv.head[from]; lv.head[from]=lv.ne; lv.ne++;
	return 0;
oom:	snprintf(err,errcap,"out of memory"); return -1;
}

/* build the reachable graph from init (the state array doubles as the
 * BFS queue), then its in-edge index.  Returns 0 ok, -1 on error. */
static int lv_build(const State *init, char *err, size_t errcap){
	memset(&lv,0,sizeof lv);
	lv.mcap=1; while(lv.mcap<(size_t)LV_MAX*4) lv.mcap<<=1;
	lv.slot=malloc(sizeof(int)*lv.mcap); lv.en=calloc((size_t)LV_MAX*(size_t)S.nact+1,1);
	State *succ=malloc(sizeof(State)*LV_SUCC);
	if(!lv.slot||!lv.en||!succ){ free(succ); snprintf(err,errcap,"out of memory"); return -1; }
	for(size_t i=0;i<lv.mcap;i++) lv.slot[i]=-1;
	int added, rc=0;
	if(lv_find_or_add(init,&added,err,errcap)<0) rc=-1;
	for(int si=0; si<lv.n && rc==0; si++){
		static State cur; lv_get(si,&cur);
		for(int a=0; a<S.nact && rc==0; a++){
			if(!act_active(&S.act[a])) continue;
			int ns=0, brc=0; Env env=env_new(NULL);
			bind_params(&S.act[a],&env,0,&cur,succ,&ns,LV_SUCC,err,errcap,&brc);
			if(brc<0){ rc=-1; break; }
			for(int k=0;k<ns;k++){
				if(state_eq(&succ[k],&cur)) continue;	/* a stutter */
				lv.en[(size_t)si*(size_t)S.nact+(size_t)a]=1;
				int ti=lv_find_or_add(&succ[k],&added,err,errcap);
				if(ti<0 || lv_add_edge(si,ti,a,err,errcap)){ rc=-1; break; }
			}
		}
	}
	free(succ);
	if(rc) return -1;
	lv.rhead=calloc((size_t)lv.n+1,sizeof(int)); lv.rsrc=malloc(sizeof(int)*((size_t)lv.ne+1));
	if(!lv.rhead||!lv.rsrc){ snprintf(err,errcap,"out of memory"); return -1; }
	for(int e=0;e<lv.ne;e++) lv.rhead[lv.to[e]+1]++;
	for(int v=0;v<lv.n;v++) lv.rhead[v+1]+=lv.rhead[v];
	int *fill=malloc(sizeof(int)*((size_t)lv.n+1));
	if(!fill){ snprintf(err,errcap,"out of memory"); return -1; }
	memcpy(fill,lv.rhead,sizeof(int)*((size_t)lv.n+1));
	for(int u=0;u<lv.n;u++) for(int e=lv.head[u];e!=-1;e=lv.next[e]) lv.rsrc[fill[lv.to[e]]++]=u;
	free(fill);
	return 0;
}

/* Tarjan's SCC algorithm over the states with mask[] set, run
 * iteratively (a recursive walk overflows the stack on a long chain).
 * Fills comp[] for masked states; returns the SCC count, -1 on OOM. */
static int lv_scc(const unsigned char *mask, int *comp){
	int n=lv.n, cnt=0, sp=0, nscc=0;
	int *num=malloc(sizeof(int)*(size_t)n), *low=malloc(sizeof(int)*(size_t)n);
	int *stk=malloc(sizeof(int)*(size_t)n), *cs=malloc(sizeof(int)*(size_t)n), *ce=malloc(sizeof(int)*(size_t)n);
	unsigned char *on=calloc((size_t)n,1);
	if(!num||!low||!stk||!cs||!ce||!on){ nscc=-1; goto out; }
	for(int i=0;i<n;i++){ num[i]=-1; comp[i]=-1; }
	for(int r=0;r<n;r++){
		if(!mask[r] || num[r]!=-1) continue;
		int top=0; cs[0]=r; ce[0]=lv.head[r]; num[r]=low[r]=cnt++; stk[sp++]=r; on[r]=1;
		while(top>=0){
			int u=cs[top], e=ce[top];
			if(e!=-1){
				ce[top]=lv.next[e];
				int v=lv.to[e]; if(!mask[v]) continue;
				if(num[v]==-1){ num[v]=low[v]=cnt++; stk[sp++]=v; on[v]=1; top++; cs[top]=v; ce[top]=lv.head[v]; }
				else if(on[v] && num[v]<low[u]) low[u]=num[v];
				continue;
			}
			if(low[u]==num[u]){ int w; do{ w=stk[--sp]; on[w]=0; comp[w]=nscc; }while(w!=u); nscc++; }
			top--;
			if(top>=0 && low[u]<low[cs[top]]) low[cs[top]]=low[u];
		}
	}
out:	free(num); free(low); free(stk); free(cs); free(ce); free(on);
	return nscc;
}

/* the fairness the spec puts on action a: 0 none, 1 WF, 2 SF */
static int act_fair(int a){
	if(!act_active(&S.act[a])) return 0;
	return S.afair[a]>S.fair_weak ? S.afair[a] : S.fair_weak;
}

/* The fair core of mask (see the section comment): core[i] is set for
 * each masked state in a component a fair run can stay in forever.
 * The work mask starts as mask; each round splits it into SCCs.  An SCC
 * in which an SF action is enabled somewhere and taken nowhere loses
 * the states that enable such an action, and what is left goes to the
 * next round.  Any other SCC is final, and fair iff every WF action is
 * disabled somewhere in it or taken inside it.  An SCC that loses
 * states loses an SF action for good, so the rounds end.  Returns 0
 * ok, -1 on OOM. */
static int lv_fair_core(const unsigned char *mask, unsigned char *core){
	int n=lv.n, na=S.nact, rc=-1;
	int *comp=malloc((sizeof(int)+1)*(size_t)n+1);
	unsigned char *work=(unsigned char *)(comp+n), *dis=NULL, *en, *tak, *cut, *shed;
	if(!comp) return -1;
	memcpy(work,mask,(size_t)n); memset(core,0,(size_t)n);
	for(int left=1; left;){
		int nscc=lv_scc(work,comp); if(nscc<0) goto out;
		/* per SCC and action: disabled somewhere, enabled somewhere,
		 * taken inside, SF and to be cut; then per SCC: sheds states */
		size_t cells=(size_t)nscc*(size_t)na;
		free(dis); dis=calloc(cells*4+(size_t)nscc+1,1); if(!dis) goto out;
		en=dis+cells; tak=en+cells; cut=tak+cells; shed=cut+cells;
		for(int u=0;u<n;u++){
			if(!work[u]) continue;
			size_t c=(size_t)comp[u]*(size_t)na;
			for(int a=0;a<na;a++){ if(lv.en[(size_t)u*(size_t)na+(size_t)a]) en[c+(size_t)a]=1; else dis[c+(size_t)a]=1; }
			for(int e=lv.head[u];e!=-1;e=lv.next[e]){ int v=lv.to[e];
				if(work[v] && comp[v]==comp[u]) tak[c+(size_t)lv.act[e]]=1; }
		}
		/* which SF actions each SCC must shed: enabled, never taken */
		for(int c=0;c<nscc;c++){ size_t o=(size_t)c*(size_t)na;
			for(int a=0;a<na;a++) if(act_fair(a)==2 && en[o+(size_t)a] && !tak[o+(size_t)a]){ cut[o+(size_t)a]=1; shed[c]=1; }
		}
		left=0;
		for(int u=0;u<n;u++){
			if(!work[u]) continue;
			size_t o=(size_t)comp[u]*(size_t)na;
			if(shed[comp[u]]){
				int keep=1;
				for(int a=0;a<na;a++) if(cut[o+(size_t)a] && lv.en[(size_t)u*(size_t)na+(size_t)a]) keep=0;
				work[u]=(unsigned char)keep; left|=keep;
				continue;
			}
			int fair=1;
			for(int a=0;a<na;a++) if(act_fair(a)==1 && !dis[o+(size_t)a] && !tak[o+(size_t)a]) fair=0;
			core[u]=(unsigned char)fair; work[u]=0;
		}
	}
	rc=0;
out:	free(comp); free(dis);
	return rc;
}

/* the fair core of mask into core[], and its bad region: reg[i] is set
 * for each masked state that reaches, through masked states, the fair
 * core.  Returns 0 ok, -1 on OOM. */
static int lv_bad_region(const unsigned char *mask, unsigned char *core, unsigned char *reg){
	int n=lv.n, qt=0;
	int *queue=malloc(sizeof(int)*(size_t)n+1);
	if(!queue || lv_fair_core(mask,core)){ free(queue); return -1; }
	memcpy(reg,core,(size_t)n);
	for(int u=0;u<n;u++) if(reg[u]) queue[qt++]=u;
	for(int qh=0; qh<qt; qh++){ int v=queue[qh];
		for(int k=lv.rhead[v];k<lv.rhead[v+1];k++){ int u=lv.rsrc[k];
			if(mask[u] && !reg[u]){ reg[u]=1; queue[qt++]=u; } } }
	free(queue);
	return 0;
}

/* A lasso counterexample under construction: the states s[0 .. n-1].st
 * of the liveness graph, each reached from the one before by action
 * s[i].act (s[0].act is -1, the initial state). */
typedef struct { struct { int st, act; } *s; int n, cap; } Lasso;

static int lasso_push(Lasso *q, int st, int act){
	if(q->n==q->cap){
		int nc=q->cap*2+64;
		void *ns=realloc(q->s,sizeof q->s[0]*(size_t)nc); if(!ns) return -1;
		q->s=ns; q->cap=nc;
	}
	q->s[q->n].st=st; q->s[q->n].act=act; q->n++;
	return 0;
}
static int lasso_last(const Lasso *q){ return q->s[q->n-1].st; }

/* The shortest path from src to a state with goal[] set, through states
 * with allow[] set (every state when allow is NULL), appended to q past
 * src, which q already ends in.  Returns the state reached, src itself
 * when goal[src] is set, or -1 on OOM or when no goal is reachable,
 * which the callers rule out. */
static int lv_path(int src, const unsigned char *allow, const unsigned char *goal, Lasso *q){
	if(goal[src]) return src;
	int n=lv.n, qt=0, hit=-1;
	int *par=malloc(sizeof(int)*3*(size_t)n); if(!par) return -1;
	int *pe=par+n, *fifo=pe+n;
	for(int i=0;i<n;i++) par[i]=-1;
	par[src]=src; fifo[qt++]=src;
	for(int qh=0; qh<qt && hit<0; qh++){ int u=fifo[qh];
		for(int e=lv.head[u]; e!=-1; e=lv.next[e]){ int v=lv.to[e];
			if(par[v]!=-1 || (allow && !allow[v])) continue;
			par[v]=u; pe[v]=e; fifo[qt++]=v;
			if(goal[v]){ hit=v; break; }
		}
	}
	/* walk back to src, then append forwards (fifo is free now) */
	int k=0; for(int v=hit; v>=0 && v!=src; v=par[v]) fifo[k++]=v;
	while(k>0){ int v=fifo[--k]; if(lasso_push(q,v,lv.act[pe[v]])){ hit=-1; break; } }
	free(par);
	return hit;
}

/* Is fair action a still owed on the walk q->s[from .. q->n-1], the
 * steps between them taken?  WF(a) is met once a is taken or disabled at
 * a state of the walk, SF(a) once a is taken or if it is enabled at no
 * state of the walk. */
static int lv_owed(const Lasso *q, int from, int a){
	int na=S.nact, f=act_fair(a), en=0;
	if(!f) return 0;
	for(int i=from;i<q->n;i++){
		if(i>from && q->s[i].act==a) return 0;
		en|=lv.en[(size_t)q->s[i].st*(size_t)na+(size_t)a];
		if(f==1 && !lv.en[(size_t)q->s[i].st*(size_t)na+(size_t)a]) return 0;
	}
	return f==1 || en;
}

/* Close a fair cycle that starts at entry, the last state of q and a
 * state of the fair core, staying inside entry's SCC of the core (each
 * such SCC is one the fixpoint in lv_fair_core() kept).  While a fair
 * action is owed (lv_owed()), walk to the nearest state of the SCC that
 * discharges it: one with an a step inside the SCC, then take it, or
 * for WF one where a is disabled.  Such a state exists: in a kept SCC a
 * WF action is disabled somewhere or taken inside it, and an SF action
 * enabled in it is taken inside it.  Once nothing is owed, walk back to
 * entry, and go on while that leg left something owed.  Each round
 * discharges an action for good or closes the cycle, so the walk ends.
 * When nothing was ever owed the cycle is a stutter at entry, *loopact
 * -1; otherwise *loopact is the action of the step back to entry, which
 * q does not hold.  Returns 0, or -1 on OOM. */
static int lv_cycle(const unsigned char *core, Lasso *q, int *loopact){
	int n=lv.n, na=S.nact, entry=lasso_last(q), start=q->n-1, rc=-1;
	int *comp=malloc((sizeof(int)+2)*(size_t)n);
	if(!comp) return -1;
	unsigned char *in=(unsigned char *)(comp+n), *goal=in+n;
	if(lv_scc(core,comp)<0) goto out;
	for(int u=0;u<n;u++) in[u]=(unsigned char)(core[u] && comp[u]==comp[entry]);
	for(;;){
		int a=0; while(a<na && !lv_owed(q,start,a)) a++;
		int last=lasso_last(q);
		if(a==na && last==entry) break;
		for(int u=0;u<n;u++){
			goal[u]=0;
			if(!in[u]) continue;
			if(a==na){ goal[u]=u==entry; continue; }
			if(act_fair(a)==1 && !lv.en[(size_t)u*(size_t)na+(size_t)a]) goal[u]=1;
			for(int e=lv.head[u];e!=-1;e=lv.next[e]) if(lv.act[e]==a && in[lv.to[e]]) goal[u]=1;
		}
		int u=lv_path(last,in,goal,q);
		if(u<0) goto out;
		if(a==na || !lv.en[(size_t)u*(size_t)na+(size_t)a]) continue;
		int e=lv.head[u]; while(lv.act[e]!=a || !in[lv.to[e]]) e=lv.next[e];
		if(lasso_push(q,lv.to[e],a)) goto out;
	}
	*loopact = q->n>start+1 ? q->s[--q->n].act : -1;
	rc=0;
out:	free(comp);
	return rc;
}

/* evaluate expr at state u into *holds; -1 on an evaluation error,
 * which fails the check rather than reading as FALSE */
static int lv_holds(const char *expr, int u, int *holds, char *err, size_t errcap){
	static State st; lv_get(u,&st);
	Value vv[SP_MAXVAR]; st_views(&st,vv);
	Env env=env_new(vv); size_t m=ar_mark();
	int ok=sp_eval_bool(expr,&env,holds,"a property",err,errcap);
	ar_release(m);
	return ok ? 0 : -1;
}

/* Print the lasso q, whose cycle runs from its last state back to state
 * q->s[loop] by action loopact (-1: it stutters there), as TLC prints
 * a temporal counterexample, and write it to the --trace-out file if
 * one was named.  Returns 0, or 3 if that file could not be written. */
static int print_lasso(const Lasso *q, int loop, int loopact){
	const char *la = loopact<0 ? NULL : S.act[loopact].name;
	printf("Error: The following behavior constitutes a counter-example:\n");
	for(int i=0;i<q->n;i++){
		static State st; lv_get(q->s[i].st,&st);
		Value vv[SP_MAXVAR]; st_views(&st,vv);
		printf("State %d: <%s>\n  ",i+1,i?S.act[q->s[i].act].name:"Initial predicate");
		for(int vi=0;vi<S.nvar;vi++){ printf("%s%s=",vi?" ":"",S.var[vi].name); val_print(&vv[vi]); }
		printf("\n");
	}
	if(la) printf("Back to state %d: <%s>\n",loop+1,la);
	else printf("State %d: Stuttering\n",q->n+1);
	if(!g_trace_out) return 0;
	const long **w=malloc((size_t)q->n*2*sizeof *w);
	if(!w){ fprintf(stderr,"cmc: out of memory\n"); exit(1); }
	const char **act=(const char **)(void *)(w+q->n);
	for(int i=0;i<q->n;i++){ w[i]=lv.pool+lv.off[q->s[i].st]; act[i]=i?S.act[q->s[i].act].name:NULL; }
	int wr=spec_write_trace(g_trace_out,g_trace_fmt,w,act,q->n,loop,la);
	free(w);
	if(wr){ fprintf(stderr,"cmc: could not write %s: %s\n",g_trace_out,strerror(errno)); return 3; }
	printf("Trace written to %s (%s, %d states, loop at state %d).\n",g_trace_out,g_trace_fmt,q->n,loop+1);
	return 0;
}

/* The counterexample to a property whose goal fails on the bad region
 * reg[] with fair core core[] (see the section comment): a shortest
 * path from the initial state to a state of start[], where the property
 * fails, on through the bad region to the core, then a fair cycle
 * there.  Prints it; returns 1, 3 if the trace file could not be
 * written, or -1 on OOM. */
static int lv_lasso(const unsigned char *start, const unsigned char *reg, const unsigned char *core, char *err, size_t errcap){
	Lasso q={0}; int rc=-1, loopact;
	if(lasso_push(&q,0,-1) || lv_path(0,NULL,start,&q)<0 || lv_path(lasso_last(&q),reg,core,&q)<0) goto out;
	int loop=q.n-1;
	if(lv_cycle(core,&q,&loopact)) goto out;
	rc=print_lasso(&q,loop,loopact) ? 3 : 1;
out:	if(rc<0) snprintf(err,errcap,"out of memory");
	free(q.s);
	return rc;
}

/* Check all liveness properties.  At the first violated one, print the
 * violation and its lasso; returns 1, or 3 if the trace file could not
 * be written.  Returns 0 if all hold, -1 on error (message in err). */
static int liveness_check(const State *init, char *err, size_t errcap){
	if(S.nprop==0) return 0;
	if(g_symok){ snprintf(err,errcap,"liveness properties cannot be checked under symmetry reduction"); return -1; }
	int rc=0;
	unsigned char *mask=NULL, *reg=NULL, *core=NULL, *start=NULL;
	if(lv_build(init,err,errcap)){ rc=-1; goto out; }
	mask=malloc((size_t)lv.n*4);
	if(!mask){ snprintf(err,errcap,"out of memory"); rc=-1; goto out; }
	reg=mask+lv.n; core=reg+lv.n; start=core+lv.n;
	for(int pi=0; pi<S.nprop && rc==0; pi++){ const SpProp *P=&S.prop[pi];
		if(P->kind==2) continue;	/* an action property: checked on each step */
		const char *goal = P->kind==0 ? P->a : P->b;
		for(int u=0;u<lv.n;u++){ int g; if(lv_holds(goal,u,&g,err,errcap)){ rc=-1; goto out; } mask[u]=!g; }
		if(lv_bad_region(mask,core,reg)){ snprintf(err,errcap,"out of memory"); rc=-1; goto out; }
		/* where the property fails: init for eventually, each A state
		 * of the bad region for A ~> B */
		int bad=0;
		for(int u=0;u<lv.n;u++){
			int a=P->kind==0 ? u==0 : 1;
			if(reg[u] && a && P->kind==1 && lv_holds(P->a,u,&a,err,errcap)){ rc=-1; goto out; }
			start[u]=(unsigned char)(reg[u] && a); bad|=start[u];
		}
		if(!bad) continue;
		printf("Error: Property %s is violated (a fair cycle avoids the goal).\n",P->name);
		rc=lv_lasso(start,reg,core,err,errcap);
	}
out:	free(mask); lv_free();
	return rc;
}

/* record the --define tags for sp_load(), which checks each gates an
 * action; returns -1 for one that cannot be held whole */
static int set_defines(char **defs, int ndefs){
	S.ndef=0;
	for(int i=0;i<ndefs;i++){
		if(S.ndef>=SP_MAXCONST || strlen(defs[i])>=SP_NAME){ fprintf(stderr,"cmc: --define %s: at most %d tags of under %d bytes\n",defs[i],SP_MAXCONST,SP_NAME); return -1; }
		snprintf(S.define[S.ndef++],SP_NAME,"%s",defs[i]);
	}
	return 0;
}

static int g_coverage;	/* cmc --spec --coverage */
void cmc_spec_set_coverage(int on){ g_coverage=on; }

/* The --coverage report, after the search: per active action, in TLC's
 * -coverage form "<A>: distinct:generated" (the new states it found,
 * and every successor it produced, a stutter or a seen state included),
 * then how many explored states enable it.  The initial state is Init's
 * one generated state, distinct if it is inside the model.  An action
 * enabled in no explored state is vacuous, and named as a warning.
 * Counts cover the states explored, all of them on a complete run. */
static void print_coverage(int init_new, const size_t *en, const size_t *gen, const size_t *nw){
	printf("The coverage statistics:\n");
	printf("<Init>: %d:1\n",init_new);
	for(int a=0;a<S.nact;a++){
		if(!act_active(&S.act[a])) continue;
		printf("<%s>: %zu:%zu  (enabled in %zu states)\n",S.act[a].name,nw[a],gen[a],en[a]);
	}
	printf("End of statistics.\n");
	for(int a=0;a<S.nact;a++)
		if(act_active(&S.act[a]) && !en[a]) printf("Warning: action %s is never enabled.\n",S.act[a].name);
}

int cmc_spec_main(const char *path, char **defs, int ndefs, int deadlock_off, const char *trace_out, const char *trace_fmt){
	g_trace_out=trace_out; g_trace_fmt=trace_fmt;
	if(set_defines(defs,ndefs) || sp_load(path)) return 3;
	char err[256];
	if(sym_init(err,sizeof err)){ fprintf(stderr,"cmc: %s: %s\n",path,err); return 3; }
	/* the liveness graph (lv_build) knows no constraints; TLC's meaning
	 * for a property under a constraint is not modelled, so refuse it */
	int nactp=0; for(int i=0;i<S.nprop;i++) nactp+=S.prop[i].kind==2;
	if(S.nprop>nactp && (S.ncon || S.nacon)){ fprintf(stderr,"cmc: %s: a liveness property cannot be checked under a constraint or an action constraint\n",path); return 3; }
	/* parse-check every invariant and guard once (over the init state) */
	State init; if(sp_init_state(&init,err,sizeof err)){ fprintf(stderr,"cmc: %s: init: %s\n",path,err); return 3; }

	seen_cap=1; while(seen_cap<SP_MAXSTATES*2) seen_cap<<=1; seen=calloc(seen_cap,sizeof *seen);
	if(!seen){ fprintf(stderr,"cmc: out of memory\n"); return 1; }

	/* the queue holds pointers to individually allocated QN nodes, so a
	 * node's address (used as prev for the counterexample path) stays
	 * stable as the queue grows */
	size_t qcap = 1024, qh = 0, qt = 0;
	QN **q = malloc(qcap * sizeof *q);
	if (!q) { fprintf(stderr, "cmc: out of memory\n"); return 1; }
	QN *root = qn_new(&init);
	size_t distinct=0, generated=1; int maxdepth=0;
	/* --coverage: per action, the states it is enabled in, its
	 * successors (TLC's "generated") and the new states among them
	 * (TLC's "distinct") */
	size_t cov_en[SP_MAXACT]={0}, cov_gen[SP_MAXACT]={0}, cov_new[SP_MAXACT]={0};
	int SUCCAP = 4096; State *succ = malloc((size_t)SUCCAP * sizeof *succ);
	if (!succ) { fprintf(stderr, "cmc: out of memory\n"); free(q); free(root); return 1; }

	printf("cmc: spec %s, %d variable%s, %d action%s, breadth-first\n", S.modname, S.nvar, S.nvar==1?"":"s", S.nact, S.nact==1?"":"s");

	/* As TLC: a state's invariants are checked when it is generated, and
	 * only a state inside the model (its constraints hold, and the action
	 * constraints on the step that reached it) is counted, kept and
	 * expanded.  A state outside the model is still checked.  So a state
	 * that one step reaches outside the model and another inside is
	 * counted once, through the second. */
	int ret = check_new(root), in = 1;
	if(!ret && sp_in_model(NULL,&init,&in,err,sizeof err)){ fprintf(stderr,"cmc: %s\n",err); ret=3; }
	int init_in = !ret && in;
	if(init_in){ seen_insert(&init); q[qt++]=root; distinct=1; }
	else free(root);
	while(qh<qt && !ret){
		QN *cur=q[qh];
		static State cst; qn_state(cur,&cst);
		if(cur->depth>maxdepth) maxdepth=cur->depth;
		/* A state with no successor at all deadlocks.  A step that changes
		 * nothing is a successor, and so is one outside the model. */
		int nsucc=0;
		for(int ai=0; ai<S.nact && !ret; ai++){
			if(!act_active(&S.act[ai])) continue;
			int ns=0; int rc=0; Env env=env_new(NULL);
			bind_params(&S.act[ai],&env,0,&cst,succ,&ns,SUCCAP,err,sizeof err,&rc);
			if(rc<0){ fprintf(stderr,"cmc: action %s: %s\n",S.act[ai].name,err); ret=3; break; }
			nsucc+=ns; cov_gen[ai]+=(size_t)ns; cov_en[ai]+=ns>0;
			for(int k=0;k<ns && !ret;k++){
				generated++;
				if(sp_in_model(&cst,&succ[k],&in,err,sizeof err)){ fprintf(stderr,"cmc: %s\n",err); ret=3; break; }
				int fresh = in && seen_insert(&succ[k]);
				if(fresh && seen_n>=SP_MAXSTATES){ printf("INCOMPLETE: more than %d states\n",SP_MAXSTATES); ret=2; break; }
				if(fresh) cov_new[ai]++;
				if(in && !fresh && !nactp) continue;
				QN *nn=qn_new(&succ[k]); nn->prev=cur; snprintf(nn->step,sizeof nn->step,"%s",S.act[ai].name); nn->depth=cur->depth+1;
				if(fresh || !in) ret=check_new(nn);
				if(!ret) ret=check_step(&cst,&succ[k],nn);
				if(!fresh || ret){ free(nn); continue; }
				if(qt>=qcap){ qcap*=2; QN **nq=realloc(q,qcap*sizeof *q); if(!nq){ fprintf(stderr,"cmc: out of memory\n"); free(nn); ret=1; break; } q=nq; }
				q[qt++]=nn; distinct++;
			}
		}
		if(!ret && nsucc==0 && !(S.deadlock_off || deadlock_off)){
			printf("Error: Deadlock reached.\n");
			ret=print_trace(cur) ? 3 : 1;
		}
		qh++;
	}
	if(ret==0){
		/* safety held; now check any liveness properties on the full graph */
		if(S.nprop>nactp){
			char lerr[256];
			int lr=liveness_check(&init,lerr,sizeof lerr);
			if(lr<0){ fprintf(stderr,"cmc: liveness: %s\n",lerr); ret=3; }
			else ret=lr;
		}
		if(ret==0){
			printf("Model checking completed. No error has been found.\n");
			printf("%zu states generated, %zu distinct states found, 0 states left on queue.\n", generated, distinct);
			printf("The depth of the complete state graph search is %d.\n", maxdepth+1);
			printf("  %d invariant%s, %d propert%s held.\n", S.ninv, S.ninv==1?"":"s", S.nprop, S.nprop==1?"y":"ies");
		}
	}
	if(g_coverage && ret!=3) print_coverage(init_in, cov_en, cov_gen, cov_new);
	for(size_t i=0;i<qt;i++) free(q[i]);
	free(q); free(succ); free(seen);
	return ret;
}

/* =====================================================================
 * refinement support (Milestone C): map an implementation's observables
 * to the abstract variables via a binding, then check the abstract
 * invariants.  The binding gives, per abstract variable, an expression
 * over observables (node[i].field, nodes, replies) in the .cml grammar.
 * A function variable is bound element by element over its domain.
 * ===================================================================== */
static struct { char var[SP_NAME]; char idx[SP_NAME]; char expr[SP_EXPR]; } B_map[SP_MAXVAR];
static int B_nmap;

int cmc_refine_load_spec(const char *path, char **defs, int ndefs){
	if(set_defines(defs,ndefs) || sp_load(path)) return -1;
	/* a constraint narrows TLC's search, not the spec; refinement against
	 * the narrowed model is not modelled */
	if(S.ncon || S.nacon){ fprintf(stderr,"cmc: %s: refinement against a spec with a constraint or an action constraint is not supported\n",path); return -1; }
	return 0;
}

/* the variable a binding binds: 1 a function of integers (bound entry
 * by entry), 0 an integer, -1 a type a binding cannot build */
static int bind_shape(const Var *V){
	const Ty *T=&S.ty[V->ty];
	if(T->kind==TY_DOM) return 0;
	if(T->kind==TY_FUN && S.ty[T->sub].kind==TY_DOM) return 1;
	return -1;
}

/* The binding file: [fields] name = byte offset, then [map] VAR = "EXPR"
 * or VAR[IDX] = "EXPR" per abstract variable, after optional spec= and
 * binary= header lines.  Every line is read or refused: a second binding
 * of a variable that hid the first, or a skipped line, would check the
 * implementation against a map nobody wrote. */
int cmc_refine_load_bind(const char *path, char *err, size_t errcap){
	FILE *f=fopen(path,"r"); if(!f){ snprintf(err,errcap,"cannot open %s",path); return -1; }
	B_nmap=0; S.nfield=0; char line[1024]; int sect=0, ln=0; /* 0=header 1=fields 2=map */
	#define BAD(...) do{ snprintf(err,errcap,__VA_ARGS__); fclose(f); return -1; }while(0)
	#define BCPY(dst,src,what) do{ if((size_t)snprintf((dst),sizeof(dst),"%s",(src))>=sizeof(dst)) BAD("line %d: %s is too long (limit %zu bytes)",ln,(what),sizeof(dst)-1); }while(0)
	while(fgets(line,sizeof line,f)){
		ln++;
		if(!strchr(line,'\n') && !feof(f)) BAD("a line is too long (limit %zu bytes)",sizeof line-2);
		char *h=strchr(line,'#'); if(h)*h=0; char *s=sp_trim(line); if(!*s) continue;
		if(s[0]=='['){
			if(!strcmp(s,"[fields]")) sect=1; else if(!strcmp(s,"[map]")) sect=2;
			else BAD("line %d: unknown section %s (a binding has [fields] and [map])",ln,s);
			continue;
		}
		char *eq=strchr(s,'=');
		if(!eq) BAD("line %d: cannot read %s (want KEY = VALUE)",ln,s);
		*eq=0; char *k=sp_trim(s); char *v=sp_trim(eq+1);
		/* strip quotes around v */
		if(*v=='"'){ v++; char *q=strrchr(v,'"'); if(q)*q=0; }
		if(sect==0){
			/* the header names the spec and binary, for a reader */
			if(strcmp(k,"spec") && strcmp(k,"binary")) BAD("line %d: cannot read %s before [fields] or [map]",ln,k);
		}
		else if(sect==1){
			char *e; long off=strtol(v,&e,10);
			if(S.nfield>=SP_MAXCONST) BAD("too many fields");
			if(!*v || *e || off<0 || off>254) BAD("line %d: the offset of %s must be a number from 0 to 254, not '%s'",ln,k,v);
			BCPY(S.fname[S.nfield],k,"field name"); S.foff[S.nfield]=(int)off; S.nfield++;
		}
		else{
			if(B_nmap>=SP_MAXVAR) BAD("too many bindings");
			/* k is VAR or VAR[idx] */
			char *lb=strchr(k,'['); if(lb){ *lb=0; char *rb=strchr(lb+1,']'); if(rb)*rb=0; BCPY(B_map[B_nmap].idx,sp_trim(lb+1),"binding index"); } else B_map[B_nmap].idx[0]=0;
			BCPY(B_map[B_nmap].var,sp_trim(k),"binding variable"); BCPY(B_map[B_nmap].expr,v,"binding expression");
			int vi=var_index(B_map[B_nmap].var);
			if(vi<0) BAD("line %d: the spec has no variable %s",ln,B_map[B_nmap].var);
			for(int m=0;m<B_nmap;m++) if(!strcmp(B_map[m].var,B_map[B_nmap].var)) BAD("line %d: %s is bound twice",ln,B_map[B_nmap].var);
			if(bind_shape(&S.var[vi])<0) BAD("line %d: %s is a record or sequence, or a function of them, which a binding cannot build",ln,B_map[B_nmap].var);
			if(bind_shape(&S.var[vi])==1 && !B_map[B_nmap].idx[0]) BAD("line %d: %s is a function; bind it as %s[i] = \"...\"",ln,B_map[B_nmap].var,B_map[B_nmap].var);
			if(bind_shape(&S.var[vi])==0 && B_map[B_nmap].idx[0]) BAD("line %d: %s is not a function; bind it as %s = \"...\"",ln,B_map[B_nmap].var,B_map[B_nmap].var);
			B_nmap++;
		}
	}
	#undef BCPY
	#undef BAD
	fclose(f);
	if(B_nmap==0){ snprintf(err,errcap,"binding has no [map]"); return -1; }
	return 0;
}

/* build the abstract state from the implementation's observables via the
 * binding (the common core of C1 and C2).  Returns 0, or -1 with a
 * message in err on an evaluation error. */
static int build_abstract(const CmcObs *obs, State *out, char *err, size_t errcap){
	Value vv[SP_MAXVAR]; for(int i=0;i<S.nvar;i++) vv[i]=vint(0);
	size_t mark=ar_mark(); int rc=-1;
	for(int m=0;m<B_nmap;m++){
		int vi=var_index(B_map[m].var);	/* checked by cmc_refine_load_bind() */
		Var *V=&S.var[vi];
		if(bind_shape(V)==1){
			Env denv=env_new(NULL); Value dom;
			if(!sp_eval_set(S.ty[V->ty].dom,&denv,&dom,err,errcap)) goto out;
			long key[SP_MAPMAX]; Value kid[SP_MAPMAX]; int n=0;
			for(int e=0;e<val_n(&dom) && n<SP_MAPMAX;e++){
				Env env=env_new(vv); env.obs=obs; env.np=1;
				snprintf(env.pname[0],SP_NAME,"%s",B_map[m].idx); env.pval[0]=set_e(&dom,e);
				Value r; if(!sp_eval(B_map[m].expr,&env,&r,err,errcap)) goto out;
				/* an observed predicate maps to 0 or 1, the spec's encoding
				 * of a flag; a set or function cannot be one entry */
				if(r.tag!=T_INT && r.tag!=T_BOOL){ snprintf(err,errcap,"binding %s[%s]: expected an integer, not %s",B_map[m].var,B_map[m].idx,tag_name(r.tag)); goto out; }
				key[n]=set_e(&dom,e); kid[n]=vint(r.i); n++;
			}
			Pst ps={0}; ps.ok=1;
			vv[vi]=mk_comp(&ps,T_FUN,n,key,kid);
			if(!ps.ok){ snprintf(err,errcap,"%s",ps.err); goto out; }
		} else {
			Env env=env_new(vv); env.obs=obs;
			Value r; if(!sp_eval(B_map[m].expr,&env,&r,err,errcap)) goto out;
			if(r.tag!=T_INT && r.tag!=T_BOOL){ snprintf(err,errcap,"binding %s: expected an integer, not %s",B_map[m].var,tag_name(r.tag)); goto out; }
			vv[vi]=vint(r.i);	/* a flag, as in a function entry */
		}
	}
	/* A bound variable outside its declared domain is no state of the
	 * spec, whatever the invariant says there (the TLA+ type invariant
	 * fails on it).  An unbound one is not observed (C1 binds only what
	 * the invariant reads).  It is an error, not a verdict: cmc_proc.c's
	 * state-level caller reads only an invariant index or -2. */
	for(int m=0;m<B_nmap;m++){
		int vi=var_index(B_map[m].var); char derr[200];
		if(var_in_domain(vi,&vv[vi],derr,sizeof derr)){ snprintf(err,errcap,"the observed state is outside the spec: %s",derr); goto out; }
	}
	rc=st_pack(out,vv,err,errcap);
out:	ar_release(mark);
	return rc;
}

int cmc_refine_check(const CmcObs *obs, const char **name, char *err, size_t errcap){
	/* build the abstract state: each variable's Value from its binding */
	State st; if(build_abstract(obs,&st,err,errcap)) return -2;
	/* check the abstract invariants on the built state */
	int bad=sp_check(&st,err,errcap);
	if(bad==-2) return -2;
	if(bad>=0){ *name=S.inv[bad].name; return bad; }
	return -1;
}

/* ---------------------------------------------------------------------
 * C2: action-level refinement with stuttering.  C1 checks that the
 * abstraction of every reached implementation state satisfies the
 * abstract invariant; it cannot see a transition the spec forbids as
 * long as both endpoints are good states.  C2 adds the step check: each
 * implementation step s -> s' must map, under the binding, to either a
 * STUTTER (alpha(s) == alpha(s')) or a real abstract step (some abstract
 * action, under some parameter binding, takes alpha(s) to exactly
 * alpha(s')).  It reuses the SAME successor enumeration (bind_params)
 * and the SAME canonical fingerprint (state_fp) the --spec BFS uses and
 * TLC calibrates, so a match is an exact, calibrated abstract successor,
 * never an approximation.
 * ------------------------------------------------------------------- */
static State C2_prev;		/* abstraction of the previous impl state */
static int   C2_have;		/* is C2_prev valid (i.e. not the path's first state)? */

void cmc_refine_reset(void){ C2_have=0; }

/* C2 matches implementation steps against abstract action steps, which
 * needs the WHOLE abstract state -- so every spec variable must have a
 * binding.  (C1 needs only the invariant-relevant variables.)  Returns
 * the name of the first unbound variable, or NULL if all are bound. */
static const char *first_unbound_var(void){
	for(int vi=0; vi<S.nvar; vi++){
		int bound=0;
		for(int m=0;m<B_nmap && !bound;m++) if(var_index(B_map[m].var)==vi) bound=1;
		if(!bound) return S.var[vi].name;
	}
	return NULL;
}

/* does some active abstract action step take `from` to exactly `to`?  An
 * action gated out by a [TAG] that was not --define'd is not part of the
 * transition relation, as in the search and the TLA+ emitter. */
static int abstract_step_exists(const State *from, const State *to, char *err, size_t errcap){
	enum { C2_SUCC = 4096 };
	State *succ = malloc((size_t)C2_SUCC * sizeof *succ);
	if(!succ){ snprintf(err,errcap,"out of memory"); return -2; }
	int found=0;
	for(int ai=0; ai<S.nact && !found; ai++){
		if(!act_active(&S.act[ai])) continue;
		int ns=0, rc=0; Env env=env_new(NULL);
		bind_params(&S.act[ai],&env,0,from,succ,&ns,C2_SUCC,err,errcap,&rc);
		if(rc<0){ free(succ); return -2; }
		for(int k=0;k<ns;k++) if(state_eq(&succ[k],to)){ found=1; break; }
	}
	free(succ);
	return found;
}

/* C2 step check at one reached implementation state.  Returns -1 if the
 * step (and the invariant) are fine, >=0 (an invariant index, name in
 * *name) if an invariant is violated, -3 if the step matches no abstract
 * action and is not a stutter (*name set to a short reason), -2 on an
 * evaluation error (message in err). */
int cmc_refine_step(const CmcObs *obs, const char **name, char *err, size_t errcap){
	if(!C2_have){
		/* action-level matching needs every variable observable */
		const char *u=first_unbound_var();
		if(u){ snprintf(err,errcap,"--refine-steps needs every spec variable bound; '%s' has no [map] entry (map it, or use --refine for the state-level check)",u); return -2; }
	}
	State cur; if(build_abstract(obs,&cur,err,errcap)) return -2;
	/* C2 subsumes C1: the invariant must hold at every state */
	int bad=sp_check(&cur,err,errcap);
	if(bad==-2) return -2;
	if(bad>=0){ *name=S.inv[bad].name; return bad; }
	if(C2_have){
		if(!state_eq(&C2_prev,&cur)){	/* not a stutter */
			int e=abstract_step_exists(&C2_prev,&cur,err,errcap);
			if(e==-2) return -2;
			if(e==0){ *name="no matching abstract action"; C2_prev=cur; C2_have=1; return -3; }
		}
	} else {
		/* the first observation of a behaviour must be an initial state */
		State init; if(sp_init_state(&init,err,errcap)) return -2;
		if(!state_eq(&init,&cur)){ *name="the first state is not the spec's init"; return -3; }
	}
	C2_prev=cur; C2_have=1;
	return -1;
}

/* =====================================================================
 * Milestone D: translate a .cml module to TLA+, so the abstract model
 * reaches TLC, Apalache and TLAPS.  The .cml expression syntax is close
 * to TLA+; this is a token-level rewrite of the operators, emitted into
 * a standard module.  cmc --emit-tla FILE.cml writes to stdout.
 * ===================================================================== */

/* the TLA+ spelling of the .cml keyword w[0 .. n), or NULL when the
 * word is a name and is written as it is */
static const char *tla_kw(const char *w, size_t n){
	static const char *kw[][2]={{"all","\\A"},{"some","\\E"},{"not","~"},{"and","/\\"},
		{"or","\\/"},{"in","\\in"},{"mod","%"},{"except","EXCEPT"}};
	for(size_t i=0;i<sizeof kw/sizeof kw[0];i++) if(strlen(kw[i][0])==n && !strncmp(w,kw[i][0],n)) return kw[i][1];
	return NULL;
}
static int tla_idch(char c){ return isalnum((unsigned char)c) || c=='_'; }
/* Translate the .cml expression e[0 .. n) (n < 0: to its end) to TLA+
 * on o: a token-level rewrite, recursing into a count body so that every
 * operator there is translated as it is outside.  A keyword is only a
 * whole word: the name votedFor keeps its `or`. */
static void tla_expr_n(const char *e, long n, FILE *o){
	const char *p=e, *end=n<0?e+strlen(e):e+n;
	while(p<end){
		if(isspace((unsigned char)*p)){ fputc(*p++,o); continue; }
		if(tla_idch(*p)){
			const char *w=p; while(p<end && tla_idch(*p)) p++;
			size_t wn=(size_t)(p-w);
			if(wn==5 && !strncmp(w,"count",5)){ fputs("Cardinality({",o);
				while(p<end&&isspace((unsigned char)*p))p++;
				while(p<end&&tla_idch(*p)) fputc(*p++,o);
				fputs(" \\in ",o); while(p<end&&isspace((unsigned char)*p))p++;
				if(!strncmp(p,"in",2)) p+=2;
				while(p<end&&isspace((unsigned char)*p))p++;
				const char *set=p; while(p<end && *p!=':') p++;
				tla_expr_n(set,p-set,o);
				if(p<end && *p==':'){ p++; fputs(" : ",o); }
				/* the body runs to the bracket that encloses the count,
				 * or a comma beside it, as the parser reads it */
				const char *body=p; int depth=0;
				while(p<end){
					if(*p=='('||*p=='['||*p=='{') depth++;
					else if(*p==')'||*p==']'||*p=='}'){ if(depth==0) break; depth--; }
					else if(*p==',' && depth==0) break;
					p++;
				}
				tla_expr_n(body,p-body,o);
				fputs("})",o); continue; }
			const char *r=tla_kw(w,wn);
			if(r) fputs(r,o); else fwrite(w,1,wn,o);
			continue;
		}
		if(!strncmp(p,"|->",3)){ fputs("|->",o); p+=3; continue; }
		if(!strncmp(p,"->",2)){ fputs("|->",o); p+=2; continue; }
		if(*p=='/' && p[1]!='\\'){ fputs("\\div ",o); p++; continue; }
		if(!strncmp(p,"==",2)){ fputc('=',o); p+=2; continue; }
		if(!strncmp(p,"!=",2)){ fputs("#",o); p+=2; continue; }
		fputc(*p++,o);
	}
}
static void tla_expr(const char *e, FILE *o){ tla_expr_n(e,-1,o); }

/* some .cml forms need structural, not token, rewriting:
 *   [i in S -> E]         -> [i \in S |-> E]   (handled: in-> \in, -> |->)
 *   [f except [k]=v, .a=w] -> [f EXCEPT ![k]=v, !.a=w]
 * The second pass gives each except clause its `!`: after EXCEPT and
 * after each comma at that except's own bracket depth, unless the
 * clause already starts with one.  <<..>> counts as a bracket, so the
 * comma of a sequence literal inside a clause is not a clause break. */
static void tla_expr_fixup(const char *e, FILE *o){
	/* first pass into a buffer */
	char buf[4096]; FILE *m=fmemopen(buf,sizeof buf,"w"); if(!m){ tla_expr(e,o); return; }
	tla_expr(e,m); fclose(m);
	int depth=0, ex[16], nex=0;
	const char *p=buf;
	while(*p){
		int clause=0;
		if(!strncmp(p,"EXCEPT ",7)){ fputs("EXCEPT",o); p+=6; if(nex<16) ex[nex++]=depth; clause=1; }
		else if(*p==',' && nex && ex[nex-1]==depth){ fputc(*p++,o); clause=1; }
		if(clause){
			while(*p && isspace((unsigned char)*p)) fputc(*p++,o);
			if(*p!='!') fputc('!',o);
			continue;
		}
		if(!strncmp(p,"<<",2) || !strncmp(p,">>",2)){ depth+=*p=='<'?1:-1; fputc(*p++,o); fputc(*p++,o); continue; }
		if(*p=='['||*p=='('||*p=='{') depth++;
		if(*p==']'||*p==')'||*p=='}'){ depth--; if(nex && ex[nex-1]>depth) nex--; }
		fputc(*p++,o);
	}
}

/* the Apalache type of Ty node ti: Int, Int -> T, { a: T, ... }, Seq(T) */
static void ty_tla(int ti, FILE *o){
	const Ty *T=&S.ty[ti];
	if(T->kind==TY_DOM){ fputs("Int",o); return; }
	if(T->kind==TY_FUN){ fputs("Int -> ",o); int b=S.ty[T->sub].kind==TY_FUN; if(b) fputc('(',o); ty_tla(T->sub,o); if(b) fputc(')',o); return; }
	if(T->kind==TY_SEQ){ fputs("Seq(",o); ty_tla(T->sub,o); fputc(')',o); return; }
	fputs("{ ",o);
	for(int j=0;j<T->nf;j++){ fprintf(o,"%s%s: ",j?", ":"",S.fld[T->fid[j]]); ty_tla(T->fty[j],o); }
	fputs(" }",o);
}

int cmc_emit_tla(const char *path, char **defs, int ndefs, int deadlock_off){
	if(set_defines(defs,ndefs) || sp_load(path)) return 3;
	FILE *o=stdout;
	fprintf(o,"---- MODULE %s ----\n",S.modname);
	fprintf(o,"\\* Generated from %s by cmc --emit-tla.  Run with TLC,\n",path);
	fprintf(o,"\\* Apalache or TLAPS; a .cfg binds the constants for TLC.  The\n");
	fprintf(o,"\\* @type annotations and the CInit operator let Apalache check it\n");
	fprintf(o,"\\* directly: apalache-mc check --cinit=CInit --inv=INV MODULE.tla\n");
	if(S.deadlock_off || deadlock_off){
		fprintf(o,"\\* The deadlock check is off for this model, so add --no-deadlock,\n");
		fprintf(o,"\\* as the .cfg's CHECK_DEADLOCK FALSE does for TLC.\n");
	}
	if(S.ncon || S.nacon){
		fprintf(o,"\\* Its CONSTRAINT and ACTION_CONSTRAINT operators bound TLC's search\n");
		fprintf(o,"\\* (cmc --emit-cfg names them); Apalache does not apply them.\n");
	}
	fprintf(o,"EXTENDS Naturals, FiniteSets, Sequences\n\n");
	/* constants, each with an Apalache @type (all our constants are Int) */
	if(S.ncst){ fprintf(o,"CONSTANTS\n"); for(int i=0;i<S.ncst;i++) fprintf(o,"  \\* @type: Int;\n  %s%s\n",S.cst[i].name,i+1<S.ncst?",":""); }
	/* variables, each with an Apalache @type: a scalar var is Int, a
	 * function var [lo..hi -> lo..hi] is Int -> Int */
	fprintf(o,"VARIABLES\n"); for(int i=0;i<S.nvar;i++){ fprintf(o,"  \\* @type: "); ty_tla(S.var[i].ty,o); fprintf(o,";\n  %s%s\n",S.var[i].name,i+1<S.nvar?",":""); }
	/* vars tuple, annotated so Apalache reads <<...>> as a tuple not a Seq */
	fprintf(o,"\\* @type: <<"); for(int i=0;i<S.nvar;i++){ fprintf(o,"%s",i?", ":""); ty_tla(S.var[i].ty,o); } fprintf(o,">>;\n");
	fprintf(o,"vars == <<"); for(int i=0;i<S.nvar;i++) fprintf(o,"%s%s",i?", ":"",S.var[i].name); fprintf(o,">>\n\n");
	/* CInit binds the constants to their .cml values, so Apalache needs no
	 * .cfg (TLC still reads the .cfg) */
	if(S.ncst){ fprintf(o,"CInit ==\n"); for(int i=0;i<S.ncst;i++) fprintf(o,"  %s %s = %ld\n",i?"/\\":"/\\",S.cst[i].name,S.cst[i].val); fprintf(o,"\n"); }
	/* each def as an operator, in declaration order (a def uses only the
	 * ones before it, as TLA+ requires) */
	for(int d=0;d<S.ndefs;d++){ const Def *D=&S.def[d];
		fprintf(o,"%s",D->name);
		for(int i=0;i<D->np;i++) fprintf(o,"%s%s",i?", ":"(",D->pname[i]);
		fprintf(o,"%s ==\n  ",D->np?")":""); tla_expr_fixup(D->body,o); fprintf(o,"\n\n"); }
	/* Init */
	fprintf(o,"Init ==\n  "); tla_expr_fixup(S.init,o); fprintf(o,"\n\n");
	/* each action as a formula: \E params \in dom : guard /\ assignments /\ UNCHANGED rest */
	for(int a=0;a<S.nact;a++){ Action *A=&S.act[a];
		if(!act_active(A)) continue;	/* a [TAG] action not enabled by --define is omitted */
		fprintf(o,"%s ==\n",A->name);
		for(int pi=0;pi<A->nparam;pi++){ fprintf(o,"  \\E %s \\in ",A->pname[pi]); tla_expr_fixup(A->pdom[pi],o); fprintf(o," :\n"); }
		fprintf(o,"    /\\ "); if(A->guard[0]) tla_expr_fixup(A->guard,o); else fprintf(o,"TRUE"); fprintf(o,"\n");
		/* which vars are assigned */
		int asgn[SP_MAXVAR]={0};
		for(int i=0;i<A->nasgn;i++){ int vi=var_index(A->lhs[i]); if(vi>=0) asgn[vi]=1;
			fprintf(o,"    /\\ %s' = ",A->lhs[i]); tla_expr_fixup(A->rhs[i],o); fprintf(o,"\n"); }
		/* UNCHANGED the rest */
		int any=0; for(int i=0;i<S.nvar;i++) if(!asgn[i]) any=1;
		if(any){ fprintf(o,"    /\\ UNCHANGED <<"); int first=1; for(int i=0;i<S.nvar;i++) if(!asgn[i]){ fprintf(o,"%s%s",first?"":", ",S.var[i].name); first=0; } fprintf(o,">>\n"); }
		fprintf(o,"\n");
	}
	/* Next = disjunction of actions */
	fprintf(o,"Next ==\n"); { int any=0; for(int a=0;a<S.nact;a++){ if(!act_active(&S.act[a])) continue; fprintf(o,"  \\/ %s\n",S.act[a].name); any=1; } if(!any) fprintf(o,"  FALSE\n"); } fprintf(o,"\n");
	fprintf(o,"Spec == Init /\\ [][Next]_vars");
	for(int a=0;a<S.nact;a++){ int k=act_fair(a); if(k) fprintf(o," /\\ %s_vars(%s)",k==2?"SF":"WF",S.act[a].name); }
	fprintf(o,"\n\n");
	/* invariants */
	for(int i=0;i<S.ninv;i++){ fprintf(o,"%s ==\n  ",S.inv[i].name); tla_expr_fixup(S.inv[i].expr,o); fprintf(o,"\n"); }
	/* state and action constraints, as TLC's CONSTRAINT and ACTION_CONSTRAINT */
	for(int i=0;i<S.ncon;i++){ fprintf(o,"%s ==\n  ",S.con[i].name); tla_expr_fixup(S.con[i].expr,o); fprintf(o,"\n"); }
	for(int i=0;i<S.nacon;i++){ fprintf(o,"%s ==\n  ",S.acon[i].name); tla_expr_fixup(S.acon[i].expr,o); fprintf(o,"\n"); }
	/* liveness properties */
	for(int i=0;i<S.nprop;i++){ SpProp *P=&S.prop[i]; fprintf(o,"%s ==\n  ",P->name);
		if(P->kind==0){ fprintf(o,"<>("); tla_expr_fixup(P->a,o); fprintf(o,")"); }
		else if(P->kind==2){ fprintf(o,"[][("); tla_expr_fixup(P->a,o); fprintf(o,")]_vars"); }
		else { fprintf(o,"("); tla_expr_fixup(P->a,o); fprintf(o,") ~> ("); tla_expr_fixup(P->b,o); fprintf(o,")"); }
		fprintf(o,"\n"); }
	fprintf(o,"====\n");
	return 0;
}

/* Write the TLC configuration for the module cmc_emit_tla() writes: the
 * constants at their .cml values, every invariant, property, state and
 * action constraint by name, and CHECK_DEADLOCK FALSE when the deadlock
 * check is off.  Returns 0 ok, 3 on a model error. */
int cmc_emit_cfg(const char *path, char **defs, int ndefs, int deadlock_off){
	if(set_defines(defs,ndefs) || sp_load(path)) return 3;
	printf("SPECIFICATION Spec\n");
	for(int i=0;i<S.ncst;i++) printf("CONSTANT %s = %ld\n",S.cst[i].name,S.cst[i].val);
	for(int i=0;i<S.ninv;i++) printf("INVARIANT %s\n",S.inv[i].name);
	for(int i=0;i<S.nprop;i++) printf("PROPERTY %s\n",S.prop[i].name);
	for(int i=0;i<S.ncon;i++) printf("CONSTRAINT %s\n",S.con[i].name);
	for(int i=0;i<S.nacon;i++) printf("ACTION_CONSTRAINT %s\n",S.acon[i].name);
	if(S.deadlock_off || deadlock_off) printf("CHECK_DEADLOCK FALSE\n");
	return 0;
}
