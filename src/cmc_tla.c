/*
 * cmc_tla.c: the TLA+ reader.  `cmc --spec FILE.tla --config FILE.cfg`
 * reads a TLA+ module and its TLC configuration, translates them to the
 * .cml abstract model they mean, and hands that text to the .cml loader
 * (cmc_spec.c), so the checker that runs is the one `cmc --spec
 * FILE.cml` runs.  `cmc --emit-cml` prints the translation.
 *
 * The reader takes the subset of TLA+ that .cml can express, and
 * refuses everything else with the construct and its line (exit 3):
 *
 *   one MODULE; EXTENDS of Naturals, Integers, FiniteSets, Sequences, TLC;
 *   CONSTANT(S) bound by the .cfg; VARIABLE(S); `Op(p, ...) == e` (each
 *   definition sees only the ones before it, so none is recursive);
 *   ASSUME of `C \in Nat`, `Int` or `BOOLEAN`, checked against the .cfg;
 *   Init a conjunction of `x = e`; Next a disjunction of actions, each
 *   a conjunction of guards, `x' = e`, `x' \in S` and UNCHANGED, under
 *   \E parameters; Spec == Init /\ [][Next]_vars /\ WF_vars(A) ...
 *
 * Definitions are expanded where they are used, as TLA+ defines them:
 * each bound name is renamed apart (name__K), so an argument never
 * reads a name the body binds.  Expressions are printed with the
 * fewest parentheses the .cml grammar needs, so a long TLA+ guard stays
 * within the loader's line limits where it can; where it cannot, the
 * loader refuses it.
 *
 * Variables carry no type in TLA+.  A scalar variable becomes `var x :
 * -2147483648 .. 2147483647` (TLC's integers, so a value TLC could not
 * hold is a model error here too), and a function variable `[K -> that
 * range]`, its key set K read from Init's `x = [i \in K |-> e]`.  A
 * value of any other type (a boolean, a set) in a variable is a model
 * error when the checker reaches it.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmc_tla.h"

/* ---- memory: every allocation of one translation, freed together ---- */
typedef struct Blk { struct Blk *next; } Blk;
static Blk *g_blk;
static jmp_buf g_jb;
static char g_err[512];

static void *zalloc(size_t n){
	Blk *b=calloc(1,sizeof *b+n);
	if(!b){ snprintf(g_err,sizeof g_err,"out of memory"); longjmp(g_jb,1); }	/* GCOVR_EXCL_LINE: calloc fails only out of memory */
	b->next=g_blk; g_blk=b;
	return b+1;
}
static void free_all(void){
	while(g_blk){ Blk *n=g_blk->next; free(g_blk); g_blk=n; }
}

/* Refuse the input: a construct outside the subset, or a malformed
 * module or configuration.  file:line names where; the message names
 * what.  Never returns. */
static const char *g_file;
static void refuse(int line, const char *fmt, ...){
	char m[400]; va_list ap; va_start(ap,fmt); vsnprintf(m,sizeof m,fmt,ap); va_end(ap);
	if(line>0) snprintf(g_err,sizeof g_err,"%s:%d: %s",g_file,line,m);
	else snprintf(g_err,sizeof g_err,"%s: %s",g_file,m);
	longjmp(g_jb,1);
}

/* ---- a growable text buffer ----------------------------------------- */
typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *fmt, ...){
	va_list ap; va_start(ap,fmt); int k=vsnprintf(NULL,0,fmt,ap); va_end(ap);
	if(b->n+(size_t)k+1>b->cap){
		size_t nc=b->cap?b->cap:256; while(nc<b->n+(size_t)k+1) nc*=2;
		char *ns=zalloc(nc); if(b->s) memcpy(ns,b->s,b->n); b->s=ns; b->cap=nc;
	}
	va_start(ap,fmt); vsnprintf(b->s+b->n,(size_t)k+1,fmt,ap); va_end(ap);
	b->n+=(size_t)k;
}
static const char *bstr(Buf *b){
	if(!b->s) bput(b,"%s","");
	return b->s;
}

/* =====================================================================
 * the lexer: tokens with their line and column, for the layout rules
 * of /\ and \/ bullet lists
 * ===================================================================== */
enum { K_ID, K_NUM, K_OP, K_SEP, K_END, K_EOF };
typedef struct { int kind; char t[48]; long num; int line, col; } Tok;
typedef struct { Tok *v; int n, cap, i; } Toks;

static void tpush(Toks *ts, int kind, const char *t, size_t len, int line, int col){
	if(ts->n==ts->cap){
		int nc=ts->cap?ts->cap*2:512; Tok *nv=zalloc(sizeof *nv*(size_t)nc);
		if(ts->v) memcpy(nv,ts->v,sizeof *nv*(size_t)ts->n);
		ts->v=nv; ts->cap=nc;
	}
	Tok *k=&ts->v[ts->n++]; k->kind=kind; k->line=line; k->col=col;
	if(len>=sizeof k->t) refuse(line,"a name or number is too long");
	memcpy(k->t,t,len); k->t[len]=0;
}

/* The ASCII spellings of one operator, mapped to the one the parser
 * reads.  Longest first, so `<=>` is not read as `<=` then `>`. */
static const char *const g_ops[][2]={
	{"<=>","<=>"},{"|->","|->"},{"==","=="},{"/\\","/\\"},{"\\/","\\/"},
	{"=>","=>"},{"<=","<="},{"=<","<="},{">=",">="},{"/=","#"},{"~>","~>"},
	{"<<","<<"},{">>",">>"},{"->","->"},{"..",".."},{"[]","[]"},{"<>","<>"},
	{"<-","<-"},{"=","="},{"#","#"},{"<","<"},{">",">"},{"+","+"},{"-","-"},
	{"*","*"},{"%","%"},{"(","("},{")",")"},{"[","["},{"]","]"},{"{","{"},
	{"}","}"},{",",","},{":",":"},{"!","!"},{"'","'"},{"~","~"},{".","."},
	{"@","@"},{"^","^"},{"|","|"},{"&","&"},{"$","$"},{"?","?"},{"/","/"},
};
/* backslash words that mean an operator the parser reads otherwise */
static const char *const g_bsyn[][2]={
	{"\\land","/\\"},{"\\lor","\\/"},{"\\lnot","~"},{"\\neg","~"},
	{"\\leq","<="},{"\\geq",">="},{"\\equiv","<=>"},
};

/* skip a (* ... *) comment, which nests; p is at its "(*" */
static const char *skip_comment(const char *p, int *line, const char **ls){
	int depth=0, start=*line;
	while(*p){
		if(p[0]=='(' && p[1]=='*'){ depth++; p+=2; continue; }
		if(p[0]=='*' && p[1]==')'){ p+=2; if(--depth==0) return p; continue; }
		if(*p=='\n'){ (*line)++; *ls=p+1; }
		p++;
	}
	refuse(start,"unterminated (* comment");
	return p;
}

/* Split text into tokens.  Columns count from 1, as an editor shows
 * them; a tab counts as one column (TLA+ forbids tabs in layout that
 * matters, and SANY reads them so too).  A run of four or more `-` is a
 * K_SEP (the module's header rule), of four or more `=` a K_END. */
static void lex(const char *text, Toks *ts){
	int line=1; const char *ls=text, *p=text;
	while(*p){
		if(*p=='\n'){ line++; p++; ls=p; continue; }
		if(isspace((unsigned char)*p)){ p++; continue; }
		int col=(int)(p-ls)+1;
		if(p[0]=='\\' && p[1]=='*'){ while(*p && *p!='\n') p++; continue; }
		if(p[0]=='(' && p[1]=='*'){ p=skip_comment(p,&line,&ls); continue; }
		if((*p=='-'||*p=='=') && p[1]==*p && p[2]==*p && p[3]==*p){
			char c=*p; while(*p==c) p++;
			tpush(ts,c=='-'?K_SEP:K_END,c=='-'?"----":"====",4,line,col); continue;
		}
		if(*p=='"') refuse(line,"a string literal: strings are not supported");
		if(isdigit((unsigned char)*p)){
			const char *q=p; while(isdigit((unsigned char)*q)) q++;
			if(isalpha((unsigned char)*q) || *q=='_') refuse(line,"the name %.*s: a name that starts with a digit is not supported",(int)(strcspn(p," \t\n()[]{},")),p);
			tpush(ts,K_NUM,p,(size_t)(q-p),line,col);
			errno=0; ts->v[ts->n-1].num=strtol(p,NULL,10);
			if(errno==ERANGE) refuse(line,"the number %s overflows",ts->v[ts->n-1].t);
			p=q; continue;
		}
		if(isalpha((unsigned char)*p) || *p=='_'){
			const char *q=p; while(isalnum((unsigned char)*q) || *q=='_') q++;
			tpush(ts,K_ID,p,(size_t)(q-p),line,col); p=q; continue;
		}
		if(*p=='\\' && isalpha((unsigned char)p[1])){
			const char *q=p+1; while(isalpha((unsigned char)*q)) q++;
			size_t n=(size_t)(q-p); const char *as=NULL;
			for(size_t i=0;i<sizeof g_bsyn/sizeof g_bsyn[0];i++)
				if(strlen(g_bsyn[i][0])==n && !strncmp(p,g_bsyn[i][0],n)) as=g_bsyn[i][1];
			if(as) tpush(ts,K_OP,as,strlen(as),line,col);
			else tpush(ts,K_OP,p,n,line,col);
			p=q; continue;
		}
		size_t i;
		for(i=0;i<sizeof g_ops/sizeof g_ops[0];i++){
			size_t n=strlen(g_ops[i][0]);
			if(!strncmp(p,g_ops[i][0],n)){ tpush(ts,K_OP,g_ops[i][1],strlen(g_ops[i][1]),line,col); p+=n; break; }
		}
		if(i==sizeof g_ops/sizeof g_ops[0]) refuse(line,"unexpected character '%c'",*p);
	}
	tpush(ts,K_EOF,"end of file",11,line,1);
}

/* =====================================================================
 * the syntax tree
 * ===================================================================== */
enum {
	X_NUM, X_ID,		/* 3, x */
	X_CALL,				/* Op(a, ...) (name, args) */
	X_BIN, X_UN,		/* a op b, op a */
	X_AND, X_OR,		/* a /\ b /\ ..., a \/ b \/ ... (bullets or infix) */
	X_QUANT,			/* \A or \E: binders bname[i] \in a[i], body a[nb] */
	X_SET,				/* {a, ...} */
	X_FILTER,			/* {bname[0] \in a[0] : a[1]} */
	X_FUNC,				/* [bname[0] \in a[0] |-> a[1]] */
	X_EXCEPT,			/* [a[0] EXCEPT ![a[1]] = a[2], ...] */
	X_APPLY,			/* a[0][a[1]] */
	X_IF,				/* IF a[0] THEN a[1] ELSE a[2] */
	X_TUPLE,			/* <<a, ...>> */
	X_REC,				/* [bname[i] |-> a[i], ...] */
	X_DOT,				/* a[0].name */
	X_PRIME,			/* a[0]' */
	X_UNCH,				/* UNCHANGED a[0] */
	X_BOX,				/* [][a[0]]_a[1] */
	X_ALWAYS, X_EVENT,	/* []a[0], <>a[0] */
	X_FAIR,				/* WF_a[0](a[1]) (op "WF") or SF */
	X_AT,				/* @, in an EXCEPT value */
	X_PATH,				/* an EXCEPT path: a[i] a key, or NULL and bname[i] a field */
	X_SETMAP,			/* {a[nb] : bname[i] \in a[i], ...}, read only as a type */
	X_FUNSET,			/* [a[0] -> a[1]], read only as a type */
	X_RECSET,			/* [bname[i] : a[i], ...], read only as a type */
};
typedef struct Node {
	int k, line, n, nb;
	char op[16], name[48];
	long num;
	struct Node **a;
	char (*bname)[48];
} Node;

static Node *mk(int k, int line, int n){
	Node *x=zalloc(sizeof *x); x->k=k; x->line=line; x->n=n;
	if(n) x->a=zalloc(sizeof *x->a*(size_t)n);
	return x;
}
/* a growable child list while parsing */
typedef struct { Node **v; int n, cap; } NList;
static void nl_add(NList *l, Node *x){
	if(l->n==l->cap){ int nc=l->cap?l->cap*2:4; Node **nv=zalloc(sizeof *nv*(size_t)nc); if(l->v) memcpy(nv,l->v,sizeof *nv*(size_t)l->n); l->v=nv; l->cap=nc; }
	l->v[l->n++]=x;
}
static Node *mk_list(int k, int line, NList *l){
	Node *x=mk(k,line,0); x->a=l->v; x->n=l->n; return x;
}

/* =====================================================================
 * the parser: precedence climbing over TLA+'s operator table, with the
 * layout rule for bullet lists.  A /\ or \/ in prefix position at
 * column c begins a list; each item ends at the first token at column
 * c or to its left, and the list goes on while that token is the same
 * bullet at column c.
 * ===================================================================== */
typedef struct { Toks *ts; int limit; int end; } Ps;	/* end: index of the token past the unit */
static Tok g_eof={K_EOF,"end of expression",0,0,0};

static Tok *peek(Ps *p){
	Tok *t=&p->ts->v[p->ts->i];
	if(p->ts->i>=p->end || (p->limit && t->col<=p->limit && t->kind!=K_EOF)){ g_eof.line=t->line; return &g_eof; }
	return t;
}
static Tok *next(Ps *p){ Tok *t=peek(p); if(t!=&g_eof && t->kind!=K_EOF) p->ts->i++; return t; }
static int is(Ps *p, const char *s){ Tok *t=peek(p); return (t->kind==K_OP||t->kind==K_ID) && !strcmp(t->t,s); }
static int accept(Ps *p, const char *s){ if(is(p,s)){ next(p); return 1; } return 0; }
static void expect(Ps *p, const char *s){
	Tok *t=peek(p);
	if(!accept(p,s)) refuse(t->line,"expected %s, found %s",s,t->t);
}
static void ident(Ps *p, char *out){
	Tok *t=peek(p);
	if(t->kind!=K_ID) refuse(t->line,"expected a name, found %s",t->t);
	snprintf(out,48,"%s",t->t); next(p);
}

static Node *expr(Ps *p, int minp);

/* keywords that begin a construct outside the subset, refused where
 * they appear */
static const char *const g_refused_kw[]={
	"LET","CHOOSE","CASE","SUBSET","UNION","DOMAIN","ENABLED","LAMBDA",
	"INSTANCE","RECURSIVE","LOCAL","THEOREM","LEMMA","AXIOM","ASSUMPTION",
	"PROPOSITION","COROLLARY","PROOF","BY","QED","CONSTANTS","CONSTANT",
	"VARIABLES","VARIABLE","ASSUME","OTHER","EXCEPT","WITH","SF_","STRING",
};

/* binders: x, y \in S, z \in T (TLA+'s bounded quantifier list) */
static void binders(Ps *p, NList *sets, char (**names)[48], int *nb){
	char tmp[16][48]; int n=0, start=0;
	for(;;){
		if(n==16) refuse(peek(p)->line,"too many bound names");
		ident(p,tmp[n++]);
		if(accept(p,",")) continue;
		Tok *t=peek(p);
		if(!accept(p,"\\in")) refuse(t->line,"an unbounded quantifier (expected \\in after the bound name): only x \\in S is supported");
		Node *s=expr(p,6);
		for(int i=start;i<n;i++) nl_add(sets,s);
		start=n;
		if(!accept(p,",")) break;
	}
	*names=zalloc(sizeof **names*(size_t)n); memcpy(*names,tmp,sizeof tmp[0]*(size_t)n); *nb=n;
}

/* infix operators: precedence (TLA+'s, from Specifying Systems table
 * 6) and whether a chain a op b op c is read (left) or refused */
typedef struct { const char *op; int prec; int chain; } Infix;
static const Infix g_infix[]={
	{"=>",1,0},{"<=>",2,0},{"~>",2,0},{"\\/",3,1},{"/\\",3,1},
	{"=",5,0},{"#",5,0},{"<",5,0},{">",5,0},{"<=",5,0},{">=",5,0},
	{"\\in",5,0},{"\\notin",5,0},{"\\subseteq",5,0},
	{"\\cup",8,1},{"\\union",8,1},{"\\cap",8,1},{"\\intersect",8,1},
	{"..",9,0},{"+",10,1},{"-",10,1},{"%",10,0},
	{"*",13,1},{"\\div",13,1},{"\\o",13,1},{"\\circ",13,1},{"\\X",10,1},{"\\times",10,1},
	{"^",14,0},
};
static const Infix *infix_of(Tok *t){
	if(t->kind!=K_OP) return NULL;
	for(size_t i=0;i<sizeof g_infix/sizeof g_infix[0];i++) if(!strcmp(g_infix[i].op,t->t)) return &g_infix[i];
	return NULL;
}

/* a bullet list: p is at a /\ or \/ in prefix position */
static Node *bullets(Ps *p){
	Tok *b=next(p); int col=b->col, line=b->line, old=p->limit;
	char bul[4]; snprintf(bul,sizeof bul,"%.3s",b->t);
	NList items={0};
	for(;;){
		p->limit=col;
		nl_add(&items,expr(p,0));
		p->limit=old;
		Tok *t=&p->ts->v[p->ts->i];
		if(t->kind==K_OP && !strcmp(t->t,bul) && t->col==col && peek(p)==t){ next(p); continue; }
		break;
	}
	if(items.n==1) return items.v[0];
	return mk_list(!strcmp(bul,"/\\")?X_AND:X_OR,line,&items);
}

/* WF_v(A) / SF_v(A): name is the identifier token's text */
static Node *fairness(Ps *p, Tok *t){
	Node *x=mk(X_FAIR,t->line,2); snprintf(x->op,sizeof x->op,"%.2s",t->t);
	if(t->t[3]){ x->a[0]=mk(X_ID,t->line,0); snprintf(x->a[0]->name,48,"%s",t->t+3); }
	else x->a[0]=expr(p,16);
	expect(p,"("); x->a[1]=expr(p,0); expect(p,")");
	return x;
}

/* a [ ... ] form: p is past the [ */
static Node *bracket(Ps *p, int line){
	Tok *a=peek(p), *b=&p->ts->v[p->ts->i+1];
	if(a->kind==K_ID && b->kind==K_OP && !strcmp(b->t,"|->")){
		NList vals={0}; char nm[16][48]; int n=0;
		do{
			if(n==16) refuse(line,"a record with too many fields");
			ident(p,nm[n++]); expect(p,"|->"); nl_add(&vals,expr(p,0));
		}while(accept(p,","));
		expect(p,"]");
		Node *x=mk_list(X_REC,line,&vals); x->nb=n;
		x->bname=zalloc(sizeof nm[0]*(size_t)n); memcpy(x->bname,nm,sizeof nm[0]*(size_t)n);
		return x;
	}
	if(a->kind==K_ID && b->kind==K_OP && !strcmp(b->t,":")){
		NList sets={0}; char nm[16][48]; int n=0;
		do{
			if(n==16) refuse(line,"a set of records with too many fields");
			ident(p,nm[n++]); expect(p,":"); nl_add(&sets,expr(p,0));
		}while(accept(p,","));
		expect(p,"]");
		Node *x=mk_list(X_RECSET,line,&sets); x->nb=n;
		x->bname=zalloc(sizeof nm[0]*(size_t)n); memcpy(x->bname,nm,sizeof nm[0]*(size_t)n);
		return x;
	}
	if(a->kind==K_ID && b->kind==K_OP && (!strcmp(b->t,"\\in")||!strcmp(b->t,","))){
		NList sets={0}; Node *x=mk(X_FUNC,line,0);
		binders(p,&sets,&x->bname,&x->nb);
		expect(p,"|->"); nl_add(&sets,expr(p,0)); expect(p,"]");
		x->a=sets.v; x->n=sets.n; return x;
	}
	Node *f=expr(p,0);
	if(is(p,"]") && p->ts->v[p->ts->i+1].kind==K_ID && p->ts->v[p->ts->i+1].t[0]=='_')
		refuse(line,"[A]_v outside [][A]_v: not supported");
	if(accept(p,"->")){
		Node *x=mk(X_FUNSET,line,2); x->a[0]=f; x->a[1]=expr(p,0); expect(p,"]");
		return x;
	}
	if(!accept(p,"EXCEPT")) refuse(peek(p)->line,"expected EXCEPT, |-> or \\in in [ ... ], found %s",peek(p)->t);
	NList l={0}; nl_add(&l,f);
	do{
		Tok *bang=peek(p); expect(p,"!");
		NList keys={0}; char nm[16][48]; int n=0;
		while(is(p,"[") || is(p,".")){
			if(n==16) refuse(bang->line,"an EXCEPT path is too long");
			if(accept(p,".")){ nl_add(&keys,NULL); ident(p,nm[n++]); continue; }
			next(p); nl_add(&keys,expr(p,0)); nm[n++][0]=0;
			if(is(p,",")) refuse(peek(p)->line,"![a, b] (a function of a tuple) is not supported");
			expect(p,"]");
		}
		if(!n) refuse(bang->line,"expected [k] or .f after ! in EXCEPT");
		Node *path=mk_list(X_PATH,bang->line,&keys); path->nb=n;
		path->bname=zalloc(sizeof nm[0]*(size_t)n); memcpy(path->bname,nm,sizeof nm[0]*(size_t)n);
		expect(p,"="); nl_add(&l,path); nl_add(&l,expr(p,0));
	}while(accept(p,","));
	expect(p,"]");
	return mk_list(X_EXCEPT,line,&l);
}

static Node *primary(Ps *p);
/* the subscript of [][A]_v or WF_v: `_vars` lexes as one name; `_<<x, y>>`
 * as `_` and a tuple */
static Node *subscript(Ps *p, int line){
	Tok *u=peek(p);
	if(u->kind!=K_ID || u->t[0]!='_') refuse(line,"[][A] needs a subscript: [][A]_vars");
	next(p);
	if(u->t[1]){ Node *v=mk(X_ID,u->line,0); snprintf(v->name,48,"%s",u->t+1); return v; }
	return primary(p);
}

/* a primary expression, with its postfix [k], .f and ' */
static Node *primary(Ps *p){
	Tok *t=peek(p); int line=t->line; Node *x=NULL;
	if(t==&g_eof || t->kind==K_EOF) refuse(line,"expected an expression, found %s",t->t);
	if(accept(p,"<>")){ x=mk(X_EVENT,line,1); x->a[0]=expr(p,4); return x; }
	if(accept(p,"@")){ x=mk(X_AT,line,0); goto post; }
	if(t->kind==K_NUM){ next(p); x=mk(X_NUM,line,0); x->num=t->num; }
	else if(t->kind==K_ID){
		for(size_t i=0;i<sizeof g_refused_kw/sizeof g_refused_kw[0];i++)
			if(!strcmp(t->t,g_refused_kw[i])) refuse(line,"%s is not supported here",t->t);
		if(!strncmp(t->t,"WF_",3)||!strncmp(t->t,"SF_",3)){ next(p); return fairness(p,t); }
		next(p);
		if(!strcmp(t->t,"IF")){
			x=mk(X_IF,line,3); x->a[0]=expr(p,0); expect(p,"THEN"); x->a[1]=expr(p,0); expect(p,"ELSE"); x->a[2]=expr(p,0);
			return x;
		}
		if(!strcmp(t->t,"UNCHANGED")){ x=mk(X_UNCH,line,1); x->a[0]=expr(p,15); return x; }
		x=mk(X_ID,line,0); snprintf(x->name,48,"%s",t->t);
		if(is(p,"(")){
			Tok *lp=peek(p);
			if(lp->line==t->line && lp->col==t->col+(int)strlen(t->t)){
				next(p); NList args={0};
				do nl_add(&args,expr(p,0)); while(accept(p,","));
				expect(p,")");
				Node *c=mk_list(X_CALL,line,&args); snprintf(c->name,48,"%s",t->t); x=c;
			}
		}
	}
	else if(accept(p,"(")){ int old=p->limit; p->limit=0; x=expr(p,0); expect(p,")"); p->limit=old; }
	else if(accept(p,"{")){
		int old=p->limit; p->limit=0;
		Tok *a=peek(p), *b=&p->ts->v[p->ts->i+1];
		if(a->kind==K_ID && b->kind==K_OP && !strcmp(b->t,"\\in")){
			int save=p->ts->i; next(p); next(p); Node *s=expr(p,6);
			if(accept(p,":")){
				x=mk(X_FILTER,line,2); x->nb=1; x->bname=zalloc(48); snprintf(x->bname[0],48,"%s",a->t);
				x->a[0]=s; x->a[1]=expr(p,0); expect(p,"}"); p->limit=old;
				goto post;
			}
			p->ts->i=save;
		}
		NList l={0};
		if(!is(p,"}")){
			nl_add(&l,expr(p,0));
			if(accept(p,":")){
				NList sets={0}; x=mk(X_SETMAP,line,0);
				binders(p,&sets,&x->bname,&x->nb); nl_add(&sets,l.v[0]);
				x->a=sets.v; x->n=sets.n; expect(p,"}"); p->limit=old;
				goto post;
			}
			while(accept(p,",")) nl_add(&l,expr(p,0));
		}
		expect(p,"}"); p->limit=old; x=mk_list(X_SET,line,&l);
	}
	else if(accept(p,"<<")){
		int old=p->limit; p->limit=0; NList l={0};
		if(!is(p,">>")) do nl_add(&l,expr(p,0)); while(accept(p,","));
		expect(p,">>"); p->limit=old; x=mk_list(X_TUPLE,line,&l);
	}
	else if(accept(p,"[]")){
		if(!accept(p,"[")){ x=mk(X_ALWAYS,line,1); x->a[0]=expr(p,4); return x; }
		int old=p->limit; p->limit=0;
		x=mk(X_BOX,line,2); x->a[0]=expr(p,0); expect(p,"]"); p->limit=old;
		x->a[1]=subscript(p,line);
		return x;
	}
	else if(accept(p,"[")){
		int old=p->limit; p->limit=0;
		x=bracket(p,line);
		p->limit=old;
		Tok *u=peek(p);
		if(u->kind==K_ID && u->t[0]=='_') refuse(line,"[A]_v outside [][A]_v: not supported");
	}
	else refuse(line,"unexpected %s",t->t);
post:
	for(;;){
		Tok *q=peek(p);
		if(q->kind!=K_OP) break;
		if(!strcmp(q->t,"[") ){
			next(p); int old=p->limit; p->limit=0;
			Node *k=expr(p,0); if(is(p,",")) refuse(q->line,"f[a, b] (a function of a tuple) is not supported");
			expect(p,"]"); p->limit=old;
			Node *ap=mk(X_APPLY,q->line,2); ap->a[0]=x; ap->a[1]=k; x=ap; continue;
		}
		if(!strcmp(q->t,".")){
			next(p); Node *d=mk(X_DOT,q->line,1); d->a[0]=x; ident(p,d->name); x=d; continue;
		}
		if(!strcmp(q->t,"'")){ next(p); Node *pr=mk(X_PRIME,q->line,1); pr->a[0]=x; x=pr; continue; }
		break;
	}
	return x;
}

/* an expression whose operators bind at least as tightly as minp */
static Node *expr(Ps *p, int minp){
	Tok *t=peek(p); int line=t->line; Node *x;
	if(t->kind==K_OP && (!strcmp(t->t,"/\\")||!strcmp(t->t,"\\/"))) x=bullets(p);
	else if(accept(p,"~")){ x=mk(X_UN,line,1); snprintf(x->op,sizeof x->op,"~"); x->a[0]=expr(p,4); }
	else if(accept(p,"-")){ x=mk(X_UN,line,1); snprintf(x->op,sizeof x->op,"-"); x->a[0]=expr(p,12); }
	else if(is(p,"\\A")||is(p,"\\E")){
		next(p); NList sets={0}; x=mk(X_QUANT,line,0); snprintf(x->op,sizeof x->op,"%.3s",t->t);
		binders(p,&sets,&x->bname,&x->nb); expect(p,":");
		nl_add(&sets,expr(p,0)); x->a=sets.v; x->n=sets.n;
	}
	else if(is(p,"\\AA")||is(p,"\\EE")) refuse(line,"temporal quantifier %s is not supported",t->t);
	else x=primary(p);
	for(;;){
		Tok *o=peek(p); const Infix *in=infix_of(o);
		if(!in){
			if(o->kind==K_OP && o->t[0]=='\\' && strcmp(o->t,"\\A") && strcmp(o->t,"\\E"))
				refuse(o->line,"the operator %s is not supported",o->t);
			break;
		}
		if(in->prec<minp) break;
		next(p);
		Node *r=expr(p,in->prec+1);
		Node *b;
		if(!strcmp(in->op,"/\\")||!strcmp(in->op,"\\/")){
			b=mk(!strcmp(in->op,"/\\")?X_AND:X_OR,o->line,2); b->a[0]=x; b->a[1]=r;
		} else {
			b=mk(X_BIN,o->line,2); snprintf(b->op,sizeof b->op,"%s",in->op); b->a[0]=x; b->a[1]=r;
		}
		/* TLA+ gives no meaning to a op b op2 c where either operator
		 * does not associate with the other (a = b = c, a + b % c,
		 * a /\ b \/ c): SANY rejects it, and so does this reader */
		Tok *q=peek(p); const Infix *i2=infix_of(q);
		if(i2 && i2->prec==in->prec && (!in->chain || !i2->chain || strcmp(in->op,q->t))
		   && !((!strcmp(in->op,"+")||!strcmp(in->op,"-")) && (!strcmp(q->t,"+")||!strcmp(q->t,"-"))))
			refuse(q->line,"%s ... %s needs parentheses",in->op,q->t);
		x=b;
	}
	return x;
}

/* =====================================================================
 * the module: its constants, variables and definitions
 * ===================================================================== */
#define TL_MAX 64
/* a definition; its body is parsed on first use (from tokens b0 to
 * end), so a definition the model never uses may hold any TLA+ */
typedef struct { char name[48]; int line, np; char (*pname)[48]; Node *body; int b0, end; } Def;
/* a constant's value from the .cfg: an integer, a boolean, a model
 * value (an integer TL_MV and up) or a set of those */
enum { CV_NONE, CV_INT, CV_BOOL, CV_SET };
typedef struct { char name[48]; int line, kind, n; long v, set[32]; } Cst;

static Toks g_ts;	/* the module's tokens */
static struct {
	char name[48];
	Cst cst[TL_MAX]; int ncst;
	char var[TL_MAX][48]; int nvar;
	Def def[256]; int ndef;
	Node *assume[TL_MAX]; int nassume;
} M;

/* model values get integers from TL_MV (2^40), past TLC's 32-bit
 * integers, so no integer TLC computes equals one, and a SYMMETRY set
 * of them is a dedicated range */
#define TL_MV 1099511627776L
static char g_mv[32][48]; static int g_nmv;

static int cst_of(const char *n){ for(int i=0;i<M.ncst;i++) if(!strcmp(M.cst[i].name,n)) return i; return -1; }
static int var_of(const char *n){ for(int i=0;i<M.nvar;i++) if(!strcmp(M.var[i],n)) return i; return -1; }
static int def_of(const char *n, int before){
	for(int i=0;i<M.ndef && i<before;i++) if(!strcmp(M.def[i].name,n)) return i;
	return -1;
}

/* An environment maps a name to its meaning where an expression is
 * read: a definition's parameter to its argument (with the argument's
 * own environment: call by name, as TLA+ substitutes), and a bound name
 * to the name it has in the .cml text.  before limits the definitions
 * visible: a body sees only the ones defined above it. */
typedef struct Env {
	const struct Env *up;
	char name[48];
	Node *arg; const struct Env *argenv;	/* a parameter */
	char as[48];							/* a bound name */
	Node *set; const struct Env *se;		/* the set a bound name ranges over */
	int before;
} Env;
/* the bound names in scope in the .cml text being written */
typedef struct Scope { const struct Scope *up; char name[48]; } Scope;

static const Env *env_find(const Env *e, const char *n){
	for(; e; e=e->up) if(e->name[0] && !strcmp(e->name,n)) return e;
	return NULL;
}
static int env_before(const Env *e){ return e ? e->before : M.ndef; }

/* names the .cml expression grammar reserves, which a bound name must
 * not take */
static const char *const g_cmlkw[]={"and","or","not","in","all","some","count","except","mod",
	"TRUE","FALSE","when","eventually","def","if","then","else",NULL};
static int cml_reserved(const char *n){
	for(int i=0;g_cmlkw[i];i++) if(!strcmp(g_cmlkw[i],n)) return 1;
	return 0;
}
static int scope_has(const Scope *s, const char *n){ for(; s; s=s->up) if(!strcmp(s->name,n)) return 1; return 0; }
/* the .cml name for a binder of n: n itself if it is free, else n_K for
 * the first K that is; deterministic in its position, so the same
 * formula reads the same wherever it is written */
static void fresh(const Scope *s, const char *n, char *out){
	snprintf(out,48,"%s",n);
	for(int k=1; scope_has(s,out) || cst_of(out)>=0 || var_of(out)>=0 || cml_reserved(out); k++){
		if(k==1000) refuse(0,"no free name for the bound name %s",n);
		snprintf(out,48,"%.36s_%d",n,k);
	}
}

/* ---- the top level: units are split before parsing ------------------ */
static const char *const g_unitkw[]={"EXTENDS","CONSTANT","CONSTANTS","VARIABLE","VARIABLES",
	"ASSUME","ASSUMPTION","AXIOM","THEOREM","LEMMA","PROPOSITION","COROLLARY","LOCAL","INSTANCE",
	"RECURSIVE","USE","HIDE","PROOF",NULL};
static int unit_kw(const Tok *t){
	if(t->kind==K_SEP || t->kind==K_END) return 1;
	if(t->kind!=K_ID) return 0;
	for(int i=0;g_unitkw[i];i++) if(!strcmp(g_unitkw[i],t->t)) return 1;
	return 0;
}
/* index of the token that opens the definition whose == is at i */
static int def_head(Toks *ts, int i){
	int j=i-1;	/* i is past the module header, so j >= 0 */
	Tok *t=&ts->v[j];
	if(t->kind==K_OP && (!strcmp(t->t,")")||!strcmp(t->t,"]"))){
		const char *open=t->t[0]==')'?"(":"[";
		int d=0;
		for(; j>=0; j--){
			Tok *u=&ts->v[j];
			if(u->kind==K_OP && (!strcmp(u->t,")")||!strcmp(u->t,"]"))) d++;
			if(u->kind==K_OP && (!strcmp(u->t,"(")||!strcmp(u->t,"["))) d--;
			if(d==0) break;
		}
		if(j<1 || strcmp(ts->v[j].t,open) || ts->v[j-1].kind!=K_ID) refuse(t->line,"a definition of this form is not supported");
		return j-1;
	}
	if(t->kind!=K_ID) refuse(t->line,"an infix or prefix operator definition (%s ==) is not supported",t->t);
	if(j>=1 && ts->v[j-1].kind==K_OP && ts->v[j-1].line==t->line && strcmp(ts->v[j-1].t,"==")
	   && !unit_kw(&ts->v[j-1]))
		refuse(t->line,"an infix operator definition (... %s %s ==) is not supported",ts->v[j-1].t,t->t);
	return j;
}

/* words that begin a proof step's line or close a proof: inside a
 * THEOREM's proof, a line at column 1 that starts with one is not a new
 * unit */
static int proof_word(const char *w){
	static const char *const pw[]={"BY","QED","PROOF","OBVIOUS","OMITTED","DEF","DEFS","USE","HIDE",NULL};
	for(int i=0;pw[i];i++) if(!strcmp(pw[i],w)) return 1;
	return 0;
}

static const char *const g_stdmods[]={"Naturals","Integers","FiniteSets","Sequences","TLC",NULL};

/* a comma-separated list of names (CONSTANTS, VARIABLES, EXTENDS) in
 * tokens [i, end); calls add for each */
static void name_list(Toks *ts, int i, int end, const char *what, void (*add)(const Tok *)){
	if(i>=end) refuse(ts->v[i-1].line,"%s with no names",what);
	for(; i<end; i+=2){
		Tok *t=&ts->v[i];
		if(t->kind!=K_ID) refuse(t->line,"%s: expected a name, found %s",what,t->t);
		if(i+1<end && !strcmp(ts->v[i+1].t,"(")) refuse(t->line,"an operator constant %s(...) is not supported",t->t);
		add(t);
		if(i+1<end && strcmp(ts->v[i+1].t,",")) refuse(ts->v[i+1].line,"%s: expected , found %s",what,ts->v[i+1].t);
	}
}
static void taken(const Tok *t){
	if(cst_of(t->t)>=0 || var_of(t->t)>=0 || def_of(t->t,M.ndef)>=0) refuse(t->line,"%s is declared twice",t->t);
}
static void add_cst(const Tok *t){
	taken(t); if(M.ncst==TL_MAX) refuse(t->line,"too many constants");
	Cst *c=&M.cst[M.ncst++]; memset(c,0,sizeof *c); snprintf(c->name,48,"%s",t->t); c->line=t->line;
}
static void add_var(const Tok *t){
	taken(t); if(M.nvar==TL_MAX) refuse(t->line,"too many variables");
	snprintf(M.var[M.nvar++],48,"%s",t->t);
}
static void add_ext(const Tok *t){
	for(int i=0;g_stdmods[i];i++) if(!strcmp(g_stdmods[i],t->t)) return;
	refuse(t->line,"EXTENDS %s: only Naturals, Integers, FiniteSets, Sequences and TLC are supported",t->t);
}

/* a definition in tokens [i, end): Name == e or Name(p, ...) == e */
static void add_def(Toks *ts, int i, int end){
	Tok *n=&ts->v[i];
	if(M.ndef==256) refuse(n->line,"too many definitions");
	taken(n);
	Def *d=&M.def[M.ndef]; memset(d,0,sizeof *d); snprintf(d->name,48,"%s",n->t); d->line=n->line;
	int j=i+1;
	if(!strcmp(ts->v[j].t,"[")) refuse(n->line,"the function definition %s[...] == is not supported (write %s == [x \\in S |-> e])",n->t,n->t);
	if(!strcmp(ts->v[j].t,"(")){
		char tmp[16][48]; int np=0;
		for(j++;; j++){
			Tok *t=&ts->v[j];
			if(t->kind!=K_ID) refuse(t->line,"definition %s: a parameter must be a name, found %s",n->t,t->t);
			if(!strcmp(ts->v[j+1].t,"(")) refuse(t->line,"definition %s: a higher-order parameter %s(_) is not supported",n->t,t->t);
			if(np==16) refuse(t->line,"definition %s: too many parameters",n->t);
			snprintf(tmp[np++],48,"%s",t->t);
			j++;
			if(!strcmp(ts->v[j].t,")")) break;
			if(strcmp(ts->v[j].t,",")) refuse(ts->v[j].line,"definition %s: expected , or ) in the parameters",n->t);
		}
		j++;
		d->np=np; d->pname=zalloc(sizeof tmp[0]*(size_t)np); memcpy(d->pname,tmp,sizeof tmp[0]*(size_t)np);
	}
	/* def_head() found the head from this ==, so ts->v[j] is it */
	d->b0=j+1; d->end=end;
	if(d->b0>=end) refuse(n->line,"definition %s has no body",n->t);
	M.ndef++;
}

/* read the module in ts into M */
static void module(Toks *ts){
	int i=0;
	while(i+2<ts->n && !(ts->v[i].kind==K_SEP && !strcmp(ts->v[i+1].t,"MODULE"))) i++;
	if(i+3>=ts->n || ts->v[i+2].kind!=K_ID) refuse(0,"no ---- MODULE Name ---- header");
	snprintf(M.name,48,"%s",ts->v[i+2].t);
	if(ts->v[i+3].kind!=K_SEP) refuse(ts->v[i+3].line,"expected ---- after MODULE %s",M.name);
	int b=i+4, e=b;
	while(e<ts->n && ts->v[e].kind!=K_END){
		if(ts->v[e].kind==K_SEP && !strcmp(ts->v[e+1].t,"MODULE")) refuse(ts->v[e].line,"a nested MODULE is not supported");
		if(ts->v[e].kind==K_EOF) refuse(ts->v[e].line,"the module has no ==== end line");
		e++;
	}
	/* mark where each unit starts: a keyword, or a definition's head */
	char *start=zalloc((size_t)(e-b)+1);
	/* A THEOREM and its proof are for TLAPS, and state facts about the
	 * rest of the module, so the reader skips them: from the keyword to
	 * the next line that begins at column 1 with a name other than a
	 * proof word (a proof's steps, <1>1. and BY lines, are indented or
	 * start with < or a proof word).  Inside, an == names nothing. */
	char *inproof=zalloc((size_t)(e-b)+1);
	for(int k=b;k<e;k++){
		Tok *t=&ts->v[k];
		if(t->kind!=K_ID || (strcmp(t->t,"THEOREM") && strcmp(t->t,"LEMMA"))) continue;
		int v=k+1;
		while(v<e && !(ts->v[v].col==1 && ts->v[v].kind==K_ID && !proof_word(ts->v[v].t))) inproof[(v++)-b]=1;
		k=v-1;
	}
	for(int k=b;k<e;k++){
		if(inproof[k-b]) continue;
		if(unit_kw(&ts->v[k])) start[k-b]=1;
		if(ts->v[k].kind!=K_OP || strcmp(ts->v[k].t,"==")) continue;
		int h=def_head(ts,k);
		/* ASSUME Name == e: the name belongs to the ASSUME */
		if(h>b && ts->v[h-1].kind==K_ID && !strcmp(ts->v[h-1].t,"ASSUME")) continue;
		start[h-b]=1;
	}
	for(int k=b;k<e;){
		int u=k+1; while(u<e && !start[u-b]) u++;
		Tok *t=&ts->v[k];
		if(!start[k-b]) refuse(t->line,"unexpected %s at the top level",t->t);
		if(t->kind==K_SEP){ if(u>k+1) refuse(t->line,"unexpected %s after a ---- rule",ts->v[k+1].t); }
		else if(!strcmp(t->t,"EXTENDS")) name_list(ts,k+1,u,"EXTENDS",add_ext);
		else if(!strncmp(t->t,"CONSTANT",8)) name_list(ts,k+1,u,t->t,add_cst);
		else if(!strncmp(t->t,"VARIABLE",8)) name_list(ts,k+1,u,t->t,add_var);
		else if(!strcmp(t->t,"THEOREM") || !strcmp(t->t,"LEMMA")){ /* skipped: see inproof */ }
		else if(!strcmp(t->t,"ASSUME")){
			if(M.nassume==TL_MAX) refuse(t->line,"too many ASSUMEs");
			/* ASSUME Name == e names the assumption */
			int a0=k+1;
			if(a0+1<u && ts->v[a0].kind==K_ID && !strcmp(ts->v[a0+1].t,"==")) a0+=2;
			Ps p={ts,0,u}; ts->i=a0; M.assume[M.nassume++]=expr(&p,0);
			if(ts->i<u) refuse(ts->v[ts->i].line,"unexpected %s in ASSUME",ts->v[ts->i].t);
		}
		else if(t->kind==K_ID && !unit_kw(t)) add_def(ts,k,u);
		else refuse(t->line,"%s is not supported",t->t);
		k=u;
	}
}

/* SANY refuses a bound name or a definition's parameter that is
 * already declared where it appears: a variable, a constant, a
 * definition above, a parameter or an enclosing bound name (TLA+ has
 * no shadowing).  Refused here too, so a module TLC would not load is
 * never checked.  in: the names bound around x, n of them; before: the
 * definition being read, which sees itself and the ones above. */
static void no_redeclare(const char *nm, int line, char (*in)[48], int n, int before){
	int dup=cst_of(nm)>=0 || var_of(nm)>=0 || def_of(nm,before+1)>=0;
	for(int i=0;i<n;i++) if(!strcmp(in[i],nm)) dup=1;
	if(dup) refuse(line,"%s is declared again: TLA+ has no shadowing, rename it",nm);
}
static void check_binders(Node *x, char (*in)[48], int n, int before){
	if(!x) return;
	int bnd=x->k==X_QUANT||x->k==X_FILTER||x->k==X_FUNC||x->k==X_SETMAP;
	int m=n+(bnd?x->nb:0);
	char (*inner)[48]=in;
	if(bnd){
		if(m>64) refuse(x->line,"bound names nest too deeply");
		inner=zalloc(sizeof *inner*(size_t)m); if(n) memcpy(inner,in,sizeof *in*(size_t)n);
		for(int i=0;i<x->nb;i++){ no_redeclare(x->bname[i],x->line,inner,n+i,before); snprintf(inner[n+i],48,"%s",x->bname[i]); }
	}
	for(int i=0;i<x->n;i++){
		/* a binder's own sets are read outside its names */
		int out=bnd && i<x->nb;
		check_binders(x->a[i],out?in:inner,out?n:m,before);
	}
}

/* the body of definition di, parsed now if this is its first use */
static Node *def_body(int di){
	Def *d=&M.def[di];
	if(d->body) return d->body;
	int save=g_ts.i;
	Ps p={&g_ts,0,d->end}; g_ts.i=d->b0;
	Node *b=expr(&p,0);
	if(g_ts.i<d->end) refuse(g_ts.v[g_ts.i].line,"unexpected %s in the definition of %s",g_ts.v[g_ts.i].t,d->name);
	for(int i=0;i<d->np;i++) no_redeclare(d->pname[i],d->line,d->pname,i,di);
	check_binders(b,d->pname,d->np,di);
	g_ts.i=save; d->body=b;
	return b;
}

/* =====================================================================
 * the TLC configuration
 * ===================================================================== */
static struct {
	char init[48], next[48], spec[48];
	char inv[TL_MAX][48]; int ninv;
	char prop[TL_MAX][48]; int nprop;
	char con[TL_MAX][48]; int ncon;
	char acon[TL_MAX][48]; int nacon;
	char sym[48]; int line_sym;
	char view[48]; int line_view;
	int deadlock;		/* -1 unset, 0 FALSE, 1 TRUE */
} C;

static const char *const g_cfgkw[]={"CONSTANT","CONSTANTS","INIT","NEXT","SPECIFICATION",
	"INVARIANT","INVARIANTS","PROPERTY","PROPERTIES","CONSTRAINT","CONSTRAINTS",
	"ACTION_CONSTRAINT","ACTION_CONSTRAINTS","SYMMETRY","VIEW","CHECK_DEADLOCK",
	"ALIAS","POSTCONDITION","POSTCONDITIONS",NULL};
static int cfg_kw(const Tok *t){
	if(t->kind!=K_ID) return 0;
	for(int i=0;g_cfgkw[i];i++) if(!strcmp(g_cfgkw[i],t->t)) return 1;
	return 0;
}

/* a model value: the integer TL_MV + its index, interned by name */
static long model_value(const Tok *t){
	for(int i=0;i<g_nmv;i++) if(!strcmp(g_mv[i],t->t)) return TL_MV+i;
	if(g_nmv==32) refuse(t->line,"too many model values");
	snprintf(g_mv[g_nmv],48,"%s",t->t);
	return TL_MV+g_nmv++;
}

/* one constant value at ts->i: an integer (possibly negative), TRUE,
 * FALSE, a model value, or a set of integers or model values */
static void cfg_value(Toks *ts, Cst *c, int line){
	Tok *t=&ts->v[ts->i];
	int neg=t->kind==K_OP && !strcmp(t->t,"-");
	if(neg){ ts->i++; t=&ts->v[ts->i]; }
	if(t->kind==K_NUM){ ts->i++; c->kind=CV_INT; c->v=neg?-t->num:t->num; return; }
	if(neg) refuse(line,"constant %s: expected a number after -",c->name);
	if(t->kind==K_ID && (!strcmp(t->t,"TRUE")||!strcmp(t->t,"FALSE"))){ ts->i++; c->kind=CV_BOOL; c->v=t->t[0]=='T'; return; }
	if(t->kind==K_ID && !cfg_kw(t)){ ts->i++; c->kind=CV_INT; c->v=model_value(t); return; }
	if(t->kind==K_OP && !strcmp(t->t,"{")){
		ts->i++; c->kind=CV_SET; c->n=0;
		while(strcmp(ts->v[ts->i].t,"}")){
			Tok *e=&ts->v[ts->i]; int en=0;
			if(e->kind==K_OP && !strcmp(e->t,"-")){ en=1; ts->i++; e=&ts->v[ts->i]; }
			if(c->n==32) refuse(e->line,"constant %s: the set is too large",c->name);
			if(e->kind==K_NUM) c->set[c->n++]=en?-e->num:e->num;
			else if(e->kind==K_ID && !en && !cfg_kw(e)) c->set[c->n++]=model_value(e);
			else refuse(e->line,"constant %s: a set element must be a number or a model value, not %s",c->name,e->t);
			ts->i++;
			if(!strcmp(ts->v[ts->i].t,",")) ts->i++;
			else if(strcmp(ts->v[ts->i].t,"}")) refuse(ts->v[ts->i].line,"constant %s: expected , or } in the set",c->name);
		}
		ts->i++; return;
	}
	refuse(line,"constant %s: the value %s is not supported (an integer, TRUE, FALSE, a model value or a set of those)",c->name,t->t);
}

/* CONSTANT(S) entries: Name = value, Name <- Op (refused) */
static void cfg_constants(Toks *ts){
	while(ts->v[ts->i].kind==K_ID && !cfg_kw(&ts->v[ts->i])){
		Tok *n=&ts->v[ts->i++];
		int ci=cst_of(n->t);
		if(ci<0) refuse(n->line,"the configuration binds %s, which the module does not declare as a constant",n->t);
		Cst *c=&M.cst[ci];
		if(c->kind!=CV_NONE) refuse(n->line,"constant %s is bound twice",n->t);
		Tok *o=&ts->v[ts->i];
		if(!strcmp(o->t,"<-")) refuse(o->line,"constant %s <- ...: substitution is not supported",n->t);
		if(!strcmp(o->t,"[")) refuse(o->line,"a constant override for a module (%s[...]) is not supported",n->t);
		if(strcmp(o->t,"=")) refuse(o->line,"constant %s: expected = after the name",n->t);
		ts->i++;
		cfg_value(ts,c,n->line);
	}
}

/* names after a keyword, into tab (cap TL_MAX) */
static void cfg_names(Toks *ts, const char *kw, char (*tab)[48], int *n){
	int any=0;
	while(ts->v[ts->i].kind==K_ID && !cfg_kw(&ts->v[ts->i])){
		Tok *t=&ts->v[ts->i++];
		if(*n==TL_MAX) refuse(t->line,"too many %s names",kw);
		snprintf(tab[(*n)++],48,"%s",t->t); any=1;
	}
	if(!any) refuse(ts->v[ts->i].line,"%s needs a name",kw);
}
static void cfg_one(Toks *ts, const char *kw, char *out){
	char tab[TL_MAX][48]; int n=0;
	Tok *k=&ts->v[ts->i-1];
	if(out[0]) refuse(k->line,"%s is given twice",kw);
	cfg_names(ts,kw,tab,&n);
	if(n>1) refuse(k->line,"%s takes one name",kw);
	snprintf(out,48,"%s",tab[0]);
}

static void config(const char *text){
	Toks ts={0}; lex(text,&ts);
	memset(&C,0,sizeof C); C.deadlock=-1;
	while(ts.v[ts.i].kind!=K_EOF){
		Tok *t=&ts.v[ts.i++];
		if(!cfg_kw(t)) refuse(t->line,"unexpected %s in the configuration",t->t);
		const char *k=t->t;
		if(!strncmp(k,"CONSTANT",8)) cfg_constants(&ts);
		else if(!strcmp(k,"INIT")) cfg_one(&ts,k,C.init);
		else if(!strcmp(k,"NEXT")) cfg_one(&ts,k,C.next);
		else if(!strcmp(k,"SPECIFICATION")) cfg_one(&ts,k,C.spec);
		else if(!strncmp(k,"INVARIANT",9)) cfg_names(&ts,k,C.inv,&C.ninv);
		else if(!strncmp(k,"PROPERT",7)) cfg_names(&ts,k,C.prop,&C.nprop);
		else if(!strncmp(k,"CONSTRAINT",10)) cfg_names(&ts,k,C.con,&C.ncon);
		else if(!strncmp(k,"ACTION_CONSTRAINT",17)) cfg_names(&ts,k,C.acon,&C.nacon);
		else if(!strcmp(k,"SYMMETRY")){ C.line_sym=t->line; cfg_one(&ts,k,C.sym); }
		else if(!strcmp(k,"VIEW")){ C.line_view=t->line; cfg_one(&ts,k,C.view); }
		else if(!strcmp(k,"CHECK_DEADLOCK")){
			Tok *v=&ts.v[ts.i++];
			if(v->kind!=K_ID || (strcmp(v->t,"TRUE") && strcmp(v->t,"FALSE"))) refuse(v->line,"CHECK_DEADLOCK is TRUE or FALSE");
			C.deadlock=v->t[0]=='T';
		}
		else refuse(t->line,"%s is not supported",k);
	}
	if(C.spec[0] && (C.init[0]||C.next[0])) refuse(0,"the configuration gives both SPECIFICATION and INIT/NEXT");
	if(!C.spec[0] && (!C.init[0]||!C.next[0])) refuse(0,"the configuration needs SPECIFICATION, or INIT and NEXT");
	for(int i=0;i<M.ncst;i++) if(M.cst[i].kind==CV_NONE) refuse(M.cst[i].line,"constant %s has no value in the configuration",M.cst[i].name);
}

/* =====================================================================
 * expressions to .cml text
 * ===================================================================== */
/* the .cml grammar's levels, loosest first: an operand printed at a
 * looser level than its place needs is parenthesized */
enum { L_OR=1, L_AND, L_NOT, L_CMP, L_MOD, L_ADD, L_MUL, L_ATOM };	/* L_MOD: a mod or / term, an operand of only a comparison */
#define CX_PRIME 1		/* an action constraint: x' reads the successor */

/* If x names a definition or a parameter, replace it by what it stands
 * for (the body in the definition's environment, the argument in the
 * caller's) and return 1; else 0. */
static int expand(Node **xp, const Env **ep){
	Node *x=*xp; const Env *e=*ep;
	if(x->k!=X_ID && x->k!=X_CALL) return 0;
	const Env *b=env_find(e,x->name);
	if(b && b->arg){
		if(x->k==X_CALL) refuse(x->line,"%s is a parameter, not an operator: higher-order parameters are not supported",x->name);
		*xp=b->arg; *ep=b->argenv; return 1;
	}
	if(b) return 0;
	int di=def_of(x->name,env_before(e));
	if(di<0){
		if(def_of(x->name,M.ndef)>=0) refuse(x->line,"%s is used before it is defined (recursion is not supported)",x->name);
		return 0;
	}
	Def *d=&M.def[di];
	int na=x->k==X_CALL?x->n:0;
	if(na!=d->np) refuse(x->line,"%s takes %d argument%s, given %d",d->name,d->np,d->np==1?"":"s",na);
	Env *ne=zalloc(sizeof *ne); ne->before=di;
	for(int i=0;i<d->np;i++){
		Env *pe=zalloc(sizeof *pe); snprintf(pe->name,48,"%s",d->pname[i]);
		pe->arg=x->a[i]; pe->argenv=e; pe->up=ne; pe->before=di; ne=pe;
	}
	*xp=def_body(di); *ep=ne;
	return 1;
}
/* expand until x is not a definition or a parameter */
static void expand_all(Node **xp, const Env **ep){ while(expand(xp,ep)) ; }

static int render(Buf *o, Node *x, const Env *e, const Scope *sc, int cx);
static void mv_check(Node *x, const Env *e);
typedef struct Ty Ty;
static Ty *g_at;				/* the type of @ in the EXCEPT value being read */
static Ty *ty_of(Node *x, const Env *e);
static Ty *ty_path(Ty *t, Node *path);
static void render_set(Buf *o, Node *x, const Env *e, const Scope *sc, int cx);

/* x printed at a level of at least need */
static void sub(Buf *o, Node *x, const Env *e, const Scope *sc, int cx, int need){
	Buf t={0}; int l=render(&t,x,e,sc,cx);
	if(l<need) bput(o,"(%s)",bstr(&t)); else bput(o,"%s",bstr(&t));
}
static void num(Buf *o, long v){ if(v<0) bput(o,"(0 - %ld)",-v); else bput(o,"%ld",v); }

/* a constant's value as .cml text, at L_ATOM */
static void cst_text(Buf *o, const Cst *c){
	if(c->kind==CV_BOOL){ bput(o,"%s",c->v?"TRUE":"FALSE"); return; }
	if(c->kind==CV_INT){ num(o,c->v); return; }
	/* a set: as a range when its elements are consecutive, since .cml
	 * reads an action parameter's set up to the next comma */
	long lo=0, hi=0; int run=c->n>0;
	for(int i=0;i<c->n;i++){ if(!i||c->set[i]<lo) lo=c->set[i]; if(!i||c->set[i]>hi) hi=c->set[i]; }
	for(int i=0;i<c->n && run;i++) for(int j=i+1;j<c->n;j++) if(c->set[i]==c->set[j]) run=0;
	if(run && hi-lo+1==c->n){ num(o,lo); bput(o," .. "); num(o,hi); return; }
	bput(o,"{");
	for(int i=0;i<c->n;i++){ if(i) bput(o,", "); num(o,c->set[i]); }
	bput(o,"}");
}

/* a quantifier or a function literal binds name: push its .cml name */
static void bind(const char *name, Node *set, const Env **e, const Scope **sc, char *as){
	fresh(*sc,name,as);
	Env *ne=zalloc(sizeof *ne); snprintf(ne->name,48,"%s",name); snprintf(ne->as,48,"%s",as);
	ne->set=set; ne->se=*e;
	ne->up=*e; ne->before=env_before(*e); *e=ne;
	Scope *ns=zalloc(sizeof *ns); snprintf(ns->name,48,"%s",as); ns->up=*sc; *sc=ns;
}

/* the variable x stands for (through definitions and parameters), or -1 */
static int var_named(Node *x, const Env *e){
	expand_all(&x,&e);
	if(x->k!=X_ID || env_find(e,x->name)) return -1;
	return var_of(x->name);
}

/* the .cml text for an operator of TLA+ that .cml spells otherwise */
static const char *cml_op(const char *op){
	static const char *const map[][2]={{"=","=="},{"#","!="},{"<","<"},{">",">"},{"<=","<="},
		{">=",">="},{"+","+"},{"-","-"},{"*","*"},{"\\div","/"},{"%","mod"},{"\\o","\\o"},{"\\circ","\\o"},{NULL,NULL}};
	for(int i=0;map[i][0];i++) if(!strcmp(map[i][0],op)) return map[i][1];
	return NULL;
}

/* a binary operator: comparisons, arithmetic, membership, => and <=> */
static int render_bin(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	const char *op=x->op, *c=cml_op(op);
	if(!strcmp(op,"\\in")||!strcmp(op,"\\notin")){
		int neg=op[1]=='n';
		if(neg) bput(o,"not (");
		sub(o,x->a[0],e,sc,cx,L_ADD); bput(o," in "); render_set(o,x->a[1],e,sc,cx);
		if(neg){ bput(o,")"); return L_NOT; }
		return L_CMP;
	}
	if(!strcmp(op,"=>")){ bput(o,"not "); sub(o,x->a[0],e,sc,cx,L_NOT); bput(o," or "); sub(o,x->a[1],e,sc,cx,L_NOT); return L_OR; }
	if(!strcmp(op,"<=>")){ sub(o,x->a[0],e,sc,cx,L_ATOM); bput(o," == "); sub(o,x->a[1],e,sc,cx,L_ATOM); return L_CMP; }
	if(!c) refuse(x->line,"the operator %s is not supported",op);
	/* .cml reads \o with + and - */
	int lv=strchr("=#<>",op[0])?L_CMP:(op[0]=='+'||op[0]=='-'||!strcmp(c,"\\o"))?L_ADD:L_MUL;
	/* .cml, as SANY, refuses mod beside another arithmetic operator and
	 * / beside * or mod: their operands are atoms, and they are
	 * parenthesized as an operand of any but a comparison */
	if(!strcmp(c,"mod") || !strcmp(c,"/")){
		sub(o,x->a[0],e,sc,cx,L_ATOM); bput(o," %s ",c); sub(o,x->a[1],e,sc,cx,L_ATOM);
		return L_MOD;
	}
	sub(o,x->a[0],e,sc,cx,lv==L_CMP?L_MOD:lv);
	bput(o," %s ",c);
	sub(o,x->a[1],e,sc,cx,lv==L_CMP?L_MOD:lv+1);
	return lv;
}

/* \A / \E with one or more binders: all x in S : all y in T : P */
static int render_quant(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	const Env *be=e; const Scope *bs=sc;
	for(int i=0;i<x->nb;i++){
		char as[48]; Buf s={0}; render_set(&s,x->a[i],e,sc,cx);
		bind(x->bname[i],x->a[i],&be,&bs,as);
		bput(o,"%s %s in %s : ",x->op[1]=='A'?"all":"some",as,bstr(&s));
	}
	sub(o,x->a[x->nb],be,bs,cx,L_OR);
	return 0;
}

/* Cardinality(S): count x in S : P for {x \in S : P}, else TRUE */
static int render_card(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	if(x->n!=1) refuse(x->line,"Cardinality takes one argument");
	Node *s=x->a[0]; const Env *se=e; expand_all(&s,&se);
	char as[48]; const Env *be=se; const Scope *bs=sc;
	if(s->k==X_FILTER){
		Buf t={0}; render_set(&t,s->a[0],se,sc,cx);
		bind(s->bname[0],s->a[0],&be,&bs,as);
		bput(o,"count %s in %s : ",as,bstr(&t)); sub(o,s->a[1],be,bs,cx,L_OR);
		return 0;
	}
	Buf t={0}; render_set(&t,s,se,sc,cx);
	fresh(sc,"e",as);
	bput(o,"count %s in %s : TRUE",as,bstr(&t));
	return 0;
}

/* IF c THEN a ELSE b: .cml's if, whose else branch, as TLA+'s, takes
 * all it can to its right, so it is parenthesized wherever it is an
 * operand */
static int render_if(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	bput(o,"if "); sub(o,x->a[0],e,sc,cx,L_OR);
	bput(o," then "); sub(o,x->a[1],e,sc,cx,L_OR);
	bput(o," else "); sub(o,x->a[2],e,sc,cx,L_OR);
	return 0;
}

/* an operator call: Cardinality, else refused */
static int render_call(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	if(!strcmp(x->name,"Cardinality")) return render_card(o,x,e,sc,cx);
	static const char *const seqops[]={"Len","Head","Tail","Append","SubSeq"};
	static const int nargs[]={1,1,1,2,3};
	for(int i=0;i<5;i++) if(!strcmp(x->name,seqops[i])){
		if(x->n!=nargs[i]) refuse(x->line,"%s takes %d argument%s",x->name,nargs[i],nargs[i]==1?"":"s");
		bput(o,"%s(",x->name);
		for(int j=0;j<x->n;j++){ if(j) bput(o,", "); sub(o,x->a[j],e,sc,cx,L_OR); }
		bput(o,")"); return L_ATOM;
	}
	refuse(x->line,"the operator %s is not supported",x->name);
	return 0;
}

/* a name: a bound name, a constant, a variable, TRUE or FALSE */
static int render_id(Buf *o, Node *x, const Env *e, int cx){
	const Env *b=env_find(e,x->name);
	if(b){ bput(o,"%s",b->as); return L_ATOM; }
	int ci=cst_of(x->name);
	if(ci>=0){
		const Cst *c=&M.cst[ci];
		if(c->kind==CV_SET){
			/* a set where a value is wanted: an explicit literal (a range
			 * is not a value in the .cml grammar) */
			bput(o,"{"); for(int i=0;i<c->n;i++){ if(i) bput(o,", "); num(o,c->set[i]); } bput(o,"}");
			return L_ATOM;
		}
		if(c->kind==CV_INT && c->v<TL_MV && !cml_reserved(c->name)){ bput(o,"%s",c->name); return L_ATOM; }
		Buf t={0}; cst_text(&t,c); bput(o,"%s",bstr(&t));
		return c->kind==CV_INT && c->v<0 ? L_ADD : L_ATOM;
	}
	if(var_of(x->name)>=0 || !strcmp(x->name,"TRUE") || !strcmp(x->name,"FALSE")){ bput(o,"%s",x->name); return L_ATOM; }
	if(!strcmp(x->name,"Nat")||!strcmp(x->name,"Int")||!strcmp(x->name,"BOOLEAN"))
		refuse(x->line,"the infinite set %s is supported only as the right side of \\in",x->name);
	refuse(x->line,"unknown name %s",x->name);
	(void)cx; return 0;
}

/* x \in Nat / Int / BOOLEAN, as a test that refuses a value of the
 * wrong type when evaluated (a type error is exit 3 in .cml) */
static int render_in_std(Buf *o, Node *lhs, const char *set, const Env *e, const Scope *sc, int cx){
	Buf t={0}; sub(&t,lhs,e,sc,cx,L_ADD);
	if(!strcmp(set,"Nat")) bput(o,"%s >= 0",bstr(&t));
	else if(!strcmp(set,"Int")) bput(o,"%s >= 0 or %s < 0",bstr(&t),bstr(&t));
	else bput(o,"%s == TRUE or %s == FALSE",bstr(&t),bstr(&t));
	return strcmp(set,"Nat") ? L_OR : L_CMP;
}

/* a /\ or \/ list */
static int render_list(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	int and=x->k==X_AND;
	for(int i=0;i<x->n;i++){
		if(i) bput(o,and?" and ":" or ");
		/* and and or associate, so a nested list of the same kind needs
		 * no parentheses; .cml, as TLA+, refuses and beside or */
		Node *y=x->a[i]; const Env *ye=e; expand_all(&y,&ye);
		sub(o,y,ye,sc,cx,y->k==x->k?(and?L_AND:L_OR):L_NOT);
	}
	return and?L_AND:L_OR;
}

/* [x \in S |-> e]; [f EXCEPT ![k] = v, ...] (f a variable) */
static int render_func(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	if(x->nb!=1) refuse(x->line,"a function of more than one argument is not supported");
	Buf s={0}; render_set(&s,x->a[0],e,sc,cx);
	char as[48]; const Env *be=e; const Scope *bs=sc; bind(x->bname[0],x->a[0],&be,&bs,as);
	bput(o,"[%s in %s -> ",as,bstr(&s)); sub(o,x->a[1],be,bs,cx,L_OR); bput(o,"]");
	return L_ATOM;
}
/* [B EXCEPT !p = v, ...]: .cml's [B except !p = v, ...], with @ */
static int render_except(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	bput(o,"["); sub(o,x->a[0],e,sc,cx,L_ATOM); bput(o," except ");
	for(int i=1;i+1<x->n;i+=2){
		Node *pa=x->a[i];
		if(i>1) bput(o,", ");
		bput(o,"!");
		for(int j=0;j<pa->n;j++){
			if(!pa->a[j]){ bput(o,".%s",pa->bname[j]); continue; }
			bput(o,"["); sub(o,pa->a[j],e,sc,cx,L_OR); bput(o,"]");
		}
		/* @ has the type of what the path reaches, for mv_check() */
		Ty *save=g_at; g_at=ty_path(ty_of(x->a[0],e),pa);
		bput(o," = "); sub(o,x->a[i+1],e,sc,cx,L_OR);
		g_at=save;
	}
	bput(o,"]");
	return L_ATOM;
}

/* <<a, ...>> and [f |-> a, ...] */
static int render_tuple(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	bput(o,"<<");
	for(int i=0;i<x->n;i++){ if(i) bput(o,", "); sub(o,x->a[i],e,sc,cx,L_OR); }
	bput(o,">>"); return L_ATOM;
}
static int render_rec(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	bput(o,"[");
	for(int i=0;i<x->nb;i++){ bput(o,"%s%s |-> ",i?", ":"",x->bname[i]); sub(o,x->a[i],e,sc,cx,L_OR); }
	bput(o,"]"); return L_ATOM;
}

/* x as .cml text; returns its level */
static int render(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	expand_all(&x,&e);
	mv_check(x,e);
	switch(x->k){
	case X_NUM:
		if(x->num>=TL_MV) refuse(x->line,"the integer %ld is past TLC's integers",x->num);
		bput(o,"%ld",x->num); return L_ATOM;
	case X_ID: return render_id(o,x,e,cx);
	case X_CALL: return render_call(o,x,e,sc,cx);
	case X_AND: case X_OR: return render_list(o,x,e,sc,cx);
	case X_QUANT: return render_quant(o,x,e,sc,cx);
	case X_IF: return render_if(o,x,e,sc,cx);
	case X_FUNC: return render_func(o,x,e,sc,cx);
	case X_EXCEPT: return render_except(o,x,e,sc,cx);
	case X_SET: case X_FILTER: render_set(o,x,e,sc,cx); return L_ATOM;
	case X_UN:
		if(x->op[0]=='~'){ bput(o,"not "); sub(o,x->a[0],e,sc,cx,L_NOT); return L_NOT; }
		bput(o,"0 - "); sub(o,x->a[0],e,sc,cx,L_MUL); return L_ADD;
	case X_BIN: {
		Node *r=x->a[1]; const Env *re=e; expand_all(&r,&re);
		if((!strcmp(x->op,"\\in")||!strcmp(x->op,"\\notin")) && r->k==X_ID && !env_find(re,r->name)
		   && (!strcmp(r->name,"Nat")||!strcmp(r->name,"Int")||!strcmp(r->name,"BOOLEAN"))){
			if(x->op[1]=='n') refuse(x->line,"\\notin %s is not supported",r->name);
			return render_in_std(o,x->a[0],r->name,e,sc,cx);
		}
		return render_bin(o,x,e,sc,cx); }
	case X_APPLY:
		sub(o,x->a[0],e,sc,cx,L_ATOM); bput(o,"["); sub(o,x->a[1],e,sc,cx,L_OR); bput(o,"]");
		return L_ATOM;
	case X_DOT: sub(o,x->a[0],e,sc,cx,L_ATOM); bput(o,".%s",x->name); return L_ATOM;
	case X_AT: bput(o,"@"); return L_ATOM;
	case X_TUPLE: return render_tuple(o,x,e,sc,cx);
	case X_REC: return render_rec(o,x,e,sc,cx);
	case X_PRIME: {
		int vi=var_named(x->a[0],e);
		if(vi<0) refuse(x->line,"a prime on something other than a variable is not supported");
		if(!(cx&CX_PRIME)) refuse(x->line,"%s' is read where only the current state is: only x' = e, x' \\in S and an ACTION_CONSTRAINT may prime",M.var[vi]);
		bput(o,"%s'",M.var[vi]); return L_ATOM; }
	case X_UNCH: refuse(x->line,"UNCHANGED is supported only as a conjunct of an action"); break;
	case X_FUNSET: case X_RECSET: case X_SETMAP:
		refuse(x->line,"a set of functions, records or sequences is supported only as a variable's type in a type invariant (x \\in [S -> T])"); break;
	case X_BOX: case X_ALWAYS: case X_EVENT: case X_FAIR:
		refuse(x->line,"a temporal formula is supported only as Spec or a PROPERTY"); break;
	}
	refuse(x->line,"this expression is not supported");
	return 0;
}

/* a set: a constant set, a range LO .. HI, a literal {a, ...}; a
 * filter {x \in S : P} is not a .cml set and is refused */
static void render_set(Buf *o, Node *x, const Env *e, const Scope *sc, int cx){
	expand_all(&x,&e);
	if(x->k==X_BIN && !strcmp(x->op,"..")){
		sub(o,x->a[0],e,sc,cx,L_ADD); bput(o," .. "); sub(o,x->a[1],e,sc,cx,L_ADD); return;
	}
	if(x->k==X_SET){
		bput(o,"{"); for(int i=0;i<x->n;i++){ if(i) bput(o,", "); sub(o,x->a[i],e,sc,cx,L_OR); } bput(o,"}"); return;
	}
	if(x->k==X_ID && !env_find(e,x->name) && cst_of(x->name)>=0 && M.cst[cst_of(x->name)].kind==CV_SET){
		cst_text(o,&M.cst[cst_of(x->name)]); return;
	}
	if(x->k==X_ID && (!strcmp(x->name,"Nat")||!strcmp(x->name,"Int")))
		refuse(x->line,"the infinite set %s cannot be enumerated",x->name);
	if(x->k==X_FILTER) refuse(x->line,"the set {x \\in S : P} is supported only inside Cardinality");
	if(x->k==X_BIN && x->op[0]=='\\') refuse(x->line,"the set operator %s is not supported",x->op);
	refuse(x->line,"expected a set (LO .. HI, {a, ...} or a constant set)");
}

/* =====================================================================
 * Next as .cml actions.  Next is read as a disjunction of actions: a
 * disjunct that holds a prime is split at each \/, \E and IF in it
 * (an \E becomes the action's parameters), and what remains is a
 * conjunction of guards, x' = e, x' \in S and UNCHANGED.
 * ===================================================================== */
/* does x (through its definitions) prime a variable, or say UNCHANGED? */
static int has_prime(Node *x, const Env *e){
	expand_all(&x,&e);
	if(x->k==X_PRIME || x->k==X_UNCH) return 1;
	for(int i=0;i<x->n;i++) if(x->a[i] && has_prime(x->a[i],e)) return 1;
	return 0;
}

#define TL_MAXP 8
typedef struct { Node *x; const Env *e; } Item;
typedef struct Work { Item it; const struct Work *next; } Work;
/* one action being built; copied at each split */
typedef struct {
	char name[48];
	int np; char pn[TL_MAXP][48]; const char *pd[TL_MAXP];
	Item guard[TL_MAX]; int ng;
	Item pset[TL_MAXP];		/* each parameter's set, where it is read */
	Item asg[TL_MAX];		/* by variable: the right side, x NULL if none */
	char asg_as[TL_MAX][48];	/* or a parameter: x' \in S */
	const Env *asg_se[TL_MAX];	/* where that S is read */
	int unch[TL_MAX];
	const Scope *sc;
	char chain[16][48]; int chain_np[16]; int nchain;	/* definitions expanded, for WF */
	int line, named;		/* named: a primed conjunct is taken, so the name is fixed */
} Act;

static Act *g_act[64]; static int g_nact;

static Act *act_copy(const Act *a){ Act *n=zalloc(sizeof *n); *n=*a; return n; }
static const Work *push(const Work *w, Node *x, const Env *e){
	Work *n=zalloc(sizeof *n); n->it.x=x; n->it.e=e; n->next=w; return n;
}

static void walk(const Work *w, Act *a);

/* x' \in S: a fresh parameter p over S, and x' = p */
static void walk_in(const Work *w, Act *a, int vi, Node *set, const Env *e, int line){
	if(a->np==TL_MAXP) refuse(line,"too many \\E parameters in one action");
	Buf s={0}; render_set(&s,set,e,a->sc,0);
	char as[48]; const Env *be=e; bind(M.var[vi],set,&be,&a->sc,as);
	a->pset[a->np].x=set; a->pset[a->np].e=e;
	snprintf(a->pn[a->np],48,"%s",as); a->pd[a->np]=bstr(&s); a->np++;
	snprintf(a->asg_as[vi],48,"%s",as); a->asg[vi].x=set; a->asg[vi].e=NULL; a->asg_se[vi]=e;
	walk(w,a);
}

/* one conjunct that primes: an assignment, x' \in S or UNCHANGED */
static void walk_conj(const Work *w, Act *a, Node *x, const Env *e){
	if(x->k==X_UNCH){
		Node *t=x->a[0]; const Env *te=e; expand_all(&t,&te);
		int n=t->k==X_TUPLE?t->n:1;
		for(int i=0;i<n;i++){
			int vi=var_named(t->k==X_TUPLE?t->a[i]:t,te);
			if(vi<0) refuse(x->line,"UNCHANGED of something other than variables is not supported");
			a->unch[vi]=1;
		}
		walk(w,a); return;
	}
	if(x->k==X_BIN && (!strcmp(x->op,"=")||!strcmp(x->op,"\\in"))){
		Node *l=x->a[0]; const Env *le=e; expand_all(&l,&le);
		if(l->k==X_PRIME){
			int vi=var_named(l->a[0],le);
			if(vi<0) refuse(x->line,"a prime on something other than a variable is not supported");
			if(a->asg[vi].x) refuse(x->line,"%s' is given a value twice in one action",M.var[vi]);
			if(has_prime(x->a[1],e)) refuse(x->line,"the value of %s' reads a primed variable: not supported",M.var[vi]);
			if(x->op[0]=='\\'){ walk_in(w,a,vi,x->a[1],e,x->line); return; }
			a->asg[vi].x=x->a[1]; a->asg[vi].e=e;
			walk(w,a); return;
		}
	}
	refuse(x->line,"this action conjunct reads a primed variable but is not x' = e, x' \\in S or UNCHANGED: not supported");
}

/* the next conjunct of w, into a */
static void walk(const Work *w, Act *a){
	if(!w){
		/* a disjunct with a guard FALSE (a constant USE_BUG = FALSE, or
		 * Next == FALSE) is never enabled: no action */
		for(int i=0;i<a->ng;i++){
			Node *g=a->guard[i].x; const Env *ge=a->guard[i].e; expand_all(&g,&ge);
			if(g->k!=X_ID || env_find(ge,g->name)) continue;
			int ci=cst_of(g->name);
			if(!strcmp(g->name,"FALSE") || (ci>=0 && M.cst[ci].kind==CV_BOOL && !M.cst[ci].v)) return;
		}
		if(g_nact==64) refuse(a->line,"Next has too many actions");
		g_act[g_nact++]=act_copy(a); return;
	}
	Node *x=w->it.x; const Env *e=w->it.e; w=w->next;
	/* expand a definition, noting it for WF and, as the innermost one
	 * that primes before the first primed conjunct, as the action's name
	 * (a name only: it labels the trace, and the search ignores it) */
	while(x->k==X_ID || x->k==X_CALL){
		const Env *b=env_find(e,x->name);
		int isdef=!b && def_of(x->name,env_before(e))>=0;
		char nm[48]; snprintf(nm,48,"%s",x->name);
		if(!expand(&x,&e)) break;
		if(isdef) isdef=has_prime(x,e);
		if(isdef && a->nchain==16) refuse(x->line,"definitions nest too deeply in one action");
		if(isdef){ snprintf(a->chain[a->nchain],48,"%s",nm); a->chain_np[a->nchain]=a->np; a->nchain++; }
		if(isdef && !a->named) snprintf(a->name,48,"%s",nm);
	}
	if(!has_prime(x,e)){
		if(a->ng==TL_MAX) refuse(x->line,"too many guards in one action");
		a->guard[a->ng].x=x; a->guard[a->ng].e=e; a->ng++;
		walk(w,a); return;
	}
	if(x->k==X_AND){ a->named=1; for(int i=x->n-1;i>=0;i--) w=push(w,x->a[i],e); walk(w,a); return; }
	if(x->k==X_OR){ for(int i=0;i<x->n;i++) walk(push(w,x->a[i],e),act_copy(a)); return; }
	if(x->k==X_IF){
		Node *nc=mk(X_UN,x->line,1); snprintf(nc->op,sizeof nc->op,"~"); nc->a[0]=x->a[0];
		walk(push(push(w,x->a[1],e),x->a[0],e),act_copy(a));
		walk(push(push(w,x->a[2],e),nc,e),act_copy(a));
		return;
	}
	if(x->k==X_QUANT && x->op[1]=='E'){
		const Env *be=e;
		for(int i=0;i<x->nb;i++){
			if(a->np==TL_MAXP) refuse(x->line,"too many \\E parameters in one action");
			Buf s={0}; render_set(&s,x->a[i],e,a->sc,0);
			char as[48]; bind(x->bname[i],x->a[i],&be,&a->sc,as);
			a->pset[a->np].x=x->a[i]; a->pset[a->np].e=e;
			snprintf(a->pn[a->np],48,"%s",as); a->pd[a->np]=bstr(&s); a->np++;
		}
		walk(push(w,x->a[x->nb],be),a); return;
	}
	a->named=1;
	walk_conj(w,a,x,e);
}

/* =====================================================================
 * types.  TLA+ variables carry none, and .cml needs one for each, so
 * the reader infers them: a variable's type is what Init gives it and
 * every action assigns to it, unified.  The types are .cml's: an
 * integer, a function from integers, a record, a sequence.  A boolean
 * or a set in a variable is refused, as are two types that do not
 * unify (TLC would compare them and stop with an error).
 * ===================================================================== */
enum { TY_U, TY_I, TY_B, TY_SET, TY_F, TY_R, TY_S, TY_MV, TY_IM };	/* TY_IM: an integer or a model value */
struct Ty {
	int k;
	struct Ty *sub;				/* TY_F: the range; TY_S: the element */
	Node *dom; const Env *de;	/* TY_F: the key set, where known */
	int nf; char (*fn)[48]; struct Ty **ft;	/* TY_R: fields */
};
static Ty *ty_new(int k){ Ty *t=zalloc(sizeof *t); t->k=k; return t; }
static Ty *g_vty[TL_MAX];		/* each variable's type */

static const char *ty_word(int k){
	static const char *const w[]={"unknown","an integer","a boolean","a set","a function","a record","a sequence","a model value","an integer or a model value"};
	return w[k];
}
static int ty_occurs(const Ty *t, const Ty *u);
/* unify b into a (a is changed); refuse at line on a conflict */
static void ty_unify(Ty *a, Ty *b, int line, const char *what){
	if(b->k==TY_U) return;
	if(a->k==TY_U){
		if(ty_occurs(b,a)) refuse(line,"%s would hold itself: a recursive type is not supported",what);
		*a=*b; return;
	}
	/* TLC lets a variable hold an integer in one state and a model
	 * value in another (x = 0, then x' = p): both are held as integers */
	int ia=a->k==TY_I||a->k==TY_MV||a->k==TY_IM, ib=b->k==TY_I||b->k==TY_MV||b->k==TY_IM;
	if(ia && ib){ if(a->k!=b->k) a->k=TY_IM; return; }
	if(a->k!=b->k) refuse(line,"%s is %s in one place and %s in another",what,ty_word(a->k),ty_word(b->k));
	if(a->k==TY_F){ if(!a->dom){ a->dom=b->dom; a->de=b->de; } ty_unify(a->sub,b->sub,line,what); }
	if(a->k==TY_S) ty_unify(a->sub,b->sub,line,what);
	if(a->k==TY_R){
		if(a->nf!=b->nf) refuse(line,"%s is a record with different fields in two places",what);
		for(int i=0;i<a->nf;i++){
			int j=0; while(j<b->nf && strcmp(a->fn[i],b->fn[j])) j++;
			if(j==b->nf) refuse(line,"%s is a record with different fields in two places",what);
			ty_unify(a->ft[i],b->ft[j],line,what);
		}
	}
}

/* does t hold u (so u = t would make an infinite type)? */
static int ty_occurs(const Ty *t, const Ty *u){
	if(t==u) return 1;
	if((t->k==TY_F||t->k==TY_S) && ty_occurs(t->sub,u)) return 1;
	for(int i=0;t->k==TY_R && i<t->nf;i++) if(ty_occurs(t->ft[i],u)) return 1;
	return 0;
}
static void ty_join(Ty *a, Ty *b, int line, const char *what){
	if(a!=b && (ty_occurs(b,a) || ty_occurs(a,b))) refuse(line,"%s would hold itself: a recursive type is not supported",what);
	ty_unify(a,b,line,what);
}

/* a bound name for typing, over set */
static const Env *bind_ty(const Env *e, const char *name, Node *set){
	Env *ne=zalloc(sizeof *ne); snprintf(ne->name,48,"%s",name); snprintf(ne->as,48,"%s",name);
	ne->set=set; ne->se=e; ne->up=e; ne->before=env_before(e); return ne;
}


/* The type of an element of the set x: an integer, or a model value
 * for a constant set of model values.  TY_U where it does not show,
 * and for a .cfg set that mixes the two. */
static Ty *ty_elem(Node *x, const Env *e){
	expand_all(&x,&e);
	if(x->k==X_BIN && !strcmp(x->op,"..")) return ty_new(TY_I);
	if(x->k==X_ID && !env_find(e,x->name) && cst_of(x->name)>=0){
		const Cst *c=&M.cst[cst_of(x->name)];
		int nmv=0;
		for(int i=0;c->kind==CV_SET && i<c->n;i++) nmv+=c->set[i]>=TL_MV;
		if(c->kind==CV_SET && c->n) return ty_new(nmv==c->n?TY_MV:nmv?TY_U:TY_I);
	}
	if(x->k==X_SET){
		Ty *t=ty_new(TY_U);
		for(int i=0;i<x->n;i++) ty_join(t,ty_of(x->a[i],e),x->line,"an element of a set");
		return t;
	}
	return ty_new(TY_U);
}

/* the type the EXCEPT path reaches in t, or a fresh unknown */
static Ty *ty_path(Ty *t, Node *path){
	for(int i=0;i<path->n && t;i++){
		if(path->a[i]){ t=(t->k==TY_F||t->k==TY_S) ? t->sub : NULL; continue; }
		Ty *f=NULL;
		for(int j=0;t->k==TY_R && j<t->nf;j++) if(!strcmp(t->fn[j],path->bname[i])) f=t->ft[j];
		t=f;
	}
	return t ? t : ty_new(TY_U);
}

/* Cardinality, Len, Head, Tail, Append, SubSeq */
static Ty *ty_call(Node *x, const Env *e){
	const char *n=x->name;
	if(!strcmp(n,"Cardinality")||!strcmp(n,"Len")) return ty_new(TY_I);
	if(!x->n) return ty_new(TY_U);
	Ty *s=ty_of(x->a[0],e);
	if(!strcmp(n,"Head")) return s->k==TY_S ? s->sub : ty_new(TY_U);
	if(!strcmp(n,"Tail")||!strcmp(n,"SubSeq")) return s;
	if(!strcmp(n,"Append") && x->n==2){
		Ty *t=ty_new(TY_S); t->sub=ty_new(TY_U);
		ty_join(t,s,x->line,"Append's sequence");
		ty_join(t->sub,ty_of(x->a[1],e),x->line,"an element Append adds");
		return t;
	}
	return ty_new(TY_U);
}

/* the type of x, as far as it shows; TY_U where it does not */
static Ty *ty_of(Node *x, const Env *e){
	expand_all(&x,&e);
	switch(x->k){
	case X_NUM: return ty_new(TY_I);
	case X_ID: {
		const Env *b=env_find(e,x->name);
		if(b) return b->set ? ty_elem(b->set,b->se) : ty_new(TY_U);
		int ci=cst_of(x->name), vi=var_of(x->name);
		if(ci>=0){
			const Cst *c=&M.cst[ci];
			return ty_new(c->kind==CV_BOOL?TY_B:c->kind==CV_SET?TY_SET:c->v>=TL_MV?TY_MV:TY_I);
		}
		if(vi>=0) return g_vty[vi] ? g_vty[vi] : ty_new(TY_U);
		return ty_new(!strcmp(x->name,"TRUE")||!strcmp(x->name,"FALSE")?TY_B:TY_U); }
	case X_PRIME: { int vi=var_named(x->a[0],e); return vi>=0 && g_vty[vi] ? g_vty[vi] : ty_new(TY_U); }
	case X_CALL: return ty_call(x,e);
	case X_AND: case X_OR: case X_QUANT: return ty_new(TY_B);
	case X_UN: return ty_new(x->op[0]=='~'?TY_B:TY_I);
	case X_SET: case X_FILTER: return ty_new(TY_SET);
	case X_BIN: {
		const char *o=x->op;
		if(!strcmp(o,"\\o")){ Ty *a=ty_of(x->a[0],e); ty_join(a,ty_of(x->a[1],e),x->line,"an operand of \\o"); return a; }
		if(strchr("=#<>",o[0]) || !strcmp(o,"\\in") || !strcmp(o,"\\notin") || !strcmp(o,"\\subseteq")) return ty_new(TY_B);
		if(!strcmp(o,"..") || o[0]=='\\') return ty_new(TY_SET);
		return ty_new(TY_I); }
	case X_IF: { Ty *a=ty_of(x->a[1],e); ty_join(a,ty_of(x->a[2],e),x->line,"an IF's two branches"); return a; }
	case X_FUNC: {
		if(x->nb!=1) return ty_new(TY_U);
		Ty *t=ty_new(TY_F); t->dom=x->a[0]; t->de=e; t->sub=ty_of(x->a[1],bind_ty(e,x->bname[0],x->a[0])); return t; }
	case X_EXCEPT: {
		Ty *b=ty_of(x->a[0],e), *save=g_at;
		for(int i=1;i+1<x->n;i+=2){
			g_at=ty_path(b,x->a[i]);
			ty_join(g_at,ty_of(x->a[i+1],e),x->line,"an EXCEPT value");
		}
		g_at=save; return b; }
	case X_AT: return g_at ? g_at : ty_new(TY_U);
	case X_APPLY: { Ty *f=ty_of(x->a[0],e); return f->k==TY_F||f->k==TY_S ? f->sub : ty_new(TY_U); }
	case X_DOT: {
		Ty *r=ty_of(x->a[0],e);
		for(int j=0;r->k==TY_R && j<r->nf;j++) if(!strcmp(r->fn[j],x->name)) return r->ft[j];
		return ty_new(TY_U); }
	case X_TUPLE: {
		Ty *t=ty_new(TY_S); t->sub=ty_new(TY_U);
		for(int i=0;i<x->n;i++) ty_join(t->sub,ty_of(x->a[i],e),x->line,"an element of a sequence");
		return t; }
	case X_REC: {
		Ty *t=ty_new(TY_R); t->nf=x->nb; t->fn=zalloc(sizeof *t->fn*(size_t)x->nb); t->ft=zalloc(sizeof *t->ft*(size_t)x->nb);
		/* fields in the order written: .cml numbers field names in the
		 * order it first reads them, and prints a record in that order */
		for(int i=0;i<x->nb;i++){
			for(int k=0;k<i;k++) if(!strcmp(t->fn[k],x->bname[i])) refuse(x->line,"the record field %s is given twice",x->bname[i]);
			snprintf(t->fn[i],48,"%s",x->bname[i]); t->ft[i]=ty_of(x->a[i],e);
		}
		return t; }
	default: return ty_new(TY_U);
	}
}

/* Model values are held as integers from TL_MV, so an operator that
 * reads one as a number would compute what TLC refuses: arithmetic and
 * order on a model value, and \in Nat or Int (true of the integer,
 * false of a model value).  Refused where the types show it.  = and
 * \in a set need nothing: TLC finds a model value unequal to every
 * integer, and so does the checker, since TL_MV lies past TLC's
 * integers. */
static void mv_check(Node *x, const Env *e){
	if(!g_nmv || (x->k!=X_BIN && x->k!=X_UN)) return;
	Ty *a=ty_of(x->a[0],e), *b=x->k==X_BIN ? ty_of(x->a[1],e) : ty_new(TY_U);
	const char *op=x->op;
	if(!strcmp(op,"\\in") || !strcmp(op,"\\notin")){
		Node *r=x->a[1]; const Env *re=e; expand_all(&r,&re);
		if(a->k>=TY_MV && r->k==X_ID && !env_find(re,r->name) && (!strcmp(r->name,"Nat")||!strcmp(r->name,"Int")))
			refuse(x->line,"%s %s of a model value is not supported",op,r->name);
		return;
	}
	if(strchr("<>+-*%.",op[0]) || !strcmp(op,"\\div"))
		if(a->k>=TY_MV || b->k>=TY_MV) refuse(x->line,"%s on %s: model values have no arithmetic or order",op,ty_word(a->k>=TY_MV?a->k:b->k));
}

/* =====================================================================
 * the specification: Init, Next, fairness, and the .cml text
 * ===================================================================== */
static Node *def_named(const char *n, const char *what){
	int di=def_of(n,M.ndef);
	if(di<0) refuse(0,"the configuration's %s %s is not defined in the module",what,n);
	if(M.def[di].np) refuse(M.def[di].line,"%s %s takes parameters",what,n);
	return def_body(di);
}
/* does x (expanded) list every variable once, as vars == <<...>> does? */
static int all_vars(Node *x){
	const Env *e=NULL; expand_all(&x,&e);
	int seen[TL_MAX]={0}, n=x->k==X_TUPLE?x->n:1;
	for(int i=0;i<n;i++){ int vi=var_named(x->k==X_TUPLE?x->a[i]:x,e); if(vi<0||seen[vi]) return 0; seen[vi]=1; }
	return n==M.nvar;
}

static Node *g_init, *g_next;
static char g_wf[TL_MAX][48]; static int g_wfk[TL_MAX], g_nwf, g_wfline;	/* g_wfk: 1 WF, 2 SF */

/* Spec == Init /\ [][Next]_vars /\ WF_vars(A) /\ ... */
static void spec_formula(Node *x, const Env *e){
	if(x->k==X_ID && !g_init && def_of(x->name,M.ndef)>=0 && !M.def[def_of(x->name,M.ndef)].np
	   && !has_prime(x,e)){ g_init=x; return; }
	expand_all(&x,&e);
	if(x->k==X_AND){ for(int i=0;i<x->n;i++) spec_formula(x->a[i],e); return; }
	if(x->k==X_BOX){
		if(g_next) refuse(x->line,"Spec has a second [][...]_v");
		if(!all_vars(x->a[1])) refuse(x->line,"[][Next]_v: v must be the tuple of all the variables");
		Node *n=x->a[0];
		if(n->k!=X_ID || def_of(n->name,M.ndef)<0) refuse(x->line,"[][Next]_v: Next must be the name of a definition");
		g_next=n; return;
	}
	if(x->k==X_FAIR){
		if(!all_vars(x->a[0])) refuse(x->line,"WF_v(A): v must be the tuple of all the variables");
		Node *a=x->a[1];
		if(a->k!=X_ID || def_of(a->name,M.ndef)<0) refuse(x->line,"WF_v(A): A must be the name of a definition without parameters");
		if(g_nwf==TL_MAX) refuse(x->line,"too many WF conjuncts");
		g_wfk[g_nwf]=x->op[0]=='S'?2:1;
		snprintf(g_wf[g_nwf++],48,"%s",a->name); g_wfline=x->line; return;
	}
	refuse(x->line,"Spec: only Init /\\ [][Next]_vars /\\ WF_vars(A) ... is supported");
}

/* WF_vars(A) and SF_vars(A), as .cml's `fair weak A` and `fair strong
 * A`: A must be exactly one .cml action of Next, with its \E
 * parameters inside it (so it is fair as their disjunction, as .cml
 * makes a parameterized action).  An A that is several actions, or that
 * sits under an \E of Next (fair per parameter value), is refused. */
static void emit_fairness(Buf *o){
	for(int w=0;w<g_nwf;w++){
		int n=0, which=-1;
		for(int a=0;a<g_nact;a++)
			for(int c=0;c<g_act[a]->nchain;c++){
				if(strcmp(g_act[a]->chain[c],g_wf[w])) continue;
				if(g_act[a]->chain_np[c]) refuse(g_wfline,"%s_vars(%s): %s sits under an \\E, so it is fair per parameter value, which .cml does not model",g_wfk[w]==2?"SF":"WF",g_wf[w],g_wf[w]);
				n++; which=a;
			}
		if(n!=1) refuse(g_wfline,"%s_vars(%s): %s is %s, so it is not one .cml action",g_wfk[w]==2?"SF":"WF",g_wf[w],g_wf[w],n?"a disjunction of actions":"not an action of Next");
		bput(o,"fair %s %s\n",g_wfk[w]==2?"strong":"weak",g_act[which]->name);
	}
}

/* give each action a distinct name: the same definition can make two
 * actions (under two disjuncts, or split by an IF) */
static void name_actions(void){
	for(int a=0;a<g_nact;a++){
		if(!g_act[a]->name[0]) snprintf(g_act[a]->name,48,"Next");
		char base[48]; snprintf(base,48,"%s",g_act[a]->name);
		for(int k=2;;k++){
			int dup=0;
			for(int b=0;b<a;b++) if(!strcmp(g_act[b]->name,g_act[a]->name)) dup=1;
			if(!dup) break;
			snprintf(g_act[a]->name,48,"%.36s_%d",base,k%1000);
		}
	}
}

/* ASSUME: a conjunction of C \in Nat, Int or BOOLEAN and C = n, checked here
 * against the configuration's values (TLC stops on a false ASSUME) */
static void assume(Node *x){
	const Env *e=NULL; expand_all(&x,&e);
	if(x->k==X_AND){ for(int i=0;i<x->n;i++) assume(x->a[i]); return; }
	if(x->k==X_BIN && !strcmp(x->op,"\\in") && x->a[0]->k==X_ID && x->a[1]->k==X_ID && cst_of(x->a[0]->name)>=0){
		const Cst *c=&M.cst[cst_of(x->a[0]->name)]; const char *s=x->a[1]->name; int ok=0;
		if(!strcmp(s,"Nat")) ok=c->kind==CV_INT && c->v>=0 && c->v<TL_MV;
		else if(!strcmp(s,"Int")) ok=c->kind==CV_INT && c->v<TL_MV;
		else if(!strcmp(s,"BOOLEAN")) ok=c->kind==CV_BOOL;
		else refuse(x->line,"ASSUME %s \\in %s: only Nat, Int and BOOLEAN are supported",c->name,s);
		if(!ok) refuse(x->line,"ASSUME %s \\in %s is false for the configuration's value",c->name,s);
		return;
	}
	/* C = n, as the ConstOK `cmc --emit-tla` writes for TLAPS */
	if(x->k==X_BIN && !strcmp(x->op,"=") && x->a[0]->k==X_ID && cst_of(x->a[0]->name)>=0){
		const Cst *c=&M.cst[cst_of(x->a[0]->name)]; Node *r=x->a[1]; long v;
		if(r->k==X_NUM) v=r->num;
		else if(r->k==X_UN && r->op[0]=='-' && r->a[0]->k==X_NUM) v=-r->a[0]->num;
		else refuse(x->line,"ASSUME %s = e: only an integer literal is supported",c->name);
		if(c->kind!=CV_INT || c->v!=v) refuse(x->line,"ASSUME %s = %ld is false for the configuration's value",c->name,v);
		return;
	}
	refuse(x->line,"this ASSUME is not supported (only a conjunction of C \\in Nat, Int or BOOLEAN, and C = n)");
}

/* a variable's .cml type, as .cml text.  An integer is any of TLC's
 * (so a value TLC could not hold is a model error here too), and a
 * sequence holds at most SEQMAX elements, all .cml can hold: a longer
 * one is a model error (exit 3), never cut short. */
#define TL_INTS "0 - 2147483648 .. 2147483647"
#define TL_SEQMAX 12
static void ty_text(Buf *o, const Ty *t, const char *var){
	switch(t->k){
	case TY_I: bput(o,TL_INTS); return;
	/* every model value, so an integer stored here is a model error */
	case TY_MV: num(o,TL_MV); bput(o," .. "); num(o,TL_MV+g_nmv-1); return;
	/* shortcut: also admits integers from 2^31 to TL_MV, which only
	 * arithmetic past TLC's integers stores (TLC stops there with an
	 * overflow); a set type of the two ranges would close it */
	case TY_IM: bput(o,"0 - 2147483648 .. "); num(o,TL_MV+g_nmv-1); return;
	/* a TY_F comes only from [x \\in S |-> e], so it has its key set */
	case TY_F:
		bput(o,"["); render_set(o,t->dom,t->de,NULL,0); bput(o," -> "); ty_text(o,t->sub,var); bput(o,"]"); return;
	case TY_S: bput(o,"Seq("); ty_text(o,t->sub,var); bput(o,", %d)",TL_SEQMAX); return;
	case TY_R:
		bput(o,"[");
		for(int i=0;i<t->nf;i++){ bput(o,"%s%s : ",i?", ":"",t->fn[i]); ty_text(o,t->ft[i],var); }
		bput(o,"]"); return;
	/* a part nothing gives a value to, such as the elements of a
	 * sequence that stays empty: an integer, which the checker tests
	 * every stored value against, so a value of another type is a model
	 * error, never a state */
	case TY_U: bput(o,TL_INTS); return;
	default: refuse(0,"the variable %s holds %s: .cml variables hold integers, functions, records and sequences",var,ty_word(t->k));
	}
}

/* Init into .cml: `var x : T` for each variable and `init x == e` */
static Item g_initv[TL_MAX];	/* Init's value for each variable */
static void emit_init(Buf *o, Node *x, const Env *e, int *set){
	expand_all(&x,&e);
	if(x->k==X_AND){ for(int i=0;i<x->n;i++) emit_init(o,x->a[i],e,set); return; }
	if(x->k==X_BIN && !strcmp(x->op,"=")){
		int vi=var_named(x->a[0],e);
		if(vi>=0){
			if(set[vi]) refuse(x->line,"Init gives %s a value twice",M.var[vi]);
			set[vi]=1;
			g_initv[vi].x=x->a[1]; g_initv[vi].e=e;
			Buf t={0}; sub(&t,x->a[1],e,NULL,0,L_ADD);
			/* the .cml loader splits init at " and " */
			if(strstr(bstr(&t)," and ")) refuse(x->line,"Init's value for %s holds a conjunction, which .cml's init cannot",M.var[vi]);
			bput(o,"init %s == %s\n",M.var[vi],bstr(&t));
			return;
		}
	}
	if(x->k==X_BIN && !strcmp(x->op,"\\in") && var_named(x->a[0],e)>=0)
		refuse(x->line,"a nondeterministic Init (x \\in S) is not supported");
	refuse(x->line,"Init must be a conjunction of x = e, one per variable");
}

/* one .cml action */
static void emit_act(Buf *o, Act *a){
	bput(o,"action %s",a->name);
	for(int i=0;i<a->np;i++){
		if(strpbrk(a->pd[i],"[:")) refuse(a->line,"action %s: the set of parameter %s holds [ or :, which .cml cannot read in a parameter set; use a range, a literal or a constant",a->name,a->pn[i]);
		bput(o,"%s%s in %s",i?", ":"(",a->pn[i],a->pd[i]);
	}
	bput(o,"%s:\n",a->np?")":"");
	if(a->ng){
		bput(o,"  when ");
		for(int i=0;i<a->ng;i++){ if(i) bput(o," and "); sub(o,a->guard[i].x,a->guard[i].e,a->sc,0,L_AND); }
		bput(o,"\n");
	}
	for(int v=0;v<M.nvar;v++){
		if(a->asg[v].x && a->unch[v]) refuse(a->line,"action %s: %s is both given a value and UNCHANGED",a->name,M.var[v]);
		if(!a->asg[v].x && !a->unch[v]) refuse(a->line,"action %s does not say what %s' is (add it to UNCHANGED)",a->name,M.var[v]);
		if(!a->asg[v].x) continue;
		if(!a->asg[v].e) bput(o,"  %s' = %s\n",M.var[v],a->asg_as[v]);
		else { bput(o,"  %s' = ",M.var[v]); sub(o,a->asg[v].x,a->asg[v].e,a->sc,0,L_OR); bput(o,"\n"); }
	}
}

/* a PROPERTY: <>P or P ~> Q */
static void emit_prop(Buf *o, const char *name){
	Node *x=def_named(name,"PROPERTY"); const Env *e=NULL; expand_all(&x,&e);
	bput(o,"property %s: ",name);
	if(x->k==X_EVENT){ bput(o,"eventually "); sub(o,x->a[0],e,NULL,0,L_OR); }
	else if(x->k==X_BIN && !strcmp(x->op,"~>")){ sub(o,x->a[0],e,NULL,0,L_AND); bput(o," ~> "); sub(o,x->a[1],e,NULL,0,L_AND); }
	else if(x->k==X_BOX && all_vars(x->a[1])){ bput(o,"always ["); sub(o,x->a[0],e,NULL,CX_PRIME,L_OR); bput(o,"]_vars"); }
	else refuse(x->line,"PROPERTY %s: only <>P, P ~> Q and [][A]_vars are supported",name);
	bput(o,"\n");
}
/* an INVARIANT, CONSTRAINT or ACTION_CONSTRAINT, as `kw name: e` */
static void emit_named(Buf *o, const char *kw, const char *name, const char *what, int cx){
	Node *x=def_named(name,what);
	bput(o,"%s %s: ",kw,name); sub(o,x,NULL,NULL,cx,L_OR); bput(o,"\n");
}

/* SYMMETRY S with S == Permutations(C), C a constant set of model
 * values: .cml's `symmetry` over their integers, which nothing else in
 * the model can hold (model values have no arithmetic) */
static void emit_sym(Buf *o){
	Node *x=def_named(C.sym,"SYMMETRY"); const Env *e=NULL; expand_all(&x,&e);
	if(x->k!=X_CALL || strcmp(x->name,"Permutations") || x->n!=1)
		refuse(C.line_sym,"SYMMETRY %s: only Permutations(C), C a constant set of model values, is supported",C.sym);
	Node *s=x->a[0]; expand_all(&s,&e);
	int ci=s->k==X_ID ? cst_of(s->name) : -1;
	if(ci<0 || M.cst[ci].kind!=CV_SET) refuse(C.line_sym,"SYMMETRY %s: Permutations of something other than a constant set",C.sym);
	for(int i=0;i<M.cst[ci].n;i++) if(M.cst[ci].set[i]<TL_MV) refuse(C.line_sym,"SYMMETRY %s: %s holds an integer, not only model values",C.sym,M.cst[ci].name);
	bput(o,"symmetry "); cst_text(o,&M.cst[ci]); bput(o,"\n");
}

/* each variable's type: what Init and every assignment give it.  Three
 * passes reach the fixed point of a model whose variables' types refer
 * to each other at most two deep; a part still unknown is refused. */
static void infer(void){
	for(int i=0;i<M.nvar;i++) g_vty[i]=ty_new(TY_U);
	for(int pass=0;pass<3;pass++)
		for(int v=0;v<M.nvar;v++){
			char what[64]; snprintf(what,sizeof what,"the variable %s",M.var[v]);
			ty_join(g_vty[v],ty_of(g_initv[v].x,g_initv[v].e),g_initv[v].x->line,what);
			for(int a=0;a<g_nact;a++){
				Item *it=&g_act[a]->asg[v];
				if(!it->x) continue;
				if(!it->e) ty_join(g_vty[v],ty_elem(it->x,g_act[a]->asg_se[v]),it->x->line,what);
				else ty_join(g_vty[v],ty_of(it->x,it->e),it->x->line,what);
			}
		}
}

/* ---- declared types: a type invariant ------------------------------
 * An INVARIANT whose conjuncts are all `x \in T`, a variable in a
 * bounded set T, is a type invariant (TypeOK).  T becomes x's .cml type,
 * in place of the one inferred, so the domain the checker enforces on
 * every state is the one the user wrote; the invariant is still
 * checked.  T is LO .. HI or a literal set of integers, [S -> T] (a
 * function), [a : T, ...] (a record), or {<<>>} \cup {<<e1>> : e1 \in
 * T} \cup ... up to k elements (Seq(T, k), the form `cmc --emit-tla`
 * writes).  Any other T is no type, and the invariant is an ordinary
 * one. */
static const char *g_decl[TL_MAX];	/* each variable's declared .cml type, or NULL */

static int decl_type(Buf *o, Node *t, const Env *e);

/* flatten a \cup chain into l */
static void cup_list(Node *t, const Env *e, NList *l){
	expand_all(&t,&e);
	if(t->k==X_BIN && !strcmp(t->op,"\\cup")){ cup_list(t->a[0],e,l); cup_list(t->a[1],e,l); return; }
	nl_add(l,t);
}
/* the sequences of 0 .. k elements of one type T, as `cmc --emit-tla`
 * writes Seq(T, k): {<<>>}, then {<<e1, ..., ei>> : e1 \in T, ..., ei \in
 * T} for each i in order.  Returns 1 and writes Seq(T, k), else 0. */
static int decl_seq(Buf *o, Node *t, const Env *e){
	NList l={0}; cup_list(t,e,&l);
	Node *z=l.v[0];
	if(z->k!=X_SET || z->n!=1 || z->a[0]->k!=X_TUPLE || z->a[0]->n) return 0;
	Buf elem={0};
	for(int i=1;i<l.n;i++){
		Node *m=l.v[i];
		if(m->k!=X_SETMAP || m->nb!=i || m->a[i]->k!=X_TUPLE || m->a[i]->n!=i) return 0;
		for(int j=0;j<i;j++){
			Node *b=m->a[i]->a[j];
			if(b->k!=X_ID || strcmp(b->name,m->bname[j])) return 0;
			Buf t1={0}; if(!decl_type(&t1,m->a[j],e)) return 0;
			if(!elem.s) bput(&elem,"%s",bstr(&t1));
			else if(strcmp(bstr(&elem),bstr(&t1))) return 0;
		}
	}
	if(l.n<2) return 0;
	bput(o,"Seq(%s, %d)",bstr(&elem),l.n-1);
	return 1;
}

/* T as a .cml type into o; 0 if it is not one */
static int decl_type(Buf *o, Node *t, const Env *e){
	expand_all(&t,&e);
	if(t->k==X_BIN && !strcmp(t->op,"..")){ render_set(o,t,e,NULL,0); return 1; }
	if(t->k==X_SET){
		for(int i=0;i<t->n;i++) if(ty_of(t->a[i],e)->k!=TY_I) return 0;
		render_set(o,t,e,NULL,0); return 1;
	}
	if(t->k==X_ID && !env_find(e,t->name) && cst_of(t->name)>=0 && M.cst[cst_of(t->name)].kind==CV_SET){
		render_set(o,t,e,NULL,0); return 1;
	}
	if(t->k==X_FUNSET){
		Buf d={0}; render_set(&d,t->a[0],e,NULL,0);
		bput(o,"[%s -> ",bstr(&d));
		if(!decl_type(o,t->a[1],e)) return 0;
		bput(o,"]"); return 1;
	}
	if(t->k==X_RECSET){
		bput(o,"[");
		for(int i=0;i<t->nb;i++){ bput(o,"%s%s : ",i?", ":"",t->bname[i]); if(!decl_type(o,t->a[i],e)) return 0; }
		bput(o,"]"); return 1;
	}
	if(t->k==X_BIN && !strcmp(t->op,"\\cup")) return decl_seq(o,t,e);
	return 0;
}

/* does x (expanded) read a variable? */
static int reads_var(Node *x, const Env *e){
	expand_all(&x,&e);
	if(x->k==X_ID && var_named(x,e)>=0) return 1;
	for(int i=0;i<x->n;i++) if(x->a[i] && reads_var(x->a[i],e)) return 1;
	return 0;
}

/* Is the invariant named n a type invariant?  If so, record each
 * variable's declared type and return 1. */
static int type_invariant(const char *n){
	Node *x=def_named(n,"INVARIANT"); const Env *e=NULL; expand_all(&x,&e);
	NList cj={0};
	if(x->k==X_AND) for(int i=0;i<x->n;i++) nl_add(&cj,x->a[i]); else nl_add(&cj,x);
	const char *decl[TL_MAX]={0};
	for(int i=0;i<cj.n;i++){
		Node *c=cj.v[i]; const Env *ce=e; expand_all(&c,&ce);
		if(c->k!=X_BIN || strcmp(c->op,"\\in")) return 0;
		int vi=var_named(c->a[0],ce);
		if(vi<0 || decl[vi]) return 0;
		/* a type is fixed before any state: a T that reads a variable
		 * is no type, and the invariant is checked as written */
		if(reads_var(c->a[1],ce)) return 0;
		Buf t={0};
		if(!decl_type(&t,c->a[1],ce)) return 0;
		decl[vi]=bstr(&t);
	}
	for(int v=0;v<M.nvar;v++) if(decl[v]) g_decl[v]=decl[v];
	return 1;
}

/* the whole model, after module() and config() */
static void emit(Buf *o, const char *path){
	for(int i=0;i<M.nassume;i++) assume(M.assume[i]);
	Node *init, *next;
	if(C.spec[0]){
		spec_formula(def_named(C.spec,"SPECIFICATION"),NULL);
		if(!g_init || !g_next) refuse(0,"SPECIFICATION %s must be Init /\\ [][Next]_vars",C.spec);
		init=g_init; next=g_next;
	} else { init=mk(X_ID,0,0); snprintf(init->name,48,"%s",C.init); next=mk(X_ID,0,0); snprintf(next->name,48,"%s",C.next); }
	(void)def_named(init->name,"INIT"); (void)def_named(next->name,"NEXT");
	bput(o,"# translated from %s by cmc\nmodule %s\n",path,M.name);
	for(int i=0;i<M.ncst;i++){
		const Cst *c=&M.cst[i];
		if(c->kind==CV_INT && c->v<TL_MV && !cml_reserved(c->name)){ bput(o,"const %s = ",c->name); num(o,c->v); bput(o,"\n"); }
	}
	for(int i=0;i<M.nvar;i++) if(cml_reserved(M.var[i])) refuse(0,"the variable name %s is reserved in .cml",M.var[i]);
	Buf inits={0}; int set[TL_MAX]={0};
	emit_init(&inits,init,NULL,set);
	for(int i=0;i<M.nvar;i++) if(!set[i]) refuse(0,"Init does not give %s a value",M.var[i]);
	Act a0; memset(&a0,0,sizeof a0); a0.line=next->line;
	g_nact=0; walk(push(NULL,next,NULL),&a0);
	name_actions();
	infer();
	/* A type invariant becomes the variables' types, which the checker
	 * enforces on every state reached: a value outside one is a model
	 * error (exit 3), where TLC reports the invariant violated (exit 1).
	 * Both stop with no pass, and the invariant itself then holds in
	 * every state the search keeps, so it is written as TRUE.  NB: the
	 * exit status differs from TLC's on a type violation.  One invariant
	 * gives the types: TypeOK if it is one, else the first listed.  Any
	 * other, of the same form or not, is checked as written, so a second
	 * `x \in T` neither replaces the first nor holds as TRUE. */
	int istype[TL_MAX]={0}, typed=0;
	for(int i=0;i<C.ninv && !typed;i++) if(!strcmp(C.inv[i],"TypeOK")) typed=istype[i]=type_invariant(C.inv[i]);
	for(int i=0;i<C.ninv && !typed;i++) typed=istype[i]=type_invariant(C.inv[i]);
	for(int i=0;i<M.nvar;i++){
		bput(o,"var %s : ",M.var[i]);
		if(g_decl[i]) bput(o,"%s",g_decl[i]); else ty_text(o,g_vty[i],M.var[i]);
		bput(o,"\n");
	}
	bput(o,"%s",bstr(&inits));
	for(int i=0;i<g_nact;i++) emit_act(o,g_act[i]);
	emit_fairness(o);
	for(int i=0;i<C.ninv;i++){
		if(istype[i]) bput(o,"invariant %s: TRUE\n",C.inv[i]);
		else emit_named(o,"invariant",C.inv[i],"INVARIANT",0);
	}
	for(int i=0;i<C.nprop;i++) emit_prop(o,C.prop[i]);
	for(int i=0;i<C.ncon;i++) emit_named(o,"constraint",C.con[i],"CONSTRAINT",0);
	for(int i=0;i<C.nacon;i++) emit_named(o,"action_constraint",C.acon[i],"ACTION_CONSTRAINT",CX_PRIME);
	if(C.sym[0]) emit_sym(o);
	if(C.view[0] && !all_vars(def_named(C.view,"VIEW")))
		refuse(C.line_view,"VIEW %s: only the tuple of all the variables (no view) is supported",C.view);
	if(C.deadlock==0) bput(o,"deadlock = off\n");
}

static char *slurp(const char *path, char *err, size_t errcap){
	FILE *f=fopen(path,"r");
	if(!f){ snprintf(err,errcap,"cannot open %s: %s",path,strerror(errno)); return NULL; }
	size_t n=0, cap=4096; char *s=malloc(cap);
	for(;;){
		if(!s){ fclose(f); snprintf(err,errcap,"out of memory"); return NULL; }	/* GCOVR_EXCL_LINE: malloc fails only out of memory */
		size_t k=fread(s+n,1,cap-n-1,f); n+=k;
		if(n+1<cap){
			if(ferror(f)){ free(s); fclose(f); snprintf(err,errcap,"cannot read %s",path); return NULL; }	/* GCOVR_EXCL_LINE: a read error on a file just opened (a directory reads as empty) */
			break;
		}
		cap*=2; char *ns=realloc(s,cap); if(!ns) free(s); s=ns;
	}
	fclose(f); s[n]=0;
	if(strlen(s)!=n){ free(s); snprintf(err,errcap,"%s: a NUL byte in the file",path); return NULL; }
	return s;
}

char *cmc_tla_translate(const char *tla, const char *cfg, char *err, size_t errcap){
	char *mt=NULL, *ct=NULL;
	char *volatile out=NULL;
	if(!(mt=slurp(tla,err,errcap)) || !(ct=slurp(cfg,err,errcap))){ free(mt); return NULL; }
	memset(&M,0,sizeof M); memset(&g_ts,0,sizeof g_ts); memset(g_initv,0,sizeof g_initv); memset(g_decl,0,sizeof g_decl); g_at=NULL; g_nmv=0; g_init=g_next=NULL; g_nwf=0; g_nact=0;
	g_err[0]=0;
	if(setjmp(g_jb)==0){
		g_file=tla; lex(mt,&g_ts); module(&g_ts);
		g_file=cfg; config(ct);
		g_file=tla;
		Buf o={0}; emit(&o,tla);
		out=strdup(bstr(&o));
		if(!out) snprintf(err,errcap,"out of memory");	/* GCOVR_EXCL_LINE: strdup fails only out of memory */
	} else snprintf(err,errcap,"%s",g_err);
	free_all(); free(mt); free(ct);
	return out;
}
