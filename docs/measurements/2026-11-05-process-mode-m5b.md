---
title: "Process mode M5b: absolute time is engine-controlled"
parent: History and provenance
nav_order: 11
permalink: /background/process-mode-m5b/
lede: >-
  A target that reads the wall clock has its clock answered by the checker's logical time -- so a stale-lease write is caught, and replay stays deterministic.
---

Date: 2026-11-05.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-05-process-mode-m5b-raw.txt` (same directory).  Follows M5a;
roadmap in `experiments/process-mode/DESIGN.md`.

## Why

M5a made *waits with a timeout* an environment action -- enough for
elections, leases and retries, which all wait.  But some code reads
*absolute* time and branches on it: "is my lease still valid?", "has this
request timed out?".  For the checker to explore those branches, and for
stateless replay to stay deterministic, it must own the clock the target
reads -- not hand it real wall-clock nanoseconds.

## The vDSO problem, and the fix

The obstacle, measured here: glibc serves `clock_gettime` from the vDSO,
so it never becomes a syscall.  A seccomp trap filter saw **0 of 2000**
calls.  Interposing at the syscall layer alone catches nothing.

The fix rides the preload the prototype already uses for a deterministic
heap (`detmalloc.so`).  It now also defines `clock_gettime`,
`gettimeofday` and `getrandom`, each of which calls `syscall()` directly,
forcing the read off the vDSO and into the checker's seccomp listener.
The checker answers from a **logical clock**:

- it starts at a fixed epoch and advances by one tick **only when the
  checker fires a timeout**;
- so a target's clock reads are a pure function of how many timeouts have
  fired -- which makes a replay reproduce the same times, the property
  M5a's stateless replay would otherwise lose the moment code reads a
  clock;
- `getrandom` returns a fixed pattern, so randomness is engine-chosen
  rather than real entropy (a nondeterministic source would need its own
  budget; deterministic is the honest default).

Three syscalls join the filter whitelist
(`clock_gettime`/`gettimeofday`/`getrandom`); the handler writes the
`timespec`/`timeval`/buffer into the child with the same
`process_vm_writev` path used for message delivery.

## What was run

`demo/lease.c`: a leaseholder that may write to shared storage only while
its lease is valid, written with ordinary blocking sockets and no libcmc
header.  It reads the clock, computes `expiry = start + one tick`, then
blocks on `recv` for a go-ahead.  The checker instead fires the
lease-expiry timeout, which advances the logical clock to `expiry`.

- Correct: on waking it **re-reads** the clock, sees `now >= expiry`, and
  refuses to write.
- `-DBUG`: it decides from the clock value cached *before* it blocked, so
  it writes with a lease that expired while it was blocked -- a
  stale-lease / missing-fence bug.

`lease.model` asserts `no_write_after_expiry`:
`not (node[0][0] == 1 and node[0][1] == 1)` -- byte 0 is "wrote", byte 1
is "the lease had really expired".

## Result

| target | verdict | exit |
|---|---|---|
| `lease` (correct) | `no_write_after_expiry` holds over 2 states | 0 |
| `lease-bug` | violated: `node[0]=1,1` -- wrote after expiry | 1 |

The checker finds the bug on the schedule "fire the lease-expiry
timeout": the buggy node trusts its stale clock read and writes though
its lease is gone.  The bug is visible *only because the checker controls
absolute time*; against a real clock the timing would be flaky and the
model checker could not force it.  Stable 10/10 runs.

**Verdict: M5b passes.**  Absolute-time reads are engine-chosen; a
timing-dependent safety violation is caught in two-process-style code
written as it ships, and replay stays deterministic.

## Still out

General N>2 topologies with network drops, M6 crash and partition in
multi-node, M7 scale, and M8 lifting process mode into `src/`.  Only after
those do the consensus examples (Paxos, Raft, Humming Consensus, ZAB)
arrive, as real implementations.
