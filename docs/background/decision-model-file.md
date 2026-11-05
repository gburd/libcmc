---
title: "Decision: one engine, a model file, the shipping binary"
parent: Background
nav_order: 7
permalink: /background/decision-model-file/
lede: >-
  libcmc validates the code that ships, driven by a declarative model file, over one engine. The step-function core is retired to a calibration role.
---

Date: 2026-11-04.  Status: accepted, implementation started.

## The decision

libcmc is, from here on, three parts:

1. **The engine** -- the environment (FIFO/bag network, crashes with
   partial durability, timers, client budget, bounded liveness), the
   search (BFS/DFS/simulate/replay), the 128-bit fingerprint visited set,
   and the properties.  Already built, and calibrated against TLC.
2. **A model file** -- a small declarative file that names the binary to
   check, the node count, how to read each node's state, the environment
   budget, and the invariants.  It carries no behaviour.
3. **The shipping binary** -- run under process-mode syscall interception.
   Running it *is* the protocol transition.  Nothing is transcribed.

The step-function core (`CmcCore`, `src/` 0.1.0) is **retired to a
calibration role**: it is how the engine is cross-checked against TLC, not
a way users write models.  It is not deleted (see "Why keep it").

## Why

The project exists because a TLA+ spec is a second program that can
diverge from the code that ships.  But `CmcCore` had the same flaw: a
core is a C transcription of the protocol, a second implementation that
can diverge too.  Neither validated the shipping code.

A model is four separable things: an initial state, a *Next* relation, the
invariants, and a notion of state equality.  *Next* is the only part that
can diverge, and it is really two things:

- **Protocol nondeterminism** -- what the code does on an event.  This
  must come from running the real code; running it *is* the transition.
- **Environment nondeterminism** -- message order, drops, crashes, which
  client acts, the clock, partitions.  This is not in the shipping code
  and should not be (production code does not choose when it crashes).  It
  is legitimately external, and the engine already owns it.

So the model file supplies only the environment budget, the observation
points, and the invariants.  The protocol is executed, never written
twice.  The divergence surface shrinks from "a whole second program" to
"a declarative wiring file plus a small state serialiser in the code."

## Why keep the step-function core

The calibration proves the engine's search and dedup match TLC exactly on
22 HummingConsensus configurations.  TLC checks a *shared-register* spec,
whose transitions are an environment-chosen enumeration of a `Next`
relation; a TLA+ spec is pure/direct by nature, not a running Unix
process.  `CmcCore`'s direct mode is the only way to present that same
enumeration to the engine and compare counts with TLC.  Delete it and the
engine loses its only tie to an independent oracle.  So it stays, as the
calibration harness, demoted from the user-facing API.

HummingConsensus therefore stops being a thing to re-validate with process
mode.  Its exact state counts are an artifact of the shared-register
encoding, not of the protocol; reproducing them with a message-passing
implementation is not well defined.  The invariants (agreement, no lost
ack) carry over unchanged; only what they are checked against changes --
the running implementation instead of a separate spec.

## What can still diverge, and the guard

- **The invariants vs the operator's intent.**  A wrong invariant checks
  the wrong thing.  True of all verification, TLA+ included; it is not
  model-code divergence.
- **The state hook vs the real state.**  If the canonical-emit hook omits
  a field that affects future behaviour, dedup merges states that differ
  and a bug hides.  This is the one genuine new divergence surface, the
  same hazard as uninitialised padding in a `CmcCore` state.  It is
  guarded the same way: a completeness check, not trust (a future
  milestone; noted so it is not forgotten).
- **The model file itself** carries no behaviour, so it cannot
  behaviourally diverge.

## The model file

A declarative file (see `docs/model-file.md` for the schema).  Sketch:

```
[target]
binary = ./myserver
nodes  = 3
state  = canon            # read each node's state from its fd-3 emit

[environment]
clients = 1
drops   = 1
crashes = 1
net     = fifo

[invariants]
no_lost_ack = "every acked value is in that node's durable state"
agreement   = "no two nodes commit different values at one slot"
```

The invariant language is a tiny, non-Turing-complete expression DSL over
each node's canonical state and the ghost reply log, with a C-predicate
escape hatch for properties the DSL cannot express.  It is the one
genuinely new design problem and is expected to take a few iterations.

## Consequences

- Process mode graduates from `experiments/` into the engine as its
  second transition provider, driven by the model file.
- The documented way to use libcmc becomes: write a model file, point it
  at your binary, run `cmc`.  One front door.
- The next milestone is multi-node process mode driven by a model file,
  with invariants, against a small real replicated server -- not
  HummingConsensus.
- `CmcCore` and `calib/hc/` remain, run only by the calibration gate.
