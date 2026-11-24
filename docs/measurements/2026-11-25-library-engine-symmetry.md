# Symmetry reduction on pg_chain's Paxos (2026-11-25)

What: libcmc 2.4 symmetry reduction (`--symmetry on`, `CmcCore.symmetry`,
`permute`, `permute_msg`) on `test/pgchain`'s Paxos model, whose
`symmetry()` makes the non-candidate acceptors interchangeable when cfg
gives them the same role (initial voter or not, member of `change` and
`change2` or not).  Each row of pg_chain's gate runs three ways: symmetry
off, on, and on with `--sym-check`.

Host: EC2 b (c7i.8xlarge class, Intel Xeon Platinum 8488C, 32 vCPU, Fedora
44), load about 0 before the run, nothing else running.  gcc 16.2.1, `-O2
-g`, branch pr/2.4-sym on f32edb9.  Command:
`docs/measurements/2026-11-25-library-engine-symmetry/run.sh build-gcc OUT`,
which runs `pgchain_mc --model paxos --inv OneValuePerSlot,Agreement,
OneValuePerBallot,LearnedChosen,RefusedNeverChosen,AnsweredOnce --prop
ChosenStable,<witnesses>` on each row's bounds.  Raw output is in the
directory of the same name.  Wall time and RSS come from `/usr/bin/time`.

## Correct core [measured]

| row | group | distinct off | distinct on | ratio | depth off / on | wall off / on / check |
|---|---|---|---|---|---|---|
| A (1 candidate, 3 rounds, 2 crashes) | 2 | 2017514 | 1017203 | 0.504 | 63 / 63 | 6.8 / 4.1 / 9.6 s |
| B (2 candidates of 3) | 1 | 4501801 | 4501801 | 1 | 48 / 48 | 16.1 / 16.2 / 16.1 s |
| C (1 candidate, 2 slots, a drop) | 2 | 53229 | 27928 | 0.525 | 40 / 40 | 0.16 / 0.10 / 0.25 s |
| D (1 candidate, 2 slots, 2 rounds, a crash) | 2 | 600110 | 305148 | 0.508 | 63 / 63 | 1.9 / 1.2 / 2.9 s |
| R (4 nodes, change=c) | 1 | 4187917 | 4187917 | 1 | 69 / 69 | 20.7 / 20.8 / 20.8 s |
| V (5 voters, q=3, 1 candidate, 1 slot, 2 rounds, a crash) | 24 | 80435937 | 3997213 | 0.0497 | 68 / 68 | 527 / 183 / 1224 s |

Every row passes in all three modes, with every named witness reached.
RSS follows the distinct count: V takes 20.1 GB off and 1.2 GB on.

Rows B and R have a trivial group: in B, two of three nodes are
candidates; in R, nodes 1, 2 and 3 each have their own role under
`change=c` (a voter left out, a voter kept, a new voter).  Their counts
equal the unreduced ones exactly, as they must.

The ratios sit just above 1/|G| (0.5 for two interchangeable nodes,
1/24 for four), because worlds fixed by some permutation (the two
acceptors in the same state) are their own orbit.  The orbit-count
equality itself is gated on a smaller core in test_cmc (test_symmetry:
31868 reduced states equal 31868 orbits counted by brute force, out of
190177).

`--sym-check` found nothing to report on any row.  It costs about 2.3
times the reduced run when the group has 2 elements and about 6.7 times
on V's 24, still faster there than the unreduced run.

## Planted bugs [measured]

`make check-pgchain-sym` (test/pgchain/sym-gate.sh) runs each of
pg_chain's planted Paxos bugs on its gate row, with symmetry off and on.
All ten are caught by the same property at the same depth:

| bug | property | row | depth (off = on) |
|---|---|---|---|
| 101 | LearnedChosen | B | 23 |
| 102 | LearnedChosen | A | 27 |
| 103 | LearnedChosen | A | 39 |
| 104 | OneValuePerBallot | A | 20 |
| 105 | LearnedChosen | A | 13 |
| 106 | LearnedChosen | B | 25 |
| 107 | ChosenStable | R | 46 |
| 108 | LearnedChosen | E | 30 |
| 109 | LearnedChosen | R | 26 |
| 110 | ChosenStable | R | 46 |

The depth cannot change: BFS reaches each orbit first at its shortest
distance, and every member of an orbit has the same distance from a
symmetric initial world.  Bugs on B, E and R run with a trivial group,
so for those rows the result shows only that symmetry on changes nothing.

## Why the hooks live in paxos_model.c

`paxos.c` is pg_chain's shipped core and stays byte for byte as pg_chain
ships it.  The hooks only need the state layout, which `paxos.h`
exports and the model's properties already read.  A non-candidate's id
appears in no message or persist body (bodies carry ballots, slots and
values, and a ballot names its candidate), so `permute_msg()` does
nothing.  In the state, ids appear only as the bitmaps `p1` and `p2[]`
and as the row index of `pb` and `pv`.  `--sym-check` on every row above
is the evidence that this reading of the core is right.
