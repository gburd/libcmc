# test/pgchain: pg_chain's protocol cores, for testing libcmc

A copy of the library-engine models of pg_chain
(https://codeberg.org/gregburd/pg_chain), taken at pg_chain commit
3918c5d (2026-11-24) with the author's permission, for libcmc's own
gates and benchmarks.  pg_chain is Apache-2.0, as libcmc is; each file
keeps its header.  The copy is for testing the engine: pg_chain itself
remains the place these cores are developed, and they are not updated
here except to follow libcmc's API.

| file | pg_chain path |
|---|---|
| `chain_member.c`, `paxos.c` | `src/core/` (the cores, linked into the extension) |
| `include/pg_chain/*.h` | `include/pg_chain/` |
| `chain_member_model.c`, `paxos_model.c`, `pgchain_mc.c` | `test/mc/` (properties and the `cmc_main()` driver) |
| `mc-bug-gate.sh` | `scripts/mc-bug-gate.sh` (pg_chain's gate, with its exact distinct-state counts) |

Build with `make B=DIR DIR/pgchain_mc` (and `DIR/pgchain_mc-bugN` for a
planted bug, `-DPGCHAIN_MC_BUG=N`).  `make check-pgchain` builds them
all and runs the fast rows of pg_chain's gate, with logs under
`$CMC_SCRATCH/pgchain`; set `PGCHAIN_MC_SLOW=1` for the slow rows too.
The fast rows take about 7.5 minutes (441 s with the build, on an idle
32-core Fedora EC2 host, 2026-11-24; the rows run one at a time).  That
would add a quarter to `make check`, so CI runs them weekly, on demand
and before a release instead (the `pgchain` job).  `make
check-pgchain-hist`, part of `make check`, runs the history rows.  The
large Paxos and chain rows are the scale benchmark for 2.4
(docs/plans/2.4-library-engine.md).

## Time in a core

These cores arm untimed timers only, which fire at any point.  A core
that needs real time (a lease, an election timeout, a retry delay) uses
three parts of `cmc.h` (cmc(7), Clocks and deadlines and Time fields):

- `ev->now` on every event is the node's own clock, in the core's unit.
  `step()` reads it instead of a clock, so it stays pure; the extension
  passes its own clock reading in the same field.
- `CmcOutput.dur` on `CMC_OUT_SET_TIMER` arms a timed timer, which fires
  when the node's clock reaches now + dur, earliest deadline first.
  `dur` 0 keeps today's untimed timer.  `--drift R` lets clocks run
  apart by up to R per thousand.
- A core that stores a time (`s->lease_end = ev->now + L`) declares its
  offset with `CmcCore.time_fields(cfg, node, off, max)` and runs with
  `--time-bound B`, B at least the largest constant it compares a past
  time against (here, at least L and every other timeout).  The engine
  then stores times relative to each node's clock, so a protocol that
  renews forever has finitely many states and needs no `--max-time`.
  The core must compare times only with `ev->now`, never two stored
  times with each other, and must not put a time in a message or a
  persist record.  `--time-check` checks this on every step taken; run
  it in the gate, as `--det` is.

`test_time_bound` in `test/test_cmc.c` is a small example: a renewing
lease with three time fields.
