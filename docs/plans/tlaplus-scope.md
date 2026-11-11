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

The decisive asymmetry: **the abstract spec can be proved; the code can
only be checked.**

- The **abstract model** is a declarative artifact we design.  Kept in a
  decidable fragment (finite sets over bounded domains, linear
  arithmetic, uninterpreted functions), it can be handed to the same
  engines TLA+ uses -- and libcmc can *discharge proof obligations about
  it* to those engines directly, exactly as requested.
- The **production code** has loops, `malloc`, pointers and syscalls.  It
  is not faithfully encodable into a solver -- which is precisely *why*
  libcmc runs it and model-checks it at the syscall boundary instead of
  encoding it.  So "prove the code" is structurally a model-checking job,
  bounded and explicit-state, done by libcmc running the binary.  No
  solver can discharge it.

So the two questions get two engines:

**Proving / symbolically checking the abstract model** -- reuse the real
engines, never rebuild them.  libcmc emits the standard artifact and
lets the proper tool answer:

| question about the spec | engine | reached by |
|---|---|---|
| model-check it exhaustively (the calibration oracle) | **TLC** | `.cml -> .tla` (Milestone D) |
| model-check it symbolically, over large/structured domains | **Apalache** (Z3-backed) | `.cml -> .tla` |
| **prove** it, unbounded (is this invariant inductive? does the spec hold for all values?) | **TLAPS** (Isabelle/TLA+, Zenon, and SMT: Z3/CVC5/veriT) | `.cml -> .tla` |
| discharge one obligation directly (inductive invariant, refinement step) | **Z3 / CVC5 / veriT** | optional `.cml -> SMT-LIB` *(tracked, deferred -- see Decisions)* |

The last row is the asked-for "discharge proof obligations to the SMT
solvers like TLA+ does": **yes, and it works** -- for the abstract model
kept in the SMT-encodable fragment, libcmc emits SMT-LIB and calls the
solver, the same way TLAPS's SMT backend does.  What libcmc does **not**
do is reimplement TLAPS's encoding of full, unrestricted TLA+ set theory
into those solvers -- that faithful encoding is years of trusted work;
for anything outside the decidable fragment, libcmc bridges to TLAPS and
lets *it* drive Isabelle/Zenon/SMT.

**Checking that the code refines the model** -- libcmc's own C, no
external engine.  This is the irreducible part no other tool does.

**The C-only guardrail holds.**  Every solver above is an *optional,
external* tool invoked as a subprocess -- exactly as `calib/hc` invokes
TLC today -- that produces an independently meaningful answer about the
*spec*.  None is a build dependency of `cmc`, and none sits on the path
of the core verdict (does the code refine the spec), which stays pure
C11 + libc, `cc` and `make`, on any host.  A user without Z3 or TLAPS
installed still gets the full refinement check of their binary; they
lose only the optional proofs *about the specification*.

libcmc is, and stays, an **explicit-state model checker** -- bounded,
concrete, exhaustive to a bound -- like TLC.  Over the running code it
matches TLC's feature set:

- structured, quantified abstract state (sets, records, sequences,
  functions), kept within the SMT-encodable fragment so the proof bridge
  stays sound;
- refinement of an implementation against an abstract model;
- invariants, action properties, witnesses;
- symmetry reduction;
- liveness with fairness (`[]`, `<>`, `~>`, weak/strong fairness);
- composable abstract modules (`import` / `extends`).

This is the honest reading of "replicate the scope of TLA+": do what TLC
does over the *code*; for proof and symbolic checking of the *spec*,
discharge to the real engines (SMT directly where decidable, TLAPS and
Apalache via the TLA+ bridge).

### Two tools considered and set aside

- **Elle** (Kingsbury, Jepsen) is a separate kind of tool: it checks a
  *recorded history* of a live system against a fixed consistency model
  (serializability, snapshot isolation) by Adya-style cycle detection.
  It judges one observed run, not all runs, and against built-in models,
  not an arbitrary spec; it is not an influence on libcmc's design.  Its
  cycle-detection method is a useful *reference* only if libcmc later
  adds a linearizability property over the ghost history it already
  keeps.
- **Pure** (a term-rewriting language, LLVM-JIT) is rejected: a second
  language and an LLVM runtime on a path the project keeps C-only.  The
  abstract-model interpreter is a few hundred lines of C, not a language
  runtime.

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

**Status (as of v0.5.0):** A, B, C, D, E and F are implemented and
calibrated against single-worker TLC; D's bridge also reaches Apalache
(the symbolic, Z3-backed engine), verified by `make
check-emit-apalache`.  G (scale) is in progress -- the
fork-from-snapshot search is proven as a mechanic
(`experiments/fork-snapshot/`) but not yet a sound engine mode
(`--fast`, on the `m9-fast-wip` branch).  C landed as C1 (invariant-
mapping refinement, the sound first form); C2 adds action-level
refinement with stuttering (`--refine-steps`): each implementation step
must map to an abstract action step or a stutter, so a transition the
spec forbids is caught even when every state it reaches satisfies the
invariant.  C's "one model, many
implementations" gate is met: a second, structurally different Paxos
(`examples/paxos/paxos2.c` -- three symmetric nodes, a different hook
layout) refines the *same* `examples/spec/paxos.cml` through its own
binding (`examples/spec/paxos2.bind`), the spec unchanged.  The whole
consensus family now has a certified abstract model -- `paxos.cml`,
`raft.cml`, `hc.cml` and `zab.cml`, each agreeing across `cmc --spec`,
TLC and Apalache.

- **A -- a value model for abstract state.**  Grow the invariant language
  beyond bytes to sets, records, sequences, functions and quantifiers
  over them.  Keep it within the **SMT-encodable fragment** (finite
  domains, linear arithmetic, uninterpreted functions) from the start,
  so the proof bridge in D stays sound.  *Gate:* re-express an existing
  invariant (Paxos agreement) in the richer logic with the identical
  verdict on every fixture.

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

- **D -- the artifact bridge.**  Mechanical `.cml` -> `.tla` (and a
  documented subset back), so the abstract model reaches the real
  engines: **TLC** (explicit), **Apalache** (symbolic, Z3-backed, for
  large/structured domains), and **TLAPS** (unbounded proof via
  Isabelle/Zenon/SMT).  *Gate:* round-trip the `consensus` base and the
  Paxos spec through TLC and Apalache (and, as a worked example, a TLAPS
  proof); the emitted artifacts are accepted and agree.  This supersedes
  the hand-kept `calib/hc` TLA+ as the calibration path.
  - **D' (tracked, deferred) -- direct SMT.**  An optional `.cml` ->
    **SMT-LIB** emitter so libcmc can discharge a single obligation (an
    inductive invariant, a refinement step) to **Z3/CVC5/veriT**
    directly, for the decidable fragment from A.  Decided worthwhile but
    **not implemented at this stage**: the `.tla` bridge already reaches
    these solvers through TLAPS/Apalache, so the direct path is an
    optimisation, not a prerequisite.  Build it only when a concrete
    obligation needs it without the TLA+ round-trip.

  Every engine here is an optional external tool, like TLC today; none is
  a build dependency of `cmc`.

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
- **The SMT path is only as sound as the fragment.**  Direct
  `.cml -> SMT-LIB` is sound only while the abstract logic stays inside
  the decidable fragment A fixes; a construct outside it must route
  through TLAPS, not be mis-encoded to a solver.  The emitter refuses
  what it cannot encode rather than guess.
- **Proof is delegated to the right engine, not rebuilt.**  For the
  decidable fragment libcmc discharges obligations to Z3/CVC5/veriT
  directly (like TLAPS's SMT backend); beyond it, it bridges to TLAPS
  (Isabelle/Zenon/SMT).  It never reimplements TLAPS's encoding of full
  TLA+ set theory, and never claims an internal proof the engines did
  not give.  The code-refinement verdict never depends on any of them.

## Decisions and sequencing

Settled with the maintainer:

- Proof of the spec is **delegated** to the TLA+-native engines via the
  `.tla` bridge (TLC, Apalache, TLAPS); libcmc does not grow a prover.
- A **direct `.cml` -> SMT-LIB** path to Z3/CVC5/veriT is a good idea and
  is **tracked but deferred** (D' above) -- not implemented at this
  stage, since the bridge already reaches those solvers.
- **Elle** and **Pure** are considered and set aside (above).

Agreed order of work, each gated and leaving the examples green:

1. **Land M9 `--fast`** (fork-from-snapshot search, the `m9-fast-wip`
   branch).  It is the unblocker for liveness (F) and scale (G); the open
   bug is known (a resumed connection-mediated node reads a real fd, not
   a checker virtual fd).  Needs a quiet Linux x86-64 host.
2. **Milestone A** -- the value model, kept inside the SMT-encodable
   fragment from the start.
3. **Milestone B** -- the abstract language + modules, with the TLA+
   emission, moving calibration onto the bridge.
4. **Milestone C** -- the binding + refinement checker.  Prototype the
   refinement check on Paxos *before* fixing the `.cml`/`.bind` surface,
   since its soundness is the riskiest part and will reveal what the
   languages actually need.
