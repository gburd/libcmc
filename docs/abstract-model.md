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
- `var NAME : [LO .. HI -> LO .. HI]` -- a function (finite map) variable,
  used as a record or an array (`NAME[k]`).
- `init PREDICATE` -- a conjunction of `var == EXPR` fixing the initial
  state (repeat `init` to add conjuncts).
- `action NAME:` / `action NAME(p in SET, ...):` -- a transition.  A
  `when GUARD` line is its enabling condition; `NAME' = EXPR` lines are
  simultaneous assignments to the next state.  Parameters range over
  finite sets, giving the non-determinism.  An action tagged
  `[TAG]` is active only with `--define TAG` (used to plant a bug).
- `invariant NAME: EXPR` -- checked in every reachable state.
- `symmetry SET` -- declare SET's values interchangeable, collapsing
  symmetric states (see below).
- `fairness weak` and `property NAME: ...` -- liveness (see below).

Expressions: integers with `+ - * / mod`; sets `{a, b}` and ranges
`LO .. HI`; functions `[i in SET -> EXPR]`, application `f[k]`, update
`[f except [k] = v]`; comparisons `== != < <= > >=`, `A in SET`; boolean
`and or not`; quantifiers `all V in SET : E`, `some V in SET : E`; and
`count V in SET : E` (cardinality).

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

At each reached implementation state the abstract state is built from the
binding and the abstract invariants are checked against it.  The binding
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

## The TLA+ bridge

```
cmc --emit-tla counter.cml > counter.tla
```

writes a TLA+ module, so the model can be checked with TLC or Apalache
and proved with TLAPS.  Name the file after the module.  `make
check-emit-tla` verifies that the generated TLA+ agrees with
`cmc --spec` on every example.

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
that changed it would be unsound.

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
