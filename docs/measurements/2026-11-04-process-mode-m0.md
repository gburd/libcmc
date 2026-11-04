---
title: "Process mode M0: catching a bug in code written as it ships"
parent: Background
nav_order: 4
permalink: /background/process-mode-m0/
lede: >-
  A feasibility prototype for the north star: check deployed C, with no step function, at the syscall boundary.
---

Date: 2026-11-04.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-04-process-mode-m0-raw.txt` (same directory).

## Why

libcmc 0.1.0 checks the shipped object files, but only code written as
pure, deterministic step functions (`CmcCore`).  The core ask is to prove
C code written **as it will be deployed** -- real sockets, disk and
timers, normal control flow, no rewrite for the checker.  `docs/WHY.md`
explains why a CBMC fork is not the way there.  Process mode is the
candidate: run the real program and model-check it at the boundary where
it talks to the OS.  M0 is the feasibility gate.

## What was run

An ordinary one-connection TCP server (`demo/server.c`): blocking
sockets, a real `write()`+`fsync()` to a file, no libcmc header, no step
function.  Built with `-DBUG` it sends the ACK before the `fsync` (the
"acknowledge before durable" bug).  The checker (`checker.c`) runs that
binary unmodified under seccomp user notification, injects one client
WRITE, and owns two environment events: the ACK send and a crash that may
land between the ACK and the flush.  Property `NoLostAck`: after a crash,
an acknowledged value is on disk.

- Host: Linux 6.18.60, x86_64, glibc 2.44, no ptrace restriction
  (`yama/ptrace_scope` = 0), no pre-existing seccomp filter.
- `experiments/process-mode/run.sh`, four configurations.

## Result

| server | crash budget | verdict | exit |
|---|---|---|---|
| correct | 0 | NoLostAck holds | 0 |
| correct | 1 | NoLostAck holds | 0 |
| buggy | 0 | NoLostAck holds | 0 |
| buggy | 1 | **NoLostAck VIOLATED** (acked W 1, crash before flush, disk lost it) | 1 |

The same shape as the step-function quickstart
(`docs/guide/02-first-model/`): the bug is invisible without a crash, and
caught with one -- but here the code under test is written as it ships.
**Verdict: M0 passes.**  Running the deployed program and model-checking
it at the syscall boundary catches a real durability bug with no step
function and no rewrite.

## What this does and does not establish

Established: the mechanism works on a current Linux.  Seccomp user
notification traps the owned syscalls; the listener fd survives `execve`
(a `SIGSYS` handler does not); `process_vm_readv`/`writev` with a
`PTRACE_SEIZE` read and write the child's memory; a real file plus a
modelled `fsync` gives honest durability, so a crash before the flush
loses the write.

Not established, and deliberately out of M0: a general search (M0 is one
request and one branch, not BFS over interleavings); **memory
canonicalization** so that equal states hash equal (the central problem
beyond M0; CMC spent 165-185 lines per program on it); `fork()`-based
state copying; the clock and the vDSO; threads with a controlled
scheduler; and wiring the existing engine (fingerprint table, properties,
replay, `--simulate`, the TLC calibration) onto the process frontier.
`experiments/process-mode/DESIGN.md` lists these as milestones, each with
its own kill criterion.  Only after them would process mode become a
second mode in `src/`.

## Honest limits

- One fork and one memory hash per step will be thousands of states per
  second, not the millions actor mode does.  Process mode is for checking
  real glue at shallow-to-moderate depth, not the deep exhaustive runs the
  step-function cores get.
- The vDSO serves `clock_gettime` without a syscall, escaping seccomp; a
  real target needs it disabled or patched.
- Inline assembly that issues `syscall` from an unexpected site, and
  threads, are not handled yet.
- Bugs the protocol never drives the code into (a malformed packet in a
  parser) are for CBMC or a fuzzer, a separate check.
