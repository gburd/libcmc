---
title: Home
layout: home
hero: false
nav_order: 1
---

# libcmc
{: .fs-9 }

An explicit-state model checker for C protocol code.  It checks the
program you ship.
{: .fs-6 .fw-300 }

[Get started]({{ '/guide/01-install/' | relative_url }}){: .btn .btn-primary .fs-5 .mb-4 .mb-md-0 .mr-2 }
[cmc(1)]({{ '/reference/man/cmc.1/' | relative_url }}){: .btn .fs-5 .mb-4 .mb-md-0 }

---

Each node is your program, run unmodified as a process, and libcmc is
the network, the clock and the crash between them.  A model file names
the program, the environment to explore (timeouts, crashes, lost
messages) and the invariants.  libcmc tries every order of connections,
messages and faults up to those budgets, checks the invariants in every
state, and when one fails prints the shortest sequence of actions that
breaks it.

The program needs no libcmc header and no rewrite for the checker: an
eight-line hook that reports its state on a file descriptor, and a
`-no-pie` build.  There is no second model to keep in sync with the
code; the model is the program.

## What it checks

- Invariants over each node's reported state and the log of replies
  clients received, in every reachable state.
- Under an environment you bound: client writes, timeouts (each advances
  a logical clock the program reads), crashes that keep only what a node
  made durable, and lost messages.

## How you know a pass means something

- A bound is not a pass.  A search stopped by a depth, state or time
  limit, or a simulation, reports "no violation found" and exits 2.
- What it does not implement is an error, not a guess: an unknown
  model-file key, a write longer than it models, a program blocked in a
  call it does not intercept, a missing preload.
- The fingerprint holds everything a run's future depends on: each
  node's state and connections, every message in flight in order, the
  budgets left, the clock.  Probes for each way it could merge two
  different states are in the test suite.
- The search engine has the semantics of TLC with one worker.  On 22
  configurations of a published consensus specification, libcmc gives
  the same verdicts, shortest counterexample depths and distinct-state
  counts as TLC
  ([calibration]({{ '/background/calibration/' | relative_url }})).

## Where to go next

1. [Install and build]({{ '/guide/01-install/' | relative_url }}).
2. [Your first model]({{ '/guide/02-first-model/' | relative_url }}): a
   replicated server written as it ships, a write that must survive a
   crash, a planted bug, and the counterexample libcmc prints for it.
3. [Using libcmc in your project]({{ '/guide/03-in-your-project/' | relative_url }}):
   what your program must do, the model file, planted bugs, and CI.
4. The manual pages: [cmc(1)]({{ '/reference/man/cmc.1/' | relative_url }}),
   [libcmc(3)]({{ '/reference/man/libcmc.3/' | relative_url }}),
   [cmc(7)]({{ '/reference/man/cmc.7/' | relative_url }}).
5. [Why libcmc]({{ '/why/' | relative_url }}) rather than TLA+, Spin,
   LTSmin, CBMC, GenMC or Stateright.

libcmc is C11 and libc only, Apache-2.0.  Checking a program needs
Linux on x86-64.  It is single-threaded and keeps its state space in
memory.
