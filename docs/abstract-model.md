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
- `const NAME = EXPR` -- a finite integer parameter.
- `var NAME : LO .. HI` -- a variable over a finite integer range.
- `var NAME : [LO .. HI -> T]` -- a function (finite map) variable over
  integer keys, used as an array (`NAME[k]`).  T is any variable type.
- `var NAME : [a : T, b : T]` -- a record variable with exactly the
  fields a and b (`NAME.a`).
- `var NAME : Seq(T, N)` -- a sequence of at most N elements of type T
  (N a constant up to 12).  The bound keeps the state space finite: a
  value longer than N is a model error (exit 3), never cut short.
  The types nest, as in `var log : [0 .. 2 -> Seq([idx : 1 .. 2, val :
  0 .. 1], 2)]`.
- `def NAME = EXPR` / `def NAME(p, ...) = EXPR` -- a named operator, as
  TLA+'s `NAME(p, ...) == EXPR`.  A use is the body with each parameter
  replaced by its argument, evaluated where the body uses it.  The body
  sees the constants, the variables, its parameters and the defs
  declared before it, so a def cannot reach itself: recursion is
  refused at load time.
- `init PREDICATE` -- a conjunction of `var == EXPR` fixing the initial
  state (repeat `init` to add conjuncts).
- `action NAME:` / `action NAME(p in SET, ...):` -- a transition.  A
  `when GUARD` line is its enabling condition; `NAME' = EXPR` lines are
  simultaneous assignments to the next state.  Parameters range over
  finite sets, giving the non-determinism.  An action tagged
  `[TAG]` is active only with `--define TAG` (used to plant a bug).
- `invariant NAME: EXPR` -- checked in every reachable state.
  `invariant NAME [inductive]: EXPR` also claims it inductive (see
  [Proof hooks](#proof-hooks)).
- `symmetry SET` -- declare SET's values interchangeable, collapsing
  symmetric states (see below).  SET is a constant set of at most 8
  values; anything else is an error.
- `fairness weak` and `property NAME: ...` -- liveness (see below).
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
quantifiers `all V in SET : E`, `some V in SET : E`; and `count V in SET
: E` (cardinality).

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
property under one, and `--refine` against a spec with one, are
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
an array.  A liveness violation has no path to write, and
a pass writes no file.

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

## Liveness

```
fairness weak
property reaches_max: eventually x == N
```

Under weak fairness (every continuously-enabled action eventually fires),
`eventually E` requires every behaviour to reach `E`, and `A ~> B`
requires every `A` to be followed by a `B`.  A violation is a fair cycle
that avoids the goal.  This is bounded, in-process liveness on the
abstract model; `cmc --emit-tla` emits the fairness and the property so
TLC can check the same thing.

## Exit status

0: no violation.  1: a violation (an invariant, a refinement, or a
liveness property).  2: incomplete (a bound stopped the search).  3: a
model-file or usage error.
