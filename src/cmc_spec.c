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

static struct {
	char modname[SP_NAME];
	Const cst[SP_MAXCONST]; int ncst;
	Var var[SP_MAXVAR]; int nvar;
	char init[SP_EXPR];
	Action act[SP_MAXACT]; int nact;
	SpInv inv[SP_MAXINV]; int ninv;
	char define[SP_MAXCONST][SP_NAME]; int ndef;	/* -D tags given */
} S;

/* ---- evaluation context: constants + variable values + bound params - */
typedef struct {
	const Value *var;			/* current value of each variable, by index */
	char pname[SP_MAXPARAM][SP_NAME]; long pval[SP_MAXPARAM]; int np;	/* bound params */
	const Value *primed; int have_primed;	/* for x' in assignments (unused in guards) */
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
	S.ncst=S.nvar=S.nact=S.ninv=0; S.init[0]=0;
	if(sp_load_into(path)) return -1;
	if(!S.modname[0]){ fprintf(stderr,"cmc: %s: no module declaration\n",path); return -1; }
	if(S.nvar==0){ fprintf(stderr,"cmc: %s: no variables\n",path); return -1; }
	if(!S.init[0]){ fprintf(stderr,"cmc: %s: no init\n",path); return -1; }
	if(S.ninv==0){ fprintf(stderr,"cmc: %s: no invariant\n",path); return -1; }
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

int cmc_spec_main(const char *path, char **defs, int ndefs){
	S.ndef=0; for(int i=0;i<ndefs && S.ndef<SP_MAXCONST;i++){ snprintf(S.define[S.ndef++],SP_NAME,"%s",defs[i]); }
	if(sp_load(path)) return 3;
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
	unsigned char bb[65536];
	size_t nb=state_bytes(&init,bb,sizeof bb); seen_insert(fnv(bb,nb));
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
				size_t n2=state_bytes(&succ[k],bb,sizeof bb); uint64_t h=fnv(bb,n2);
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
		printf("Model checking completed. No error has been found.\n");
		printf("%zu states generated, %zu distinct states found, 0 states left on queue.\n", generated, distinct);
		printf("The depth of the complete state graph search is %d.\n", maxdepth+1);
		printf("  %d invariant%s held.\n", S.ninv, S.ninv==1?"":"s");
	}
	for(size_t i=0;i<qt;i++) free(q[i]);
	free(q); free(succ); free(seen);
	return ret;
}
