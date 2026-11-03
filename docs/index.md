---
title: Home
layout: home
hero: false
nav_order: 1
---

# libcmc
{: .fs-9 }

An explicit-state model checker for C protocol code.  The code it
checks is the code that ships.
{: .fs-6 .fw-300 }

[Get started]({{ '/guide/01-install/' | relative_url }}){: .btn .btn-primary .fs-5 .mb-4 .mb-md-0 .mr-2 }
[cmc(1)]({{ '/reference/man/cmc.1/' | relative_url }}){: .btn .fs-5 .mb-4 .mb-md-0 }

---

You write a protocol's logic once, as pure C step functions: a node
gets an event (a message, a timer, a finished disk write, a client
request, a restart) and emits effects (send, set a timer, persist,
reply).  Your product calls those functions.  libcmc links the same
object files and explores every order in which the events can arrive,
including lost messages, crashes that keep only some of the pending
writes, and connections that reset, up to the budgets you set.  It
checks your properties in every state it reaches, and when one fails it
prints the shortest sequence of events that breaks it.

There is no second model to keep in sync with the code.  A TLA+
specification is a second program, and the two drift; with libcmc the
model is the C you ship.

## What it checks

- Invariants, in every reachable state.
- Action properties, on every transition.
- Witnesses: states that must be reachable, so a property cannot pass
  just because nothing interesting happened.
- Quiescence properties, in states where nothing more can happen.
- Bounded liveness: from every state, the fair schedule reaches the goal
  within K steps.

## How you know a pass means something

- The search has the semantics of TLC with one worker.  On 22
  configurations of a published consensus specification, libcmc gives
  the same verdicts, the same shortest counterexample depths, and the
  same distinct-state counts as TLC
  ([calibration]({{ '/background/calibration/' | relative_url }})).
- The engine decides what can happen next only from what the cores
  emitted, never by reading their state
  ([design]({{ '/reference/design/' | relative_url }})).
- A purity gate rejects step code that reads a clock, calls a function
  other than `mem*`, keeps writable globals, or uses `asm`.  A
  determinism gate runs every step twice.  Planted bugs in the shipped
  source must each be caught.

## Where to go next

1. [Install and build]({{ '/guide/01-install/' | relative_url }}).
2. [Your first model]({{ '/guide/02-first-model/' | relative_url }}): a
   write that must survive a crash, a planted bug, and the counterexample
   libcmc prints for it.
3. [Using libcmc in your project]({{ '/guide/03-in-your-project/' | relative_url }}):
   linking it, gating your cores, planting bugs, and running it in CI.
4. The manual pages: [cmc(1)]({{ '/reference/man/cmc.1/' | relative_url }}),
   [libcmc(3)]({{ '/reference/man/libcmc.3/' | relative_url }}),
   [cmc(7)]({{ '/reference/man/cmc.7/' | relative_url }}).
5. [Why libcmc]({{ '/why/' | relative_url }}) rather than TLA+, Spin,
   LTSmin, CBMC, GenMC or Stateright.

libcmc is C11 and libc only, about 3,400 lines, Apache-2.0.  It is
single-threaded and keeps its state space in memory.
