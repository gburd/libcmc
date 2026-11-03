---
title: "Calibration against TLC"
parent: Background
nav_order: 3
permalink: /background/calibration/
lede: >-
  22 configurations of HummingConsensus: verdicts, depths and exact state counts against TLC.
---

The design is in the survey, §4a, and the calibration table in its §4b.1.  Raw output:
`2026-11-02-mc-calibration-raw.txt` (same directory).

**Verdict: calibrated.**  On all 22 rows cmc gives the same verdict
and the same BFS depth as TLC.  On the 6 exhaustive passing rows it also
gives exactly the same distinct-state count and the same states-generated
count, and on 7 more small configurations it matches both counts exactly.
The survey's §4b table needed two corrections, explained below: TLC
depths taken from multi-worker runs are not shortest, and one PASS depth
was misreported.

## What was run

- cmc: the engine as of 2026-11-02.
  Built with clang 21.1.8 `-O2 -g -std=c11 -Wall -Wextra -Werror`
  (the Makefile's flags).  gcc 16.2.0 builds clean too and gives identical
  results on the fast rows.  Single-threaded BFS, no worker pool.
- TLC: TLC2 2.19 rev 5a47802 (nixpkgs tlaplus 1.7.4), OpenJDK 1.8.0_504,
  `-Xmx32g -workers 1 -deadlock`, through `calib/hc/tlc.sh`.  These are
  new runs made for this calibration.  Their output is in
  `~/tmp/mc-build/tlc/*.w1.txt`, and the summary lines are in the raw
  file §2.
- Host: Intel Xeon Platinum 8375C, 48 cores, 185 GB, Linux 6.18.60.  This
  is a shared development host and **not** an EC2 measurement host.  Load
  average was 20-76 during the runs (other users' jobs and concurrent TLC
  runs), so wall times are indicative only.  The verdicts and counts do not
  depend on the host.
- Command: `make check-calibration`, which runs
  `calib/hc/calibrate.sh build/cmc all`.  For each row it runs
  `cmc --model hc --tlc-cfg calib/hc/cfg/<cfg>.cfg --cfg tlc_mult=TRUE
  --det`.  cmc reads the same TLC `.cfg` files TLC read (CONSTANTS,
  INVARIANTS, PROPERTIES), and the determinism gate is on.
- Bounds (from the cfg files): N=3, MaxEpoch=3, Writes={w1}, links = any
  subset of the 3 pairs.  The variant switches are CPMode, UnanimityRule,
  Confirm and Zerf.

## Calibration table

TLC numbers are from `-workers 1`, except where marked.  The depth is
TLC's "depth of the complete state graph search", with the initial state
counted as 1.  On a violation this equals the length of the shortest
counterexample (the number of states in it).  Distinct-state counts
taken at the moment of a violation depend on order within a BFS level,
so only PASS rows are required to match them.  They matched in several
FAIL rows anyway (marked =).

| cfg (calib/hc/cfg/) | property | TLC verdict / depth / distinct | cmc verdict / depth / distinct | cmc wall (s) | TLC 1-worker wall (s) |
|---|---|---|---|---|---|
| hc_paper_cp_n3 | S1_Agreement | FAIL / 6 / 1,193 | FAIL / 6 / 1,193 = | 0.00 | 1 |
| hc_paper_cp_n3_s1 (**A**) | S1_AgreementNonNone | FAIL / 7 / 17,179 | FAIL / 7 / 14,341 | 0.02 | 4 |
| hc_code_ap_n3_S1 | S1_AgreementNonNone | FAIL / 6 / 1,200 | FAIL / 6 / 1,200 = | 0.00 | 1 |
| hc_paper_ap_n3_s1 | S1_AgreementNonNone | FAIL / 6 / 1,206 | FAIL / 6 / 1,206 = | 0.00 | 2 |
| hc_paper_cp_n3_S2_… | S2_NoSplitBrainServing | FAIL / 5 / 365 | FAIL / 5 / 365 = | 0.00 | 1 |
| hc_paper_ap_n3_S2w | S2w_NoConcurrentWriteChains | FAIL / 6 / 1,206 | FAIL / 6 / 1,206 = | 0.00 | 2 |
| hc_paper_cp_n3_S2r_… | S2r_NoStaleRead | FAIL / 9 / 118,080 | FAIL / 9 / 118,080 = | 0.18 | 14 |
| hc_code_cp_n3_S2r_… | S2r_NoStaleRead | FAIL / 9 / 99,357 | FAIL / 9 / 99,357 = | 0.16 | 15 |
| hc_codeCZ_cp_n3_S2r | S2r_NoStaleRead | FAIL / 11 / 564,157 | FAIL / 11 / 564,157 = | 1.3 | 89 |
| hc_paper_ap_n3_S3a | S3a_NoLostAckActive | FAIL / 9 / 81,813 | FAIL / 9 / 81,813 = | 0.13 | 10 |
| hc_paper_cp_n3_W | W_AckAfterReconfig (must fail) | FAIL / 9 / 118,080 | FAIL / 9 / 118,080 = | 0.19 | 13 |
| hc_paper_cp_n3_S3_NoLostAck (**B′**) | S3_NoLostAck | FAIL / 12 / 2,779,865 | FAIL / 12 / 2,442,641 | 5.4 | 394 |
| hc_code_cp_n3_S3_NoLostAck | S3_NoLostAck | FAIL / 12 / 1,540,447 | FAIL / 12 / 1,388,080 | 3.2 | 246 |
| hc_codeZ_cp_n3_S3 | S3_NoLostAck | FAIL / 12 / 1,590,151 | FAIL / 12 / 1,431,275 | 3.6 | 258 |
| hc_codeZ_cp_n3_S3a (**C**) | S3a_NoLostAckActive | FAIL / 13 / 2,598,076 | FAIL / 13 / 2,374,450 | 6.4 | 493 |
| hc_codeC_cp_n3_S3 | S3_NoLostAck | FAIL / 14 / 3,425,701 | FAIL / 14 / 3,179,823 | 8.9 | 651 |
| hc_code_cp_n3_S1_AgreementNonNone | S1 non-none | **PASS / 24 / 7,412,120** | **PASS / 24 / 7,412,120** | 31.2 | 1,978 |
| hc_code_cp_n3_S3a | S3a | PASS / 24 / 7,412,120 | PASS / 24 / 7,412,120 | 28.2 | (369, 6 workers) |
| hc_codeC_cp_n3_S3a | S3a | PASS / 28 / 9,428,240 | PASS / 28 / 9,428,240 | 38.4 | 2,681 |
| hc_codeCZ_cp_n3_S1 (+S3, S3a) | S1, S3, S3a | **PASS / 32 / 15,523,080** | **PASS / 32 / 15,523,080** | 63.4 | 4,522 |
| hc_paper_cp_n3_S2w_… | S2w | PASS / 32ᵐ / 35,895,280ᵐ | PASS / 30 / 35,895,280 | 152 | (853, 16 workers) |
| hc_paper_cp_n3_S3a | S3a | **incomplete** at 20 min (≥26.7M)ᵐ | **PASS / 30 / 35,895,280** | 146 | — |

ᵐ: multi-worker TLC (2026-11-01 runs).  At write-up the 1-worker S2w run
was still going (16.2M distinct at depth 16), so the S2w depth is not
TLC-confirmed.  See discrepancy 2.

States generated also match TLC exactly on every PASS row: 64,710,625,
82,484,959, 136,045,719 and 323,513,489 (raw §1).  They match on most
FAIL rows too, for example 2,981, 842, 440,453 and 2,760,688.  S4
(`S4_EpochMonotone`, `S4_PrivWriteOnce`) held in every run, as in TLC.

Further exact matches, on configurations chosen for this calibration
(exhaustive, TypeOK only; raw §3).  Every pair agrees on both distinct
states and states generated:

| N | E | variant | TLC = cmc distinct / generated |
|---|---|---|---|
| 2 | 3 | code cp | 1,856 / 4,545 |
| 2 | 4 | code cp | 34,360 / 89,443 |
| 2 | 3 | paper ap | 7,760 / 29,039 |
| 2 | 4 | code+C+Z cp | 66,536 / 184,735 |
| 3 | 2 | paper cp | 37,840 / 330,843 |
| 3 | 2 | code+C+Z cp | 23,760 / 204,901 |
| 3 | 2 | code ap | 36,800 / 340,717 |

Counterexamples have the same shape as TLC's.  The action sequence of
counterexample A is identical step for step: NetChange, Propose(2),
Propose(3), NetChange, Adopt(2), Adopt(3), giving
`priv[2][2] = <1,2> ≠ priv[3][2] = <1,3>` (raw §7).  B′, C and S2r have
the same action-name sequences as TLC's 1-worker traces.

Throughput: cmc does about 2.1-2.5 M generated states/s on one core
under load 20-76, against TLC's about 33 K/s on one worker on the same
host.  So single-threaded cmc finished the 15.5 M-state run in 63 s,
where TLC took 4,522 s (1 worker) and 475-841 s (6-16 workers).  No worker
pool was needed.  The 35.9 M-state run used 6.4 GB RSS (world 376 B,
table entry 48 B).

## Discrepancies and their explanations

1. **Multi-worker TLC depths are not shortest.**  The survey's §4b, taken
   from the TLC runs of 2026-11-01, lists A at depth 9, B′ at 14, C at 14,
   paper-cp S2r at 11 and code+C S3 at 15.  Those runs used 2-16 workers.
   Parallel TLC does not guarantee a shortest counterexample, and its
   reported depth varies with scheduling.  The same state space gave depth
   25 in one run and 24 in another (code-cp S1 vs S3a), and 32 vs 33
   (code+C+Z).  With `-workers 1`, TLC gives A = 7, B′ = 12, C = 13,
   S2r = 9 and code+C S3 = 14, and cmc matches every one.  The
   2026-11-01 traces themselves are longer (A had 9 states), which fits
   non-shortest search.  I did not tune cmc to the old numbers.
2. **PASS depths.**  For the same three state spaces as above, 1-worker
   TLC gives 24 and 32, and cmc agrees.  For code+C S3a, TLC said 29
   with 6 workers, but 1-worker TLC and cmc both give 28.  For paper-cp
   S2w, TLC said 32 with 16 workers and cmc says 30.  The 1-worker
   TLC run had not finished at write-up, so 30 is not yet TLC-confirmed,
   but the distinct and generated counts match exactly.
3. **"States generated" without `tlc_mult`.**  By default cmc
   produces one successor per distinct action binding.  TLC produces one
   successor per true leaf of a disjunction (`\/`) inside an action body.
   In Adopt, the `MajOK` disjunction (3 leaves) and the `FinalReview`
   disjunction (4 leaves) can each be true on more than one leaf.  TLC's
   `-coverage` showed Adopt as the only action whose "generated" count
   differed.  `tlc_mult=TRUE` (a cfg knob, not a TLA+ constant) emits
   Adopt that many times, and the counts then match exactly.  Distinct
   states and depths are the same either way: 7,412,120 / 64,680,415 /
   24 without the knob (raw §4).
4. **Paper-cp S3a now has an answer.**  TLC hit its 20-minute box on it
   (≥26.7 M distinct).  cmc finished it: PASS, 35,895,280 distinct,
   depth 30.  That is the same state space and the same counts as the S2w
   run, because the reachable states depend only on the constants.  So
   under the paper rule in CP mode, no acked write is lost from an
   *active* later projection.  It is lost from an inactive one (B′).
   This finding is new and **not TLC-confirmed**.
5. **CHOOSE in `Base` (zerf).**  CHOOSE is deterministic but
   implementation-defined.  The port checks a property, `HC_Wellformed`,
   in every state.  It fails if two distinct confirmed projections ever
   tie for the maximal epoch.  That never happened in any run, so CHOOSE
   always had a unique answer and the port does not depend on TLC's value
   order.  The same invariant flags any C capacity overflow (props 12,
   stamp 8, acked 8).

## The checker's own teeth

- Every invariant that TLC found violated, cmc found violated, at the
  same depth.  cmc found no counterexample that TLC missed.  For
  paper-cp S3a, TLC never finished, and cmc finished with no violation.
- Planted bugs in the HC port (`calib/hc/hc_bug_gate.sh`, `-DCMC_BUG=n`;
  raw §5) all fail calibration:
  - 101 drops A21's "every member answered": code-cp S1 fails at depth 6.
  - 102 makes MajOK ignore the repairing list: still PASS, but with
    7,213,400 distinct, so exact-count calibration catches a one-token
    slip that no invariant notices.
  - 103 keeps sets unsorted (non-canonical): TypeOK fails, and dedup
    splits states.
- Toy core (`test/toy_gate.sh`; raw §6).  The correct core passes with
  exactly 34 distinct states and depth 11; 33 of them are derived by hand
  in `test/toy/toy_core.c`, plus one that splits by reply order.  Each planted
  bug is caught by BFS (shortest trace), by `--simulate` and by
  `--replay` of the written trace:
  - bug 1 (no holder check): Mutex at depth 7.
  - bug 2 (lost wake): AllReplied (quiescence) at depth 9.
  - bug 3 (phase regresses): action property PhaseMonotone at depth 5.
- Engine gates (`test/test_cmc.c`).  These are triggered:
  - The determinism gate: a core that reads a global.
  - The durable-state rule: durable bytes changed outside FLUSHED.
  - Non-omniscience: scrambling all core-state bytes of reachable worlds
    leaves the enabled-action set unchanged.
  - Purity: `nm -u` of the core step objects lists only `mem*`.
  - Isolation: the engine does not include any core's private header.
- Witnesses: `Reach_AckAfterReconfig` and `Reach_RepairJoinsUPI` are
  reached at depth 11 in the code+C+Z space.  `W_RepairJoinsUPI` at E≤4
  is violated by BFS at depth 11 and by simulation.  TLC only simulated
  that one.
- ASan+UBSan build: the toy gate and the 16 fast calibration rows ran
  clean.

## Not covered

- Message-passing cores at scale.  HC uses the documented calibration
  exception: its actions are the TLA+ actions.  The actor-mode
  environment (SEND, timers, PERSIST/FLUSHED, WAKE, CLIENT, DROP, DUP,
  CRASH) is exercised only by the toy and by small test cores.  Real
  protocol cores are where it gets real load.
- No worker pool, no symmetry reduction, no on-disk frontier.  The queue
  holds full worlds in RAM (about 1.5 GB at the frontier peak of the
  35.9 M run).  Bounded liveness is limited to the quiescence check; there
  is no fair-schedule K-step check yet.
- The paper-cp S2w depth (30) is waiting on the 1-worker TLC run.  The
  N=4 and E=4 simulation configs were not compared, except
  W_RepairJoinsUPI.
- These are development-host runs, not a dedicated measurement host.
  Model counts and verdicts do not depend
  on the host, but the wall times do.  Verdict on the wall times: NOISY.

## Reproduce

```
make check-unit                 # engine tests and the toy gate, ~1 s
make check-calibration-fast     # the 16 FAIL rows, ~30 s
make check-calibration          # + the 6 PASS rows, ~8 min, ~7 GB RAM
make check-hc-bugs              # ~30 s
build/cmc --model hc --tlc-cfg calib/hc/cfg/hc_paper_cp_n3_s1.cfg   # counterexample A
(cd calib/hc && ./tlc.sh HummingConsensus.tla cfg/<cfg>.cfg -workers 1)   # the TLC reference
```
