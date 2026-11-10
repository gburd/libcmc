---
title: "Plan: TLA+ scope over the shipped code"
parent: Reference
nav_order: 9
permalink: /reference/plan-tlaplus-scope/
lede: >-
  The north star: TLA+'s checking power, but the artifact validated is the production binary, joined to a reusable abstract model by a binding file.
---

This is a plan, not built code.  It sets the north star for libcmc past
0.3.0 and stages the work so each step ships something usable, keeps the
existing examples green, and never weakens the checker.

## The north star

Give the checking power of TLA+/TLC, but applied to **the production
code**, so the model and the implementation can never diverge.  A
protocol is checked with **two config files**:

1. an **abstract model** -- the algorithm, complete, in a declarative
   pseudocode (state variables, an initial condition, named actions with
   guards and effects, invariants, temporal properties).  This is the
   specification.  It is implementation-independent.
2. a **binding** -- the connective tissue that maps the abstract model's
   variables and actions onto what one implementation *observably does*:
   the bytes of its state hook, the syscalls and messages it emits, the
   environment actions (deliver, drop, crash, timeout) the checker
   drives.

libcmc then runs the shipped binary (process mode, as today) and checks
that **every behaviour of the implementation is a behaviour the abstract
model allows** -- refinement.  Two consequences, both requested:

- **One model, many implementations.**  Two Paxos implementations
  checked against the *same* abstract model, each with its own binding,
  are shown to refine the one specification -- so both are correct in the
  same sense, and interchangeable.
- **Shared bases across a category.**  The abstract models for Paxos,
  Raft, Humming Consensus and ZAB `import` a common `consensus` base, so
  the vocabulary they share -- quorums, ballots/terms/epochs, the
  Agreement property, monotonicity -- is modelled once and identically,
  and each protocol's model adds only what is its own.

## Why this is the right framing

In TLA+ terms the abstract model is a high-level specification, the
implementation (seen through the binding) is a low-level specification,
and the check is a **refinement mapping**: the low-level behaviours are a
subset of the high-level behaviours under the mapping.  This is exactly
how TLA+ relates a spec to its implementation, and it is what makes "one
spec, many implementations, all correct" a precise claim rather than a
hope.

libcmc already owns the hard, unusual half: it runs the real program and
observes it only through what it emits (the state hook and the
intercepted syscalls), never by reading its internals -- the
no-omniscient-oracle rule in [Why libcmc]({{ '/why/' | relative_url }}).
The binding may reference only those observables, which keeps the
refinement honest and inherits the existing completeness discipline: the
abstract state must be fully determined by what the code emits, or the
check could pass a bad implementation.  That is the same burden as
today's "canonical state must be complete", now stated for the mapping.

## What is in scope, and what is delegated

libcmc is, and stays, an **explicit-state model checker** -- bounded,
concrete, exhaustive to a bound -- like TLC.  The plan matches TLC's
feature set over the running code:

- structured, quantified abstract state (sets, records, sequences,
  functions);
- refinement of an implementation against an abstract model;
- invariants, action properties, witnesses;
- symmetry reduction;
- liveness with fairness (`[]`, `<>`, `~>`, weak/strong fairness);
- composable abstract modules (`import` / `extends`).

One TLA+ capability is **delegated, not rebuilt**: deductive proof
(TLAPS).  TLAPS is a theorem prover, a different engine (proof
obligations discharged by SMT and Isabelle), not a model checker.
libcmc will not grow a homegrown prover -- a weak one would be a
liability for a tool others trust as a safety gate.  Instead, because
the abstract model language is TLA+-shaped, libcmc will **translate the
abstract model to and from TLA+** (Milestone D), so the user keeps the
whole existing ecosystem for the specification itself: TLC to check the
abstract model in isolation, and **TLAPS to prove it**.  libcmc's unique
job -- the one no other tool does -- is to prove that the *shipped code*
refines that specification.

This is the honest reading of "replicate the scope of TLA+": do what TLC
does, over the code; for deductive proof, hand the user the real TLAPS
on the same artifact.

## The three layers

```
  Layer 1  ABSTRACT MODELS  (reusable, implementation-independent)
    base/consensus.cml          quorums, ballots, Agreement, monotonicity
    paxos.cml  extends consensus
    raft.cml   extends consensus
    hc.cml     extends consensus
    zab.cml    extends consensus

  Layer 2  BINDINGS  (one per spec x implementation)
    paxos-impl-a.bind   maps paxos.cml <- implementation A's observables
    paxos-impl-b.bind   maps paxos.cml <- implementation B's observables
                        (same spec, reused unchanged)

  Layer 3  IMPLEMENTATION  (the shipped binary, run in process mode)
```

The check, per reached implementation state and transition:

1. map the observed state to an abstract state via the binding;
2. map the observed transition to a named abstract action (or to a
   **stutter**: the implementation ran internal code without changing
   the observable state);
3. require the step to be legal under the abstract model's `Next` (or a
   stutter), and every abstract invariant to hold;
4. over the whole induced abstract behaviour graph, check the temporal
   properties.

This is refinement by simulation.  It is sound given a deterministic
binding (the abstract action is a function of the observables) and a
complete state hook.

## Relation to today's model file

The current single model file
([`docs/model-file.md`]({{ '/reference/model-file/' | relative_url }}))
is the **degenerate case**: the abstract state *is* the hook bytes, the
binding is the identity, and there is no separate `Next` relation, so
only invariants and bounded reachability are checked.  Every existing
example keeps working unchanged under this reading.  The two-file mode is
the richer, opt-in path; nothing is forced to migrate.

## Milestones

Each is independently useful, independently gated, and leaves every
existing example green.  Ordered by dependency.

- **A -- a value model for abstract state.**  Grow the invariant language
  beyond bytes to sets, records, sequences, functions and quantifiers
  over them.  *Gate:* re-express an existing invariant (Paxos agreement)
  in the richer logic with the identical verdict on every fixture.

- **B -- the abstract model language with modules.**  A `.cml` file with
  `CONSTANTS`, `VARIABLES`, `Init`, named actions (`Next`), `INVARIANTS`
  and `PROPERTIES`; `import`/`extends` for a shared `consensus` base.
  Checkable on its own (no implementation yet).  *Gate:* a hand-written
  TLA+ of the same abstract model gives the same verdict and
  shortest-counterexample depth under TLC -- the new, tighter form of
  today's `calib/hc` calibration.

- **C -- the binding language and the refinement checker.**  The heart.
  A `.bind` maps observables (hook fields and environment actions) to
  abstract state and actions; the engine runs the implementation and
  checks every observed step refines the abstract model.  *Gate:*
  re-cast one example (Paxos) as spec + binding over the same binary and
  have refinement pass; a planted implementation bug breaks refinement
  with a shortest trace; a **second** Paxos implementation reuses the
  spec unchanged and also passes -- the "one model, many implementations"
  claim, demonstrated.

- **D -- the TLA+ bridge.**  Mechanical `.cml` -> `.tla` (and a
  documented subset back).  *Gate:* round-trip the `consensus` base and
  the Paxos spec through TLC (and, as a worked example, TLAPS); the
  emitted TLA+ is accepted and agrees.  This supersedes the hand-kept
  `calib/hc` TLA+ as the calibration path.

- **E -- symmetry reduction.**  Canonicalise interchangeable abstract
  values (node identities, ballots).  *Gate:* identical verdict and
  counterexamples with symmetry on and off, and a strictly smaller state
  count, on a spec with symmetric node ids.

- **F -- liveness with fairness.**  `[]`, `<>`, `~>`, weak/strong
  fairness over the refined behaviour graph (lasso finding).  This needs
  the search to revisit states, which the process front end can do only
  once **fork-from-snapshot** lands (the `m9-fast-wip` work); until then
  liveness stays an in-process-engine capability.  *Gate:* against TLC's
  liveness result on the translated spec.

- **G -- scale.**  Make B-F affordable at useful sizes: the
  fork-snapshot search, symmetry, and -- if search is still the
  bottleneck -- a sharded parallel table, then an LTSmin PINS back end
  (the "when to revisit" note in [Why libcmc]({{ '/why/' | relative_url }})).
  An enabler, not a feature.

## What this does to `calib/hc`

`calib/hc` is today's evidence that the engine agrees with TLC: a
hand-written HC core and a hand-written `HummingConsensus.tla`, compared
row by row.  It stays until **Milestone D** gives the mechanical TLA+
bridge, at which point calibration becomes "libcmc and TLC agree on the
*same* abstract model", with no hand-kept duplicate to drift.  Removing
`calib/hc` before then would delete the only proof that the search is
correct, so it is retired *by* D, not before.

## Risks and honesty

- **The value logic and the refinement checker are real interpreters.**
  This is the bulk of the work and where subtle unsoundness could hide;
  each lands behind the usual red-test-first discipline and a TLC
  cross-check (Milestone B/D), never on trust.
- **Refinement is only as fine as the hook.**  If the state hook
  under-reports, a bad implementation can refine a good spec.  This is
  the existing completeness burden, restated; the binding must draw its
  abstract state entirely from observables, and that is checked, not
  assumed.
- **Liveness waits on fork-from-snapshot.**  Honest dependency: no
  fairness-based liveness over the process front end until the engine can
  revisit a state (M9).
- **Proof is delegated.**  libcmc will not claim to prove what only a
  theorem prover can; it bridges to TLAPS instead.
