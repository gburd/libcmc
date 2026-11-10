---
title: "Adversarial review of actor mode"
parent: History and provenance
nav_order: 2
permalink: /background/review/
lede: >-
  Five ways a real bug could pass, found by review, each now a regression test.
---

Date: 2026-11-02.  Reviewer: a fresh-context agent with no part in
writing the engine.  It ran out of time (Bedrock 503) before writing up,
so this file was written from its experiments and transcript.  Every
finding below was reproduced by a small C program linked against a copy of
the engine (`~/tmp/mc-review/exp/*.c`, local, not in the repo), unless
it is marked [source].  The engine was not modified.

The question was: how could `cmc` report PASS when a real bug
exists?  HC calibration covers only direct mode (survey §4b.1).
Actor mode, which real protocol cores use, had so far been exercised
only by the 3-bug toy.  All findings are about actor mode.

## Verdict

The engine is solid where it was tested.  Search, dedup, fingerprints,
the action-list and output caps, and the replay of a wrong trace all
behave correctly.  But five holes would let realistic protocol bugs pass,
and they must be fixed before any production core relies on the engine.
Each fix should land with the experiment that exposed it, turned into a
regression test in `test/test_cmc.c`.

## Blockers (a real bug of the kind protocol cores have passes)

| # | finding | evidence | fix |
|---|---|---|---|
| B1 | **A periodic timer hides every lost wake.**  The quiescence check is the only lost-wake detector, and it runs only in states with no enabled action.  A core that re-arms a timer never reaches such a state.  Every production core will have a heartbeat or a lease renewal. | E1: toy-bug2 (a lost wake) is caught, 31 distinct.  The same core plus a server heartbeat timer gives PASS, 36 distinct.  `--simulate` with 10^5 walks also gives PASS. | Bounded liveness: from every reachable state with fault budgets spent, run the fair schedule (deliver everything, flush, wake, fire each timer once, advance clocks) for at most K steps.  Fail with `Eventually<name>` unless every pending client request has a reply.  A state whose only enabled actions are timer fires leading back to the same fingerprint counts as quiescent. |
| B2 | **State kept in a `static` passes both gates.**  The purity gate runs only `nm -u`, and a function-local static is a defined `b` symbol.  The determinism gate also passes.  The hidden state is not in the fingerprint, so dedup merges states that differ and misses a violation. | E2: an epoch filter with its high-water mark in core state is caught (`NeverTwo`, depth 6).  With the mark in a `static` it gives PASS, 20 distinct.  `nm` shows `0000000000000000 b e2.hw`. | Also fail the gate on any defined data symbol in a core object: `nm --defined-only` of types `b B d D` (and TLS), allowing only `r R` (const).  The cores take a `const` cfg, so they never need writable globals. |
| B3 | **A crash never keeps a write that reached disk before FLUSHED was delivered.**  CRASH drops every pending persist.  In PostgreSQL, `XLogFlush` can finish and the backend can die before acting on it.  So restart logic that assumes "durable means I already replied" is never challenged. | E4: a core whose RESTART treats "durable" as "already acknowledged" gives PASS with crash budget 1. | On CRASH(n) with k pending persists, branch k+1 ways.  Branch j applies the first j records to the durable part, discards the core's outputs (the process is dead), then zeroes volatile and delivers RESTART. |
| B4 | **FLUSHED may copy unflushed volatile values into the durable part.**  The durable/volatile rule checks only events other than FLUSHED.  Inside FLUSHED the core can write anything to durable, including a later, unpersisted ballot.  A crash then "keeps" a value that was never written. | E6: a Paxos-style acceptor that reports its promise from volatile state is caught when FLUSHED sets `durable := record` (`PromiseMonotone`, depth 10).  The same bug gives PASS when FLUSHED sets `durable := volatile`. | The engine should own the durable part.  A core declares `apply_persist(cfg, durable, record)`, a pure function, and the engine calls it on FLUSH.  `step` then gets durable as read-only, which the engine checks with memcmp on every event, including FLUSHED. |
| B5 | **Messages survive the crash of their destination, and there are no connections.**  CRASH leaves in-flight messages to and from n in the bag.  A protocol that runs over TCP connections (libpq, for example) sees a crash reset them.  The bag also delivers out of order, while a protocol may rely on FIFO connections with a gap check ("a gap drops the connection and the predecessor resends from `recv + 1`"). | E10: a client that sends once and never retries gives PASS with a server crash, because the old request is delivered to the new incarnation.  Over TCP that client hangs. | Per-pair FIFO channels, each tagged with an incarnation.  CRASH(n) empties every channel to and from n and delivers `CONN_RESET(peer)` to each peer.  Keep the bag as a mode for transports without FIFO.  For safety the bag is a superset of FIFO behaviour, so its counterexamples are real for bag transports but may be spurious for a FIFO protocol. |

## Major

| # | finding | evidence | fix |
|---|---|---|---|
| M1 | The durable check silently looks at only the first 512 bytes: `uint8_t dur[512]; if (ds > sizeof(dur)) ds = sizeof(dur)`. | E3: `durable_size = 600`, and a write to byte 550 on CLIENT gives PASS. | Reject `durable_size` above the buffer in `cmc_run_new`, or size the buffer per run.  B4 replaces the mechanism anyway. |
| M2 | An uninitialised read in a core is not caught by `--det`.  Both runs see the same stack contents. | E5 gives PASS under `--det`.  MemorySanitizer reports it in the engine's `bcmp`, with the read originating in the core. | Add `make check-msan`: clang `-fsanitize=memory` over the engine, cores and gates at small bounds, in CI.  Uninitialised padding in core state would also break dedup, and MSan covers that too. |
| M3 | Writing more than `CMC_OUT_MAX` outputs corrupts the engine's stack before `out->n` is checked. | E8, 20 outputs: "stack smashing detected", abort.  ASan reports stack-buffer-overflow.  Without the stack protector it is caught only by luck. | Cores emit through `cmc_out_push(out, ...)`, which refuses past `CMC_OUT_MAX` and sets an overflow flag the engine reports.  A grep gate bans `out->o[` in core sources. |
| M4 | Replay ignores the `model`, `cfg` and `env` lines it writes and uses the command-line budgets. | The replay file records `drop=1`.  Replayed with `--drops 1 --clients 2`, it prints "Replay completed: 6 states, no property violated", because the extra client changes quiescence.  Without `--drops` it reports "not enabled". | Replay sets the model, cfg and budgets from the file, and refuses on conflict. |

## Minor

| # | finding | evidence | fix |
|---|---|---|---|
| m1 | DFS prints the simulate message, "No error found in 1000 walks (not exhaustive)", and exits 0 with a witness not reached.  BFS exits 1 in the same case. | `toy-bug3 --dfs 40 --clients 0`: exit 0; BFS: exit 1 | Use a DFS-specific message.  An unreached witness fails in every mode. |
| m2 | The purity gate misses inline asm (`rdtsc`), `__builtin_readcyclecounter` and reads through absolute addresses.  `nm -u` sees only the extern global in the probe. | `asm.c` probe: `nm -u` shows only `glob_counter` | Also run `objdump -d` over the core objects and reject `rdtsc`, `rdtscp`, `rdrand`, `rdseed`, `syscall`, `cpuid`.  Grep core sources for `asm`. |
| m3 | A quiescence violation reports depth 9 for a trace of 8 states. | `cmc --model toy-bug2`: last state 8, "depth ... is 9" | Report the depth of the violating state. |
| m4 | Negative budgets and caps are accepted.  `--drops -1` runs as 255 drops (67 distinct, the same as `--drops 255`), and `--net-cap -1` fails only at the first send. | runs above | Reject `< 0` where `> 255` is already rejected. |

## Checked and sound

- Fingerprints: on 4.2 M structured inputs, each 64-bit half has 2,044
  and 1,984 duplicates in its low 32 bits (about 2,048 expected).  The
  joint 20+20-bit duplicate count is 9 (about 8 expected), so the halves
  are independent.  Mixed 64-bit gave 0.
- Caps: `CMC_MAX_ACTIONS`, `net_cap`, `persist_cap` and
  `reply_cap` are reported as engine violations, never truncated.
- A crash budget does not hide a lost wake when there are no timers.  E9
  catches it with 0 and with 1 crash.
- Replaying a buggy trace on the correct core reports "not enabled" at
  the first diverging step.
- Enabled actions in actor mode read only the environment part of the
  world.  No core state is consulted.

## Missing for real protocol cores

Rows marked B5 or B1 are covered by those fixes.

| need | for |
|---|---|
| `TICK(n, d)`: per-node monotonic clocks with drift bounded by ρ, plus `CLOCK` events | leases and their `L(1+ρ)` wait, commit timeouts |
| `LINKS`: partition sets, including asymmetric ones (HC had them); today only per-message DROP exists | failure detection, sealing, S2r stale reads |
| FIFO connections with `CONN_RESET` (B5) | gap check and resend |
| APPLY/APPLIED as an environment completion like PERSIST/FLUSHED, lost on crash | applier lag, promotion fences, low-water marks |
| `LOCAL_FAIL(txid)`, plus `DIE`, `TIMEOUT` and `CAS_TAKEN` for a commit waiter | a transaction that fails after its acknowledgement; waiter timeouts |
| Several cores per node: replication, consensus and lease logic on the same node share a crash.  Today a model has one core | components that fail together |
| A ghost history of client requests (txid, key, value), not just replies | "every acked write in every later history" (S3) |
| Bounded liveness (B1) | the "every client replied, no chain wedged, one leader" check |
| Symmetry reduction and an on-disk queue (survey §4a) | N=4 bounds |

## Resolution (2026-11-02)

Every finding was first turned into a regression test that failed on
the engine as reviewed: 25 new assertions in `test/test_cmc.c` failed, and the new
`purity_teeth.sh` reported 4 HOLEs.  Then came the fix, and then the
rerun.  The tests assert the right verdict, not just "something
changed".  Scratch, logs and the pre-fix copy are in `~/tmp/mc-fix`
(local).

| # | fix | regression test | before -> after |
|---|---|---|---|
| B1 | New property kind `CMC_EVENTUALLY` (bounded liveness).  From **every** reachable state, not only those with budgets spent, the engine runs a deterministic fair schedule: no faults and no new clients.  Each round drains every head-of-channel DELIVER, FLUSH, WAKE and CONN_RESET, then fires each timer armed at round start once, draining after each.  It fails if the goal is not reached within `--live-steps K` (default 1000), if the schedule stops, or if a round-start world repeats.  Every fair state is reachable, so the result is sound without budget bookkeeping.  Round starts that reach the goal are memoised, so the cost stays near one run per state.  The counterexample is the BFS-shortest prefix plus the fair suffix.  It is printed with a marker, written to the replay file, and replays.  `CMC_QUIESCENT` is kept.  A state whose only actions are timer fires that return the same world now also counts as quiescent.  The toy gains `EventuallyAllReplied` (replies == `cmc_requests()`, the env's own CLIENT count). | `test_b1_heartbeat_lost_wake` (E1: BFS, simulate, replay; the correct core with a heartbeat passes; the self-loop quiescence check) | toy-bug2 + heartbeat: PASS, 36 distinct -> `EventuallyAllReplied` violated at depth 3 (fair suffix cycles); simulate PASS -> violated |
| B2 | `purity_gate.sh` (it replaces the Makefile loop).  It also rejects defined `b B d D g G s S` symbols, non-empty writable sections, and TLS symbols.  Only const data (`r R`) is allowed.  hc_core.o and toy[0-3].o pass unchanged. | `purity_teeth.sh`, `probe/static_state.c` (E2) | accepted -> `writable data symbol hw.0 (type b)` |
| B3 | `CMC_A_CRASH(n, j)` for j = 0..k, where k is the number of pending persists.  Branch j feeds the first j records to `apply_persist`, which has no outputs (the dead process sends nothing).  Then volatile is zeroed and RESTART runs.  Cores with `durable_size 0` get only j = 0. | `test_b3_crash_keeps_disk_write` (E4) | PASS, 7 distinct -> `DurableImpliesReplied` violated, depth 3 |
| B4 | `CmcCore.apply_persist(cfg, node, durable, body, len)`.  FLUSH calls it, then delivers FLUSHED.  `step` must leave the durable bytes untouched on **every** event, FLUSHED included (memcmp).  `apply_persist` may not touch volatile, and the determinism gate covers it.  `durable_size > 0` without `apply_persist` is refused.  Toy and HC have `durable_size 0` and needed no change.  The test cores were migrated. | `test_b4_flushed_cannot_write_durable` (E6, both variants, fifo and bag) | honest: violated (kept); `durable := volatile`: PASS, 236 distinct -> `durable rule: ... changed durable state on FLUSHED`, depth 6 |
| B5 | `--net fifo` (the default) or `--net bag`.  FIFO keeps one ordered channel per (src, dst), and only a channel's head is deliverable.  CRASH(n) empties every channel to and from n and marks `CONN_RESET(n)` pending at each peer (new `CMC_A_RESET` action and `CMC_EV_CONN_RESET` event).  Until that peer takes the reset, its channel from n is closed and its sends to n are lost, because the old socket is dead.  This plays the role of an incarnation tag without one more world byte.  DROP on FIFO = connection reset: both directions are emptied and both ends get CONN_RESET.  DUP only in bag mode: TCP never duplicates, and `--dups` with fifo is refused.  The bag is kept for non-FIFO transports.  It is a superset of FIFO for safety, so its counterexamples may be spurious for a FIFO protocol. | `test_b5_crash_resets_connections` (E10; bag still PASSes, documenting the difference) | PASS, 6 distinct -> `Answered` violated, depth 4 |
| M1 | The 512-byte stack buffer is replaced by per-run scratch of `state_size` bytes (no clipping).  `durable_size > state_size` is refused. | `test_m1_large_durable` (E3) | PASS -> `durable rule ... on CLIENT` |
| M2 | `make check-msan`: clang `-fsanitize=memory` with origins over test_cmc, the toy gate and one HC FAIL row with `--det`.  It then builds `probe/uninit.c` (E5) and requires an MSan report. | `make check-msan` | `--det` blind -> reported at `copy_garbage probe/uninit.c:26` |
| M3 | `cmc_out_push(out, kind)` in `cmc.h`.  Past `CMC_OUT_MAX` it writes a sink entry and counts `overflow`, which the engine reports as a violation.  Toy and test cores were converted.  HC emits nothing.  The purity gate bans `out->o[` in core sources. | `test_m3_output_overflow` (E8, in a child process), `probe/out_index.c` | stack smash, abort -> `core emitted more than CMC_OUT_MAX (16) outputs (4 refused)` |
| M4 | Replay v2 header (`env ... mode= live=`).  `cmc --replay` takes model, cfg and env from the file unless they are given.  The engine refuses a model, cfg or budget conflict. | `test_m4_replay_conflict` | `--clients 2` "Replay completed" -> `recorded with different environment bounds`; another model is refused |
| m1 | DFS prints its own message.  An unreached witness fails BFS, DFS and simulate (exit 1). | `test_min1_dfs_witness` | `toy-bug3 --dfs 40 --clients 0`: exit 0 -> exit 1 |
| m2 | `purity_gate.sh` runs objdump and rejects `rdtsc(p)`, `rdrand`, `rdseed`, `syscall`, `sysenter`, `cpuid`, `int`, `rdpid`, `rdpmc` and absolute-address operands (`%fs:0x28` canary excepted).  It also greps sources for `asm`. | `probe/asm_clock.c`, `probe/builtin_clock.c` (`__builtin_ia32_rdtsc` + `*(u64*)0x7fff0000`) | accepted -> `instruction: rdtsc` |
| m3 | The depth of a violation is that of the violating state (for liveness, the state the fair suffix starts from). | `test_min3_quiescent_depth` | 9 -> 8 |
| m4 | Negative budgets and caps, depth < 1, and `live_steps` < 1 are refused. | `test_min4_negative_bounds` | `--drops -1` = 255 -> rejected |

Changed expectations, all from B5: `test_persist_crash_drop`'s DROP
trace is 8 states under FIFO (it adds the two CONN_RESET deliveries).
The old 6-state trace is still asserted under `--net bag`.  The DUP case
now runs in bag mode.  The toy control is 34 distinct in both modes, and
the toy gate depths are unchanged (Mutex 7, AllReplied 8, PhaseMonotone 5).

**Calibration** (`make check-calibration`, `--det`, this
dev host, same session; NOISY, indicative only): all 22 rows still match
TLC in verdict, depth and counts.  Before -> after wall time: code-cp S1
PASS 7,412,120 / depth 24 / 64,710,625 generated, 28.6 s -> 25.8 s.
code+C+Z S1 PASS 15,523,080 / depth 32, 62.7 s -> 59.6 s.  paper-cp S3a
35,895,280, 155.5 s -> 144.2 s.  The FAIL rows are identical (raw:
`~/tmp/mc-fix/calib-{before,after}.keep`).  The time dropped because
`core_step` no longer mallocs per step under `--det`.
`make check-hc-bugs`: all three planted bugs caught.  gcc 16.2 and clang
21.1: `-Wall -Wextra -Werror` clean, test_cmc and toy gate pass under both.

**Not done.**  The list under "Missing for real protocol cores": `TICK`/clocks, `LINKS`
partitions, APPLY/APPLIED, `LOCAL_FAIL`/`DIE`/`TIMEOUT`/`CAS_TAKEN`,
several cores per node, a ghost request history, symmetry and an
on-disk queue.  B1's fair schedule advances no clock, because none
exists until TICK.  B5 has no gap-check/resend model beyond CONN_RESET,
and a reset is per connection, not per incarnation number.  HC is direct
mode, so B1/B3/B4/B5 do not exercise it.  The MSan target was not yet
wired into CI.
