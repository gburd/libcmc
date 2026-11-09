---
title: "Process mode M5: time is an environment action"
parent: History and provenance
nav_order: 10
permalink: /background/process-mode-m5/
lede: >-
  A blocked recv is satisfied by a message or by a timeout the search fires, so leader election is explorable -- and a split brain is caught.
---

Date: 2026-11-05.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-05-process-mode-m5-raw.txt` (same directory).  Follows M4;
roadmap in `experiments/process-mode/DESIGN.md`.

## Why

Raft, ZAB and Humming Consensus elect leaders and hold leases on
*timeouts*: a node that hears nothing before its timer fires acts.  To
check such code, the model checker must treat "the timer fired" as a
nondeterministic event it controls -- the same way a TLA+ Raft spec has a
`Timeout` action -- not as real wall-clock time.  M5 adds that.

## The mechanism, and why it needed no new interception

A node waiting for a heartbeat is blocked on `recv`.  M4 already
represents a blocked `recv` as a *deferred* seccomp notification.  M5
observes that a timeout is simply a *second way to satisfy that same
blocked recv*: instead of delivering bytes, the checker answers the
deferred recv with a one-byte `'T'`.  So a timeout is a pseudo-message
(from synthetic peer `src = -2`), offered as a deliverable action
whenever a node is blocked and a `timeouts` budget remains.  No clock, no
new syscall -- it reuses M4's machinery.

One change was needed: a channel `recv` now **always** defers, even when
a message is already buffered.  Delivery must be an environment choice,
or the search cannot explore message-versus-timeout.  The budget
(`timeouts = K` in the model file) keeps the search finite, exactly as
the drop and crash budgets do.

## What was run

`demo/elect.c`: a two-node election sketch, ordinary blocking sockets, no
libcmc header.  Node 0 starts leader (term 1) and sends one heartbeat;
node 1 is the follower, blocked on `recv`.  The checker delivers either
the heartbeat or a timeout.

`elect.model`:

```
[target]
nodes = 2
[environment]
client = 0          # node 0 initiates; no client write
timeouts = 1        # the follower's election timer may fire once
depth = 4
[invariants]
one_leader_per_term = "not (node[0][0] == 1 and node[1][0] == 1 and node[0][1] == node[1][1])"
```

`node[i][0]` is node i's role (1 = leader), `node[i][1]` its term.

- Host: Linux 6.18.60, x86_64, glibc 2.44, `-no-pie` targets.
- `experiments/process-mode/run.sh` (M0..M5).

## Result

| target | verdict | exit |
|---|---|---|
| `elect` (correct) | `one_leader_per_term` holds over 3 states | 0 |
| `elect-bug` | violated: `node[0]=1,1` and `node[1]=1,1` -- two leaders at term 1 | 1 |

The correct follower, on a timeout, becomes leader at term 2, strictly
above the old leader's term, so the two are never leaders at the same
term.  The `-DBUG` follower promotes without advancing its term; the
checker fires the timeout and finds the split brain -- both nodes leader
at term 1.  Stable 10/10 runs.

**Verdict: M5a passes.**  Time, as a bounded nondeterministic timeout, is
an environment action the search explores, and a timing-dependent safety
violation (split brain) is caught in two-process code written as it ships.

## Absolute-time reads (M5b, measured, not yet built)

M5a covers waits-with-timeout, which is how elections, leases and retries
all behave.  A target that reads *absolute* time needs that made
engine-chosen too.  Measured here: a seccomp trap filter on
`clock_gettime` saw **0 of 2000 calls** -- glibc serves them from the
vDSO, invisible to seccomp.  An `LD_PRELOAD` shim forcing `clock_gettime`
through a real `syscall()` made every call trap.  So M5b will extend the
preload (already loaded as `detmalloc.so`) to route
`clock_gettime`/`gettimeofday`/`getrandom` to syscalls the checker
answers with a logical clock that advances only at timeout events,
keeping stateless replay deterministic.  The mechanism is proven; the
code is the next step.

## Still out

General N>2 topologies, routing through the engine's real FIFO/bag with
drops, M5b absolute-time reads, M6 crash and partition in multi-node, M7
scale, and M8 lifting process mode into `src/`.  Only after those do the
consensus examples (Paxos, Raft, Humming Consensus, ZAB) arrive, as real
implementations.
