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
static int val_eq(const Value *a, const Value *b){
	if(a->tag!=b->tag) return 0;
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
	const Value *primed; int have_primed;	/* for x' in assignments (unused in guards) */
	const CmcObs *obs;			/* refinement: observable accessor, or NULL */
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

/* read an identifier */
static int sp_ident(Pst *s, char *out, int cap){
	sp_skip(s); int n=0;
	while((isalnum((unsigned char)*s->p)||*s->p=='_') && n<cap-1) out[n++]=*s->p++;
	out[n]=0; return n;
}

/* a SET value literal or range: {a,b,c} or LO .. HI */
static Value sp_setspec(Pst *s);

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
				Env ne=*s->env; if(ne.np<SP_MAXPARAM){ snprintf(ne.pname[ne.np],SP_NAME,"%s",id); ne.pval[ne.np]=dom.e[e]; ne.np++; } sub.env=&ne;
				Value bv=sp_or(&sub); if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); return z; }
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
		for(;;){
			sp_skip(s); if(*s->p!='['){ fail(s,"expected [ in except"); return z; } s->p++;
			Value kk=sp_or(s); if(!s->ok) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ]"); return z; } s->p++;
			sp_skip(s); if(*s->p!='='){ fail(s,"expected = in except"); return z; } s->p++;
			Value vv=sp_or(s); if(!s->ok) return z;
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
			for(;;){ Value ev=sp_or(s); if(!s->ok) return z; if(set.n>=SP_SETMAX){ fail(s,"set too large"); return z; } set.e[set.n++]=ev.i; sp_skip(s); if(*s->p==','){ s->p++; continue; } break; }
		}
		sp_skip(s); if(*s->p!='}'){ fail(s,"expected }"); return z; } s->p++;
		set_canon(&set); return set;
	}
	if(isdigit((unsigned char)*s->p)){ long x=strtol(s->p,(char**)&s->p,10); return vint(x); }
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
			Value iv=sp_or(s); if(!s->ok) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ]"); return z; } s->p++;
			int i=(int)iv.i; long k=-1;
			sp_skip(s);
			if(*s->p=='['){ s->p++; Value kv=sp_or(s); if(!s->ok) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ]"); return z; } s->p++; k=kv.i; }
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
	/* variable, possibly primed, possibly applied f[k] */
	int primed=0; if(*s->p=='\''){ s->p++; primed=1; }
	int vi=var_index(id);
	if(vi>=0){
		Value base = (primed && s->env->have_primed) ? s->env->primed[vi] : s->env->var[vi];
		sp_skip(s);
		if(*s->p=='['){ s->p++; Value kk=sp_or(s); if(!s->ok) return z; sp_skip(s); if(*s->p!=']'){ fail(s,"expected ] in application"); return z; } s->p++;
			if(base.tag!=T_FUN){ fail(s,"applied a non-function"); return z; }
			for(int i=0;i<base.n;i++) if(base.k[i]==kk.i) return vint(base.v[i]);
			fail(s,"function applied outside its domain"); return z; }
		return base;
	}
	(void)save; fail(s,"unknown identifier"); return z;
}

/* a set spec used in domains and quantifiers: {..} literal or LO .. HI */
static Value sp_add(Pst *s);
static Value sp_setspec(Pst *s){
	sp_skip(s);
	if(*s->p=='{') return sp_atom(s);
	/* LO .. HI (inclusive); bounds are arithmetic expressions */
	Value lo=sp_add(s); if(!s->ok) return lo; sp_skip(s);
	if(s->p[0]=='.'&&s->p[1]=='.'){ s->p+=2; Value hi=sp_add(s); if(!s->ok) return hi;
		Value set={0}; set.tag=T_SET; for(long x=lo.i;x<=hi.i;x++){ if(set.n>=SP_SETMAX){ fail(s,"range too large"); return set; } set.e[set.n++]=x; } set_canon(&set); return set; }
	/* a bare set-valued expression (a variable of set type) */
	return lo;
}

static Value sp_mul(Pst *s){
	Value a=sp_atom(s);
	for(;;){ sp_skip(s);
		if(*s->p=='*'){ s->p++; Value b=sp_atom(s); a=vint(a.i*b.i); }
		else if(*s->p=='/'){ s->p++; Value b=sp_atom(s); if(b.i==0){ fail(s,"divide by zero"); return a; } a=vint(a.i/b.i); }
		else if(sp_word(s,"mod")){ Value b=sp_atom(s); if(b.i==0){ fail(s,"mod by zero"); return a; } a=vint(a.i%b.i); }
		else break; }
	return a;
}
static Value sp_add(Pst *s){
	Value a=sp_mul(s);
	for(;;){ sp_skip(s);
		if(*s->p=='+'){ s->p++; Value b=sp_mul(s); a=vint(a.i+b.i); }
		else if(*s->p=='-'&&s->p[1]!='>'){ s->p++; Value b=sp_mul(s); a=vint(a.i-b.i); }
		else break; }
	return a;
}

/* quantifiers and membership: (all|some|count) V in SET : EXPR, A in SET */
static Value sp_quant(Pst *s){
	sp_skip(s);
	int isall=sp_word(s,"all"), issome=0, iscount=0;
	if(!isall){ issome=sp_word(s,"some"); if(!issome) iscount=sp_word(s,"count"); }
	if(isall||issome||iscount){
		char var[SP_NAME]; sp_ident(s,var,sizeof var);
		if(!sp_word(s,"in")){ fail(s,"expected 'in' in quantifier"); return vbool(0); }
		Value set=sp_setspec(s); if(!s->ok) return vbool(0);
		sp_skip(s); if(*s->p!=':'){ fail(s,"expected ':' in quantifier"); return vbool(0); } s->p++;
		const char *body=s->p; long cnt=0; int res=isall?1:0; const char *endp=body;
		for(int e=0;e<set.n;e++){
			Pst sub=*s; sub.p=body; Env ne=*s->env; if(ne.np<SP_MAXPARAM){ snprintf(ne.pname[ne.np],SP_NAME,"%s",var); ne.pval[ne.np]=set.e[e]; ne.np++; } sub.env=&ne;
			Value bv=sp_or(&sub); if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); return vbool(0); }
			endp=sub.p;
			if(isall && !bv.i) res=0;
			if(issome && bv.i) res=1;
			if(iscount && bv.i) cnt++;
		}
		if(set.n==0){ /* still parse the body once so a broken body is caught */
			Pst sub=*s; sub.p=body; Env ne=*s->env; if(ne.np<SP_MAXPARAM){ snprintf(ne.pname[ne.np],SP_NAME,"%s",var); ne.pval[ne.np]=0; ne.np++; } sub.env=&ne;
			(void)sp_or(&sub); if(!sub.ok){ s->ok=0; snprintf(s->err,sizeof s->err,"%s",sub.err); return vbool(0); } endp=sub.p;
		}
		s->p=endp;
		return iscount?vint(cnt):vbool(res);
	}
	/* a comparison or arithmetic term, possibly `A in SET` */
	Value a=sp_add(s); sp_skip(s);
	if(sp_word(s,"in")){ Value set=sp_setspec(s); if(!s->ok) return vbool(0); int in=0; for(int i=0;i<set.n;i++) if(set.e[i]==a.i) in=1; return vbool(in); }
	const char *op=s->p;
	if(!strncmp(op,"==",2)){ s->p+=2; Value b=sp_add(s); return vbool(val_eq(&a,&b)); }
	if(!strncmp(op,"!=",2)){ s->p+=2; Value b=sp_add(s); return vbool(!val_eq(&a,&b)); }
	if(!strncmp(op,"<=",2)){ s->p+=2; Value b=sp_add(s); return vbool(a.i<=b.i); }
	if(!strncmp(op,">=",2)){ s->p+=2; Value b=sp_add(s); return vbool(a.i>=b.i); }
	if(*op=='<'){ s->p++; Value b=sp_add(s); return vbool(a.i<b.i); }
	if(*op=='>'){ s->p++; Value b=sp_add(s); return vbool(a.i>b.i); }
	return a;			/* a bare arithmetic/boolean value */
}
static Value sp_not(Pst *s){ sp_skip(s); if(sp_word(s,"not")){ Value v=sp_not(s); return vbool(!v.i); } return sp_quant(s); }
static Value sp_and(Pst *s){ Value a=sp_not(s); for(;;){ if(sp_word(s,"and")){ Value b=sp_not(s); a=vbool(a.i&&b.i); } else break; } return a; }
static Value sp_or(Pst *s){ Value a=sp_and(s); for(;;){ if(sp_word(s,"or")){ Value b=sp_and(s); a=vbool(a.i||b.i); } else break; } return a; }

/* evaluate EXPR to a Value in env; returns 1 ok, 0 error (msg in err) */
static int sp_eval(const char *expr, Env *env, Value *out, char *errbuf, size_t errcap){
	Pst s={0}; s.p=expr; s.env=env; s.ok=1;
	Value v=sp_or(&s);
	sp_skip(&s);
	if(s.ok && *s.p){ s.ok=0; snprintf(s.err,sizeof s.err,"trailing text: %s",s.p); }
	if(!s.ok){ if(errbuf) snprintf(errbuf,errcap,"%s",s.err); return 0; }
	*out=v; return 1;
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
	Env env; memset(&env,0,sizeof env); Value none[SP_MAXVAR]; env.var=none; env.np=0; env.have_primed=0;
	Value v; char e[128];
	if(!sp_eval(expr,&env,&v,e,sizeof e)){ *ok=0; return 0; }
	*ok=1; return v.i;
}

static int sp_load_into(const char *path);

static int sp_load(const char *path){
	memset(&S.modname,0,sizeof S.modname);
	S.ncst=S.nvar=S.nact=S.ninv=0; S.init[0]=0; S.has_sym=0; S.nprop=0; S.fair_weak=0;
	if(sp_load_into(path)) return -1;
	if(!S.modname[0]){ fprintf(stderr,"cmc: %s: no module declaration\n",path); return -1; }
	if(S.nvar==0){ fprintf(stderr,"cmc: %s: no variables\n",path); return -1; }
	if(!S.init[0]){ fprintf(stderr,"cmc: %s: no init\n",path); return -1; }
	if(S.ninv==0 && S.nprop==0){ fprintf(stderr,"cmc: %s: no invariant or property\n",path); return -1; }
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
		char *h=strchr(line,'#'); if(h)*h=0;
		char *s=sp_trim(line);
		if(!*s) continue;
		if(!strncmp(s,"module ",7)){ if(!S.modname[0]) snprintf(S.modname,SP_NAME,"%s",sp_trim(s+7)); in_action=0; continue; }
		if(!strncmp(s,"extends ",8) || !strncmp(s,"import ",7)){
			const char *base=sp_trim(s+(s[0]=='e'?8:7));
			/* resolve relative to this file's directory; add .cml if absent */
			char bp[1024]; const char *sl=strrchr(path,'/');
			if(base[0]=='/'||!sl) snprintf(bp,sizeof bp,"%s",base);
			else snprintf(bp,sizeof bp,"%.*s/%s",(int)(sl-path),path,base);
			if(!strstr(bp,".cml")) strncat(bp,".cml",sizeof bp-strlen(bp)-1);
			if(sp_load_into(bp)){ fprintf(stderr,"cmc: %s:%d: cannot extend %s\n",path,ln,base); goto bad; }
			in_action=0; continue;
		}
		if(!strncmp(s,"const ",6)){
			char *rest=sp_trim(s+6); char *eq=strchr(rest,'='); if(!eq){ fprintf(stderr,"cmc: %s:%d: const needs =\n",path,ln); goto bad; }
			*eq=0; char *nm=sp_trim(rest), *vv=sp_trim(eq+1);
			if(S.ncst>=SP_MAXCONST){ fprintf(stderr,"cmc: too many consts\n"); goto bad; }
			int ok; long val=sp_const_eval(vv,&ok); if(!ok){ fprintf(stderr,"cmc: %s:%d: const %s: bad value\n",path,ln,nm); goto bad; }
			snprintf(S.cst[S.ncst].name,SP_NAME,"%s",nm); S.cst[S.ncst].val=val; S.ncst++; in_action=0; continue;
		}
		if(!strncmp(s,"var ",4)){
			char *rest=sp_trim(s+4); char *col=strchr(rest,':'); if(!col){ fprintf(stderr,"cmc: %s:%d: var needs :\n",path,ln); goto bad; }
			*col=0; char *nm=sp_trim(rest), *dom=sp_trim(col+1);
			if(S.nvar>=SP_MAXVAR){ fprintf(stderr,"cmc: too many vars\n"); goto bad; }
			Var *V=&S.var[S.nvar];
			snprintf(V->name,SP_NAME,"%s",nm);
			if(dom[0]=='['){ /* [DOM -> RANGE] : a function variable */
				V->is_fun=1; char *arrow=strstr(dom,"->"); if(!arrow){ fprintf(stderr,"cmc: %s:%d: function var needs ->\n",path,ln); goto bad; }
				*arrow=0; char *d=sp_trim(dom+1); char *r=sp_trim(arrow+2); char *rb=strrchr(r,']'); if(rb)*rb=0; r=sp_trim(r);
				snprintf(V->fdom,SP_EXPR,"%s",d); snprintf(V->dom,SP_EXPR,"%s",r);
			} else { V->is_fun=0; snprintf(V->dom,SP_EXPR,"%s",dom); V->fdom[0]=0; }
			S.nvar++; in_action=0; continue;
		}
		if(!strncmp(s,"fairness ",9)){ if(!strcmp(sp_trim(s+9),"weak")) S.fair_weak=1; else { fprintf(stderr,"cmc: %s:%d: only 'fairness weak' is supported\n",path,ln); goto bad; } in_action=0; continue; }
		if(!strncmp(s,"property",8)){
			char *rest=sp_trim(s+8); char *col=strchr(rest,':'); if(!col){ fprintf(stderr,"cmc: %s:%d: property needs name:\n",path,ln); goto bad; }
			*col=0; if(S.nprop>=SP_MAXINV){ fprintf(stderr,"cmc: too many properties\n"); goto bad; }
			SpProp *P=&S.prop[S.nprop]; snprintf(P->name,SP_NAME,"%s",sp_trim(rest)); char *body=sp_trim(col+1);
			char *lt=strstr(body,"~>");
			if(lt){ *lt=0; P->kind=1; snprintf(P->a,SP_EXPR,"%s",sp_trim(body)); snprintf(P->b,SP_EXPR,"%s",sp_trim(lt+2)); }
			else if(!strncmp(body,"eventually",10)){ P->kind=0; snprintf(P->a,SP_EXPR,"%s",sp_trim(body+10)); P->b[0]=0; }
			else { fprintf(stderr,"cmc: %s:%d: property must be 'eventually E' or 'A ~> B'\n",path,ln); goto bad; }
			S.nprop++; in_action=0; continue;
		}
		if(!strncmp(s,"symmetry ",9)){ snprintf(S.symmetry,SP_EXPR,"%s",sp_trim(s+9)); S.has_sym=1; in_action=0; continue; }
		if(!strncmp(s,"init ",5)){ const char *ic=sp_trim(s+5);
			if(S.init[0]){ strncat(S.init," and ",SP_EXPR-strlen(S.init)-1); strncat(S.init,ic,SP_EXPR-strlen(S.init)-1); }
			else snprintf(S.init,SP_EXPR,"%s",ic);
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
			if(tb && strchr(tb,']')){ char *te=strchr(tb,']'); *te=0; snprintf(cur->tag,SP_NAME,"%s",sp_trim(tb+1)); *tb=0; rest=sp_trim(rest); }
			char *lp=strchr(rest,'(');
			if(lp){ *lp=0; char *params=lp+1; char *pe=strrchr(params,')'); if(pe)*pe=0;
				snprintf(cur->name,SP_NAME,"%s",sp_trim(rest));
				/* parse  p in SET, q in SET */
				char *save=NULL; for(char *t=strtok_r(params,",",&save); t; t=strtok_r(NULL,",",&save)){
					char *in=strstr(t," in "); if(!in){ fprintf(stderr,"cmc: %s:%d: action param needs 'in'\n",path,ln); goto bad; }
					*in=0; if(cur->nparam>=SP_MAXPARAM){ fprintf(stderr,"cmc: too many params\n"); goto bad; }
					snprintf(cur->pname[cur->nparam],SP_NAME,"%s",sp_trim(t)); snprintf(cur->pdom[cur->nparam],SP_EXPR,"%s",sp_trim(in+4)); cur->nparam++;
				}
			} else snprintf(cur->name,SP_NAME,"%s",sp_trim(rest));
			in_action=1; continue;
		}
		if(!strncmp(s,"when ",5)){ if(!cur){ fprintf(stderr,"cmc: %s:%d: when outside action\n",path,ln); goto bad; } snprintf(cur->guard,SP_EXPR,"%s",sp_trim(s+5)); continue; }
		if(!strncmp(s,"invariant",9)){
			char *rest=sp_trim(s+9); char *col=strchr(rest,':'); if(!col){ fprintf(stderr,"cmc: %s:%d: invariant needs name:\n",path,ln); goto bad; }
			*col=0; if(S.ninv>=SP_MAXINV){ fprintf(stderr,"cmc: too many invariants\n"); goto bad; }
			snprintf(S.inv[S.ninv].name,SP_NAME,"%s",sp_trim(rest)); snprintf(S.inv[S.ninv].expr,SP_EXPR,"%s",sp_trim(col+1)); S.ninv++; in_action=0; continue;
		}
		/* otherwise: an assignment  lhs' = rhs  inside an action */
		if(in_action && cur){
			char *pr=strstr(s,"'"); char *eq=strchr(s,'=');
			if(pr && eq && pr<eq){
				*pr=0; char *lhs=sp_trim(s); char *rhs=sp_trim(eq+1);
				if(cur->nasgn>=SP_MAXVAR){ fprintf(stderr,"cmc: too many assignments\n"); goto bad; }
				snprintf(cur->lhs[cur->nasgn],SP_NAME,"%s",lhs); snprintf(cur->rhs[cur->nasgn],SP_EXPR,"%s",rhs); cur->nasgn++;
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
static Value g_symset; static int g_symok;
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
/* the canonical fingerprint of a state (min over permutations if symmetric) */
static void sym_perms(int *perm, int k, int n, const State *st, uint64_t *best, int *first){
	if(k==n){ State p; sym_apply(st,&p,perm); unsigned char bb[65536]; size_t nb=state_bytes(&p,bb,sizeof bb); uint64_t h=fnv(bb,nb); if(*first||h<*best){ *best=h; *first=0; } return; }
	for(int i=k;i<n;i++){ int t=perm[k]; perm[k]=perm[i]; perm[i]=t; sym_perms(perm,k+1,n,st,best,first); t=perm[k]; perm[k]=perm[i]; perm[i]=t; }
}
static uint64_t state_fp(const State *st){
	if(!g_symok){ unsigned char bb[65536]; size_t nb=state_bytes(st,bb,sizeof bb); return fnv(bb,nb); }
	int perm[8]; for(int i=0;i<g_symset.n;i++) perm[i]=i; uint64_t best=0; int first=1;
	sym_perms(perm,0,g_symset.n,st,&best,&first); return best;
}

/* set the initial state from the `init` predicate (a conjunction of
 * `var == EXPR`).  Returns 0 ok. */
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
		Env env; memset(&env,0,sizeof env); env.var=st->v; env.np=0;
		Value v; if(!sp_eval(rhs,&env,&v,err,errcap)) return -1;
		st->v[vi]=v; set[vi]=1;
		if(!nx) break;
		p=nx+5;
	}
	for(int i=0;i<S.nvar;i++) if(!set[i]){ snprintf(err,errcap,"init does not set %s",S.var[i].name); return -1; }
	return 0;
}

/* apply action a with bound params (in env) to st, producing *out;
 * returns 1 if the guard held and the step was taken, 0 if disabled. */
static int sp_step(const Action *a, Env *env, const State *st, State *out, char *err, size_t errcap){
	env->var=st->v; env->have_primed=0;
	if(a->guard[0]){ Value g; if(!sp_eval(a->guard,env,&g,err,errcap)) return -1; if(!g.i) return 0; }
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
		if(r==1){ if(*nsucc<cap) succ[(*nsucc)++]=out; else { snprintf(err,errcap,"too many successors"); *rc=-1; } }
		return;
	}
	Env e2=*env; Pst s={0}; /* evaluate the param domain in the current env */
	Value dom; char derr[128]; Env denv=*env; denv.var=st->v;
	if(!sp_eval_set(a->pdom[pi],&denv,&dom,derr,sizeof derr)){ snprintf(err,errcap,"param domain: %s",derr); *rc=-1; return; }
	(void)s;
	for(int k=0;k<dom.n;k++){
		Env e3=e2; if(e3.np<SP_MAXPARAM){ snprintf(e3.pname[e3.np],SP_NAME,"%s",a->pname[pi]); e3.pval[e3.np]=dom.e[k]; e3.np++; }
		bind_params(a,&e3,pi+1,st,succ,nsucc,cap,err,errcap,rc);
		if(*rc<0) return;
	}
}

/* check every invariant in state st; return the index of the first that
 * fails, -1 if all hold, -2 on an evaluation error (msg in err) */
static int sp_check(const State *st, char *err, size_t errcap){
	Env env; memset(&env,0,sizeof env); env.var=st->v; env.np=0;
	for(int i=0;i<S.ninv;i++){ Value v; if(!sp_eval(S.inv[i].expr,&env,&v,err,errcap)){ return -2; } if(!v.i) return i; }
	return -1;
}

/* BFS state store (open-addressed hash of fingerprints) + a queue of
 * states with their shortest path (action descriptions) */
#define SP_MAXSTATES 2000000
static uint64_t *seen; static size_t seen_cap, seen_n;
static int seen_insert(uint64_t h){
	size_t m=seen_cap-1, i=h&m;
	for(;;){ if(seen[i]==0){ seen[i]=h?h:1; seen_n++; return 1; } if(seen[i]==(h?h:1)) return 0; i=(i+1)&m; }
}

typedef struct QN { State st; struct QN *prev; char step[64]; int depth; } QN;

/* =====================================================================
 * liveness (Milestone F): weak-fairness-based temporal properties on the
 * abstract model.  After the safety search, rebuild the reachable state
 * graph explicitly (states, action-labeled edges, per-state enabled
 * actions), then for each property find a FAIR violating cycle (lasso):
 *   eventually P : a reachable cycle, all of whose states have ~P, that
 *                  is weakly fair (no action is enabled throughout the
 *                  cycle yet never taken in it) -> P is not guaranteed.
 *   A ~> B       : a reachable A-state from which such a ~B fair cycle
 *                  is reachable -> B does not always follow A.
 * Sound under the stated weak-fairness-of-all-actions assumption; gated
 * against TLC's liveness on the emitted TLA+.
 * ===================================================================== */
#define LV_MAX 200000
static int lv_cap;
static State *lv_st;				/* lv_st[i] = state i */
static int *lv_eadj, *lv_eact, *lv_ehead, *lv_enext; /* edge lists */
static int lv_ne;
static unsigned char *lv_enabled;	/* lv_enabled[i*nact + a] = action a enabled at state i */
static int lv_n;
/* fp -> index map (open addressing) */
static uint64_t *lv_fp; static int *lv_idx; static size_t lv_mcap;
static int lv_find_or_add(uint64_t h, int *added){
	size_t m=lv_mcap-1, i=h&m;
	for(;;){ if(lv_fp[i]==0 && lv_idx[i]==0 && !(h==0)){ /* empty */ }
		if(lv_idx[i]==-1){ lv_fp[i]=h; lv_idx[i]=lv_n; *added=1; return lv_n; }
		if(lv_fp[i]==h){ *added=0; return lv_idx[i]; } i=(i+1)&m; }
}
static void lv_add_edge(int from,int to,int act){ lv_eadj[lv_ne]=to; lv_eact[lv_ne]=act; lv_enext[lv_ne]=lv_ehead[from]; lv_ehead[from]=lv_ne; lv_ne++; }

/* build the reachable graph from init; returns 0 ok, -1 too big/err */
static int lv_build(const State *init, char *err, size_t errcap){
	lv_cap=LV_MAX; lv_mcap=1; while(lv_mcap<(size_t)LV_MAX*4) lv_mcap<<=1;
	lv_st=malloc(sizeof(State)*lv_cap); lv_ehead=malloc(sizeof(int)*lv_cap); lv_enabled=calloc((size_t)lv_cap*S.nact,1);
	lv_eadj=malloc(sizeof(int)*lv_cap*4); lv_eact=malloc(sizeof(int)*lv_cap*4); lv_enext=malloc(sizeof(int)*lv_cap*4);
	lv_fp=calloc(lv_mcap,sizeof(uint64_t)); lv_idx=malloc(sizeof(int)*lv_mcap);
	if(!lv_st||!lv_ehead||!lv_enabled||!lv_eadj||!lv_eact||!lv_enext||!lv_fp||!lv_idx){ snprintf(err,errcap,"out of memory"); return -1; }
	for(size_t i=0;i<lv_mcap;i++) lv_idx[i]=-1;
	for(int i=0;i<lv_cap;i++) lv_ehead[i]=-1;
	lv_n=0; lv_ne=0;
	int *queue=malloc(sizeof(int)*lv_cap); size_t qh=0,qt=0;
	int added; int i0=lv_find_or_add(state_fp(init),&added); lv_st[lv_n++]=*init; queue[qt++]=i0;
	while(qh<qt){
		int si=queue[qh++]; State *cs=&lv_st[si];
		for(int a=0;a<S.nact && lv_ne+16<lv_cap*4;a++){
			if(!act_active(&S.act[a])) continue;
			State succ[256]; int ns=0,rc=0; Env env; memset(&env,0,sizeof env); env.np=0;
			bind_params(&S.act[a],&env,0,cs,succ,&ns,256,err,errcap,&rc);
			if(rc<0) return -1;
			if(ns>0) lv_enabled[(size_t)si*S.nact+a]=1;
			for(int k=0;k<ns;k++){
				uint64_t h=state_fp(&succ[k]); int ad; int ti=lv_find_or_add(h,&ad);
				if(ad){ if(lv_n>=lv_cap){ snprintf(err,errcap,"liveness: more than %d states",LV_MAX); return -1; } lv_st[lv_n++]=succ[k]; queue[qt++]=ti; }
				lv_add_edge(si,ti,a);
			}
		}
	}
	free(queue); return 0;
}

/* Tarjan SCC restricted to states where mask[i]==1, over edges between
 * masked states; fills comp[] (only for masked states).  lv_nscc SCCs. */
static int *lv_low,*lv_num,*lv_comp,*lv_stk,*lv_onstk; static int lv_cnt,lv_sp,lv_nscc;
static const unsigned char *lv_mask;
static void lv_scc(int u){
	lv_num[u]=lv_low[u]=lv_cnt++; lv_stk[lv_sp++]=u; lv_onstk[u]=1;
	for(int e=lv_ehead[u];e!=-1;e=lv_enext[e]){ int v=lv_eadj[e]; if(!lv_mask[v]) continue;
		if(lv_num[v]==-1){ lv_scc(v); if(lv_low[v]<lv_low[u]) lv_low[u]=lv_low[v]; }
		else if(lv_onstk[v] && lv_num[v]<lv_low[u]) lv_low[u]=lv_num[v]; }
	if(lv_low[u]==lv_num[u]){ int w; do{ w=lv_stk[--lv_sp]; lv_onstk[w]=0; lv_comp[w]=lv_nscc; }while(w!=u); lv_nscc++; }
}

/* is SCC c (within the masked subgraph) a weakly-fair cycle?  It must
 * have an internal edge, and no action may be enabled at every state of
 * c yet taken on no intra-c edge. */
static int lv_fair(int c){
	int has_internal=0;
	for(int u=0;u<lv_n;u++){ if(!lv_mask[u]||lv_comp[u]!=c) continue;
		for(int ed=lv_ehead[u];ed!=-1;ed=lv_enext[ed]) if(lv_mask[lv_eadj[ed]]&&lv_comp[lv_eadj[ed]]==c) has_internal=1; }
	if(!has_internal) return 0;
	if(S.fair_weak){
		for(int a=0;a<S.nact;a++){ if(!act_active(&S.act[a])) continue;
			int enabled_all=1, taken=0;
			for(int u=0;u<lv_n;u++){ if(!lv_mask[u]||lv_comp[u]!=c) continue;
				if(!lv_enabled[(size_t)u*S.nact+a]) enabled_all=0;
				for(int ed=lv_ehead[u];ed!=-1;ed=lv_enext[ed]) if(lv_mask[lv_eadj[ed]]&&lv_comp[lv_eadj[ed]]==c && lv_eact[ed]==a) taken=1; }
			if(enabled_all && !taken) return 0;	/* WF forbids staying in c */
		}
	}
	return 1;
}

/* find a weakly-fair cycle entirely within states where `goal` is FALSE;
 * returns the SCC id, or -1 if none.  (A fair ~goal cycle = a liveness
 * counterexample for `eventually goal`.) */
static unsigned char *lv_maskbuf;
static int lv_bad_cycle(const char *goal){
	for(int u=0;u<lv_n;u++){ Env env; memset(&env,0,sizeof env); env.var=lv_st[u].v; env.np=0; Value v; char e[64];
		int g = sp_eval(goal,&env,&v,e,sizeof e) ? (int)v.i : 0; lv_maskbuf[u] = g?0:1; }
	lv_mask=lv_maskbuf;
	for(int i=0;i<lv_n;i++){ lv_num[i]=-1; lv_low[i]=0; lv_comp[i]=-1; lv_onstk[i]=0; }
	lv_cnt=0; lv_sp=0; lv_nscc=0;
	for(int i=0;i<lv_n;i++) if(lv_mask[i] && lv_num[i]==-1) lv_scc(i);
	for(int c=0;c<lv_nscc;c++) if(lv_fair(c)) return c;
	return -1;
}

/* check all liveness properties; returns index+1 of a violated property
 * (name via *name), 0 if all hold, -1 on error */
static int liveness_check(const State *init, const char **name, char *err, size_t errcap){
	if(S.nprop==0) return 0;
	if(lv_build(init,err,errcap)) return -1;
	lv_low=malloc(sizeof(int)*lv_n); lv_num=malloc(sizeof(int)*lv_n); lv_comp=malloc(sizeof(int)*lv_n);
	lv_stk=malloc(sizeof(int)*lv_n); lv_onstk=calloc(lv_n,sizeof(int)); lv_maskbuf=malloc(lv_n);
	int rc=0;
	for(int pi=0;pi<S.nprop && !rc;pi++){ SpProp *P=&S.prop[pi];
		if(P->kind==0){ /* eventually a: fail if a fair ~a cycle is reachable */
			if(lv_bad_cycle(P->a)>=0){ *name=P->name; rc=pi+1; }
		} else { /* a ~> b: fail if an A-state can reach a fair ~b cycle */
			int bad=lv_bad_cycle(P->b);
			if(bad>=0){
				unsigned char *canreach=calloc(lv_n,1);
				for(int u=0;u<lv_n;u++) if(lv_maskbuf[u]&&lv_comp[u]==bad) canreach[u]=1;
				int chg=1; while(chg){ chg=0; for(int u=0;u<lv_n;u++){ if(canreach[u]) continue; for(int ed=lv_ehead[u];ed!=-1;ed=lv_enext[ed]) if(canreach[lv_eadj[ed]]){ canreach[u]=1; chg=1; break; } } }
				for(int u=0;u<lv_n && !rc;u++) if(canreach[u]){ Env env; memset(&env,0,sizeof env); env.var=lv_st[u].v; env.np=0; Value v; char e[64]; if(sp_eval(P->a,&env,&v,e,sizeof e) && v.i){ *name=P->name; rc=pi+1; } }
				free(canreach);
			}
		}
	}
	free(lv_low);free(lv_num);free(lv_comp);free(lv_stk);free(lv_onstk);free(lv_maskbuf);
	free(lv_st);free(lv_ehead);free(lv_enabled);free(lv_eadj);free(lv_eact);free(lv_enext);free(lv_fp);free(lv_idx);
	return rc;
}

int cmc_spec_main(const char *path, char **defs, int ndefs){
	S.ndef=0; for(int i=0;i<ndefs && S.ndef<SP_MAXCONST;i++){ snprintf(S.define[S.ndef++],SP_NAME,"%s",defs[i]); }
	if(sp_load(path)) return 3;
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
	seen_insert(state_fp(&init));
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
				uint64_t h=state_fp(&succ[k]);
				if(!seen_insert(h)) continue;
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
	S.ndef=0; for(int i=0;i<ndefs && S.ndef<SP_MAXCONST;i++) snprintf(S.define[S.ndef++],SP_NAME,"%s",defs[i]);
	return sp_load(path);
}

int cmc_refine_load_bind(const char *path, char *err, size_t errcap){
	FILE *f=fopen(path,"r"); if(!f){ snprintf(err,errcap,"cannot open %s",path); return -1; }
	B_nmap=0; S.nfield=0; char line[1024]; int sect=0; /* 1=fields 2=map */
	while(fgets(line,sizeof line,f)){
		char *h=strchr(line,'#'); if(h)*h=0; char *s=sp_trim(line); if(!*s) continue;
		if(s[0]=='['){ if(!strncmp(s,"[fields]",8)) sect=1; else if(!strncmp(s,"[map]",5)) sect=2; else sect=0; continue; }
		char *eq=strchr(s,'='); if(!eq){ continue; } /* spec=/binary= header lines ignored here */
		*eq=0; char *k=sp_trim(s); char *v=sp_trim(eq+1);
		/* strip quotes around v */
		if(*v=='"'){ v++; char *q=strrchr(v,'"'); if(q)*q=0; }
		if(sect==1){ if(S.nfield<SP_MAXCONST){ snprintf(S.fname[S.nfield],SP_NAME,"%s",k); S.foff[S.nfield]=atoi(v); S.nfield++; } }
		else if(sect==2){
			if(B_nmap>=SP_MAXVAR){ snprintf(err,errcap,"too many bindings"); fclose(f); return -1; }
			/* k is VAR or VAR[idx] */
			char *lb=strchr(k,'['); if(lb){ *lb=0; char *rb=strchr(lb+1,']'); if(rb)*rb=0; snprintf(B_map[B_nmap].idx,SP_NAME,"%s",sp_trim(lb+1)); } else B_map[B_nmap].idx[0]=0;
			snprintf(B_map[B_nmap].var,SP_NAME,"%s",sp_trim(k)); snprintf(B_map[B_nmap].expr,SP_EXPR,"%s",v); B_nmap++;
		}
	}
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

int cmc_refine_check(const CmcObs *obs, const char **name, char *err, size_t errcap){
	/* build the abstract state: each variable's Value from its binding */
	State st; for(int i=0;i<S.nvar;i++) st.v[i]=vint(0);
	for(int m=0;m<B_nmap;m++){
		int vi=var_index(B_map[m].var); if(vi<0){ snprintf(err,errcap,"binding for unknown var %s",B_map[m].var); return -2; }
		Var *V=&S.var[vi];
		if(B_map[m].idx[0] && V->is_fun){
			/* function var: evaluate expr for each key in the domain, with idx bound */
			Value dom; if(var_domain(V,&dom,err,errcap)){ return -2; }
			Value fn={0}; fn.tag=T_FUN;
			for(int e=0;e<dom.n && fn.n<SP_MAPMAX;e++){
				Env env; memset(&env,0,sizeof env); env.var=st.v; env.obs=obs; env.np=1;
				snprintf(env.pname[0],SP_NAME,"%s",B_map[m].idx); env.pval[0]=dom.e[e];
				Value r; if(!sp_eval(B_map[m].expr,&env,&r,err,errcap)) return -2;
				fn.k[fn.n]=dom.e[e]; fn.v[fn.n]=r.i; fn.n++;
			}
			st.v[vi]=fn;
		} else {
			Env env; memset(&env,0,sizeof env); env.var=st.v; env.obs=obs; env.np=0;
			Value r; if(!sp_eval(B_map[m].expr,&env,&r,err,errcap)) return -2;
			st.v[vi]=r;
		}
	}
	/* check the abstract invariants on the built state */
	int bad=sp_check(&st,err,errcap);
	if(bad==-2) return -2;
	if(bad>=0){ *name=S.inv[bad].name; return bad; }
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
static void tla_expr(const char *e, FILE *o){
	const char *p=e;
	while(*p){
		if(isspace((unsigned char)*p)){ fputc(*p++,o); continue; }
		if(tla_word(&p,"all",o,"\\A")) continue;
		if(tla_word(&p,"some",o,"\\E")) continue;
		if(!strncmp(p,"count",5) && !isalnum((unsigned char)p[5]) && p[5]!='_'){ fputs("Cardinality({",o); p+=5;
			while(*p&&isspace((unsigned char)*p))p++;
			while(*p&&(isalnum((unsigned char)*p)||*p=='_')) fputc(*p++,o);
			fputs(" \\in ",o); while(*p&&isspace((unsigned char)*p))p++;
			if(!strncmp(p,"in",2)) p+=2;
			while(*p&&isspace((unsigned char)*p))p++;
			while(*p && *p!=':'){ fputc(*p++,o); }
			if(*p==':'){ p++; fputs(" : ",o); }
			int depth=0; while(*p){ if(*p=='(')depth++; if(*p==')'){ if(depth==0) break; depth--; }
				if(tla_word(&p,"and",o,"/\\")) continue;
				if(tla_word(&p,"or",o,"\\/")) continue;
				if(!strncmp(p,"==",2)){ fputc('=',o); p+=2; continue; }
				if(!strncmp(p,"!=",2)){ fputs("#",o); p+=2; continue; }
				if(tla_word(&p,"mod",o,"%")) continue;
				if(*p=='/' && p[1]!='\\'){ fputs("\\div ",o); p++; continue; }
				fputc(*p++,o); }
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
	S.ndef=0; for(int i=0;i<ndefs && S.ndef<SP_MAXCONST;i++) snprintf(S.define[S.ndef++],SP_NAME,"%s",defs[i]);
	if(sp_load(path)) return 3;
	FILE *o=stdout;
	fprintf(o,"---- MODULE %s ----\n",S.modname);
	fprintf(o,"\\* Generated from %s by cmc --emit-tla.  Run with TLC,\n",path);
	fprintf(o,"\\* Apalache or TLAPS; a .cfg binds the constants.\n");
	fprintf(o,"EXTENDS Naturals, FiniteSets\n\n");
	/* constants */
	if(S.ncst){ fprintf(o,"CONSTANTS"); for(int i=0;i<S.ncst;i++) fprintf(o,"%s %s",i?",":"",S.cst[i].name); fprintf(o,"\n"); }
	/* variables */
	fprintf(o,"VARIABLES"); for(int i=0;i<S.nvar;i++) fprintf(o,"%s %s",i?",":"",S.var[i].name); fprintf(o,"\n");
	fprintf(o,"vars == <<"); for(int i=0;i<S.nvar;i++) fprintf(o,"%s%s",i?", ":"",S.var[i].name); fprintf(o,">>\n\n");
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
