---
title: "When libcmc fits"
nav_order: 7
permalink: /fit/
lede: >-
  Where an explicit-state checker of the shipping code is the right tool,
  and where it is the wrong one.
---

libcmc is an explicit-state model checker: it enumerates reachable
states of the program you ship, exhaustively up to a bound, and checks a
safety property in every one.  That shape makes it strong for some
problems and the wrong tool for others.  This page says which, plainly,
so you do not reach for it where it cannot help.

## The shape of the tool

- **Explicit-state, bounded, exhaustive.**  It visits each distinct
  reachable state once, breadth-first (shortest counterexample) or
  depth-first, up to a finite instance you choose: a few nodes, a few
  messages, a bounded number of crashes, drops and timeouts.  It also
  runs seeded simulation and replay.
- **It checks the code, not a model of it.**  Each node is your
  compiled binary, run as a process; libcmc is the network, the clock
  and the crash between them.  A pass is a statement about the program,
  not a transcription of it.
- **Safety is a state or step invariant.**  The property holds in every
  reachable state (an invariant), or every step refines an abstract
  action (refinement).  Bounded liveness exists (`eventually`, leads-to,
  under a fixed fair schedule) but is not full temporal logic.
- **Finite, small domains.**  The abstract `.cml` fragment is finite
  integers and finite maps; the engine caps nodes, message sizes and
  the like.  Symmetry reduction helps when values are interchangeable.

## Where it is a strong fit

Read this section with the [soundness remediation plan]({{ '/reference/plan-soundness/' | relative_url }}):
an audit of 1.3.0 found false passes, so these are the problems the tool
is designed for, not ones on which a pass is yet evidence.

- **Crash-fault agreement and replication protocols at a small
  instance.**  Safety in this family often rests on quorum intersection
  (two quorums overlap, so a write-once or vote-once discipline on the
  overlap forbids two decisions), and a three- to five-node instance can
  expose a classic bug.  The `examples/spec/` models illustrate this
  shape; they are small demonstrations, not complete models of the
  protocols they are named after (see the examples page).
- **The quorum-intersection knob.**  Undersizing a quorum so two disjoint
  quorums can each decide is a bug class this search finds at a small
  instance.  `fpaxos.cml` runs Paxos's ballots and promises with a
  phase 1 quorum of 3 and a phase 2 quorum of 2 out of 4 acceptors, and
  a phase 1 quorum of 2 breaks agreement.
- **Commit durability and "no committed entry lost."**  The
  single-contested-slot reduction (as in the Raft-log process example)
  checks that an acknowledged value survives the crashes in the budget.
  `zab.cml` is a guarded counter, not a model of ZAB's recovery.
- **A real implementation against an invariant.**  `cmc --refine` maps
  the binary's observable state onto an abstract `.cml` and checks the
  abstract invariants on it.  That is invariant reuse, not proof that
  the binary's steps are the model's steps.  `--refine-steps` is meant
  to check each step, but it currently skips the initial-state and
  action-gate checks (remediation 1.1, 1.2).
- **Finding the shortest failing schedule.**  Breadth-first search
  returns the shortest interleaving that breaks the property, which is
  what you want to read and to turn into a regression test.  This is the
  use the tool serves well today: a counterexample it prints is a real
  schedule you can replay, even where a pass is not yet trustworthy.

## Where it is a partial fit

- **Multi-slot logs.**  Full state-machine safety over an unbounded log
  (log matching across all slots, contiguity, commit-index
  monotonicity) is not directly expressible.  The honest reduction is a
  single contested slot: it catches a lost acknowledged entry at that
  slot, but does not check the across-slot
  properties.  Model one slot, and state that limit.
- **Protocols whose interesting objects are not interchangeable.**
  Reconfiguration across many configurations, or ordering over many
  commands: a two-configuration or single-steal reduction checks the one
  quorum-intersection or ordering knob at a tiny instance; the full
  protocol over many objects is out of reach because symmetry cannot
  collapse objects that behave differently.

## Where it is the wrong tool

- **Sole evidence for a safety-critical system.**  Banking ledgers,
  launch control, medical devices: do not accept such a system on a cmc
  pass.  1.3.0 has reproduced false passes, and even a sound bounded
  check covers only the instance and budgets it searched.  Use it, once
  remediated, as one input beside formal specifications, proofs where
  feasible, review, testing and fault injection.
- **Liveness and progress under partial synchrony.**  "Eventually a
  value is chosen," leader stabilization, termination under an
  adversarial-but-fair scheduler: these are what the FLP result and the
  partial-synchrony model are about.  libcmc's liveness is bounded and
  under a fixed fair schedule; it can show "under this schedule the goal
  is reached within L steps," not general termination.  Use a tool built
  for temporal reasoning (TLC's liveness checking, for one).
- **Real-time and timing.**  Timeouts, leases and clock skew as real
  time are not modelled; the clock is bounded logical time.  A protocol
  whose correctness rests on a timing assumption needs a timed model
  checker.
- **Byzantine faults and cryptography.**  Safety that rests on
  cryptographic non-forgeability or non-equivocation (signatures, Merkle
  trees) has no faithful finite encoding here: an adversary that cannot
  forge trivializes it, and one that can explodes it.  Use a
  Byzantine-aware method.
- **Performance.**  Latency, throughput and message count, the point of
  optimizations like Fast, Ring and WPaxos, are not correctness
  properties a model checker evaluates.  Those optimizations change
  safety only through their quorum knobs, which this tool does check; the
  speedup itself it does not.
- **Very large or unbounded state spaces.**  If the bug only appears at a
  scale the bounded search cannot reach, even with symmetry, an
  explicit-state checker will run out of memory before it finds it.  A
  symbolic engine (Apalache, reached through `cmc --emit-tla`) or a
  proof (TLAPS) is the better path for large or unbounded domains; the
  bridge is there for exactly that hand-off.

## The rule of thumb

If the property is a safety invariant, the bug shows up at a handful of
nodes, and you want the guarantee to be about the code you ship, libcmc
is a strong fit.  If the property is liveness, timing, Byzantine or
performance, or the bug only appears at a scale no bounded search
reaches, it is the wrong tool, and the `--emit-tla` bridge hands the
model to an engine that is the right one.
