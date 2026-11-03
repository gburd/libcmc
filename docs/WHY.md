# Why libcmc exists

libcmc model-checks C protocol code by exploring every interleaving of
the C functions that ship, not of a separate model.  This file records
why a new tool was written instead of using an existing one.  It is the
decision record; `01-survey-and-design.md` has the full survey with
sources, and `02-review-actor-mode.md` the adversarial review.

## The requirement

The first user, pg_chain (`~/ws/pg_chain`), asked for "TLA+ like model
validation that uses the sources we build directly so that the model
and the code can never diverge: the code validated is the code that is
run".  Its protocol logic must also be 100 % C, and no other language
may be a test oracle or sit on the product path.

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
