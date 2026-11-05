---
title: "Why libcmc"
nav_order: 5
permalink: /why/
lede: >-
  Why a new checker rather than TLA+, Spin, LTSmin, CBMC, GenMC or Stateright.
---

libcmc model-checks C protocol code by exploring every interleaving of
the C functions that ship, not of a separate model.  This file records
why a new tool was written instead of using an existing one.  It is the
decision record; `01-survey-and-design.md` has the full survey with
sources, and `02-review-actor-mode.md` the adversarial review.

**What 0.1.0 checks, and what it does not.**  libcmc links the object
files the product links, so there is no second model to keep in sync.
But it checks only code written as pure, deterministic step functions
(`CmcCore`); the I/O glue around a core -- the sockets, the disk, the
timers -- is not checked.  The core ask is to prove C code written **as
it will be deployed**, with no such rewrite.  0.1.0 does not meet it.
The path to it is process mode, not a different tool: see "The north
star" below and `docs/measurements/2026-11-04-process-mode-m0.md`.

## The requirement

The request was for "TLA+ like model validation that uses the sources
we build directly so that the model and the code can never diverge: the
code validated is the code that is run".  The protocol logic must also
be 100 % C, and no other language may be a test oracle or sit on the
product path.

From that, four needs:

1. **Same object code.**  The checker links the object files the product
   links.  No transcription, no generated code, no annotations that change
   behaviour.
2. **Exhaustive to a bound**, with TLC-grade answers: shortest
   counterexamples, exact distinct-state counts, action properties,
   witnesses, and bounded liveness.
3. **A distributed-systems environment**: message loss and reordering,
   FIFO connections that reset on crash, crash and restart with partial
   durability, timers, and fault budgets.  The checker may learn about the
   system only from what the code emits, never by reading its internals
   (the "omniscient oracle" that hid 8 bugs in libxtc's simulator).
4. **C only, small, buildable with `cc` and `make`** on any host.

## Why the existing tools fall short

| tool | runs the shipped C? | why not |
|---|---|---|
| TLC / TLA+ | no: the spec is a second program | It is exactly the divergence the requirement rules out.  libcmc uses TLC as its calibration oracle instead (calib/hc). |
| Spin with `c_code`/`c_track` | yes, the C runs inside Promela | The environment (network, crashes, timers) would be written in Promela, a second language on the critical path.  C state is tracked by address and size, with no canonical form.  This is the documented fallback if libcmc stalls. |
| LTSmin (PINS, dlopen API) | yes, through a C next-state function | It is the closest fit: a C language module plus mature search (multi-core, POR, LTL, symbolic).  It brings no environment model, so everything in need 3 would still have to be written.  It is also a large autotools build with several dependencies, not in nixpkgs, with the last release 3.0.2 in 2018 and the last commit in 2024.  It is a candidate *back end* if libcmc's own search becomes the bottleneck: a `CmcCore` can be exposed as a PINS module. |
| CBMC / ESBMC | yes | Bounded model checking of one program.  It explores thread interleavings, not a network of processes with crashes and message buffers, and the formulas blow up for protocols this long.  The CBMC 6.x in nixpkgs also aborts on libxtc's concurrency harnesses.  CBMC is still used, for shared-memory code inside one node. |
| GenMC, Nidhugg, CDSChecker | yes, at LLVM IR | They check memory-model interleavings of threads in one process: the right tool for lock-free code, the wrong one for messages, crashes and durability.  None is in nixpkgs. |
| DIVINE | yes, C/C++ via LLVM | Threads in one program again, with no network or crash environment.  The GitHub mirror's last commit is from 2021. |
| KLEE | yes | Symbolic execution of input paths.  Useful for codecs at a trust boundary, not for interleavings. |
| Stateright | yes, but Rust | It has the right design: the actor code is both the model and the deployment, with BFS/DFS/simulation, crash and recover, and network semantics per model.  But it is Rust.  libcmc's API copies its shape in C. |
| CMC, MaceMC, MoDist, Demeter, SAMC | yes | These are research systems: papers, not maintained tools.  CMC (2002) is the template libcmc follows, and MaceMC supplied the liveness idea. |
| P / P# / Coyote | the compiled program, yes | They work on their own language or on .NET.  Their search is randomized or priority-based, not exhaustive. |
| FoundationDB simulation, TigerBeetle VOPR, libxtc DST | yes | Deterministic simulation: seeded random walks, not exhaustive, and tied to each system's runtime.  libcmc's `--simulate` is this mode, over the same harness. |

Summary: no maintained tool meets needs 1-4 together.  The tools that run
real C either check threads in one process (CBMC, GenMC, DIVINE) or
supply only the search, not the environment (LTSmin, Spin).  The tools
with the right environment are in another language (Stateright, P) or
were never released as tools (CMC, MaceMC).

## What it cost, and what it showed

- Engine, header and CLI: 3.4k lines of C.  HC calibration core: 1.8k.
  Tests, toy core and probes: 1.75k.  Written in about a day, on
  2026-11-02.
- Calibration: on all 22 HummingConsensus configurations, libcmc gives
  the same verdict, the same shortest counterexample depth and the same
  distinct-state count as single-worker TLC.  Single-threaded it is 30-70x
  faster (`docs/measurements/2026-11-02-mc-calibration.md`).
- An independent review found 5 ways a realistic bug could pass in actor
  mode.  Each is now a regression test, and each is fixed
  (`02-review-actor-mode.md`).

## When to revisit

- If the state spaces of real cores outgrow single-threaded BFS:
  parallelise first (sharded table), then consider LTSmin as a back end.
- If someone has to write a second environment model in another
  language, the case for Spin or LTSmin returns.

## The north star

The step-function interface is a restriction the core ask does not want:
it checks logic extracted into pure functions, not code written as it
ships.  The way to lift it is not a different tool but a second mode of
this one.

**Why not fork CBMC.**  CBMC proves properties of one C program up to a
fixed unwinding.  Three things rule out a fork as the answer:

- It still needs the environment (the network, the disks, crashes)
  written as C harness code -- a second implementation of exactly what
  need 3 above rules out.  CBMC's library models `read` and `write` and
  pthreads, but nothing for `socket`, `connect`, `send`, `recv`, `poll`,
  `epoll_wait`, `fsync` or `pwrite`.
- It does not scale to protocol depth.  On the HC core already in
  step-function form -- the easy case -- stock CBMC 5.91 proved one step
  in 59 s using 7.9 GB, and had not finished two steps when stopped; the
  bug is six steps deep.  libcmc finds it in 0.01 s.  Production code with
  loops, buffers and `malloc` is worse.
- It refuses threaded code that writes shared pointers ("pointer handling
  for concurrency is unsound", open since 2016).

CBMC earns its place on single functions at a trust boundary (a parser, a
codec): use it there, as an ordinary dependency.

**Process mode.**  Run the deployed program itself, and model-check it at
the boundary where it calls the OS: intercept the syscalls, serve them
from libcmc's existing environment model, count a step as run-to-syscall,
branch with `fork()`, and recognise repeated states by hashing the
program's memory.  This is CMC's method (OSDI 2002) and MoDist's (NSDI
2009), and it keeps everything 0.1.0 has: the search, the environment,
the properties, replay and the TLC calibration.  The step-function mode
stays as the fast path for deep exhaustive search, not a requirement.

A feasibility prototype catches the "acknowledge before durable" bug in
an ordinary blocking TCP server with a real `fsync`, no step function
(M0); a second milestone runs a terminating breadth-first search over the
interleavings of a server with no step function, deduping states that
reconverge (M1); and a third takes a real `malloc` heap and finds the
boundary (M2): a deterministic allocator makes one schedule reproducible,
but a reconverging search over heap state needs a small opt-in
canonical-emit hook (~8 lines on the target, not a step-function
rewrite).  A fourth wires it to a declarative **model file** (M3): the
file names the binary, the environment to explore, and the invariants (a
small expression DSL), and the running binary is the transition relation
-- the shape this document's decision record settled on
(`docs/background/decision-model-file.md`, `docs/model-file.md`).  A fifth
goes multi-node (M4): the checker runs several real node processes and
owns the network between them, choosing the delivery order, and catches
an acknowledged write a backup never received in a two-node replicated
register written as it ships.  A sixth makes time an environment action
(M5): a blocked `recv` is satisfied by a message or by a bounded timeout
the search fires, so leader election is explorable, and a split brain --
two nodes leader at the same term -- is caught in a two-process election
sketch.  See
`experiments/process-mode/` and the measurements
`docs/measurements/2026-11-04-process-mode-m0.md`, `-m1.md`, `-m2.md`,
`2026-11-05-process-mode-m3.md`, `-m4.md`, `-m5.md`, and `-m5b.md` (the
last makes absolute-time reads engine-controlled, catching a stale-lease
write only the checker's clock can force).
The conclusion: process mode checks arbitrary heap-using C along a fixed
schedule with no target change, and a full reconverging search needs that
small hook; if even that proves unworkable at scale, the step-function
mode remains the shipping answer.
