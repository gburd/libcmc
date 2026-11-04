---
title: "Process mode M1: canonical state and a real search"
parent: Background
nav_order: 5
permalink: /background/process-mode-m1/
lede: >-
  The milestone that decides whether process mode scales: a state reached two ways is counted once.
---

Date: 2026-11-04.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-04-process-mode-m1-raw.txt` (same directory).  Follows
`2026-11-04-process-mode-m0.md`.

## Why this is the milestone that matters

M0 ran one path with one branch and caught a durability bug in a server
written as it ships.  The open question was whether process mode can be a
real model checker, which needs two things M0 did not have: a **canonical
state**, so two runs that reach the same logical state are recognised as
one; and a **search** that branches over interleavings.  Without the
first, an exhaustive search over any protocol whose states reconverge
never terminates -- it re-explores the same state by every order that
reaches it.  That is M1's kill criterion.

## What was run

`demo/kv.c`: an ordinary one-connection TCP server, no libcmc header, no
step function.  It holds two independent one-byte cells in a declared
region, `unsigned char cmc_state[2]`.  The client may SET a or SET b, one
message each.  The orders (a then b) and (b then a) reach the same state.

`search.c`: a breadth-first search over client-message prefixes.  To
reach a state it launches a fresh server under seccomp user-notification
interception (the M0 mechanism) and replays the prefix -- stateless
replay, no process snapshots.  It finds the `cmc_state` address from the
binary's ELF symbol with `nm` (so the target carries no checker API),
reads the region from the child with `process_vm_readv` after each ACK,
and dedups states with the same 128-bit fingerprint actor mode uses.

- Host: Linux 6.18.60, x86_64, glibc 2.44.
- `experiments/process-mode/run.sh` (M0 cases plus the two M1 cases).

## Result

| target | verdict | exit |
|---|---|---|
| correct `kv` | 4 distinct states, 5 dedup merges; invariant holds | 0 |
| `kv` built `-DBUG` | invariant violated; prefix `SET-b SET-a` | 1 |

The reachable states are `00, 10, 01, 11`.  Both `10 -> 11` and
`01 -> 11` reach `11`, and their fingerprints are equal, so the diamond
closes: four distinct states, not five, and the search terminates.
**Verdict: M1 passes.**  A canonical state over a declared region dedups
correctly, and a real search over interleavings terminates because of it.

Built `-DBUG`, SET a also clears b, so the order "set b, then set a"
loses b.  The invariant "a cell once set stays set" catches it, and BFS
reports the shortest prefix that triggers it.

## A hole found and closed

A position-independent (PIE) build first passed with a wrong answer:
`nm` gives the link-time symbol address, which ASLR relocates at runtime,
so the checker read the wrong memory and saw every state as all-zero --
"1 distinct state", a silent false pass.  The checker now reads the ELF
type and **refuses a PIE** rather than report a wrong result; M1 is
`-no-pie` by design.  Lifting that means adding the load bias from
`/proc/PID/maps`, a later step.

## What this establishes, and what it does not

Established: a canonical state over a declared region is enough to dedup
a reconverging protocol and make an exhaustive, terminating search over
interleavings of deployed-style code, with shortest-prefix
counterexamples -- the same guarantees actor mode gives, on code with no
step function.

Not established, and the next milestone: canonicalising a **real
allocator's heap**, the stack and TLS, not just a region the target
names.  That is the central problem (CMC spent 165-185 lines per program
on it); if it cannot be made to scale, the declared-region form is the
limit of process mode and the step-function mode stays the answer for
deep search.  Also still out: several nodes, the clock and the vDSO,
threads with a controlled scheduler, and wiring libcmc's engine
(properties, replay, `--simulate`, the TLC calibration) onto the process
frontier.

## Honest limits

- Stateless replay re-executes the server once per state.  Combined with
  a fork and a memory read per step, this is thousands of states per
  second, not the millions actor mode does.
- The declared region must be fully owned and initialised by the target
  (the demo zeroes `cmc_state` at startup); uninitialised padding in it
  would break dedup, the same hazard actor mode guards with
  MemorySanitizer.
- `-no-pie` only, until the PIE load bias is read.
- The vDSO, threads and inline-assembly syscalls are not handled yet.
