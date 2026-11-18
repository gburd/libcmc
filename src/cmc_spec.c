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
 *
 * Values are bounded integers, booleans, finite sets of integers, and
 * finite maps from integers to integers (functions/records).  A world is
 * the tuple of variable values, canonicalised so equal states hash
 * equal; the search is breadth-first, so a violation is reported with
 * the shortest action sequence, in TLC's wording.
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

/* ---- the value model ------------------------------------------------ */
enum { T_INT, T_BOOL, T_SET, T_FUN };
typedef struct {
	int tag;
	long i;						/* T_INT / T_BOOL (0/1) */
	int n;						/* T_SET: count; T_FUN: entry count */
	long e[SP_SETMAX];			/* T_SET: sorted, unique elements */
	long k[SP_MAPMAX], v[SP_MAPMAX];	/* T_FUN: keys (sorted) -> values */
} Value;

static Value vint(long x){ Value v={0}; v.tag=T_INT; v.i=x; return v; }
static Value vbool(int b){ Value v={0}; v.tag=T_BOOL; v.i=b?1:0; return v; }

/* canonical order so equal sets/maps compare and hash equal */
static int lcmp(const void *a, const void *b){ long x=*(const long*)a,y=*(const long*)b; return x<y?-1:x>y?1:0; }
static void set_canon(Value *s){
	/* sort, then drop duplicates */
	qsort(s->e, (size_t)s->n, sizeof s->e[0], lcmp);
	int w=0; for(int r=0;r<s->n;r++){ if(w==0||s->e[r]!=s->e[w-1]) s->e[w++]=s->e[r]; } s->n=w;
}
/* equality of two values of one type (same_type() checks that first) */
static int val_eq(const Value *a, const Value *b){
	if(a->tag==T_INT||a->tag==T_BOOL) return a->i==b->i;
	if(a->tag==T_SET){ if(a->n!=b->n) return 0; for(int i=0;i<a->n;i++) if(a->e[i]!=b->e[i]) return 0; return 1; }
	/* T_FUN */ if(a->n!=b->n) return 0; for(int i=0;i<a->n;i++) if(a->k[i]!=b->k[i]||a->v[i]!=b->v[i]) return 0; return 1;
}

/* ---- the model ------------------------------------------------------ */
typedef struct { char name[SP_NAME]; long val; } Const;
typedef struct { char name[SP_NAME]; char dom[SP_EXPR]; int is_fun; char fdom[SP_EXPR]; } Var;
typedef struct {
	char name[SP_NAME];
	int nparam; char pname[SP_MAXPARAM][SP_NAME]; char pdom[SP_MAXPARAM][SP_EXPR];
	char guard[SP_EXPR];
	int nasgn; char lhs[SP_MAXVAR][SP_NAME]; char rhs[SP_MAXVAR][SP_EXPR];
	char tag[SP_NAME];			/* a [TAG] gate: action active only if -D TAG */
} Action;
typedef struct { char name[SP_NAME]; char expr[SP_EXPR]; } SpInv;
typedef struct { char name[SP_NAME]; int kind; char a[SP_EXPR]; char b[SP_EXPR]; } SpProp;	/* kind: 0 eventually(a), 1 a~>b */

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
	int fair_weak;						/* all actions weakly fair */
} S;

#include "cmc_spec.h"

/* ---- evaluation context: constants + variable values + bound params - */
typedef struct {
	const Value *var;			/* current value of each variable, by index */
	char pname[SP_MAXPARAM][SP_NAME]; long pval[SP_MAXPARAM]; int np;	/* bound params */
	const CmcObs *obs;			/* refinement: observable accessor, or NULL */
	const int *assigned;			/* init: which vars are set so far, or NULL (all) */
} Env;

static long cst_lookup(const char *name, int *found){
	for(int i=0;i<S.ncst;i++) if(!strcmp(S.cst[i].name,name)){ *found=1; return S.cst[i].val; }
	*found=0; return 0;
}
static int var_index(const char *name){ for(int i=0;i<S.nvar;i++) if(!strcmp(S.var[i].name,name)) return i; return -1; }

/* =====================================================================
 * expression evaluator over Value.  Grammar (low -> high precedence):
 *   or  ->  and  ->  not  ->  cmp  ->  add  ->  mul  ->  atom
 * plus quantifiers  (all|some|count) V in SET : EXPR  and set/fun forms.
 * Returns a Value; sets *ok=0 on an evaluation/parse error.
 * ===================================================================== */
typedef struct { const char *p; Env *env; int ok; char err[128]; } Pst;

static void sp_skip(Pst *s){ while(*s->p && isspace((unsigned char)*s->p)) s->p++; }
static int sp_word(Pst *s, const char *w){
	sp_skip(s); size_t n=strlen(w);
	if(!strncmp(s->p,w,n) && !isalnum((unsigned char)s->p[n]) && s->p[n]!='_'){ s->p+=n; return 1; }
	return 0;
}
static Value sp_or(Pst *s);
static void fail(Pst *s, const char *m){ if(s->ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",m); } }

static const char *tag_name(int t){ return t==T_INT?"an integer":t==T_BOOL?"a boolean":t==T_SET?"a set":"a function"; }

/* An operand of the wrong type is a model error, as TLC reports it.
 * Read as its integer field instead, `not votes` held in every state and
 * `x < {1}` compared x with 0.  need() returns 1 when v has type tag, and
 * otherwise fails s, naming where the value was used; same_type() does
 * the same for the two sides of == and != (TLC refuses TRUE = 1). */
static int need(Pst *s, const Value *v, int tag, const char *where){
	if(v->tag==tag) return 1;
	if(s->ok){ s->ok=0; snprintf(s->err,sizeof s->err,"expected %s in %s, not %s",tag_name(tag),where,tag_name(v->tag)); }
	return 0;
}
static int same_type(Pst *s, const Value *a, const Value *b, const char *op){
	if(a->tag==b->tag) return 1;
	if(s->ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s compares %s with %s",op,tag_name(a->tag),tag_name(b->tag)); }
	return 0;
}

/* read an identifier */
static int sp_ident(Pst *s, char *out, int cap){
	sp_skip(s); int n=0;
	while((isalnum((unsigned char)*s->p)||*s->p=='_') && n<cap-1) out[n++]=*s->p++;
	out[n]=0; return n;
}

/* a SET value literal or range: {a,b,c} or LO .. HI */
static Value sp_setspec(Pst *s);
static int env_bind(Pst *s, Env *ne, const char *name, long val);

/* atom: integer, TRUE/FALSE, const, variable, param, x', fun app f[k],
 * fun literal [i in SET -> EXPR], [f except [k]=v], ( EXPR ), set literal */
static Value sp_atom(Pst *s){
	sp_skip(s);
	Value z={0};
	if(*s->p=='('){ s->p++; Value v=sp_or(s); sp_skip(s); if(*s->p!=')'){ fail(s,"expected )"); return z; } s->p++; return v; }
	if(*s->p=='['){
		/* function literal [i in SET -> EXPR] or update [f except [k]=v,...] */
		s->p++; sp_skip(s);
		const char *save=s->p;
		char id[SP_NAME]; sp_ident(s,id,sizeof id);
		if(sp_word(s,"in")){
			/* [i in SET -> EXPR] : build the map */
			Value dom=sp_setspec(s); if(!s->ok) return z;
			sp_skip(s); if(!(s->p[0]=='-'&&s->p[1]=='>')){ fail(s,"expected -> in function literal"); return z; } s->p+=2;
			const char *body=s->p;
			Value f={0}; f.tag=T_FUN; f.n=0;
			for(int e=0;e<dom.n;e++){
				if(f.n>=SP_MAPMAX){ fail(s,"function too large"); return z; }
				/* bind id=dom.e[e], evaluate body */
				Pst sub=*s; sub.p=body;
				/* temporary param binding */
				Env ne=*s->env; if(!env_bind(s,&ne,id,dom.e[e])) return z; sub.env=&ne;
				Value bv=sp_or(&sub); need(&sub,&bv,T_INT,"a function value");
				if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); return z; }
				f.k[f.n]=dom.e[e]; f.v[f.n]=bv.i; f.n++;
				s->p=sub.p;
			}
			sp_skip(s); if(*s->p!=']'){ fail(s,"expected ]"); return z; } s->p++;
			return f;
		}
		/* [f except [k]=v, ...] : id is the function name */
		s->p=save; char fn[SP_NAME]; sp_ident(s,fn,sizeof fn);
		if(!sp_word(s,"except")){ fail(s,"expected 'in' or 'except' in [..]"); return z; }
		int fi=var_index(fn);
		Value f;
		if(fi>=0) f=s->env->var[fi]; else { fail(s,"except on a non-variable"); return z; }
		if(f.tag!=T_FUN){ fail(s,"except on a variable that is not a function"); return z; }
		for(;;){
			sp_skip(s); if(*s->p!='['){ fail(s,"expected [ in except"); return z; } s->p++;
			Value kk=sp_or(s); if(!s->ok || !need(s,&kk,T_INT,"a function key")) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ]"); return z; } s->p++;
			sp_skip(s); if(*s->p!='='){ fail(s,"expected = in except"); return z; } s->p++;
			Value vv=sp_or(s); if(!s->ok || !need(s,&vv,T_INT,"a function value")) return z;
			int hit=0; for(int i=0;i<f.n;i++) if(f.k[i]==kk.i){ f.v[i]=vv.i; hit=1; break; }
			if(!hit){ if(f.n>=SP_MAPMAX){ fail(s,"function too large"); return z; } f.k[f.n]=kk.i; f.v[f.n]=vv.i; f.n++; }
			sp_skip(s); if(*s->p==','){ s->p++; continue; } break;
		}
		sp_skip(s); if(*s->p!=']'){ fail(s,"expected ] to close except"); return z; } s->p++;
		return f;
	}
	if(*s->p=='{'){
		/* set literal {a, b, ...} (possibly empty) */
		s->p++; Value set={0}; set.tag=T_SET; set.n=0; sp_skip(s);
		if(*s->p!='}'){
			for(;;){ Value ev=sp_or(s); if(!s->ok || !need(s,&ev,T_INT,"a set")) return z; if(set.n>=SP_SETMAX){ fail(s,"set too large"); return z; } set.e[set.n++]=ev.i; sp_skip(s); if(*s->p==','){ s->p++; continue; } break; }
		}
		sp_skip(s); if(*s->p!='}'){ fail(s,"expected }"); return z; } s->p++;
		set_canon(&set); return set;
	}
	if(isdigit((unsigned char)*s->p)){
		errno=0; long x=strtol(s->p,(char**)&s->p,10);
		if(errno==ERANGE){ fail(s,"integer literal overflows a long"); return z; }
		return vint(x); }
	/* identifier-led: keywords, const, var, param, fun application */
	const char *save=s->p; char id[SP_NAME]; int n=sp_ident(s,id,sizeof id);
	if(n==0){ fail(s,"expected an expression"); return z; }
	if(!strcmp(id,"TRUE")) return vbool(1);
	if(!strcmp(id,"FALSE")) return vbool(0);
	/* refinement observables (only when an obs accessor is present) */
	if(s->env->obs){
		const CmcObs *o=s->env->obs;
		if(!strcmp(id,"nodes")){ Value set={0}; set.tag=T_SET; for(int i=0;i<o->nnodes && set.n<SP_SETMAX;i++) set.e[set.n++]=i; set_canon(&set); return set; }
		if(!strcmp(id,"replies")){ Value set={0}; set.tag=T_SET; for(int r=0;r<o->nrepl && set.n<SP_SETMAX;r++) set.e[set.n++]=r; set_canon(&set); return set; }
		if(!strcmp(id,"node")){
			sp_skip(s); if(*s->p!='['){ fail(s,"expected [ after node"); return z; } s->p++;
			Value iv=sp_or(s); if(!s->ok || !need(s,&iv,T_INT,"node[]")) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ]"); return z; } s->p++;
			int i=(int)iv.i; long k=-1;
			sp_skip(s);
			if(*s->p=='['){ s->p++; Value kv=sp_or(s); if(!s->ok || !need(s,&kv,T_INT,"node[][]")) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ]"); return z; } s->p++; k=kv.i; }
			else if(*s->p=='.'){ s->p++; char fn[SP_NAME]; sp_ident(s,fn,sizeof fn); int fi=-1; for(int j=0;j<S.nfield;j++) if(!strcmp(S.fname[j],fn)) fi=j; if(fi<0){ fail(s,"unknown field"); return z; } k=S.foff[fi]; }
			else { fail(s,"node[i] needs [k] or .field"); return z; }
			return vint(o->node_byte(o->ctx,i,(int)k));
		}
		if(!strcmp(id,"r")){
			/* r.src / r.val when ranging over replies (param r bound to index) */
			if(*s->p=='.'){ s->p++; char fn[SP_NAME]; sp_ident(s,fn,sizeof fn);
				long ri=0; for(int i=0;i<s->env->np;i++) if(!strcmp("r",s->env->pname[i])) ri=s->env->pval[i];
				if(!strcmp(fn,"src")) return vint(o->repl_src(o->ctx,(int)ri));
				if(!strcmp(fn,"val")) return vint(o->repl_val(o->ctx,(int)ri));
				fail(s,"reply has .src and .val"); return z; }
		}
	}
	/* bound parameter? */
	for(int i=0;i<s->env->np;i++) if(!strcmp(id,s->env->pname[i])) return vint(s->env->pval[i]);
	/* constant? */
	{ int f; long c=cst_lookup(id,&f); if(f) return vint(c); }
	/* variable, possibly primed, possibly applied f[k].  A primed
	 * variable has no next-state value here: guards, right-hand sides and
	 * invariants are all evaluated against the current state, so reading
	 * x' as x would silently change the spec's meaning (and the emitted
	 * TLA+ keeps the prime).  Refuse it. */
	if(*s->p=='\''){ fail(s,"a primed variable can only be assigned (x' = E), not read"); return z; }
	int vi=var_index(id);
	if(vi>=0){
		if(s->env->assigned && !s->env->assigned[vi]){ fail(s,"init reads a variable before assigning it"); return z; }
		Value base = s->env->var[vi];
		sp_skip(s);
		if(*s->p=='['){ s->p++; Value kk=sp_or(s); if(!s->ok || !need(s,&kk,T_INT,"a function key")) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ] in application"); return z; } s->p++;
			if(base.tag!=T_FUN){ fail(s,"applied a non-function"); return z; }
			for(int i=0;i<base.n;i++) if(base.k[i]==kk.i) return vint(base.v[i]);
			fail(s,"function applied outside its domain"); return z; }
		return base;
	}
	(void)save; fail(s,"unknown identifier"); return z;
}

/* a set spec used in domains and quantifiers: {..} literal or LO .. HI */
static Value sp_add(Pst *s);

/* env with one more bound name, or an error when SP_MAXPARAM names are
 * already bound (silently skipping the binding would let the body read
 * an outer value of the same name) */
static int env_bind(Pst *s, Env *ne, const char *name, long val){
	if(ne->np>=SP_MAXPARAM){ fail(s,"too many nested bound names"); return 0; }
	/* TLA+ forbids redeclaring a name in scope (SANY rejects it); here an
	 * inner binding would be shadowed by the outer one, not the reverse */
	int f; (void)cst_lookup(name,&f);
	for(int i=0;i<ne->np;i++) if(!strcmp(ne->pname[i],name)) f=1;
	if(f || var_index(name)>=0){ fail(s,"a bound name redeclares a name already in scope"); return 0; }
	snprintf(ne->pname[ne->np],SP_NAME,"%s",name); ne->pval[ne->np]=val; ne->np++;
	return 1;
}

static Value sp_setspec(Pst *s){
	sp_skip(s);
	if(*s->p=='{') return sp_atom(s);
	/* LO .. HI (inclusive); bounds are arithmetic expressions */
	Value lo=sp_add(s); if(!s->ok) return lo; sp_skip(s);
	if(s->p[0]=='.'&&s->p[1]=='.'){ s->p+=2; Value hi=sp_add(s); if(!s->ok) return hi;
		if(!need(s,&lo,T_INT,"..") || !need(s,&hi,T_INT,"..")) return lo;
		Value set={0}; set.tag=T_SET; for(long x=lo.i;x<=hi.i;x++){ if(set.n>=SP_SETMAX){ fail(s,"range too large"); return set; } set.e[set.n++]=x; } set_canon(&set); return set; }
	/* a bare set-valued expression (a variable of set type) */
	if(s->ok && lo.tag!=T_SET){ fail(s,"expected a set (LO .. HI or {..}), not a scalar"); return lo; }
	return lo;
}

/* arithmetic is checked: an overflow is a model error, never a wrapped
 * value (signed overflow is also undefined in C).  `/` and `mod` are
 * refused on negative operands, where C's truncating division differs
 * from TLA+'s \div and %, so the emitted module would mean something
 * else. */
static Value sp_mul(Pst *s){
	Value a=sp_atom(s);
	for(;;){ sp_skip(s);
		if(*s->p=='*'){ s->p++; Value b=sp_atom(s); long r; if(!need(s,&a,T_INT,"*") || !need(s,&b,T_INT,"*")) return a; if(__builtin_mul_overflow(a.i,b.i,&r)){ fail(s,"integer overflow in *"); return a; } a=vint(r); }
		else if(*s->p=='/'){ s->p++; Value b=sp_atom(s); if(!need(s,&a,T_INT,"/") || !need(s,&b,T_INT,"/")) return a; if(b.i==0){ fail(s,"divide by zero"); return a; } if(a.i<0||b.i<0){ fail(s,"/ of a negative number is not supported"); return a; } a=vint(a.i/b.i); }
		else if(sp_word(s,"mod")){ Value b=sp_atom(s); if(!need(s,&a,T_INT,"mod") || !need(s,&b,T_INT,"mod")) return a; if(b.i==0){ fail(s,"mod by zero"); return a; } if(a.i<0||b.i<0){ fail(s,"mod of a negative number is not supported"); return a; } a=vint(a.i%b.i); }
		else break; }
	return a;
}
static Value sp_add(Pst *s){
	Value a=sp_mul(s);
	for(;;){ sp_skip(s);
		if(*s->p=='+'){ s->p++; Value b=sp_mul(s); long r; if(!need(s,&a,T_INT,"+") || !need(s,&b,T_INT,"+")) return a; if(__builtin_add_overflow(a.i,b.i,&r)){ fail(s,"integer overflow in +"); return a; } a=vint(r); }
		else if(*s->p=='-'&&s->p[1]!='>'){ s->p++; Value b=sp_mul(s); long r; if(!need(s,&a,T_INT,"-") || !need(s,&b,T_INT,"-")) return a; if(__builtin_sub_overflow(a.i,b.i,&r)){ fail(s,"integer overflow in -"); return a; } a=vint(r); }
		else break; }
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
		sp_skip(s); if(*s->p!=':'){ fail(s,"expected ':' in quantifier"); return vbool(0); } s->p++;
		const char *body=s->p; long cnt=0; int res=isall?1:0; const char *endp=body;
		for(int e=0;e<set.n;e++){
			Pst sub=*s; sub.p=body; Env ne=*s->env; if(!env_bind(s,&ne,var,set.e[e])) return vbool(0); sub.env=&ne;
			Value bv=sp_or(&sub); need(&sub,&bv,T_BOOL,q); if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); return vbool(0); }
			endp=sub.p;
			if(isall && !bv.i) res=0;
			if(issome && bv.i) res=1;
			if(iscount && bv.i) cnt++;
		}
		if(set.n==0){ /* still parse the body once so a broken body is caught */
			Pst sub=*s; sub.p=body; Env ne=*s->env; if(!env_bind(s,&ne,var,0)) return vbool(0); sub.env=&ne;
			Value bv=sp_or(&sub); need(&sub,&bv,T_BOOL,q); if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); return vbool(0); } endp=sub.p;
		}
		s->p=endp;
		return iscount?vint(cnt):vbool(res);
	}
	/* a comparison or arithmetic term, possibly `A in SET` */
	Value a=sp_add(s); sp_skip(s);
	if(sp_word(s,"in")){ if(!need(s,&a,T_INT,"in")) return vbool(0); Value set=sp_setspec(s); if(!s->ok) return vbool(0); int in=0; for(int i=0;i<set.n;i++) if(set.e[i]==a.i) in=1; return vbool(in); }
	const char *op=s->p;
	if(!strncmp(op,"==",2)){ s->p+=2; Value b=sp_add(s); if(!same_type(s,&a,&b,"==")) return vbool(0); return vbool(val_eq(&a,&b)); }
	if(!strncmp(op,"!=",2)){ s->p+=2; Value b=sp_add(s); if(!same_type(s,&a,&b,"!=")) return vbool(0); return vbool(!val_eq(&a,&b)); }
	/* an order: <= and >= before < and >, which they begin with */
	int len=!strncmp(op,"<=",2)||!strncmp(op,">=",2)?2:*op=='<'||*op=='>'?1:0;
	if(len){
		char o[3]={op[0],len==2?op[1]:0,0};
		s->p+=len; Value b=sp_add(s);
		if(!need(s,&a,T_INT,o) || !need(s,&b,T_INT,o)) return vbool(0);
		return vbool(o[0]=='<' ? (len==2 ? a.i<=b.i : a.i<b.i) : (len==2 ? a.i>=b.i : a.i>b.i));
	}
	return a;			/* a bare arithmetic/boolean value */
}
static Value sp_not(Pst *s){ sp_skip(s); if(sp_word(s,"not")){ Value v=sp_not(s); need(s,&v,T_BOOL,"not"); return vbool(!v.i); } return sp_quant(s); }
static Value sp_and(Pst *s){ Value a=sp_not(s); for(;;){ if(sp_word(s,"and")){ Value b=sp_not(s); if(need(s,&a,T_BOOL,"and")) need(s,&b,T_BOOL,"and"); a=vbool(a.i&&b.i); } else break; } return a; }
static Value sp_or(Pst *s){ Value a=sp_and(s); for(;;){ if(sp_word(s,"or")){ Value b=sp_and(s); if(need(s,&a,T_BOOL,"or")) need(s,&b,T_BOOL,"or"); a=vbool(a.i||b.i); } else break; } return a; }

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
	Env env; memset(&env,0,sizeof env); Value none[SP_MAXVAR]; env.var=none; env.np=0;
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

static int sp_load(const char *path){
	memset(&S.modname,0,sizeof S.modname);
	S.ncst=S.nvar=S.nact=S.ninv=0; S.init[0]=0; S.has_sym=0; S.nprop=0; S.fair_weak=0;
	g_symok=0;	/* no symmetry carries over from a previously loaded spec */
	if(sp_load_into(path)) return -1;
	if(!S.modname[0]){ fprintf(stderr,"cmc: %s: no module declaration\n",path); return -1; }
	if(S.nvar==0){ fprintf(stderr,"cmc: %s: no variables\n",path); return -1; }
	if(!S.init[0]){ fprintf(stderr,"cmc: %s: no init\n",path); return -1; }
	if(S.ninv==0 && S.nprop==0){ fprintf(stderr,"cmc: %s: no invariant or property\n",path); return -1; }
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
			if(dom[0]=='['){ /* [DOM -> RANGE] : a function variable */
				V->is_fun=1; char *arrow=strstr(dom,"->"); if(!arrow){ fprintf(stderr,"cmc: %s:%d: function var needs ->\n",path,ln); goto bad; }
				*arrow=0; char *d=sp_trim(dom+1); char *r=sp_trim(arrow+2); char *rb=strrchr(r,']'); if(rb)*rb=0; r=sp_trim(r);
				CPY(V->fdom,d,"function domain"); CPY(V->dom,r,"function range");
			} else { V->is_fun=0; CPY(V->dom,dom,"var domain"); V->fdom[0]=0; }
			S.nvar++; in_action=0; continue;
		}
		if(!strncmp(s,"fairness ",9)){ if(!strcmp(sp_trim(s+9),"weak")) S.fair_weak=1; else { fprintf(stderr,"cmc: %s:%d: only 'fairness weak' is supported\n",path,ln); goto bad; } in_action=0; continue; }
		if(!strncmp(s,"property",8)){
			char *rest=sp_trim(s+8); char *col=strchr(rest,':'); if(!col){ fprintf(stderr,"cmc: %s:%d: property needs name:\n",path,ln); goto bad; }
			*col=0; if(S.nprop>=SP_MAXINV){ fprintf(stderr,"cmc: too many properties\n"); goto bad; }
			SpProp *P=&S.prop[S.nprop]; CPY(P->name,sp_trim(rest),"property name"); char *body=sp_trim(col+1);
			char *lt=strstr(body,"~>");
			if(lt){ *lt=0; P->kind=1; CPY(P->a,sp_trim(body),"property"); CPY(P->b,sp_trim(lt+2),"property"); }
			else if(!strncmp(body,"eventually",10)){ P->kind=0; CPY(P->a,sp_trim(body+10),"property"); P->b[0]=0; }
			else { fprintf(stderr,"cmc: %s:%d: property must be 'eventually E' or 'A ~> B'\n",path,ln); goto bad; }
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
			/* name( params ) [TAG] : */
			char *cl=strrchr(rest,':');
			if(cl) *cl=0;
			rest=sp_trim(rest);
			/* an optional [TAG] gate before the colon */
			char *tb=strrchr(rest,'[');
			if(tb && strchr(tb,']')){ char *te=strchr(tb,']'); *te=0; CPY(cur->tag,sp_trim(tb+1),"action tag"); *tb=0; rest=sp_trim(rest); }
			char *lp=strchr(rest,'(');
			if(lp){ *lp=0; char *params=lp+1; char *pe=strrchr(params,')'); if(pe)*pe=0;
				CPY(cur->name,sp_trim(rest),"action name");
				/* parse  p in SET, q in SET */
				char *save=NULL; for(char *t=strtok_r(params,",",&save); t; t=strtok_r(NULL,",",&save)){
					char *in=strstr(t," in "); if(!in){ fprintf(stderr,"cmc: %s:%d: action param needs 'in'\n",path,ln); goto bad; }
					*in=0; if(cur->nparam>=SP_MAXPARAM){ fprintf(stderr,"cmc: too many params\n"); goto bad; }
					CPY(cur->pname[cur->nparam],sp_trim(t),"parameter name"); CPY(cur->pdom[cur->nparam],sp_trim(in+4),"parameter domain"); cur->nparam++;
				}
			} else CPY(cur->name,sp_trim(rest),"action name");
			in_action=1; continue;
		}
		if(!strncmp(s,"when ",5)){ if(!cur){ fprintf(stderr,"cmc: %s:%d: when outside action\n",path,ln); goto bad; } CPY(cur->guard,sp_trim(s+5),"guard"); continue; }
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
typedef struct { Value v[SP_MAXVAR]; } State;

/* serialize a state to bytes for hashing/dedup (canonical already) */
static size_t state_bytes(const State *st, unsigned char *buf, size_t cap){
	size_t o=0;
	for(int i=0;i<S.nvar;i++){ const Value *v=&st->v[i];
		#define PUT(x) do{ long _x=(x); if(o+sizeof _x<=cap){ memcpy(buf+o,&_x,sizeof _x);} o+=sizeof _x; }while(0)
		PUT(v->tag); PUT(v->i); PUT(v->n);
		if(v->tag==T_SET) for(int j=0;j<v->n;j++) PUT(v->e[j]);
		if(v->tag==T_FUN) for(int j=0;j<v->n;j++){ PUT(v->k[j]); PUT(v->v[j]); }
		#undef PUT
	}
	return o;
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
 * that is a member of S, wherever it appears (int values, set elements,
 * and both the keys and values of functions). */
static Value g_symset;
static void sym_init(void){
	g_symok=0; if(!S.has_sym) return;
	Env env; memset(&env,0,sizeof env); Value none[SP_MAXVAR]; env.var=none; env.np=0;
	char e[128]; if(sp_eval_set(S.symmetry,&env,&g_symset,e,sizeof e) && g_symset.tag==T_SET && g_symset.n>=2 && g_symset.n<=8) g_symok=1;
}
/* remap x under a permutation perm[] of the symmetry set (perm[i] is the
 * image of g_symset.e[i]); non-members pass through */
static long sym_map(long x, const int *perm){
	for(int i=0;i<g_symset.n;i++) if(g_symset.e[i]==x) return g_symset.e[perm[i]];
	return x;
}
static void sym_apply(const State *in, State *out, const int *perm){
	*out=*in;
	for(int vi=0;vi<S.nvar;vi++){ Value *v=&out->v[vi];
		if(v->tag==T_INT) v->i=sym_map(v->i,perm);
		else if(v->tag==T_SET){ for(int j=0;j<v->n;j++) v->e[j]=sym_map(v->e[j],perm); set_canon(v); }
		else if(v->tag==T_FUN){ /* remap keys and values; re-sort by key */
			for(int j=0;j<v->n;j++){ v->k[j]=sym_map(v->k[j],perm); v->v[j]=sym_map(v->v[j],perm); }
			/* insertion sort by key to keep canonical */
			for(int a=1;a<v->n;a++){ long kk=v->k[a],vv=v->v[a]; int b=a-1; while(b>=0&&v->k[b]>kk){ v->k[b+1]=v->k[b]; v->v[b+1]=v->v[b]; b--; } v->k[b+1]=kk; v->v[b+1]=vv; }
		}
	}
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
	int perm[8]; for(int i=0;i<g_symset.n;i++) perm[i]=i; size_t n=0; int first=1;
	sym_perms(perm,0,g_symset.n,st,out,&n,cap,&first); return n;
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
static int in_domains(const State *st, char *err, size_t errcap);
static int sp_init_state(State *st, char *err, size_t errcap){
	for(int i=0;i<S.nvar;i++) st->v[i]=vint(0);
	/* split init on " and " at top level (init conjuncts are var == expr) */
	int set[SP_MAXVAR]={0};
	char work[SP_EXPR]; snprintf(work,sizeof work,"%s",S.init);
	char *p=work;
	while(*p){
		char *nx=strstr(p," and "); if(nx)*nx=0;
		char *conj=sp_trim(p);
		char *eq=strstr(conj,"==");
		if(!eq){ snprintf(err,errcap,"init conjunct is not var == expr: %s",conj); return -1; }
		*eq=0; char *lhs=sp_trim(conj); char *rhs=sp_trim(eq+2);
		int vi=var_index(lhs); if(vi<0){ snprintf(err,errcap,"init assigns unknown var %s",lhs); return -1; }
		if(set[vi]){ snprintf(err,errcap,"init assigns %s twice",lhs); return -1; }
		Env env; memset(&env,0,sizeof env); env.var=st->v; env.np=0; env.assigned=set;
		Value v; if(!sp_eval(rhs,&env,&v,err,errcap)) return -1;
		st->v[vi]=v; set[vi]=1;
		if(!nx) break;
		p=nx+5;
	}
	for(int i=0;i<S.nvar;i++) if(!set[i]){ snprintf(err,errcap,"init does not set %s",S.var[i].name); return -1; }
	return in_domains(st,err,errcap);
}

static int set_has(const Value *set, long x){
	for(int j=0;j<set->n;j++) if(set->e[j]==x) return 1;
	return 0;
}

/* is x a member of the domain expression dom?  A range `LO .. HI` is
 * tested by its bounds, since a value range (0 .. 99) can exceed
 * SP_SETMAX; any other form is evaluated as a set.  Returns 1 member,
 * 0 not, -1 on an error (message in err). */
static int dom_has(const char *dom, long x, char *err, size_t errcap){
	Env env; memset(&env,0,sizeof env); Value none[SP_MAXVAR]; env.var=none; env.np=0;
	const char *dd=strstr(dom,"..");
	if(dd && !strchr(dom,'{')){
		char lo[SP_EXPR]; snprintf(lo,sizeof lo,"%.*s",(int)(dd-dom),dom);
		Value l,h; if(!sp_eval(lo,&env,&l,err,errcap) || !sp_eval(dd+2,&env,&h,err,errcap)) return -1;
		if(l.tag!=T_INT || h.tag!=T_INT){ snprintf(err,errcap,"the bounds of %s are not integers",dom); return -1; }
		return x>=l.i && x<=h.i;
	}
	Value set; if(!sp_eval_set(dom,&env,&set,err,errcap)) return -1;
	if(set.tag!=T_SET){ snprintf(err,errcap,"%s is not a set",dom); return -1; }
	return set_has(&set,x);
}

/* does state st lie in every variable's declared domain?  A scalar var
 * `x : D` must be an integer in D; a function var `f : [K -> R]` must be
 * a function with key set exactly K and every value in R.  The checker
 * enumerates states, not declarations, so a value outside its domain is
 * a different model than the one written (and than the emitted TLA+'s
 * type annotations allow): a model error, never a silently explored
 * state.  Returns 0 if in, -1 (message in err) if not or on an
 * evaluation error. */
static int in_domains(const State *st, char *err, size_t errcap){
	Env env; memset(&env,0,sizeof env); Value none[SP_MAXVAR]; env.var=none; env.np=0;
	for(int vi=0;vi<S.nvar;vi++){ const Var *V=&S.var[vi]; const Value *v=&st->v[vi];
		if(!V->is_fun){
			if(v->tag!=T_INT){ snprintf(err,errcap,"%s is %s, not an integer",V->name,tag_name(v->tag)); return -1; }
			int in=dom_has(V->dom,v->i,err,errcap); if(in<0) return -1;
			if(!in){ snprintf(err,errcap,"%s = %ld is outside its domain %s",V->name,v->i,V->dom); return -1; }
			continue;
		}
		Value keys; if(!sp_eval_set(V->fdom,&env,&keys,err,errcap)) return -1;
		if(v->tag!=T_FUN || keys.tag!=T_SET || v->n!=keys.n){ snprintf(err,errcap,"%s is not a function on %s",V->name,V->fdom); return -1; }
		for(int j=0;j<v->n;j++){
			if(v->k[j]!=keys.e[j]){ snprintf(err,errcap,"%s is not a function on %s",V->name,V->fdom); return -1; }
			int in=dom_has(V->dom,v->v[j],err,errcap); if(in<0) return -1;
			if(!in){ snprintf(err,errcap,"%s[%ld] = %ld is outside its range %s",V->name,v->k[j],v->v[j],V->dom); return -1; }
		}
	}
	return 0;
}

/* apply action a with bound params (in env) to st, producing *out;
 * returns 1 if the guard held and the step was taken, 0 if disabled. */
static int sp_step(const Action *a, Env *env, const State *st, State *out, char *err, size_t errcap){
	env->var=st->v;
	if(a->guard[0]){ int g; if(!sp_eval_bool(a->guard,env,&g,"a guard",err,errcap)) return -1; if(!g) return 0; }
	*out=*st;
	/* evaluate all RHS against the CURRENT state (simultaneous assignment) */
	for(int i=0;i<a->nasgn;i++){
		int vi=var_index(a->lhs[i]); if(vi<0){ snprintf(err,errcap,"assign to unknown var %s",a->lhs[i]); return -1; }
		Value v; env->var=st->v; if(!sp_eval(a->rhs[i],env,&v,err,errcap)) return -1;
		out->v[vi]=v;
	}
	return 1;
}

/* recursively bind an action's parameters over their finite domains and
 * invoke the body for each combination */
typedef struct { const Action *a; const State *st; int depth_i; int found;
	State *succ; int *nsucc; int cap; char *err; size_t errcap; int rc; } BindCtx;
static void bind_params(const Action *a, Env *env, int pi, const State *st,
	State *succ, int *nsucc, int cap, char *err, size_t errcap, int *rc){
	if(*rc<0) return;
	if(pi==a->nparam){
		State out; int r=sp_step(a,env,st,&out,err,errcap);
		if(r<0){ *rc=-1; return; }
		if(r==1){
			if(in_domains(&out,err,errcap)){ *rc=-1; return; }
			if(*nsucc<cap) succ[(*nsucc)++]=out; else { snprintf(err,errcap,"too many successors"); *rc=-1; }
		}
		return;
	}
	Env e2=*env; Pst s={0}; /* evaluate the param domain in the current env */
	Value dom; char derr[128]; Env denv=*env; denv.var=st->v;
	if(!sp_eval_set(a->pdom[pi],&denv,&dom,derr,sizeof derr)){ snprintf(err,errcap,"param domain: %s",derr); *rc=-1; return; }
	if(dom.tag!=T_SET){ snprintf(err,errcap,"action %s: the domain of %s is not a set",a->name,a->pname[pi]); *rc=-1; return; }
	(void)s;
	for(int k=0;k<dom.n;k++){
		Env e3=e2; if(e3.np>=SP_MAXPARAM){ snprintf(err,errcap,"action %s: too many parameters",a->name); *rc=-1; return; }
		{ int f; (void)cst_lookup(a->pname[pi],&f); for(int i=0;i<e3.np;i++) if(!strcmp(e3.pname[i],a->pname[pi])) f=1;
		  if(f || var_index(a->pname[pi])>=0){ snprintf(err,errcap,"action %s: parameter %s redeclares a name already in scope",a->name,a->pname[pi]); *rc=-1; return; } }
		snprintf(e3.pname[e3.np],SP_NAME,"%s",a->pname[pi]); e3.pval[e3.np]=dom.e[k]; e3.np++;
		bind_params(a,&e3,pi+1,st,succ,nsucc,cap,err,errcap,rc);
		if(*rc<0) return;
	}
}

/* check every invariant in state st; return the index of the first that
 * fails, -1 if all hold, -2 on an evaluation error (msg in err) */
static int sp_check(const State *st, char *err, size_t errcap){
	Env env; memset(&env,0,sizeof env); env.var=st->v; env.np=0;
	for(int i=0;i<S.ninv;i++){ int h; if(!sp_eval_bool(S.inv[i].expr,&env,&h,"an invariant",err,errcap)){ return -2; } if(!h) return i; }
	return -1;
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

typedef struct QN { State st; struct QN *prev; char step[64]; int depth; } QN;

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
	State *st; int n, cap;			/* states by index; 0 is init */
	int *head;				/* first out-edge of each state, -1 none */
	int *to, *act, *next; int ne, ecap;	/* out-edges, as linked lists */
	int *rhead, *rsrc;			/* in-edges, CSR: rsrc[rhead[v] .. rhead[v+1]) */
	unsigned char *en;			/* en[i*nact+a]: a has a non-stuttering successor at i */
	int *slot; size_t mcap;			/* open-addressed index of states, -1 empty */
} lv;

static void lv_free(void){
	free(lv.st); free(lv.head); free(lv.to); free(lv.act); free(lv.next);
	free(lv.rhead); free(lv.rsrc); free(lv.en); free(lv.slot);
	memset(&lv,0,sizeof lv);
}

/* the index of state s, adding it if new (*added says which).  Lookup
 * compares states exactly, so a hash collision cannot merge two. */
static int lv_find_or_add(const State *s, int *added, char *err, size_t errcap){
	size_t m=lv.mcap-1, i=(size_t)state_fp(s)&m;
	while(lv.slot[i]!=-1){
		if(state_eq(&lv.st[lv.slot[i]],s)){ *added=0; return lv.slot[i]; }
		i=(i+1)&m;
	}
	if(lv.n>=LV_MAX){ snprintf(err,errcap,"more than %d states",LV_MAX); return -1; }
	if(lv.n==lv.cap){
		int nc=lv.cap?lv.cap*2:1024; if(nc>LV_MAX) nc=LV_MAX;
		State *ns=realloc(lv.st,sizeof(State)*(size_t)nc); if(!ns) goto oom; lv.st=ns;
		int *nh=realloc(lv.head,sizeof(int)*(size_t)nc); if(!nh) goto oom; lv.head=nh;
		lv.cap=nc;
	}
	lv.st[lv.n]=*s; lv.head[lv.n]=-1; lv.slot[i]=lv.n; *added=1;
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
		State cur=lv.st[si];	/* lv.st may move as states are added */
		for(int a=0; a<S.nact && rc==0; a++){
			if(!act_active(&S.act[a])) continue;
			int ns=0, brc=0; Env env; memset(&env,0,sizeof env); env.np=0;
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

/* the bad region of mask (see the section comment): reg[i] is set for
 * each masked state that reaches, through masked states, an SCC a fair
 * run can stay in forever.  Returns 0 ok, -1 on OOM. */
static int lv_bad_region(const unsigned char *mask, unsigned char *reg){
	int n=lv.n, na=S.nact, rc=-1;
	int *comp=malloc(sizeof(int)*(size_t)n), *queue=malloc(sizeof(int)*(size_t)n);
	unsigned char *dis=NULL, *tak=NULL, *fair=NULL;
	if(!comp||!queue) goto out;
	int nscc=lv_scc(mask,comp); if(nscc<0) goto out;
	dis=calloc((size_t)nscc*(size_t)na+1,1); tak=calloc((size_t)nscc*(size_t)na+1,1); fair=calloc((size_t)nscc+1,1);
	if(!dis||!tak||!fair) goto out;
	for(int u=0;u<n;u++){
		if(!mask[u]) continue;
		size_t c=(size_t)comp[u];
		for(int a=0;a<na;a++) if(!lv.en[(size_t)u*(size_t)na+(size_t)a]) dis[c*(size_t)na+(size_t)a]=1;
		for(int e=lv.head[u];e!=-1;e=lv.next[e]){ int v=lv.to[e];
			if(mask[v] && comp[v]==comp[u]) tak[c*(size_t)na+(size_t)lv.act[e]]=1; }
	}
	for(int c=0;c<nscc;c++){
		fair[c]=1;
		if(!S.fair_weak) continue;
		for(int a=0;a<na;a++){
			if(!act_active(&S.act[a])) continue;
			if(!dis[(size_t)c*(size_t)na+(size_t)a] && !tak[(size_t)c*(size_t)na+(size_t)a]) fair[c]=0;
		}
	}
	int qt=0;
	for(int u=0;u<n;u++){ reg[u]=mask[u] && fair[comp[u]]; if(reg[u]) queue[qt++]=u; }
	for(int qh=0; qh<qt; qh++){ int v=queue[qh];
		for(int k=lv.rhead[v];k<lv.rhead[v+1];k++){ int u=lv.rsrc[k];
			if(mask[u] && !reg[u]){ reg[u]=1; queue[qt++]=u; } } }
	rc=0;
out:	free(comp); free(queue); free(dis); free(tak); free(fair);
	return rc;
}

/* evaluate expr at state u into *holds; -1 on an evaluation error,
 * which fails the check rather than reading as FALSE */
static int lv_holds(const char *expr, int u, int *holds, char *err, size_t errcap){
	Env env; memset(&env,0,sizeof env); env.var=lv.st[u].v; env.np=0;
	return sp_eval_bool(expr,&env,holds,"a property",err,errcap) ? 0 : -1;
}

/* check all liveness properties; returns index+1 of a violated property
 * (name via *name), 0 if all hold, -1 on error */
static int liveness_check(const State *init, const char **name, char *err, size_t errcap){
	if(S.nprop==0) return 0;
	if(g_symok){ snprintf(err,errcap,"liveness properties cannot be checked under symmetry reduction"); return -1; }
	int rc=0;
	unsigned char *mask=NULL, *reg=NULL;
	if(lv_build(init,err,errcap)){ rc=-1; goto out; }
	mask=malloc((size_t)lv.n); reg=malloc((size_t)lv.n);
	if(!mask||!reg){ snprintf(err,errcap,"out of memory"); rc=-1; goto out; }
	for(int pi=0; pi<S.nprop && rc==0; pi++){ const SpProp *P=&S.prop[pi];
		const char *goal = P->kind==0 ? P->a : P->b;
		for(int u=0;u<lv.n;u++){ int g; if(lv_holds(goal,u,&g,err,errcap)){ rc=-1; goto out; } mask[u]=!g; }
		if(lv_bad_region(mask,reg)){ snprintf(err,errcap,"out of memory"); rc=-1; goto out; }
		if(P->kind==0){ if(reg[0]){ *name=P->name; rc=pi+1; } continue; }
		for(int u=0;u<lv.n && rc==0;u++){
			if(!reg[u]) continue;
			int a; if(lv_holds(P->a,u,&a,err,errcap)){ rc=-1; goto out; }
			if(a){ *name=P->name; rc=pi+1; }
		}
	}
out:	free(mask); free(reg); lv_free();
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

int cmc_spec_main(const char *path, char **defs, int ndefs){
	if(set_defines(defs,ndefs) || sp_load(path)) return 3;
	sym_init();
	/* parse-check every invariant and guard once (over the init state) */
	char err[256];
	State init; if(sp_init_state(&init,err,sizeof err)){ fprintf(stderr,"cmc: %s: init: %s\n",path,err); return 3; }

	seen_cap=1; while(seen_cap<SP_MAXSTATES*2) seen_cap<<=1; seen=calloc(seen_cap,sizeof *seen);
	if(!seen){ fprintf(stderr,"cmc: out of memory\n"); return 1; }

	/* the queue holds pointers to individually allocated QN nodes, so a
	 * node's address (used as prev for the counterexample path) stays
	 * stable as the queue grows */
	size_t qcap = 1024, qh = 0, qt = 0;
	QN **q = malloc(qcap * sizeof *q);
	if (!q) { fprintf(stderr, "cmc: out of memory\n"); return 1; }
	seen_insert(&init);
	QN *root = calloc(1, sizeof *root); root->st=init; root->prev=NULL; root->step[0]=0; root->depth=0;
	q[qt++] = root;
	size_t distinct=1, generated=1; int maxdepth=0;
	int SUCCAP = 4096; State *succ = malloc((size_t)SUCCAP * sizeof *succ);
	if (!succ) { fprintf(stderr, "cmc: out of memory\n"); free(q); return 1; }

	printf("cmc: spec %s, %d variable%s, %d action%s, breadth-first\n", S.modname, S.nvar, S.nvar==1?"":"s", S.nact, S.nact==1?"":"s");

	int ret = 0;
	while(qh<qt){
		QN *cur=q[qh];
		int bad=sp_check(&cur->st,err,sizeof err);
		if(bad==-2){ fprintf(stderr,"cmc: invariant evaluation error: %s\n",err); ret=3; break; }
		if(bad>=0){
			printf("Error: Invariant %s is violated.\n", S.inv[bad].name);
			/* reconstruct the shortest path root..cur */
			int cn=0; for(QN *x=cur; x; x=x->prev) cn++;
			printf("Error: The behavior up to this point is (%d actions):\n", cn-1);
			char **steps = malloc((size_t)cn * sizeof *steps); int si=0;
			for(QN *x=cur; x; x=x->prev) steps[si++]=x->step;
			for(int i=cn-2;i>=0;i--) printf("    %d. %s\n", cn-1-i, steps[i]);
			free(steps);
			/* the variable values at the failing state */
			printf("  state:");
			for(int vi=0;vi<S.nvar;vi++){ const Value *v=&cur->st.v[vi]; printf(" %s=",S.var[vi].name);
				if(v->tag==T_FUN){ printf("["); for(int j=0;j<v->n;j++) printf("%s%ld:%ld",j?",":"",v->k[j],v->v[j]); printf("]"); }
				else if(v->tag==T_SET){ printf("{"); for(int j=0;j<v->n;j++) printf("%s%ld",j?",":"",v->e[j]); printf("}"); }
				else printf("%ld",v->i); }
			printf("\n");
			ret=1; break;
		}
		if(cur->depth>maxdepth) maxdepth=cur->depth;
		for(int ai=0; ai<S.nact && !ret; ai++){
			if(!act_active(&S.act[ai])) continue;
			int ns=0; int rc=0; Env env; memset(&env,0,sizeof env); env.np=0;
			bind_params(&S.act[ai],&env,0,&cur->st,succ,&ns,SUCCAP,err,sizeof err,&rc);
			if(rc<0){ fprintf(stderr,"cmc: action %s: %s\n",S.act[ai].name,err); ret=3; break; }
			for(int k=0;k<ns;k++){
				generated++;
				if(!seen_insert(&succ[k])) continue;
				if(seen_n>=SP_MAXSTATES){ printf("INCOMPLETE: more than %d states\n",SP_MAXSTATES); ret=2; break; }
				if(qt>=qcap){ qcap*=2; QN **nq=realloc(q,qcap*sizeof *q); if(!nq){ fprintf(stderr,"cmc: out of memory\n"); ret=1; break; } q=nq; }
				QN *nn=calloc(1,sizeof *nn); nn->st=succ[k]; nn->prev=cur; snprintf(nn->step,sizeof nn->step,"%s",S.act[ai].name); nn->depth=cur->depth+1;
				q[qt++]=nn; distinct++;
			}
		}
		qh++;
	}
	if(ret==0){
		/* safety held; now check any liveness properties on the full graph */
		if(S.nprop>0){
			const char *pn=NULL; char lerr[256];
			int lr=liveness_check(&init,&pn,lerr,sizeof lerr);
			if(lr<0){ fprintf(stderr,"cmc: liveness: %s\n",lerr); ret=3; }
			else if(lr>0){ printf("Error: Property %s is violated (a fair cycle avoids the goal).\n",pn); ret=1; }
		}
		if(ret==0){
			printf("Model checking completed. No error has been found.\n");
			printf("%zu states generated, %zu distinct states found, 0 states left on queue.\n", generated, distinct);
			printf("The depth of the complete state graph search is %d.\n", maxdepth+1);
			printf("  %d invariant%s, %d propert%s held.\n", S.ninv, S.ninv==1?"":"s", S.nprop, S.nprop==1?"y":"ies");
		}
	}
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
	if(set_defines(defs,ndefs)) return -1;
	return sp_load(path);
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
			if(S.var[vi].is_fun && !B_map[B_nmap].idx[0]) BAD("line %d: %s is a function; bind it as %s[i] = \"...\"",ln,B_map[B_nmap].var,B_map[B_nmap].var);
			if(!S.var[vi].is_fun && B_map[B_nmap].idx[0]) BAD("line %d: %s is not a function; bind it as %s = \"...\"",ln,B_map[B_nmap].var,B_map[B_nmap].var);
			B_nmap++;
		}
	}
	#undef BCPY
	#undef BAD
	fclose(f);
	if(B_nmap==0){ snprintf(err,errcap,"binding has no [map]"); return -1; }
	return 0;
}

/* evaluate the domain of a function variable (its key set) from its
 * declared range-domain string, using constants only */
static int var_domain(const Var *V, Value *dom, char *err, size_t errcap){
	Env env; memset(&env,0,sizeof env); Value none[SP_MAXVAR]; env.var=none; env.np=0;
	return sp_eval_set(V->fdom,&env,dom,err,errcap)?0:-1;
}

/* build the abstract state from the implementation's observables via the
 * binding (the common core of C1 and C2).  Returns 0, or -1 with a
 * message in err on an evaluation error. */
static int build_abstract(const CmcObs *obs, State *out, char *err, size_t errcap){
	State st; for(int i=0;i<S.nvar;i++) st.v[i]=vint(0);
	for(int m=0;m<B_nmap;m++){
		int vi=var_index(B_map[m].var);	/* checked by cmc_refine_load_bind() */
		Var *V=&S.var[vi];
		if(V->is_fun){
			Value dom; if(var_domain(V,&dom,err,errcap)){ return -1; }
			Value fn={0}; fn.tag=T_FUN;
			for(int e=0;e<dom.n && fn.n<SP_MAPMAX;e++){
				Env env; memset(&env,0,sizeof env); env.var=st.v; env.obs=obs; env.np=1;
				snprintf(env.pname[0],SP_NAME,"%s",B_map[m].idx); env.pval[0]=dom.e[e];
				Value r; if(!sp_eval(B_map[m].expr,&env,&r,err,errcap)) return -1;
				/* an observed predicate maps to 0 or 1, the spec's encoding
				 * of a flag; a set or function cannot be one entry */
				if(r.tag!=T_INT && r.tag!=T_BOOL){ snprintf(err,errcap,"binding %s[%s]: expected an integer, not %s",B_map[m].var,B_map[m].idx,tag_name(r.tag)); return -1; }
				fn.k[fn.n]=dom.e[e]; fn.v[fn.n]=r.i; fn.n++;
			}
			st.v[vi]=fn;
		} else {
			Env env; memset(&env,0,sizeof env); env.var=st.v; env.obs=obs; env.np=0;
			Value r; if(!sp_eval(B_map[m].expr,&env,&r,err,errcap)) return -1;
			if(r.tag!=T_INT && r.tag!=T_BOOL){ snprintf(err,errcap,"binding %s: expected an integer, not %s",B_map[m].var,tag_name(r.tag)); return -1; }
			r.tag=T_INT;	/* a flag, as in a function entry */
			st.v[vi]=r;
		}
	}
	*out=st; return 0;
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
		int ns=0, rc=0; Env env; memset(&env,0,sizeof env); env.np=0;
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

/* translate one .cml expression to TLA+ into out (bounded) */
static int tla_word(const char **p, const char *w, FILE *o, const char *repl){
	size_t n=strlen(w);
	if(!strncmp(*p,w,n) && !isalnum((unsigned char)(*p)[n]) && (*p)[n]!='_'){ fputs(repl,o); *p+=n; return 1; }
	return 0;
}
/* Translate the .cml expression e[0 .. n) (n < 0: to its end) to TLA+
 * on o: a token-level rewrite, recursing into a count body so that every
 * operator there is translated as it is outside. */
static void tla_expr_n(const char *e, long n, FILE *o){
	const char *p=e, *end=n<0?e+strlen(e):e+n;
	while(p<end){
		if(isspace((unsigned char)*p)){ fputc(*p++,o); continue; }
		if(tla_word(&p,"all",o,"\\A")) continue;
		if(tla_word(&p,"some",o,"\\E")) continue;
		if(!strncmp(p,"count",5) && !isalnum((unsigned char)p[5]) && p[5]!='_'){ fputs("Cardinality({",o); p+=5;
			while(p<end&&isspace((unsigned char)*p))p++;
			while(p<end&&(isalnum((unsigned char)*p)||*p=='_')) fputc(*p++,o);
			fputs(" \\in ",o); while(p<end&&isspace((unsigned char)*p))p++;
			if(!strncmp(p,"in",2)) p+=2;
			while(p<end&&isspace((unsigned char)*p))p++;
			const char *set=p; while(p<end && *p!=':') p++;
			tla_expr_n(set,p-set,o);
			if(p<end && *p==':'){ p++; fputs(" : ",o); }
			/* the body runs to the paren that closes the count */
			const char *body=p; int depth=0;
			while(p<end){ if(*p=='(') depth++; if(*p==')'){ if(depth==0) break; depth--; } p++; }
			tla_expr_n(body,p-body,o);
			fputs("})",o); continue; }
		if(tla_word(&p,"not",o,"~")) continue;
		if(tla_word(&p,"and",o,"/\\")) continue;
		if(tla_word(&p,"or",o,"\\/")) continue;
		if(tla_word(&p,"in",o,"\\in")) continue;
		if(tla_word(&p,"mod",o,"%")) continue;
		if(tla_word(&p,"except",o,"EXCEPT")) continue;
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
 *   [f except [k]=v]      -> [f EXCEPT ![k]=v] (except-> EXCEPT; ! added below)
 * We post-process: insert '!' before a '[' that follows "EXCEPT ".        */
static void tla_expr_fixup(const char *e, FILE *o){
	/* first pass into a buffer */
	char buf[4096]; FILE *m=fmemopen(buf,sizeof buf,"w"); if(!m){ tla_expr(e,o); return; }
	tla_expr(e,m); fclose(m);
	/* insert ! after EXCEPT before each top [k] */
	const char *p=buf;
	while(*p){
		if(!strncmp(p,"EXCEPT ",7)){ fputs("EXCEPT ",o); p+=7;
			/* the following [k] groups each get a ! */
			while(*p=='['){ fputs("![",o); p++; int d=1; while(*p&&d){ if(*p=='[')d++; if(*p==']')d--; if(d==0){ fputc(']',o); p++; break; } fputc(*p++,o);} 
				while(*p&&isspace((unsigned char)*p)) fputc(*p++,o);
				if(*p=='='){ fputc(*p++,o); /* copy value until , or ] */ while(*p&&*p!=','&&*p!=']'){ fputc(*p++,o);} if(*p==','){ fputc(*p++,o); while(*p&&isspace((unsigned char)*p))fputc(*p++,o);} }
			}
			continue; }
		fputc(*p++,o);
	}
}

int cmc_emit_tla(const char *path, char **defs, int ndefs){
	if(set_defines(defs,ndefs) || sp_load(path)) return 3;
	FILE *o=stdout;
	fprintf(o,"---- MODULE %s ----\n",S.modname);
	fprintf(o,"\\* Generated from %s by cmc --emit-tla.  Run with TLC,\n",path);
	fprintf(o,"\\* Apalache or TLAPS; a .cfg binds the constants for TLC.  The\n");
	fprintf(o,"\\* @type annotations and the CInit operator let Apalache check it\n");
	fprintf(o,"\\* directly: apalache-mc check --cinit=CInit --inv=INV MODULE.tla\n");
	fprintf(o,"\\* (add --no-deadlock, as TLC's CHECK_DEADLOCK FALSE, since a\n");
	fprintf(o,"\\* terminal state is not a safety fault for these models).\n");
	fprintf(o,"EXTENDS Naturals, FiniteSets\n\n");
	/* constants, each with an Apalache @type (all our constants are Int) */
	if(S.ncst){ fprintf(o,"CONSTANTS\n"); for(int i=0;i<S.ncst;i++) fprintf(o,"  \\* @type: Int;\n  %s%s\n",S.cst[i].name,i+1<S.ncst?",":""); }
	/* variables, each with an Apalache @type: a scalar var is Int, a
	 * function var [lo..hi -> lo..hi] is Int -> Int */
	fprintf(o,"VARIABLES\n"); for(int i=0;i<S.nvar;i++) fprintf(o,"  \\* @type: %s;\n  %s%s\n",S.var[i].is_fun?"Int -> Int":"Int",S.var[i].name,i+1<S.nvar?",":"");
	/* vars tuple, annotated so Apalache reads <<...>> as a tuple not a Seq */
	fprintf(o,"\\* @type: <<"); for(int i=0;i<S.nvar;i++) fprintf(o,"%s%s",i?", ":"",S.var[i].is_fun?"Int -> Int":"Int"); fprintf(o,">>;\n");
	fprintf(o,"vars == <<"); for(int i=0;i<S.nvar;i++) fprintf(o,"%s%s",i?", ":"",S.var[i].name); fprintf(o,">>\n\n");
	/* CInit binds the constants to their .cml values, so Apalache needs no
	 * .cfg (TLC still reads the .cfg) */
	if(S.ncst){ fprintf(o,"CInit ==\n"); for(int i=0;i<S.ncst;i++) fprintf(o,"  %s %s = %ld\n",i?"/\\":"/\\",S.cst[i].name,S.cst[i].val); fprintf(o,"\n"); }
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
	fprintf(o,"Next ==\n"); { int any=0; for(int a=0;a<S.nact;a++){ if(!act_active(&S.act[a])) continue; fprintf(o,"  \\/ %s\n",S.act[a].name); any=1; } if(!any) fprintf(o,"  UNCHANGED vars\n"); } fprintf(o,"\n");
	fprintf(o,"Spec == Init /\\ [][Next]_vars");
	if(S.fair_weak){ for(int a=0;a<S.nact;a++){ if(!act_active(&S.act[a])) continue; fprintf(o," /\\ WF_vars(%s)",S.act[a].name); } }
	fprintf(o,"\n\n");
	/* invariants */
	for(int i=0;i<S.ninv;i++){ fprintf(o,"%s ==\n  ",S.inv[i].name); tla_expr_fixup(S.inv[i].expr,o); fprintf(o,"\n"); }
	/* liveness properties */
	for(int i=0;i<S.nprop;i++){ SpProp *P=&S.prop[i]; fprintf(o,"%s ==\n  ",P->name);
		if(P->kind==0){ fprintf(o,"<>("); tla_expr_fixup(P->a,o); fprintf(o,")"); }
		else { fprintf(o,"("); tla_expr_fixup(P->a,o); fprintf(o,") ~> ("); tla_expr_fixup(P->b,o); fprintf(o,")"); }
		fprintf(o,"\n"); }
	fprintf(o,"====\n");
	return 0;
}
