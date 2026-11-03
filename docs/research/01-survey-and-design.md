# 11: Model checking the code that ships (implementation-level model checking)

> **Historical record**, copied from pg_chain (`~/ws/pg_chain`, `docs/research/11-implementation-model-checking.md`).  It was written while libcmc was `chainmc` in pg_chain's `test/mc/`.  The
> names map as follows: `chainmc` is now `cmc`, `mc_`/`Mc`/`MC_` is now
> `cmc_`/`Cmc`/`CMC_`, `test/mc/mc.c` is now `src/cmc.c`, `test/mc/hc/` is now `calib/hc/`, and
> `spec/tla/cfg/` is now `calib/hc/cfg/`.  "PLAN" means pg_chain's
> `docs/PLAN.md`.  The current design is `docs/DESIGN.md`.


Status: research, 2026-11-02, time-boxed (about 45 min reading, 10 min
writing).  Answers the user's request: *"a way to have TLA+ like model
validation that uses the sources we build directly so that the model and
the code can never diverge (the code validated is the code that is run).
Another one of the projects (~/ws/libxtc) may have found something like
that."*

Labels: `[source: path:line]` (libxtc paths under `~/ws/libxtc`),
`[web: URL]` (fetched copies in `~/tmp/mc`), `[measured: ...]` (this host,
2026-11-02), `[inference]`, `[estimate]`, `[unverified]`, `[not read]`.
`PLAN` is `docs/PLAN.md` rev 3.2.  Scratch: `~/tmp/mc` (fetched papers and
READMEs, `tools.txt`, CBMC runs in `~/tmp/mc/cbmc`).

## Short answer

- **libxtc has two pieces of the answer and one warning.**  Its DST tier
  links the *real* library built with `--with-io-backend=sim`, so the code
  simulated is the code shipped.  Its CBMC tier checks real C in only 1 of
  14 harnesses (`deque_harness.c` includes `deque.h`); the other 13 are
  verbatim or "faithful" transcriptions that must be "updated in lockstep",
  which is exactly the drift the user wants to rule out.  The warning is
  the "omniscient oracle": the simulator decided runnability by reading
  internal state that production code has to learn by being woken, and
  that hid 8 bugs that reached releases.
- **Prior art says it works.**  CMC (2002) model-checked unmodified C
  protocol code with a hash table of visited states and found 34 bugs in
  3 AODV implementations.  MaceMC, MoDist and Demeter did the same for
  C++/binaries.  Stateright (Rust) has the cleanest design: the actor's
  `on_msg` is both the model and the deployed code.  None of them is
  usable as-is under the 100 % C rule, but the design ports directly.
- **Recommendation:** write the protocol cores once, in C, as
  deterministic step functions `(state, event) -> (state', outputs)`.
  The extension calls them, and a C explicit-state checker, `chainmc`,
  links the same object files.  Calibrate `chainmc` against TLC by
  porting the HC adoption rule and reproducing counterexamples A, B' and
  C plus the exhaustive passes at the same state counts.  TLA+ stays as
  the calibration oracle and an optional design sketch tool.  CBMC 5.x
  (not 6.x) covers the shmem code inside one node.

## 1. libxtc: what it does, and where it falls short

### 1.1 What satisfies "the code checked is the code run"

| mechanism | what is real | evidence |
|---|---|---|
| DST build | the whole library is rebuilt with the sim I/O backend; tests link `libxtc.a` from that build, plus real example sources (`examples/06_sqlxtc/*.c`) compiled against it | `test/sim/run_sim_tests.sh:64` (`configure --with-io-backend=sim ... --enable-diagnostic`), `:88-110`; `meson_options.txt:12` |
| sim scheduler | "the identical concurrency code runs; only WHICH runnable loop advances next, and when virtual time advances, are decided by the seed" | `test/sim/README:1-8` |
| determinism guard | traps real clock, environment and thread id from sim-reachable code.  A meta-test calls each *real* primitive and asserts the guard fired, because an audit had found the guard wired into only one of the four classes it claimed to cover | `test/sim/test_sim_determinism_guard.c:5-29`; `docs/testing.md:57` |
| planted bugs | `-DXTC_DST_INJECT_BUG=n` turns on one bug in the shipped source (18 sites in `src/`).  The gate requires (1) the control run to pass on a clean build, (2) the failure output to match the invariant regex for that bug, (3) a first catching seed within a budget of 64, (4) a timeout, and (5) kept evidence.  23 cases | `scripts/dst-bug-inject.sh:1-36`, `:64-88`; `src/inc/xtc_dst_inject.h:124` |
| failing-seed corpus | pinned seeds re-run on every push; a pin must fail on the pre-fix build | `test/sim/README:105-138` |
| CBMC on a real header | `deque_harness.c` `#include "deque.h"` and drives the real push/pop/steal | `test/cbmc/deque_harness.c:25,42` |

### 1.2 Where it falls short

| gap | detail | evidence |
|---|---|---|
| CBMC harnesses transcribe | 13 of 14 harnesses copy or model the algorithm.  11 of them say they must be "updated in lockstep" (all except deque, credit, mpsc).  Nothing checks the copy against the source.  The README's "REAL bm_read_begin/valid" for seqlock contradicts the harness, which says "transcribed VERBATIM" | `test/cbmc/README.md:27-49`; `test/cbmc/seqlock_harness.c:18-29`; `test/cbmc/wakepark_harness.c:20-37` ("real loop.c is not includable standalone") |
| why they transcribe | the real `.c` drags in the executor, RCU, slab and waits, so it does not compile standalone | `test/cbmc/README.md:44-49` |
| CBMC 6.x | aborts with "pointer handling for concurrency is unsound" on every concurrent harness, so `run.sh` SKIPs on 6.x.  5.91/5.95 work | `test/cbmc/README.md:88-104`; `test/cbmc/run.sh:12-56`; [measured: nixpkgs cbmc 6.9.0 aborts on wakepark, hlc and deque in 0.17 s each; `github:NixOS/nixpkgs/nixos-23.11#cbmc` 5.91.0 gives VERIFICATION SUCCESSFUL on wakepark (`--unwind 6`) and deque (`--unwind 4`) in 3.8 s] |
| omniscient oracle | `__sim_loop_runnable` marks a loop runnable when its inbox is non-empty or a completion is due, by reading loop-owned state.  Production must be *nudged* to notice.  8 cross-loop wake bugs escaped DST: 9/9 planted bugs were caught while a whole family was invisible | `.agent/DST_GAP_ANALYSIS_2026-09-11.md:9-60`; `src/evt/exec.c:792-830` (the gap doc's `exec.c:666` reference has itself drifted) |
| fix attempts | nudge-gated runnability was tried and backed out: it hung correct code, because the wake is an edge and not a level.  The real-thread guard test passes on both the fixed and the unfixed library.  NONUDGE is recorded as "NOT plantable" | gap doc `:131-170`; `scripts/dst-bug-inject.sh:142-158` |
| single thread | DST cannot reach TOCTOU races between real threads; those are left to TSan | `test/sim/README:146-152`; gap doc `:62-70` |

### 1.3 Lessons for pg_chain

1. Link the shipped objects.  Never transcribe.  If something must be
   copied, extract it mechanically at build time.  libdb's CBMC tier
   `#include`s the real `.c` for pure functions and claims "verified by
   `diff`" for the two it copies [source: `~/ws/libdb/test/cbmc/README.md:12-18`].
2. Design for checkability: the code libxtc could check directly was code
   that compiled standalone.  pg_chain's cores must compile with no PG
   headers.
3. **The environment may only do what the code's own outputs enable.**
   "A model that supplies the information for free cannot test the code
   path that acquires it" (gap doc `:175-177`).  Wakes, flush completions
   and timer firings must be explicit outputs of the code, delivered as
   events.
4. A planted-bug gate with a control run, an expected-reason match and a
   budget is what makes "the checker finds bugs" a claim with evidence.
5. Pin tool versions.  The CBMC major version changes the verdict.

## 2. Survey

"Ships?" means the checked code is the deployed code.  "Exh." means
exhaustive within bounds, as opposed to random.

| tool | ships? | exh.? | what it needs from the code | scale reported | fit for pg_chain cores |
|---|---|---|---|---|---|
| **CMC** (Musuvathi et al., OSDI 2002) [web: https://www.usenix.org/events/osdi02/tech/full_papers/musuvathi/musuvathi.pdf] | yes, unmodified C (mad-hoc, Kernel AODV, AODV-UU, 3.3-5.3 kLoC) | yes (BFS/DFS + visited hash table, hash compaction); heuristics when too big | per-process heap/globals that can be saved and restored; an event-handler loop mapped to transitions; an env model (400 LoC network + stubs); hand-written canonicalization, 165-185 LoC per implementation (Table 3); time stubbed (`gettimeofday` constant, timeouts nondeterministic) | 2-4 processes, network queue 1-3, bounded sequence numbers; 34 unique bugs (about 1 per 328 LoC), most in minutes, longest about 40 min | **the template.**  Our cores are simpler: fixed-size POD state, so no heap canonicalization |
| CMC on Linux TCP (Musuvathi & Engler, NSDI 2004) [web: https://www.usenix.org/conference/nsdi-04/model-checking-large-network-protocol-implementations] | yes | yes | as above, kernel code extracted | 4 errors in Linux TCP (abstract) [paper not read] | shows that it scales to large legacy C |
| **MaceMC** (Killian et al., NSDI 2007) [web: http://usenix.org/event/nsdi07/tech/killian/killian.pdf] | yes: the Mace compiler emits the deployed C++ | bounded DFS, then random walks | atomic, non-blocking event handlers; every nondeterministic choice through `Toss`; `MaceTime`/`MonotoneTime` so time comparisons become choices; state hash = all node state + pending events, with annotations to ignore or canonicalize fields | 52 bugs (Pastry, Chord, transport, overlay tree).  Liveness by "critical transition" search.  Duplicate-hash cut-off skips 50-90 % of walks (2-10x speedup) | liveness method worth copying; time as choice points matches our leases |
| **MoDist** (Yang et al., NSDI 2009) [web: https://www.usenix.org/legacy/event/nsdi09/tech/full_papers/yang/yang.pdf] | yes: unmodified binaries via WinAPI interposition | stateless: replay the action prefix to restore a state; DPOR + random | deterministic replay; a virtual clock for timeouts | 35 bugs, 10 protocol-level (Berkeley DB replication, MPS Paxos, PacificA) | interposing on a whole PG cluster is too heavy; the virtual-clock idea carries over |
| **Demeter** (Guo et al., SOSP 2011) [web: MSR abstract only; paper not read] | yes (in MaceMC and MoDist) | yes, with dynamic interface reduction | per-component interfaces; about 1 kLoC to integrate | 5x to 10^5x reduction; "explore completely ... both phases" of a production Paxos | reduction to consider later, only after calibration |
| SAMC (Leesatapornwongsa et al., OSDI 2014) [web: abstract] | yes (cloud systems) | systematic, with semantic reductions (message/crash independence, crash and reboot symmetry) | a few lines of protocol rules | 1-2 orders of magnitude faster on deep bugs | the cheap reductions for crash/reboot symmetry |
| **Stateright** (Rust) [web: https://github.com/stateright/stateright README, `src/actor.rs`, `src/actor/model.rs`, `src/lib.rs`] | yes: the same `Actor` impl runs on its UDP runtime (`spawn`) | BFS, DFS, on-demand, random simulation | `State: Clone+Debug+PartialEq+Hash`; `on_start/on_msg/on_timeout/on_random` return commands `Send/SetTimer/CancelTimer/ChooseRandom/Save`; network semantics chosen per model (unordered duplicating, unordered non-duplicating, ordered) plus lossy drop; `max_crashes` with Crash/Recover (`on_start` from `Storage`); 64-bit fingerprints (`lib.rs:343-351`); symmetry via `Representative`; `always`/`sometimes` properties, `eventually` "experimental/incomplete"; linearizability tester | Paxos example: 2 clients, 3 servers, 16,668 unique states (`examples/paxos.rs:333`) | **the API to copy** into C.  Rust itself is excluded |
| P / P# / Coyote [web: p-org/P README; MSR TR-2012-116; FAST 2016 abstract; microsoft/coyote README] | P: compiled code plus checker, ghost env machines erased by the compiler.  Coyote: real C# tasks/actors | systematic *testing* (randomized and priority schedulers), mostly stateless | own language or the .NET runtime | P is used across AWS (S3, EBS, DynamoDB ...).  P# found Azure storage bugs | C# or a DSL: no.  "Ghost machines erased" = our env-only code |
| FoundationDB simulation [web: https://apple.github.io/foundationdb/testing.html] | yes (Flow runtime, sim mode) | random | everything through Flow; single-threaded deterministic | "roughly one trillion CPU-hours" (estimate on that page); swizzle-clogging | = PLAN L3.  Random only |
| TigerBeetle VOPR [web: tigerbeetle `docs/internals/vopr.md`] | yes (production code; clock, network and disk stubbed) | random, seed + commit replay | deterministic code, assertions kept on in production | (not quantified there) | = PLAN L3 |
| GenMC [web: MPI-SWS/genmc README, manual] | yes (C/C++/Rust at LLVM IR) | stateless MC with DPOR, exhaustive for bounded threads | threads plus atomics; SC, TSO, RC11, IMM | (not read) | shmem algorithms in one node.  **Not in nixpkgs**: build from source (LLVM 19-22) or `docker pull genmc/genmc` |
| Nidhugg [web: nidhugg README] | yes (LLVM IR) | stateless, DPOR | pthreads; SC, TSO, PSO, POWER, ARM; "each thread should be deterministic when run in isolation" | (not read) | same niche.  Not in nixpkgs |
| CDSChecker [web: plrg.eecs.uci.edu CDSChecker page] | yes (C11 atomics, library) | exhaustive under the C11 memory model, with POR | C11 `<threads.h>` only | (OOPSLA 2013, not read) | PG uses `pg_atomic_*`, not C11 atomics: poor fit.  Not in nixpkgs |
| CBMC [measured; libxtc] / ESBMC [web: esbmc README] | yes, real C | bounded (unwinding); ESBMC also k-induction | standalone-compilable code; `--unwind` bounds | libxtc: 14 harnesses in about 3.5 min (`test/cbmc/README.md:58-60`) | shmem protocol and codec.  CBMC needs 5.x.  ESBMC not in nixpkgs |
| KLEE [web: klee-se.org] | yes (LLVM IR) | path exploration with symbolic inputs | inputs marked symbolic; small code | (not read) | message/record **decoder** at the trust boundary.  nixpkgs 3.3 |
| PGo [web: DistCompiler/pgo README] | generated Go from Modular PlusCal, so one source; TraceLink for traces | TLC on the PlusCal | write the system in MPCal | Raft KV in `systems/` | the "one source" idea, but the target is Go, and it would put PlusCal on the product path |
| Smart casual verification (CCF, NSDI 2025) [web: https://www.usenix.org/system/files/nsdi25-howard.pdf] | TLA+ is separate; traces from the C++ are checked against it | trace check (DFS in TLC) + model checking + simulation of the spec | about 15 log statements at linearization points; a global clock in the test driver | 6 bugs; logging took about 1 day, the trace spec about 2 engineer-months over 4 months; spec 1134 LoC vs 63 kLoC of code; the union-tally bug took 48 h of exhaustive TLC on 128 cores | = PLAN L4.  It reduces divergence but does not remove it: the spec is a second source |
| Jepsen / Elle [web: jepsen-io/elle README] | yes, black box | random histories, sound anomaly detection | client histories only | "~linear in history length" | = PLAN L6 |
| Spin `c_code`/`c_track`, Modex [web: spinroot.com/spin/Man/c_code.html, c_decl.html, spinroot.com/modex] | Promela env calling real C; `c_track` puts C data into the state vector | yes (Spin DFS, bitstate) | C state tracked by address and size; an env written in Promela | (not read) | a **fallback** if writing `chainmc` stalls: real cores plus Spin's search, at the price of Promela for the environment.  nixpkgs spin 6.5.2 |

Tools on this host [measured, `~/tmp/mc/tools.txt`]: `nixpkgs#cbmc` 6.9.0
(fails the libxtc concurrency harnesses), `github:NixOS/nixpkgs/nixos-23.11#cbmc`
5.91.0 (works), `klee` 3.3, `spin` 6.5.2, `tlaplus` 1.7.4.  ESBMC, GenMC,
Nidhugg, CDSChecker, CPAchecker, Symbiotic, DIVINE and Dartagnan are not
in nixpkgs.  Nothing was installed system-wide.

## 3. What the survey settles

- Exhaustive checking of real protocol code works when the code is
  **event-driven with atomic handlers** and **all nondeterminism is an
  input** (CMC, MaceMC, Stateright).  Instrumenting arbitrary code
  (MoDist) works too, but costs far more.
- The expensive parts are always the same three: the environment model,
  canonicalization of states for hashing, and bounding counters and time.
  CMC spent 165-185 LoC per implementation on canonicalization.  A
  fixed-size, pointer-free state struct removes most of that.
- Random simulation (FDB, VOPR, libxtc DST) and exhaustive checking use
  the same harness with different choosers (Stateright `spawn_simulation`,
  TLC `-simulate`).  One harness can do both.
- Trace validation (CCF) is the answer when the code was not written to be
  checked.  pg_chain has no code yet, so it can be written to be checked.

## 4. Recommendation for pg_chain

### 4a. Cores written once in C, and `chainmc` links them

**Cores** (`src/core/`, header `include/pg_chain/core.h`):

| core | covers (PLAN) | events in | outputs |
|---|---|---|---|
| `chain_member.c` | §5.2-5.6, §8.2-8.3: recv/durable/acked/applied/lwm per chain, epoch adopt/seal, promotion fence, wedge plus ambiguity generation, repair state, restart-means-rejoin | `RECV(msg)`, `FLUSHED(lsn)`, `APPLIED(s)`, `LOCAL_COMMITTED(s, ok)`, `TIMER(k)`, `CLOCK(t)`, `RESTART(durable)` | `SEND(dst,msg)`, `PERSIST(rec)`, `APPLY(s)`, `WAKE(waiter)`, `SET_TIMER(k,t)`, `REPLY(txid, ok/err/08007)` |
| `commit_waiter.c` | §5.2 steps 3-6, §5.5: the per-transaction wait outcome | `WAKE`, `TIMEOUT`, `DIE`, `CAS_TAKEN(bool)` | `NOOP_REPLACE`, `WEDGE`, `REPLY` |
| `paxos.c` (acceptor, proposer, learner) | §7.2: Flexible Multi-Paxos log, `VoterCfg` with per-configuration tally, `SetChain` CAS | `RECV`, `FLUSHED`, `TIMER`, `CLOCK` | `SEND`, `PERSIST`, `SET_TIMER`, `COMMITTED(slot,entry)` |
| `lease.c` | §7.2 leases: `valid_until = t_send + L(1-rho)`, the `L(1+rho)` wait | `CLOCK(t)`, `RECV(grant)` | lease state only |
| `txn2pc.c` (later) | §10.2-10.4: PREPARE, decision CAS, `decided[]`, closed timestamps | as above | as above |

Rules, each enforced by a build check:

- One entry point per core:
  `int core_step(const CoreCfg *, CoreState *, const CoreEvent *, CoreOut *)`.
  It is total and deterministic.  It reads no clock, no randomness, no
  globals, no shmem, and it does not allocate.  Time and randomness arrive
  in events.  State is a fixed-size, pointer-free struct with compile-time
  capacities (`CORE_MAX_NODES`, `CORE_MAX_INFLIGHT` ...).  Its contents
  are kept canonical (sorted arrays, unused slots zeroed), so the bytes
  are the encoding.
- `CoreState` is split into `durable` and `volatile` parts.  `PERSIST`
  takes effect in `durable` only when the environment later delivers
  `FLUSHED`.  A crash keeps `durable` minus unflushed persists.  Then
  "reply only after flush" (steering) is checkable, and so is restart
  coming back wedged (§8.3).
- **Purity gate** (`make check-core-purity`): compile `src/core/*.c` with
  only `include/pg_chain/` and libc headers, and run `nm -u` against an
  allowlist (`memcpy`, `memset`, `memcmp`).  Determinism gate: `chainmc`
  runs every step twice from a copy and compares output bytes (this
  catches uninitialised reads), under ASan/UBSan at small bounds.
- **PG glue** (`src/repl/`, `src/config/worker.c`) only translates.  It
  turns shmem, latches, libpq, WAL flush and xact callbacks into events,
  and it executes outputs (generic WAL for `PERSIST`, `SetLatch` for
  `WAKE`).  The glue is the only protocol-relevant code `chainmc` does not
  run.  It is covered by 4c.
- Broken variants (PLAN §11 lists about 25) are `#if PGCHAIN_MC_BUG == n`
  sites **in the shipped core source**, compiled out by default.  This is
  libxtc's `XTC_DST_INJECT_BUG` pattern.

**`chainmc`** (`test/mc/`, a C program, not a backend, so threads are
allowed):

| aspect | design |
|---|---|
| global state | `World = {CoreState node[N]; Bag net; Env env; Ghost ghost}`.  `net` is a sorted multiset of `(src, dst, msg)` with counts (bounded duplicates).  `env` holds the link set (pairs, as in HC's TLA+), crashed bits, per-node clock, per-node pending-persist FIFO, armed timers, and the remaining fault budgets.  `ghost` holds client requests issued and replies received, written **only** from `REPLY` outputs (Stateright's `record_msg_in/out`) |
| enabled actions | `DELIVER(m)` for each distinct `m` in `net`; `DROP(m)` and `DUP(m)` (budgeted); `FLUSH(n)` (completes the oldest pending persist); `FIRE(n,k)` for timers the core armed; `TICK(n,d)` with drift inside rho (budgeted); `LINKS(s)` (budgeted partition changes); `CRASH(n)` and `RESTART(n)` (budgeted); `CLIENT(txid)` (budgeted); `LOCAL_FAIL(txid)` for §5.4's residual post-ack abort (budgeted) |
| non-omniscience | (1) `chainmc` sees `CoreState` as opaque bytes: the header it includes declares an incomplete type plus `core_state_size()`, `core_step()`, `core_describe()`, `core_permute()`.  (2) An action is enabled only by something the code emitted (`SEND`, `SET_TIMER`, `PERSIST`, `WAKE`) or by the environment's own fault budget.  (3) Waking a waiter is an action that exists only if the core emitted `WAKE`.  A forgotten `SetLatch` then shows up as a waiter that never replies, caught by the quiescence check below.  (4) Invariants may read internals through a separate `test/mc/inv_*.c` that includes the private header, takes `const World *`, and is never consulted when computing enabled actions.  A grep gate keeps the private header out of `chainmc`'s search code |
| hashing | 128-bit fingerprint of the concatenated canonical bytes (two independent 64-bit hashes, written in C, no libm), kept in an open-addressing table of `(fp, parent fp, action id)` (about 40 B/state, so 10^8 states is about 4-6 GB; this host has 185 GB).  Print the collision probability as TLC does |
| search | BFS (shortest counterexample, matching TLC), with a worker pool over a sharded table.  The frontier queue holds full worlds in an mmap'd file under `~/tmp`.  Also DFS with a depth bound, and `--simulate --seed` (random walk: this **is** PLAN L3) |
| bounds | N (3-4), epochs, slots, client txns, and per-fault budgets (drops, dups, crashes, partition changes, ticks).  Rows are abstract `(key id, version)` inside the cores: the core tracks positions and per-key versions, never tuples |
| symmetry | optional `core_permute(state, perm)`; canonical form = minimum encoding over permutations of interchangeable nodes (non-members, learners).  Off by default.  A run counts only if the same counterexamples appear with it on and off |
| properties | `MC_INVARIANT(name, bool (*)(const World *, McWhy *))`; `MC_ACTION_PROP(name, bool (*)(const World *pre, const McAction *, const World *post, McWhy *))` (epoch monotone, every ack/promise preceded by `FLUSHED`, no reply after removal ...); `MC_WITNESS(name, ...)`, which must be reached (vacuity, like `W_AckAfterReconfig`) |
| liveness | Bounded only.  The quiescence check: from any state with fault budgets spent, the fair schedule (deliver everything, flush, fire timers, advance clocks) must reach "every client replied, no chain wedged, one leader" within K steps.  MaceMC-style random walks from the BFS frontier for deeper cases.  No lasso/fairness checking in v1 |
| POR | none in v1.  Sound DPOR over message handlers is subtle, and SAMC-style independence rules are only safe once calibrated.  Revisit only if N=4 bounds do not finish |
| counterexample | the action sequence (from the parent chain), printed with `core_describe()` and a per-step state diff.  It is also written as a replay file that `chainmc --replay` runs through the **real** cores (so it can be stepped in gdb), and the same format seeds `--simulate` and the pinned-seed corpus |
| bug gate | `scripts/mc-bug-gate.sh` follows `dst-bug-inject.sh`: a control run must pass; with `-DPGCHAIN_MC_BUG=n` the run must report the expected invariant name within a depth/state budget; the evidence is kept.  A variant that is not caught is a HOLE and fails the gate |

Throughput: TLC did 15.5 M distinct HC states in 475-841 s with 16
workers [source: `docs/measurements/2026-11-01-tla-humming-raw.txt:11,18`].
Compiled step functions over flat structs should be at least as fast per
state **[estimate, unverified]**.  The risk is the other way: real C
state is larger than a TLA+ abstraction, so keep the core state small.

### 4b. Where TLA+ still earns its place

1. **Calibrating `chainmc`** (required, one-time, then kept in CI).  Write
   `test/mc/hc/hc_core.c`, a C port of `spec/tla/HummingConsensus.tla`
   with the same switches (`CPMode`, `UnanimityRule`, `Confirm`, `Zerf`),
   at the same bounds (N=3, MaxEpoch=3, one write, no crash, links = any
   set of pairs).  HC is a shared-register model, so this port's actions
   are the TLA+ actions (`Propose`, `Deliver`, `ReadRepair`, `Adopt`,
   `Forward`, `Ack`, `Repair`, `DoConfirm`, `NetChange`).  It tests
   `chainmc`'s search, dedup and invariant machinery, not the core API.
   The encoding must be a bijection of the TLA+ variables (`props` as a
   sorted set, write-once registers).  Required, from
   `docs/measurements/2026-11-01-tla-humming.md` and the raw file:

   | config | invariant | TLC result | chainmc must give |
   |---|---|---|---|
   | paper, cp | S1 non-none | FAIL, counterexample A, depth 9, 8,976 distinct | FAIL at depth 9; the trace is the A shape |
   | paper, cp | S3 | FAIL, B', depth 14, 1,396,108 distinct | FAIL at depth 14 |
   | code+Z, cp | S3a | FAIL, C, depth 14, 2,210,647 distinct | FAIL at depth 14 |
   | paper, cp | S2r | FAIL, depth 11 (9-step trace), 133,552 distinct | FAIL, same depth |
   | code, ap | S1 non-none | FAIL, depth 6, 1,206 distinct | FAIL at depth 6 |
   | code, cp | S1 non-none | PASS, 7,412,120 distinct, depth 25 | PASS, **exactly** 7,412,120, depth 25 |
   | code+C+Z, cp | S1, S3, S3a | PASS, 15,523,080 distinct, depth 32-33 | PASS, **exactly** 15,523,080 |
   | paper, cp | W_AckAfterReconfig | violated as required, depth 9 | witness reached |

   **Correction (2026-11-02, measured):** the depths above come from
   multi-worker TLC, which does not guarantee shortest counterexamples.
   With `-workers 1`, TLC gives A at 7, B' at 12, C at 13, S2r at 9; the
   passing counts are unchanged.  chainmc matches the 1-worker numbers on
   all 22 rows: `docs/measurements/2026-11-02-mc-calibration.md`.

   For failing runs, compare depth and verdict.  Distinct counts at the
   moment of failure depend on order within a BFS level.  Optionally,
   feed the C trace to TLC as a trace check.  Exact counts on the passing
   runs are the strong test: they show that dedup neither merges distinct
   states nor misses any.
2. **Design sketches** for protocols not yet settled, before their core
   exists, when iterating in TLA+ is cheaper than in C: multi-chain 2PC
   (§10.2) and parallel commits (M9).  Such a spec is a draft and never a
   gate.  The gate is `chainmc` on the C.
3. HC and HardenedHC stay as the record of why HC was rejected (L1).

### 4c. What stays with other tools

| concern | tool | rule |
|---|---|---|
| shmem protocol inside one node: chain LWLock + `s` assignment + noop CAS, sender-takes-record CAS, commit-wait flag polling, ring to the sender | CBMC **5.91** pinned via `github:NixOS/nixpkgs/nixos-23.11#cbmc` (6.x fails, measured); GenMC (Docker or a source build) if relaxed atomics appear | put the protocol in a small `src/repl/commit_slot.c` that compiles standalone over a shim mapping `pg_atomic_*` and LWLock acquire/release to C11/CBMC primitives, and **`#include` the real file** (libxtc's deque pattern).  No transcriptions.  A harness that cannot include its source is a design bug in the source |
| record/message decoding from the network (trust boundary) | KLEE 3.3 or CBMC on `src/log/record.c` | symbolic bytes: no OOB or UB, and `encode(decode(b)) == b` for accepted `b` |
| random deep runs | `chainmc --simulate` (= L3) | 10^6 seeds per CI run; fault densities in the spirit of FDB swizzle-clogging; pinned failing seeds |
| glue correctness (events delivered, outputs executed) | **C replay validation** (replaces most of L4): the glue logs `(node, event)` and `(node, output)`; a checker feeds the events through the same `core_step` and requires identical outputs, and checks that every delivered message was previously sent | cheap: the core is its own specification.  TLA+ `Trace.tla` becomes optional |
| PG integration: `PRE_COMMIT` window, WAL, xact abort paths, interrupts | pg_regress, isolation, TAP + injection points (L5); EC2 chaos + C history checker (L6) | unchanged |

### 4d. Changes to PLAN §11 and §12

Proposed; not applied (research only).  The project steering rule
"protocol changes go through the model first" would read "through
`chainmc` first": change the core, add the invariant and the broken
variant, show that `chainmc` catches the variant and passes at the
bounds.  That needs a PLAN revision bump and a steering edit.

| layer | now | proposed |
|---|---|---|
| L1 | HC TLA+ | unchanged, plus `chainmc` calibration against it (4b.1) |
| L2 | `ChainConfig.tla`, `ChainData.tla` at N ≤ 4, epochs ≤ 4 | `chainmc` exhaustive on the shipped cores at the same bounds, with every §11 invariant and broken variant |
| L2b | (none) | CBMC 5.x on the shmem modules; KLEE/CBMC on the codec |
| L3 | separate C simulator | `chainmc --simulate`, same harness |
| L4 | TLA+ trace validation, M3 exit | C replay validation of glue logs; TLA+ trace check optional |
| L5, L6 | unchanged | unchanged |

| milestone | now | proposed | est. **[estimate]** |
|---|---|---|---|
| M-1 | two TLA+ specs, 7-10 wk | **M-1a** `chainmc` engine (BFS/DFS/simulate, fingerprint table, replay, bug gate, purity and determinism gates): 3-4 wk.  **M-1b** HC calibration core plus the table in 4b.1: 1-1.5 wk.  **M-1c** cores `chain_member`, `commit_waiter`, `paxos`, `lease` as production C, with invariants and broken variants passing L2: 7-9 wk | 11-14.5 |
| M1 | pure-logic C + simulator, 6-8 wk | projection `mk/3`, quorum runtime, placement, record codec (cores and simulator moved to M-1): 3-4 wk | 3-4 |
| M3 | trace validation (CCF: about 2 engineer-months for the trace spec) | C replay validator: about 1 wk | saves 4-7 |
| M5b/M5c | TLA+ first | `txn2pc` core + `chainmc`; optional TLA+ sketch first | about the same |

Net: M-1+M1 goes from 13-18 to 14-18.5 weeks.  M3 saves about 1-1.5
months.  The result has one source for protocol logic.  Risks: (1)
design iteration in C is slower than in TLA+ for unsettled protocols
(mitigation: 4b.2); (2) the state space of real cores may exceed TLA+
bounds (mitigation: compile-time capacities, abstract rows, symmetry);
(3) `chainmc` itself can be wrong (mitigation: 4b.1 calibration, the bug
gate, determinism double-runs).  Fallback if M-1a overruns: Spin 6.5.2
with `c_code`/`c_track` driving the same cores.

## 5. Not done in the time box

- Not read: Demeter and SAMC full papers, CMC on Linux TCP (NSDI 2004),
  the GenMC and Nidhugg papers, the CDSChecker paper, PGo's ASPLOS paper,
  P#/Coyote papers, the Stateright book, and MoDist beyond §1-2.
- Not measured: `chainmc` throughput (nothing is written yet), GenMC or
  Nidhugg on any code, KLEE on anything, Spin `c_track` on a C step
  function.
- Not checked: whether PG's `pg_atomic_*` and LWLock semantics map
  soundly onto CBMC's concurrency primitives.  That belongs in the first
  L2b harness.
