---
title: "The abstract model (.cml)"
parent: Reference
nav_order: 6
permalink: /reference/abstract-model/
lede: >-
  A declarative specification libcmc checks on its own, bridges to TLA+, and checks the shipping binary against.
---

A `.cml` file is an abstract model: the algorithm, written declaratively,
the way a TLA+ module is.  libcmc can model-check it on its own
(`cmc --spec`), translate it to TLA+ for TLC, Apalache or TLAPS
(`cmc --emit-tla`), and -- with a binding -- check that a shipping binary
**refines** it (`cmc --refine`).  One model then validates many
implementations, and a family of protocols shares a common base.  The
design and rationale are in
[the TLA+-scope plan]({{ '/reference/plan-tlaplus-scope/' | relative_url }}).

## A first model

```
module counter

const N = 3

var x : 0 .. N

init x == 0

action inc:
  when x < N          # the guard
  x' = x + 1          # the next-state assignment

action reset:
  when x == N
  x' = 0

invariant in_range: x >= 0 and x <= N
```

```
cmc --spec counter.cml
```

The search is breadth-first over the reachable states, so a violated
invariant is reported with the shortest sequence of actions and the
state it reaches, in TLC's wording.

## The language

A `.cml` module is a small, explicit, bounded fragment -- decidable and
SMT-encodable -- not full TLA+.

- `module NAME` -- the module name (and, for TLA+, the output file name).
- `extends NAME` (or `import NAME`) -- merge another module's constants,
  variables, actions and invariants; a shared base is written once.
- `const NAME = EXPR` -- a finite integer parameter.  A name is declared
  once: a second `const` of it, in the module or in one it extends, is
  an error (the extended module already evaluated with the first value;
  to check a bigger instance, edit the base or give it its own module).
- `var NAME : LO .. HI` -- a variable over a finite integer range.
- `var NAME : [LO .. HI -> T]` -- a function (finite map) variable over
  integer keys, used as an array (`NAME[k]`).  T is any variable type.
- `var NAME : [a : T, b : T]` -- a record variable with exactly the
  fields a and b (`NAME.a`).
- `var NAME : Seq(T, N)` -- a sequence of at most N elements of type T
  (N a constant up to 12).  The bound keeps the state space finite: a
  value longer than N is a model error (exit 3), never cut short.
  The types nest, as in `var log : [0 .. 2 -> Seq([idx : 1 .. 2, val :
  0 .. 1], 2)]`.  A type is fixed before any state, so its sets and
  bounds are constant expressions: one that reads a variable (`var x :
  y .. 3`) is refused at load, and so is a `const` that does.
- `def NAME = EXPR` / `def NAME(p, ...) = EXPR` -- a named operator, as
  TLA+'s `NAME(p, ...) == EXPR`.  A use is the body with each parameter
  replaced by its argument, evaluated where the body uses it.  The body
  sees the constants, the variables, its parameters and the defs
  declared before it, so a def cannot reach itself: recursion is
  refused at load time.
- `init PREDICATE` -- a conjunction of `var == EXPR` fixing the initial
  state (repeat `init` to add conjuncts).
- `action NAME:` / `action NAME(p in SET, ...):` -- a transition.  A
  `when GUARD` line is its enabling condition (several `when` lines
  are conjoined, as TLA+ `/\` lines); `NAME' = EXPR` lines are
  simultaneous assignments to the next state.  Parameters range over
  finite sets, giving the non-determinism.  An action tagged
  `[TAG]`, written after the parameter list (`action NAME(p in SET)
  [TAG]:`), is active only with `--define TAG` (used to plant a bug).
  A bracket inside a parameter domain, `j in 0 .. f[i]`, is part of the
  domain, and a header with anything else in it is an error.
- `invariant NAME: EXPR` -- checked in every reachable state.
  `invariant NAME [inductive]: EXPR` also claims it inductive (see
  [Proof hooks](#proof-hooks)).
- `symmetry SET` -- declare SET's values interchangeable, collapsing
  symmetric states (see below).  SET is a constant set of at most 8
  values; anything else is an error.
- `fairness weak|strong`, `fair weak|strong ACTION` and
  `property NAME: ...` -- liveness and action properties (see below).
- `deadlock = off` (or `deadlock = on`, the default) -- whether a state
  with no enabled action is an error (see below).
- `constraint NAME: EXPR` and `action_constraint NAME: EXPR` -- bound
  the search, as TLC's `CONSTRAINT` and `ACTION_CONSTRAINT` (see below).

Expressions: integers with `+ - * / mod`; sets of integers `{a, b}` and
ranges `LO .. HI`; functions `[i in SET -> EXPR]` and application `f[k]`;
records `[a |-> E, b |-> E]` and field access `r.a`; sequences (and
tuples, which are sequences) `<<E, E>>`, `s[i]` from 1, `Len(s)`,
`Head(s)`, `Tail(s)`, `Append(s, e)`, `SubSeq(s, m, n)` and `s \o t`;
comparisons `== != < <= > >=`, `A in SET`; boolean `and or not`;
quantifiers `all V in SET : E`, `some V in SET : E`; `count V in SET
: E` (cardinality); and `if C then A else B`, TLA+'s `IF`, which
evaluates only the branch it takes and whose `else` branch extends as
far right as it can.
An arithmetic result of 2^40 or more in magnitude is an overflow, a
model error (exit 3), as TLC's past 2^31 is.  TLA+'s integers are
unbounded, but 2^40 and up are the reader's model values, which no
integer may equal.

Precedence is TLA+'s, and the mixes TLA+ refuses are refused: `/` or
`mod` next to another `*`, `/` or `mod` (`x mod 3 * 2`, `x * 2 / 2`),
`mod` next to `+` or `-` (`1 + x mod 4`), and `and` next to `or` (`a or
b and c`) each need parentheses.  `*` chains (`x * 2 * 3`) and `+`/`-`
chains do not.

An update `[B except P = V, ...]` replaces the part of B at each path P,
as TLA+'s `[B EXCEPT !P = V]`: P is `[k]` (a function key or sequence
position) or `.a` (a record field), chained for nested values, with an
optional leading `!`.  In V, `@` is the part being replaced:
`[srv except ![c].votes = @ + 1]`.  A path outside B (a key not in the
function's domain, a position past the sequence's end, a missing field)
is a model error; TLC leaves the value unchanged with a warning.

Every operand is checked against its operator, as TLC checks it.
Arithmetic, orders, range bounds, set elements, function keys and
sequence positions are integers; `and`, `or`, `not`, quantifier bodies, guards,
invariants and liveness goals are booleans; `==` and `!=` compare two
values of one type, part by part (a record with a function, or a field
`TRUE` with a field `1`, is an error).  `Head` and `Tail` of `<<>>`, an
index outside `1 .. Len(s)`, `SubSeq` bounds past the end and a missing
field are errors too.  Anything else, such as `not votes` for a set
`votes`, or `TRUE == 1`, is a model error (exit 3), never a value.

`and` and `or` evaluate left to right and stop once the left operand
decides the result, as TLC does.  So a guard can protect a partial
operation: `x > 0 and y / x == 2` is FALSE at `x = 0`, and
`i == N or f[i] == 0` never applies `f` outside its domain.  The operand
that is skipped is still parsed, so an unknown name or a syntax error in
it is refused, but its type and its evaluation errors are not checked.

## Deadlock and constraints

These are TLC's checking options, with TLC's meaning; the calibration
checks each against single-worker TLC on the same model.

A reachable state in which no action is enabled is a deadlock, and
`cmc --spec` reports it as TLC does: `Error: Deadlock reached.`, the
shortest trace to the state, and exit status 1.  The check is on by
default, as in TLC.  A model whose terminal states are intended says
so with a `deadlock = off` line, or the run passes `--no-deadlock`;
`cmc --emit-cfg` then writes `CHECK_DEADLOCK FALSE` into the TLC
configuration.  An action step that changes no variable still counts
as a step, so a state where only such a step is enabled is not
deadlocked: TLC's deadlock is about `Next`, not about progress.
Liveness is separate.  A deadlocked state stutters forever in TLA+, so
`eventually` and `~>` treat it as a state the behaviour stays in, with
or without the deadlock check.

```
constraint small: x + y <= 2
action_constraint follow: y' <= x' or y' < y
```

A state that fails a `constraint`, or that a step failing an
`action_constraint` reaches, is outside the model: it is not counted
among the distinct states, not kept and not expanded.  Its invariants
are still checked, as TLC checks them, so a violation found in such a
state is reported.  An `action_constraint` reads the variables before
the step unprimed and after it primed (`x'`); a primed variable is
refused anywhere else.  A step outside the model is still a step, so
the state it leaves is not deadlocked; a state that is never expanded
is never deadlocked.  An initial state outside a `constraint` is
checked but not counted, so the search can end with 0 distinct states,
as TLC's does.

A constraint narrows the search, not the specification, so a liveness
property (an action property is fine: it is checked step by step, as
TLC does) under one, and `--refine` against a spec with one, are
refused (exit 3): neither is modelled.  `cmc --emit-tla` writes each
constraint as an operator, and `cmc --emit-cfg` names it under
`CONSTRAINT` or `ACTION_CONSTRAINT`.

## Modules and a shared base

`examples/spec/consensus.cml` is a base for the consensus family -- it
declares the agreed-value record and the Agreement property (at most one
value chosen).  `examples/spec/paxos.cml` extends it:

```
module paxos
extends consensus
...
```

so Paxos inherits Agreement unchanged and adds only its own acceptors,
votes and the quorum rule.  A category's shared requirements are thus
modelled once, identically.

## Refinement: the shipping code against the model

`cmc --refine` checks that a running binary refines an abstract model.
A `.bind` file maps the implementation's observables -- the bytes of its
state hook, named by `[fields]`, and the reply log -- to the abstract
variables:

```
[fields]
decided = 7

[map]
chosen[v] = "some i in nodes : node[i].decided == v"
```

```
cmc --model-file paxos-refine.model --binary ./paxos \
    --refine examples/spec/paxos.cml --bind examples/spec/paxos.bind
```

A function variable is bound entry by entry (`chosen[v] = ...`), a
scalar one whole (`c = ...`); each spec variable at most once.  The
file may start with `spec =` and `binary =` lines for its reader.  A
line the reader cannot use (an unknown section or variable, a second
binding, a field offset that is not 0 to 254) is an error, never
skipped.  A binding builds integers and functions of integers only: a
record or sequence variable, or a function of them, is refused.

At each reached implementation state the abstract state is built from the
binding, each bound variable is checked against its declared domain (a
value outside it is an error, not a state of the spec), and the
abstract invariants are checked against it.  The binding
may reference only what the code emits, never its internals, so the
refinement is exactly as honest as the state hook is complete.  Point the
same spec and binding at a second implementation to check it too.

Add `--refine-steps` to check refinement at the level of *steps*, not
only states: each implementation transition must map to an abstract
action step or a stutter (the abstraction unchanged).  This catches a
step the model forbids even when every state it reaches is in range,
which the state-level check cannot see.  It needs every spec variable to
have a binding (matching an action step needs the whole abstract state)
and refuses otherwise.

A counterexample reads in the spec's terms as well as the program's.
Below each listed step after which the check judges the world (settled,
not between or inside nodes' steps), a `spec:` line gives the abstract
state the binding built there; with `--refine-steps` it also names the
abstract action the step refined, with its parameters, or `stutter`:

```
Error: the implementation does not refine the spec: a step has no matching abstract action.
Error: The behavior up to this point is (1 actions):
         spec: init -> c=0
    1. timeout n0
         spec: no matching abstract action -> c=2
```

`--trace-out` carries the same in each world's `spec` object
(`{"action": "stutter", "state": {"c": 1}}`; `null` at a world the
check does not judge).

## The TLA+ bridge

```
cmc --emit-tla counter.cml > counter.tla
cmc --emit-cfg counter.cml > counter.cfg
```

writes a TLA+ module, so the model can be checked with TLC or Apalache
and proved with TLAPS, and the TLC configuration that goes with it (the
constants, invariants, properties and constraints by name, and
`CHECK_DEADLOCK FALSE` when the deadlock check is off).  Name the files
after the module.  `make
check-emit-tla` verifies that the generated TLA+ agrees with
`cmc --spec` on every example.

Records, sequences and defs are emitted as their TLA+ forms (the module
extends `Sequences`, and each def becomes an operator definition).  The
module carries Apalache `@type` annotations (each constant and scalar
variable is `Int`, a function variable `Int -> T`, a record `{ a: T, b:
T }`, a sequence `Seq(T)`) and a
`CInit` operator that binds the constants to their `.cml` values, so
Apalache's symbolic, Z3-backed checker accepts it directly:

```
apalache-mc check --cinit=CInit --inv=in_range counter.tla
```

The annotations are `\*` comments, so TLC reads the same module
unchanged.  `make check-emit-apalache` verifies that the generated TLA+
type-checks in Apalache and agrees with `cmc --spec`.  Apalache is
optional (not a build dependency); the gate fetches it once or uses
`$APALACHE_MC`, and needs Java 21+.

## Reading TLA+

```
cmc --spec logrep.tla --config logrep_read_safe.cfg
cmc --emit-cml logrep.tla --config logrep_read_safe.cfg
```

`cmc --spec` reads a TLA+ module and its TLC configuration directly.  A
file named `*.tla`, or one given with `--config`, is a module; without
`--config` the configuration is the `.cfg` beside it, as TLC finds it.
The reader translates the module to the `.cml` model it means and
checks that, so everything above holds for it, and `--emit-cml` prints
the translation.  It reads the part of TLA+ that `.cml` can express and
refuses the rest with the line and the construct (exit 3), never by
guessing.

The module may `EXTENDS` only `Naturals`, `Integers`, `FiniteSets`,
`Sequences` and `TLC`.  It declares `CONSTANTS`, which the configuration
binds, and `VARIABLES`.  A definition `Op(p, ...) == e` is expanded where
it is used, with each argument passed by name and each bound name
renamed apart, as TLA+ substitutes; a definition sees only those above
it, so none is recursive, and one the model never uses may hold
anything.  `ASSUME` may say `C \in Nat`, `Int` or `BOOLEAN`, and is
checked against the configuration.

`Init` is a conjunction of `x = e`, one per variable.  `Next` is read as
a disjunction of actions: a disjunct that primes a variable is split at
each `\/`, `\E` and `IF` in it, an `\E` becomes the action's
parameters, and what is left must be a conjunction of guards, `x' = e`,
`x' \in S` (a parameter over S) and `UNCHANGED`, saying what every
variable's next value is.  An action takes the name of the innermost
definition that holds it, `Recv` for `\E f \in Followers : Recv(f)`.
A disjunct whose guard is a constant `FALSE`, such as a `USE_BUG`
switch, is left out.  `Spec` is `Init /\ [][Next]_vars`, with `vars` all
the variables, and for liveness any number of `WF_vars(A)` and
`SF_vars(A)` conjuncts, each A one action of Next (its own `\E`
parameters inside it), read as `.cml`'s `fair weak A` and `fair strong
A`.  Fairness on a disjunction of actions, or on an action under an
`\E` of Next (fair per parameter value), is refused.  A theorem and its
proof are skipped, and `ASSUME` may also say `C = n`.

Expressions are the `.cml` ones in their TLA+ spelling: `=`, `#`,
`\in`, `\A`, `\E`, `=>`, `<=>`, `Cardinality(S)` and
`Cardinality({x \in S : P})`, `\div` and `%`, `[x \in S |-> e]`,
`[f EXCEPT ![k].a = @ + 1]`, records `[a |-> e]` and `r.a`, tuples and
sequences with the `Sequences` operators, `IF THEN ELSE`, `x \in Nat`
and `/\` and `\/` bullet lists, an item ending at the first token at or
left of its bullet's column.  As in SANY, `a /\ b \/ c` without
parentheses is refused, and so is a definition parameter or bound name
that names a variable, constant, definition or enclosing bound name
again (TLA+ has no shadowing).  TLA+'s variables have no types and
`.cml`'s do, so the reader infers each from `Init` and every assignment:
an integer becomes any of TLC's (`-2^31 .. 2^31-1`, so a value TLC could
not hold is a model error here too), a function's key set comes from
its `Init` value, a sequence holds at most 12 elements (a longer one is
a model error, never cut short), and a variable that holds a boolean or
a set, two types in two places, or a type that holds itself (`x' =
<<x>>`), is refused at load, never searched.  An `INVARIANT` whose
conjuncts are all `x \in T`, such as the `TypeOK` `cmc --emit-tla`
writes, gives the variables their types instead: T is `LO .. HI`, a
literal or constant set of integers, `[S -> T]`, `[a : T, ...]`, or
`{<<>>} \cup {<<e1>> : e1 \in T} \cup ...` up to k elements (`Seq(T,
k)`).  One invariant gives the types: `TypeOK` if the `.cfg` lists it,
else the first of that form.  Every other invariant, including another
`x \in T` on the same variable, is checked as written, and so is one
whose T reads a variable (`x \in y..5`), which is no type.  The checker
enforces a variable's type on every state, so a
value outside it is a model error (exit 3), where TLC reports the
invariant violated (exit 1).

The configuration may give `CONSTANT(S)` as `N = 3`, `TRUE`, `FALSE`,
a model value `p1` or a set `{p1, p2}` of integers or model values;
`SPECIFICATION`, or `INIT` and `NEXT`; `INVARIANT(S)`;
`PROPERTY`/`PROPERTIES` of the forms `<>P`, `P ~> Q` and `[][A]_vars`;
`CONSTRAINT(S)`; `ACTION_CONSTRAINT(S)`; and `CHECK_DEADLOCK`.  A model
value becomes an integer from 2^40 up, past TLC's integers, so it is
unequal to every integer, as in TLC.  Arithmetic and order on a model
value, and `\in Nat` or `Int` of one, which TLC stops on, are refused;
a variable may hold an integer in one state and a model value in
another.  `SYMMETRY Sym` with `Sym == Permutations(P)` for a set
P of model values is `.cml`'s `symmetry` over those integers: the
dedicated range its obligation asks for.  `SYMMETRY` of anything else
is refused, and so is a `VIEW` other than the tuple of all the
variables.  A substitution `N <- Op`, `ALIAS` and `POSTCONDITION` are
refused.

Refused, by name: `LET`, `CHOOSE`, `CASE`, `SUBSET`, `UNION`, `DOMAIN`,
`ENABLED`, `INSTANCE`, `RECURSIVE`, strings, set operators (`\cup`,
`\X`, ...), sets of functions or records other than as a type
invariant's types, unbounded and temporal quantifiers, a nondeterministic
`Init`, a prime outside an assignment or an action constraint, and
infix or function definitions.  Of the hand-written twins in the
repository, the test/spec ones and examples/spec/logrep.tla (with a
configuration that leaves out its `TypeOK`, which uses `UNION`) read directly, with TLC's
counts; paxos, fpaxos, caspaxos, fastpaxos and zab are refused (their
messages are records with string tags in a set).  `make check` reads
back every module `cmc --emit-tla` writes, with its `--emit-cfg`
configuration, to the same verdict, counts and trace.
## Proof hooks

An invariant that holds in every reachable state need not be
*inductive*: a step from some unreachable state that satisfies it may
break it, and a proof by induction over the steps then fails.  Mark the
invariants you claim are inductive:

```
invariant mutual_exclusion [inductive]: (count i in 0 .. N-1 : pc[i] == 2) <= 1
invariant lock_matches [inductive]: held == (count i in 0 .. N-1 : pc[i] == 2)
```

`cmc --spec FILE --check-inductive` checks the claim.  It enumerates
every state of the declared types (TypeOK), not only the reachable ones,
and for each that satisfies the marked invariants takes every step of
every action.  It reports a counterexample to induction, a state and
the step out, or that the conjunction is inductive:

```
Error: the invariant is not inductive: a step of enter leaves mutual_exclusion.
Error: a counterexample to induction (a state of TypeOK and the invariant, then a step):
  state: pc=[0:1,1:2] held=0
    1. enter(i=0)
  after: pc=[0:2,1:2] held=1
```

That is mutual exclusion alone (test/spec/ind/mutex_ind.cml with
lock_matches unmarked): it holds in every reachable state, but not from
the state where process 1 is critical and the lock is free.  The check
is exhaustive over the declared domains, so it refuses (exit 3) a
TypeOK of more than 1,000,000 states or a function of more than 12
keys, and a model with a constraint.  A step that leaves a variable's
type is a counterexample too, since TypeOK is part of the claim.

`cmc --emit-tla` writes `TypeOK` for every model (and `--emit-cfg` names
it as an invariant), and for the marked invariants `IndInv == TypeOK /\
...`, the constants' values as an `ASSUME`, and the theorem with a TLAPS
proof outline:

```
THEOREM Inductive == Spec => []IndInv
<1>1. Init => IndInv
  BY ConstOK DEF Init, IndInv, TypeOK, mutual_exclusion, lock_matches
<1>2. IndInv /\ [Next]_vars => IndInv'
  BY ConstOK DEF Next, vars, try, enter, leave, IndInv, TypeOK, ...
<1>3. QED
  BY <1>1, <1>2, PTL DEF Spec
```

`PTL` is defined in the module as TLAPS's `TLAPS` module defines it,
so TLC and Apalache read the module without that library.  The outline
is the shape TLAPS expects; whether its two leaves go through depends on
the backends: on counter.cml with its invariant marked, TLAPS 1.6.0-pre
proves all 11 obligations, and on mutex_ind.cml it proves 12 of 14 and
needs facts about `Cardinality` for the other two.  Apalache's check of
the same claim is

```
apalache-mc check --cinit=CInit --init=IndInv --inv=IndInv --length=1 mutex_ind.tla
```

and `make check-emit-apalache` requires it to agree with `cmc
--check-inductive` on each model in test/spec/ind, as `make
check-emit-tla` requires TLC (from `INIT IndInv`) to find as many states
in IndInv as cmc counts.

## Counterexamples as files

```
cmc --spec mutex.cml --define BUG --trace-out bug.itf.json
cmc --spec mutex.cml --define BUG --trace-out bug.json --trace-format json
```

On an invariant violation, `--trace-out FILE` writes the shortest
counterexample, the one cmc prints, as a file as well.  The default
format, `itf`, is the [Informal Trace Format](https://apalache-mc.org/docs/adr/015adr-trace.html)
Apalache writes and its tools read (`itf-py`, `itf-rs`).  It is laid
out as Apalache lays out a counterexample for the module `cmc
--emit-tla` makes of the spec: the constants under `params` and in
every state, each variable's type in `varTypes`, an integer as
`{"#bigint": "N"}`, a function as `{"#map": [[k, v], ...]}`, a
record as an object of its fields in the order of their names, and a
sequence as an array, nested as the value is; `varTypes` reads as
Apalache writes it, e.g., `(Int -> Seq({ idx: Int, val: Int }))`.  Each
state's `#meta` holds its index and, past the first, the action that
led to it (Apalache leaves that out).  `json` writes a plain array, one
object per state with its `index`, its `action` (`null` for the first)
and its `vars`, an integer as a number, a function as an object keyed
by its arguments, a record as an object of its fields and a sequence as
an array.  A pass writes no file.

A liveness violation writes the lasso cmc prints (see Liveness): ITF
lists the prefix and the cycle and marks the state the cycle goes back
to with ADR-015's `"loop": INDEX`, and the header's `#meta` names the
closing action as `loopAction` (`Stuttering` for a cycle that stays put).
JSON lists the same states, then the loop state again, with that action
and `"back_to": INDEX`.  Apalache 0.63 writes no `loop` for a temporal
violation; it repeats the loop state and adds its own `__InLoop`
variables, so the Apalache gate compares invariant traces only.

The spec gate checks that each file parses, holds the printed actions
plus the initial state, and ends in the printed state; the Apalache gate
checks that cmc's ITF file for each planted bug has the header, the
variables, the types, the length and the value forms of the one Apalache
writes for the same module.
## Symmetry

```
symmetry 1 .. N
```

declares the values `1 .. N` interchangeable.  States that differ only by
a permutation of them collapse to one representative, shrinking the
search.  **Obligation:** the values must be genuinely interchangeable
wherever they appear -- use a dedicated range, disjoint from other
integer uses (the discipline TLA+ enforces with model values).  The gate
checks the verdict is identical with symmetry on and off; a declaration
that changed it would be unsound.  A permutation maps every integer of
the set wherever a value holds it: in sets, function keys and values,
record fields and sequence elements (not field names or positions).

cmc checks the part of the obligation one state can show: for each
state it reaches, swapping the set's first value with each other one
must give a state inside the declared domains on which every invariant
and constraint has the same value.  A model that fails this (a counter
`x : 0 .. 2` under `symmetry 1 .. 2`, or a value of the set used as a
plain number) is exit 3.  An action whose guard tells the values apart
is not seen by this check and remains the model's obligation.

## Liveness

```
fairness weak
property reaches_max: eventually x == N
```

Under weak fairness (every continuously-enabled action eventually fires),
`eventually E` requires every behaviour to reach `E`, and `A ~> B`
requires every `A` to be followed by a `B`.  A violation is a fair cycle
that avoids the goal, printed as TLC prints it: the states of a path
to it, then `Back to state N: <action>` for the step that closes the
cycle, or `State N: Stuttering` when the run just stops there.  The
prefix takes a shortest path to a state where the property fails, then
a shortest one on to the cycle, and the cycle takes each action the
fairness asks it to take, so the lasso is a fair run that never meets
the goal.  It is a valid fair counterexample, but in general not the
one TLC prints: its prefix or cycle can be longer or shorter than
TLC's, or close at another state (an independent review found 663 of
1060 random violations differed in shape, and all 1089 lassos it
checked valid).  On the gated TEMPORAL rows the lasso is TLC's, and
the emit gate compares the two there, since both searches are
deterministic; a change that moves one of them is flagged.  This is bounded, in-process liveness on the
abstract model; `cmc --emit-tla` emits the fairness and the property so
TLC can check the same thing.

Fairness can be strong, and it can be per action:

```
fair weak flip            # WF_vars(flip)
fair strong finish        # SF_vars(finish)
fairness strong           # SF_vars(A) for every action A
```

Weak fairness forbids a run that from some point on has an action
continuously enabled and never takes it; strong fairness forbids one
that has it enabled infinitely often and never takes it.  An action
with more than one fairness line gets the strongest.  A parameterized
action `A(p in S)` is fair as a whole, as TLA+'s `WF_vars(A)` with `A ==
\E p \in S : ...`: some instance is taken, not each one.  The check
finds a fair cycle by TLC's method for strong fairness: split the
non-goal states into strongly connected components; a component in
which a strongly fair action is enabled somewhere and taken nowhere
loses the states that enable it, and the rest is split again; a
component left standing is fair when every weakly fair action is
disabled somewhere in it or taken inside it.  The `toggle` and `inner`
models in `test/spec/fair/` give different verdicts under weak and
strong fairness, and TLC agrees on each.

An action property is TLA+'s `[][E]_vars`:

```
property monotone: always [x' >= x]_vars
```

Every step that changes a variable must satisfy E, an expression over
the variables before (`x`) and after (`x'`) the step; a stutter is
allowed.  It is checked on every step the search generates, to a new
state, a seen one, or one outside a `constraint`, as TLC checks it, and
a violation is reported with the shortest trace that ends in the bad
step ("Error: Action property monotone is violated.").

## Coverage

`cmc --spec FILE --coverage` prints, after the search, each action's
line of TLC's `-coverage` report: `<A>: distinct:generated`, the new
states it found and all the successors it produced (stutters and states
already seen included), and how many explored states enable it.  An
action no explored state enables is named in a warning, since a model
in which it never fires checks nothing about it.  The counts match
TLC's on the same model (see `test/spec_calibrate.sh`), with one
difference: TLC counts the successors of an action whose guard has an
existential quantifier once per witness of the quantifier, and one
whose guard has a disjunction once per true disjunct, so its
"generated" can be larger; cmc counts each successor once.  The distinct count, the one that shows vacuity, is
the same.

## One worker

`cmc --spec` searches with one thread and keeps its queue in memory.
`--workers` and `--queue-mem`, the parallel search and the queue spill
of the in-process engine (`cmc --model`, see cmc(1)), are refused here
with exit 3 rather than ignored.

## Exit status

0: no violation.  1: a violation (an invariant, a refinement, or a
liveness property).  2: incomplete (a bound stopped the search).  3: a
model-file or usage error.
