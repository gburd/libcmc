# libcmc

An explicit-state model checker for C protocol code.  You write a
protocol's logic once, as pure C step functions.  Your product calls
them, and libcmc links the same object files and explores every
interleaving of messages, timers, disk flushes, crashes and connection
resets, up to a bound.  The code checked is the code that runs.

Output follows TLC's wording ("N states generated, M distinct states
found", "The depth of the complete state graph search is D"), and the
search semantics match single-worker TLC: on 22 calibration
configurations of a published protocol model, libcmc gives the same
verdicts, the same shortest counterexample depths, and exactly the same
distinct-state counts as TLC (`calib/hc`).

Why a new tool rather than TLA+, Spin, LTSmin, CBMC, GenMC or
Stateright: `docs/WHY.md`.

## Build

    make            # build/libcmc.a, build/cmc, build/test_cmc
    make check      # engine tests, bug gates, purity gate, fast calibration

C11 and libc only.  gcc and clang, `-Wall -Wextra -Werror`.

## Use

1. Write a core: a `CmcCore` vtable over a fixed-size, pointer-free
   per-node state.  `step(cfg, node, state, event, out)` must be total and
   deterministic.  It reads no clock, randomness, globals or I/O, and emits
   effects through `cmc_out_push()`: send, set or cancel a timer, persist,
   wake, reply.  The first `durable_size` bytes of the state are durable.
   They change only through `apply_persist`, which the engine calls when a
   persist is flushed.  `test/toy/` is a complete small example.
2. Write properties: `CMC_INVARIANT`, `CMC_ACTION_PROPERTY`,
   `CMC_WITNESS_PROP`, `CMC_QUIESCENT_PROP`, `CMC_EVENTUALLY_PROP`.
3. Write a ten-line `main()` that calls `cmc_main()` with your model table
   (`test/cmc_models.c`), and link it against `libcmc.a`.
4. Run it:

       cmc --model mymodel --all-props --clients 2 --crashes 1 --drops 1
       cmc --model mymodel --all-props --simulate --seed 7 --walks 100000
       cmc --model mymodel --all-props --trace-out bug.replay
       cmc --replay bug.replay

5. Gate your cores with `scripts/purity_gate.sh "OBJS" "SRCS"`.  Plant
   bugs behind `#if` in your shipped source, and require each one to be
   caught (`test/toy_gate.sh` shows the pattern).

## Layout

| path | what |
|---|---|
| `include/cmc.h` | the whole public API |
| `src/cmc.c` | the engine: world encoding, fingerprints, BFS/DFS/simulate/replay, environment, liveness |
| `src/cmc_cli.c` | `cmc_main()`, the TLC-style command-line driver |
| `test/` | engine tests, the toy core with planted bugs, purity probes |
| `calib/hc/` | calibration: HummingConsensus.tla, its C port, the TLC cfgs, `calibrate.sh` |
| `scripts/` | the purity gate and its teeth |
| `docs/` | design, decision record, review, measurements |

## Status

Version 0, extracted from pg_chain on 2026-11-03.  Single-threaded.
There is no symmetry reduction or on-disk queue yet.  Clocks, partitions
and multi-core nodes are planned (`docs/DESIGN.md`, "Roadmap").

## License

Apache-2.0.  See `LICENSE`.
