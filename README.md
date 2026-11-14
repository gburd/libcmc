# libcmc

An explicit-state model checker for C protocol code, which checks the
program you ship.  Each node is that program, run unmodified as a
process; libcmc is the network, the clock and the crash between them.
A model file names the program, the environment to explore and the
invariants:

    [target]
    binary = ./replog
    nodes = 3
    [environment]
    client_write = 5
    crashes = 1
    drops = 1
    [invariants]
    no_lost_ack = "all r in replies : r.val in node[0] or r.val in node[1] or r.val in node[2]"

    $ cmc --model-file replog.model

libcmc explores every order of connections, messages, timeouts, crashes
and lost messages, up to those budgets, checks the invariants in every
state, and prints the shortest sequence of actions that breaks one.  The
program needs no libcmc header and no rewrite: only an eight-line hook
that reports its state on a file descriptor, and `-no-pie`.

The engine underneath is calibrated against TLC: on 22 configurations of
a published consensus specification, written as in-process step
functions, it gives the same verdicts, the same shortest counterexample
depths and exactly the same distinct-state counts as single-worker TLC
(`calib/hc`).  Output follows TLC's wording.

Documentation, with a guide that builds a first model and the manual
pages: <https://gregburd.codeberg.page/libcmc/> (mirrored at
<https://gburd.github.io/libcmc/>).  Why a new tool rather than TLA+,
Spin, LTSmin, CBMC, GenMC or Stateright: `docs/WHY.md`.

## Build

    make            # build/cmc, build/cmc-preload.so, build/libcmc.a, build/test_cmc
    make check      # engine tests, the process front end gate, bug gates,
                    # purity and docs gates, fast calibration
    make install    # cmc, the preload, libcmc.a, cmc.h and the man pages, under PREFIX=/usr/local

C11 and libc only.  gcc and clang, `-Wall -Wextra -Werror`.  With Nix,
`nix build` gives the library and man pages, `nix flake check` runs
`make check` under gcc and clang, and `nix develop` has the tools for the
docs site.

## Use

1. Build your program `-no-pie`.  Node `i` gets `i` as its first
   argument and listens on 127.0.0.1 port 20000 + `i`.
2. Add the state hook: after each change, write a length byte and a
   canonical, complete encoding of the node's state to descriptor 3,
   and what it has made durable to descriptor 193.
3. Write a model file (`docs/model-file.md`) and run
   `cmc --model-file FILE`.  `--binary` checks another build against the
   same file; plant bugs behind `#ifdef` and require each to be caught.
   `--simulate` takes seeded random walks when the full search is too
   big.

Exit status: 0 pass, 1 violation, 2 incomplete (a bound stopped the
search; never a pass), 3 usage or model-file error.  The guide checks a
complete example: `docs/guide/02-first-model.md`.

The process front end needs Linux x86-64.  The engine also checks
in-process models, C step functions against `include/cmc.h`
(`cmc --model NAME`, `libcmc(3)`); that is its calibration path.

## Layout

| path | what |
|---|---|
| `include/cmc.h` | the whole public API |
| `src/cmc_proc.c` | the process front end: `cmc --model-file`, running the shipped program under seccomp |
| `src/cmc_preload.c` | `cmc-preload.so`: the deterministic heap and logical clock a checked program runs under |
| `src/cmc.c` | the engine for in-process models: world encoding, fingerprints, BFS/DFS/simulate/replay, environment, liveness |
| `src/cmc_cli.c` | `cmc_main()`, the TLC-style command-line driver |
| `test/proc/` | programs written as they ship, with model files and planted bugs, single-decree Paxos among them |
| `test/` | engine tests, the toy core with planted bugs, purity probes |
| `calib/hc/` | calibration: HummingConsensus.tla, its C port, the TLC cfgs, `calibrate.sh` |
| `scripts/` | the purity gate and its teeth |
| `man/` | `cmc(1)`, `libcmc(3)`, `cmc(7)` |
| `docs/` | the documentation site: guide, design, decision record, review, measurements |

## Status

Release 1.1.0.  `cmc --model-file` checks the shipped binary as it runs,
under seccomp, with the network, timeouts, crashes and connection resets
as search decisions.  Blocking sockets and the readiness vocabulary an
event loop uses (`poll`/`ppoll`, `fcntl` for `O_NONBLOCK`, `eventfd`) are
all modelled; single-decree Paxos, Raft leader election and a replicated
log, HummingConsensus and ZAB ship as worked examples.  Single-threaded.
Process mode replays every state from a fresh launch, so the rate scales
with how much the program does per launch: the small fixtures run several
hundred replays a second (HummingConsensus with three processes, three
timeouts and one drop is 11,545 distinct states in about 50 seconds, 555
replays a second, on the CI box).  The abstract engine (`cmc --model`,
`.cml` specs checked against TLC and Apalache) is faster.  History and
measurements are in `docs/measurements/`.

## CI

GitHub Actions runs `make check` under gcc and clang, the MemorySanitizer
gate, the Nix build, and weekly the full calibration.  Codeberg runs one
small `make check` job.  Both forges build the documentation site, check
its links, and publish it to Pages.

## License

Apache-2.0.  See `LICENSE`.
