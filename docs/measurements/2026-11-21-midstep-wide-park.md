# Example state counts across the pr/2.2-fix3 series (2026-11-21)

The series freezes every clock while a node is stopped inside a step,
parks a mid-step dial, non-blocking accept and listener poll while any
node that could change their outcome can still act, delivers an RST
after the bytes queued before it, reports `POLLHUP` once both
directions are shut, keeps a reader's kernel bytes across a reset, and
lets a threaded node's dial to a crashed host time out.  This records
what each step did to the examples' search spaces.

## Setup

- Host: floki, Intel Xeon Platinum 8375C (48 threads), Linux 6.18,
  load average 6 to 10 from other users' work during the runs.
- cmc built with clang -O2 at each commit of pr/2.2-fix3; "base" is
  68c5323 (origin pr/2.2).
- Targets built `cc -O1 -no-pie`; the models in `examples/*/`,
  `test/proc/repl.model` and the guide's `kvserver.model`; hc with
  `--depth 12` or `--depth 15`.  One run each, single-threaded.

## Results [measured]

Distinct states (time for the slow ones):

| example | base | clocks frozen (225c9d9) | wide dial park (64b6ad4) | listener poll (a3513a3) |
|---|---|---|---|---|
| repl | 7 | 7 | 7 | 7 |
| paxos | 561 (9 s) | 561 | 7711 (127 s) | 7711 |
| raft | 5182 (66 s) | 5182 | 10309 (116 s) | 10320 (120 s) |
| raftlog | 107 | 107 | 115 | 115 |
| zab | 107 | 107 | 115 | 115 |
| kvserver | 32 | 32 | 32 | 32 |
| hc `--depth 12` | 23787 (258 s) | 14534 (176 s) | 12570 (112 s) | 12715 (108 s) |
| hc `--depth 15` | 78155 (877 s) | 36625 (422 s) | 46184 (429 s) | 49539 (458 s) |

The RST, POLLHUP, reset and threaded-dial commits change no count.

The wide dial park without the clock freeze (measured first, on a
build with only that change): paxos 7711, raft 18732 (211 s), hc at
depth 11 111350 states in 885 s, and hc at depth 12 did not finish in
30 minutes.  Freezing the clocks first is what keeps hc's slow row
under ten minutes.

The planted hc bug (`-DBUG --timeouts 12 --simulate --walks 30000
--depth 600`): seed 3 found it at walk 15905 on base, at walk 29761
after the freeze, and not within 30,000 walks with the wide park
alone; seed 1 finds it at walk 1457 after the freeze, 2164 after the
wide park, and 583 at the end of the series (119 actions, about 5 s).
The gate uses seed 1.

## Not re-measured

The paxos README's `--crashes 1 --drops 1` count (46909, about twenty
minutes) and the raft README's `--crashes 1` count (15391) predate the
wide park; a raft `--crashes 1` run on 64b6ad4 had not finished after
15 minutes when it was stopped to keep the host free for `make check`.
Both READMEs say so.
