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

The in-process engine (`cmc --model`) is calibrated against TLC: on 22
configurations of a published consensus specification, written as
in-process step functions, it gives the same verdicts, the same
shortest counterexample depths and exactly the same distinct-state
counts as single-worker TLC (`calib/hc`; two rows' expected values come
from a multi-worker or unfinished TLC run, see `calibrate.sh`).  The
process front end (`cmc --model-file`) and the abstract checker (`cmc
--spec`) have separate search loops that this calibration does not
cover.  Output follows TLC's wording.

**Not verification evidence yet.**  An audit of 1.3.0 found false
passes in all three engines and gates that could report success after a
tool failed.  Until the [soundness remediation plan](docs/plans/soundness-remediation.md)
is done, treat a cmc pass as a debugging aid, not as evidence that a
system is correct.

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

Release 1.3.0, under remediation (see above).  `cmc --model-file`
checks the shipped binary as it runs, under seccomp, with the network,
timeouts, crashes and connection resets as search decisions.  Blocking
sockets, `poll`/`ppoll`/`epoll`, `fcntl` for `O_NONBLOCK`, `eventfd`,
threads and node restart are intercepted, each with the limits listed in
cmc(7) and the remediation plan.  The order of a multi-threaded node's
system calls on checker-owned fds, futexes and the state hook is a
search choice, and such a node may not share pipes or files between its
threads; interleavings of memory accesses between those calls are not
explored.  The worked examples are small
safety demonstrations, not complete models of the protocols they are
named after.
Process mode replays every state from a fresh launch, so the rate scales
with how much the program does per launch: the small fixtures run several
hundred replays a second (HummingConsensus with three processes, three
timeouts and one drop is 14,730 distinct states in about 90 seconds, 380
replays a second, on a 48-core Xeon).  History and measurements are in
`docs/measurements/`.

## CI

GitHub Actions runs `make check` under gcc and clang, the MemorySanitizer
gate, the Nix build, and weekly the full calibration.  Codeberg runs one
small `make check` job.  Both forges build the documentation site, check
its links, and publish it to Pages.

## License

Apache-2.0.  See `LICENSE`.
