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
planted bug, `-DPGCHAIN_MC_BUG=N`).  `make check-pgchain` runs the fast
rows of pg_chain's gate; the large Paxos and chain rows are the scale
benchmark for 2.4 (docs/plans/2.4-library-engine.md).
