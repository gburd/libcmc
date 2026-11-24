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

**What a pass means.**  An audit of 1.3.0 found false passes in all
three engines and gates that could report success after a tool failed.
2.0.0 fixes each one found, with a test that fails on 1.3.0 (see the
[soundness remediation plan](docs/plans/soundness-remediation.md) and
[CHANGELOG.md](CHANGELOG.md)); 2.1.0 closes the gaps 2.0.0 documented
and holds every source file to 90% function and branch coverage (see
the [2.1 plan](docs/plans/2.1-boundaries.md)); 2.2.0 lets a crash fall
between any two effectful calls and adds per-node clocks, a modelled
file system and the socket calls a libpq client makes (see the
[2.2 plan](docs/plans/2.2-in-place.md)); 2.3.0 gives the abstract
checker what a TLA+ user expects: TLC's deadlock check and constraints,
records, sequences and definitions, strong and per-action fairness,
ITF traces, a reader for TLA+ modules and TLC configurations, and proof
hooks for TLAPS and Apalache (see the
[2.3 plan](docs/plans/2.3-tla-users.md) and the
[guide for TLA+ users](docs/guide/04-from-tla.md)).  A pass covers only the instance and the
budgets searched, and only the behaviour
[docs/qualification.md](docs/qualification.md) lists as modelled; a
construct outside it is refused, not passed.  Use cmc as one input
beside review, testing and, for critical systems, a formal proof.

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
complete example: `docs/guide/02-first-model.md`.  Coming from TLA+,
`docs/guide/04-from-tla.md` takes Lamport's TCommit from tlaplus/Examples
to `.cml` with TLC's counts, then checks a C implementation against it.

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

Release 2.3.0.  `cmc --model-file`
checks the shipped binary as it runs, under seccomp, with the network,
timeouts, crashes, connection resets, partitions and short writes as
search decisions.  Blocking and non-blocking sockets,
`poll`/`ppoll`/`epoll`, finite timeouts on per-node clocks, `fcntl`,
`eventfd`, threads and node restart are intercepted, each with the limits listed in cmc(7) and
docs/qualification.md.  The order of nodes and of a multi-threaded
node's system calls and lock acquisitions is a search choice; memory
accesses between them are checked for data races by the race runtime
(`libcmc-race.a`, linked into a node built with `-fsanitize=thread`),
and without it a multi-threaded node cannot pass.  The file system is
read-only to a node except under a directory the model file names,
whose files the checker owns and a crash may roll back to their last
fsync; a node's channels to other processes are refused.  The worked examples are small
safety demonstrations, not complete models of the protocols they are
named after.
Process mode replays every state from a fresh launch, so the rate scales
with how much the program does per launch: the small fixtures run several
hundred replays a second, a five-process protocol about a hundred
(the Paxos example, five processes and no faults, is 7,711 distinct
states in about 130 seconds, 111 replays a second, on one core of an EC2
c7i.8xlarge).  History and measurements are in
`docs/measurements/`.

## CI

GitHub Actions runs `make check` under gcc and clang, the MemorySanitizer
gate, the Nix build, and weekly the full calibration.  Codeberg runs one
small `make check` job.  Both forges build the documentation site, check
its links, and publish it to Pages.

## License

Apache-2.0.  See `LICENSE`.
