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

Documentation, with a guide that builds a first model and the manual
pages: <https://gregburd.codeberg.page/libcmc/> (mirrored at
<https://gburd.github.io/libcmc/>).  Why a new tool rather than TLA+,
Spin, LTSmin, CBMC, GenMC or Stateright: `docs/WHY.md`.

## Build

    make            # build/libcmc.a, build/cmc, build/test_cmc
    make check      # engine tests, bug gates, purity and docs gates, fast calibration
    make install    # libcmc.a, cmc.h and the man pages, under PREFIX=/usr/local

C11 and libc only.  gcc and clang, `-Wall -Wextra -Werror`.  With Nix,
`nix build` gives the library and man pages, `nix flake check` runs
`make check` under gcc and clang, and `nix develop` has the tools for the
docs site.

## Use

1. Write a core: a `CmcCore` vtable over a fixed-size, pointer-free
   per-node state.  `step(cfg, node, state, event, out)` must be total and
   deterministic.  It reads no clock, randomness, globals or I/O, and emits
   effects through `cmc_out_push()`: send, set or cancel a timer, persist,
   wake, reply.  The first `durable_size` bytes of the state are durable.
   They change only through `apply_persist`, which the engine calls when a
   persist is flushed.  `docs/_includes/snippets/quickstart.c` is a
   complete model in one file, and the guide walks through it.
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
| `man/` | `cmc(1)`, `libcmc(3)`, `cmc(7)` |
| `docs/` | the documentation site: guide, design, decision record, review, measurements |

## Status

Release 0.1.0, 2026-11-03.  Single-threaded.
There is no symmetry reduction or on-disk queue yet.  Clocks, partitions
and multi-core nodes are planned (`docs/DESIGN.md`, "Roadmap").

## CI

GitHub Actions runs `make check` under gcc and clang, the MemorySanitizer
gate, the Nix build, and weekly the full calibration.  Codeberg runs one
small `make check` job.  Both forges build the documentation site, check
its links, and publish it to Pages.

## License

Apache-2.0.  See `LICENSE`.
