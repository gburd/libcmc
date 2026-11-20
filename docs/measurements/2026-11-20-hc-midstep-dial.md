# hc after the mid-step dial park (2026-11-20)

Commit d8ae8c2 made a blocking `connect()` after a send earlier in the
same step a point where the dialled node may act first, when that node
is alive and holds unread bytes from the dialler.  On Linux the peer
may exit in that window and the dial is then refused; before d8ae8c2
cmc never reached that outcome.  The hc example dials the next
participant right after answering one, so most of its dials became
such points, and its search space grew.  This records by how much, and
why `make check-examples-slow` now bounds hc by depth.

## Setup

- Host: floki, Intel Xeon Platinum 8375C (48 threads), Linux 6.18.60,
  load average about 8 from other users' work during the runs.
- cmc: pr/2.2-fix2 at f806005 (the search is the same from d8ae8c2
  on); the "before" row is 85051f4.  Built with clang -O2.
- Target: `cc -O1 -no-pie examples/hc/hc.c`, model
  `examples/hc/hc.model` (three participants, four timeouts).
- Command: `cmc --model-file examples/hc/hc.model --binary hc [--depth N]`,
  one run each, single-threaded.  Raw output:
  [2026-11-20-hc-midstep-dial-raw.txt](2026-11-20-hc-midstep-dial-raw.txt).

## Results [measured]

| run | distinct states | time | verdict |
|---|---|---|---|
| full, 85051f4 | 36,331 | 792 s | no error, depth 89 |
| `--depth 12`, 85051f4 | 8,095 | about 125 s | INCOMPLETE |
| `--depth 12` | 23,787 | 276 s | INCOMPLETE |
| `--depth 14` | 53,360 | 590 s | INCOMPLETE |
| `--depth 15` | 78,155 | 880 s | INCOMPLETE |
| `--depth 16` | 112,920 | 1,335 s | INCOMPLETE |
| full | not finished | over 7,200 s | stopped by its timeout |

No run found a violation.  The full run on d8ae8c2 shared the host with
`make check` runs for part of its two hours, so its time is a lower
bound, not a measurement of the full search.

The planted bug (`-DBUG`, `--timeouts 12 --simulate --walks 30000
--depth 600`) is found with seeds 1, 2, 3 and 7, and not with 4, 5 or
6 (30,000 walks, about 244 s each).  Seed 3 finds it at walk 15905 in
129 s, a 77-action schedule.  Seed 4, the one the gate used, found it
at walk 2529 before d8ae8c2: the new park points change which
schedules a seeded walk takes.

## Decision

The park rule stays as it is: narrowing it would drop a Linux outcome
(a refused dial after the peer exits).  `make check-examples-slow`
runs hc to depth 15 (about 15 minutes) and the planted bug with seed 3
(about 2 minutes).  `make check` keeps its depth-12 row, now about 4.5
minutes.  A full hc search is no longer part of any gate.
