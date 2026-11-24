---
title: "Library engine memory and parallel BFS"
parent: History and provenance
nav_order: 17
permalink: /background/library-engine-memory/
lede: >-
  Where the BFS memory went on pg_chain's two biggest rows, what the
  compact store and the queue spill save, and the wall time with 1 to
  32 workers.
---

# Library engine memory and parallel BFS (2026-11-24)

Item 5 of the [2.4 plan]({{ '/reference/plan-2.4/' | relative_url }}),
its memory and parallel parts.  Raw output (one line per run: log,
world size, counts, wall time, peak RSS in kB, and the command) is in
`2026-11-24-library-engine-memory-raw.txt` beside this file.

## Setup

Host m: EC2 r7i.8xlarge, Intel Xeon Platinum 8488C, 32 vCPUs, 247 GB,
Fedora 44, kernel 7.3.4, gcc 16.2.1, glibc 2.43, otherwise idle; one
run at a time.  Peak RSS and wall time from `/usr/bin/time -v`.
[measured]

The rows are pg_chain's cores from `test/pgchain`, built as
`test/pgchain/README.md` says with `-O2`:

- **paxos**: `--model paxos --inv OneValuePerSlot,Agreement,OneValuePerBallot,LearnedChosen,RefusedNeverChosen,AnsweredOnce --prop ChosenStable --persist-cap 6 --net-cap 12 --cfg slots=1,rounds=2 --clients 1 --crashes 1`.
  161,613,053 distinct states, 877,429,630 generated, depth 66.  This
  is the plan's "two candidates" row; the plan's 187,043,769 came from
  an earlier revision of the core (pg_chain
  `docs/measurements/2026-11-23-mc-paxos.md` at 1c9fff5).
- **chain**: the 10,086,243-state row of `mc-bug-gate.sh`
  (`--persist-cap 5 --net-cap 16 --cfg reconfigs=3 --crashes 1 --clients 1`)
  with all its invariants, action properties, witnesses and
  EventuallyReplied.  61,832,969 generated, depth 58.

Every run below, of every engine and worker count, printed these
counts and depths and "No error has been found". [measured]

## Where the memory went (before)

Engine: pr/2.4 at 8c2c97e (libcmc 2.3.1's BFS).  A state was a 48-byte
table entry (two fingerprints, the parent's two fingerprints, the
action, depth and flags) in one open-addressing table that doubled at
75% load, and the queue held whole worlds.  A copy of `src/cmc.c` with
byte counters (`/scratch/libcmc/scale/cmc_instr.c`, not committed)
recorded table, grow transient and queue at the moment their sum peaked:

| row | world | table | new table during the last grow | queue | sum | peak RSS |
|---|---|---|---|---|---|---|
| paxos | 1272 B | 6.44 GB | 12.88 GB | 10.08 GB | 29.41 GB | 28.83 GB (9:08 wall) |
| chain | 1480 B | 0.40 GB | 0.81 GB | 1.30 GB | 2.50 GB | 2.46 GB (0:38) |

[measured]  "Other" (code, scratch, stdio) is below the counters'
resolution: the sums exceed RSS slightly because a freshly calloc'd
table is not all touched when the sum peaks.  The table is 2^27 slots
x 48 B for 161.6 M states (a 60% load), and the grow briefly holds the
old 2^27 and the new 2^28 tables.  The queue peaks at about 7.9 M
worlds near depth 41 (the progress lines), 7.9 M x 1272 B.

## The change

- **Store.**  A 32-byte record per state (fingerprint, parent record
  number, action), in 256 shards by fingerprint.  Each shard's index is
  8-byte slots (a 24-bit tag of the fingerprint and the record number)
  at most 75% full; it grows by freeing itself and rebuilding from the
  records, so no second copy exists and the transient is one shard's
  index (1/256 of it).  A trace follows parent numbers.
- **Queue.**  Entries are {parent record number, world}.  With
  `--queue-mem MB` the full chunks between the two ends go to files in
  `$CMC_SCRATCH` (or `$TMPDIR`) once the budget is used, and come back
  when the head reaches them.
- **Workers.**  `--workers N` expands each batch (256 states per
  worker) in N threads; one thread then merges the batch in queue
  order.  See `docs/DESIGN.md` (Search).

Expected size for paxos [estimate, from the code]: records 256 shards x
1024 x 1.5^16 (the first capacity past 631 k) x 32 B = 5.5 GB; index
256 x 2^20 slots x 8 B = 2.1 GB; queue 7.9 M x (8 + 1272) B = 10.2 GB:
17.8 GB, against 17.4 GB measured.

## After, same world layout

Engine: commit 1eb5d50 (this change on 8c2c97e).  [measured]

| row | workers | queue | wall | peak RSS |
|---|---|---|---|---|
| paxos | 1 | memory | 7:58 | 17.40 GB |
| paxos | 8 | memory | 2:38 | 17.45 GB |
| paxos | 16 | memory | 1:55 | 17.42 GB |
| paxos | 32 | memory | 1:36 | 17.43 GB |
| paxos | 32 | `--queue-mem 1024` | 2:08 | 8.81 GB |
| chain | 1 | memory | 0:33.5 | 1.83 GB |
| chain | 8 | memory | 0:11.0 | 1.83 GB |
| chain | 16 | memory | 0:08.0 | 1.88 GB |
| chain | 32 | memory | 0:07.4 | 1.96 GB |

Peak RSS falls from 28.8 GB to 17.4 GB on paxos (40%) and from 2.46 GB
to 1.83 GB on chain (26%); one worker is also 13% faster (7:58 against
9:08); I suspect the tag in the index slot, which settles most probes
without reading a record, but did not profile it.  With the queue
spilled the paxos row fits in 8.8 GB: the store (7.6 GB) plus the
budget.  The spill directory held 8.5 GB when sampled mid-run and was empty
at the end.

## After, merged with pr/2.4's clocks and timer tables

pr/2.4 at f1be837 adds per-node clocks and timer and wake tables to the
world, so worlds grow (paxos 2048 B, chain 2512 B) and so does the
queue.  Same rows, same counts.  [measured]

| row | engine | workers | queue | wall | peak RSS |
|---|---|---|---|---|---|
| paxos | f1be837 (old store) | 1 | memory | 12:13 | 34.89 GB |
| paxos | merged (e9631c1) | 1 | memory | 11:19 | 23.65 GB |
| paxos | merged | 8 | memory | 3:40 | 23.33 GB |
| paxos | merged | 16 | memory | 2:38 | 23.56 GB |
| paxos | merged | 32 | memory | 2:21 | 23.64 GB |
| paxos | merged | 32 | `--queue-mem 1024` | 3:29 | 8.83 GB |
| chain | f1be837 (old store) | 1 | memory | 0:54.5 | 3.35 GB |
| chain | merged | 1 | memory | 0:50.4 | 2.74 GB |
| chain | merged | 8 | memory | 0:15.6 | 2.75 GB |
| chain | merged | 16 | memory | 0:11.6 | 2.81 GB |
| chain | merged | 32 | memory | 0:10.8 | 2.97 GB |
| chain | merged | 8 | `--queue-mem 256` | 0:18.3 | 0.93 GB |

The queue now dominates: 7.9 M x 2056 B = 16.3 GB of the 23.6 GB.  The
spill takes the paxos row to 8.8 GB at a 48% time cost at 32 workers.

## Scaling

Wall time on paxos with the merged engine falls 3.1x at 8 workers,
4.3x at 16 and 4.8x at 32.  The batch's merge (counts, witness depths,
the queue) runs on one thread, and memory bandwidth for copying worlds
into and out of the queue is shared, which bounds it; the chain row,
whose liveness check makes expansion costlier, scales the same way
(3.2x, 4.3x, 4.7x).  [measured; the cause is inferred, not profiled]

## What did not change

For every run the counts and depth equal the one-worker, in-memory
run's.  The gates that pin more than counts:

- `test_cmc` `test_workers_agree`: one against three or four workers,
  and against a spilling queue, on the toy (every property kind), the
  fair-schedule core (pass, cycles, out of steps, engine errors), the
  output-cap core, a 2^20-state core with a state limit, a stop at the
  witnesses, the lease core with drift 0, 50 and 200 and a horizon,
  deadline order and the clock-range error: verdict, property, reason,
  counts, states left, depth, witness depths, coverage, and the trace
  byte for byte.
- `calib/hc/calibrate.sh` reruns every HC row with `--workers 4` and
  requires the same output apart from the wall time (counterexample
  included); `test/toy_gate.sh` does the same for the toy.
- `make check-pgchain`: pg_chain's fast gate rows with `--workers 8`,
  every count and every planted bug's depth as in pg_chain's gate.
