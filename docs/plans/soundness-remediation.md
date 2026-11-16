---
title: "Plan: soundness remediation"
parent: Reference
nav_order: 10
permalink: /reference/plan-soundness/
lede: >-
  The audit of 1.3.0 found false passes, gates that report success after a tool failed, and claims the evidence does not support.  This plan fixes them in order of risk.
---

# Plan: soundness remediation

Status: in progress.  Audited commit: `b2eab84` (1.3.0).  Branch:
`pr/soundness`.

## Why

libcmc's product is a verdict another project trusts.  The steering
rule is one question: could this make cmc report PASS where a real bug
exists?  An audit of 1.3.0 answered yes, with reproductions, in all three
engines and in the gates.  Until this plan is done, a cmc PASS is a
debugging aid, not verification evidence, and the documentation must say
so.

The probes behind each finding are kept under
`/scratch/libcmc/audit-*` on the audit host.  Each fix below starts by
turning one of them into a red test in the repository.

## Rules for every item

1. **Red first.**  A test in the repository that gives the wrong verdict
   on the current engine, shown failing, then the fix, then the test
   green.  The test asserts the right verdict, not that output changed.
2. **Fail closed.**  A construct the engine cannot model exactly is a
   model error (exit 3) or INCOMPLETE (exit 2), never PASS.  Supporting
   less, correctly, beats supporting more, approximately.
3. **No claim without its test.**  A sentence in the README, the man
   pages, the site or the CHANGELOG that states a capability names the
   gate that checks it.  A capability with no gate is described as
   unverified or is removed.

## Phase 0: stop overstating (docs and gates)

- **0.1 Truthful status.**  README, `docs/fit.md`,
  `docs/plans/tlaplus-scope.md`, `docs/examples.md` and the model files'
  comments stop calling the protocol models "certified".  Three tools
  agreeing on a `.cml` shows the rules were executed as written, not that
  the rules are the named protocol.  The README's "calibrated against
  TLC" names the in-process engine only; the process and abstract engines
  have separate search loops.  The paxos2 "one model, many
  implementations" claim is withdrawn until 4.1 lands.
- **0.2 Strict Apalache outcome.**  `test/apalache_gate.sh` accepts only
  `The outcome is: NoError` (or `ExecutionsTooShort` with the bound
  stated) for a pass, and only `The outcome is: Error` with an invariant
  violation for a failure.  Any `Input error`, `Typing input error`,
  parser error or nonzero exit other than the violation code fails the
  gate.  A self-test with a fake tool that prints a parser error must
  fail.
- **0.3 Release path.**  `check-emit-tla` and `check-emit-apalache` run
  in CI before a tag, or the docs stop citing them as release evidence.
  Status: done.  A `bridge` job in `.github/workflows/ci.yml` runs both
  on the calibration's triggers, which the release workflow calls on a
  tag, with `CMC_REQUIRE_TOOLS=1` so a missing tool fails instead of
  skipping.
- **0.4 A qualification matrix** (`docs/qualification.md`, done; kept
  current as each item lands): for each
  engine, each supported syscall and each property kind, whether it is
  exact, approximate or refused, and the gate that checks it.  This
  replaces prose claims as the source of truth.

## Phase 1: abstract checker (`src/cmc_spec.c`)

Each item has a reproduction in the audit; each becomes a row in
`test/spec_gate.sh`.

- **1.1 C2 checks Init.**  The first observation after a reset must
  satisfy the spec's `init`, or refinement fails.
- **1.2 C2 respects action gates.**  `abstract_step_exists()` skips an
  action that `act_active()` rejects, as the search and the emitter do.
- **1.3 C2 compares exact values.**  Stutter and successor matching
  compare abstract variable values, not `state_fp()`, so neither
  symmetry nor a hash collision can match two different states.
  Loading a spec resets every model global (symmetry, defines,
  properties).
- **1.4 Bounded copies are errors.**  An expression, name, logical line
  or concatenated `init` that does not fit its buffer is a model error.
  No silent truncation anywhere in the loader or the binding loader.
- **1.5 Types, domains and numbers.**  A quantifier or parameter domain
  must be a set; `init` and every successor must lie in each variable's
  declared domain; integer literals and arithmetic that overflow are
  model errors.  `/` and `mod` refuse negative operands, where C's
  truncating division differs from TLA+.  Constructs TLC rejects are
  errors too: an `init` conjunct that reads a variable not yet set, two
  assignments to one primed variable, and a bound name that redeclares
  one already in scope.
- **1.6 Primes.**  A primed variable is rejected anywhere the evaluator
  would read it as unprimed (right-hand sides, guards, invariants).
- **1.7 Liveness semantics match TLA+.**  Stuttering is a behavior: a
  state with no enabled action, and unfair stuttering where fairness
  does not forbid it, are cycles.  `eventually P` is violated only by a
  fair path that never reaches P.  `A ~> B` checks every fair bad
  component, and its backward search stops at B states.  The gate
  compares each liveness probe against TLC on the emitted module.
- **1.8 Capacity.**  The liveness graph checks capacity on every edge;
  overflow is an error, never a truncated graph.  AddressSanitizer runs
  the spec gate in CI.

Phase 1 status: done on `pr/soundness`.  Every item has a row in
`test/spec_gate.sh` that failed on `b2eab84`; the liveness probes also
run through TLC in `test/emit_gate.sh`; `make check-asan` runs the spec
gate under ASan and UBSan.  Liveness under `symmetry` is refused rather
than checked, since a permuted successor folds into a self-loop.

## Phase 2: in-process engine (`src/cmc.c`)

- **2.1 A crash clears the node's self-channel.**
- **2.2 Simulation checks every visited state**, including the last one
  of a walk, for quiescence and eventuality properties.
- **2.3 The property cap is an error**, checked before any array write.
- **2.4 Replay handles an enabled-action error** at every state,
  including the last, with the same verdict BFS gives.
- **2.5 The fair schedule** either cannot postpone an armed timer
  forever, or is documented as a fixed-priority bounded schedule whose
  liveness verdict is not a fairness verdict.

Phase 2 status: done on `wip/soundness-phase2`.  Each item has a test
in `test/test_cmc.c` that fails on `1b88c86`:
`test_p21_crash_clears_self_channel` (a crash drops the messages a node
sent itself, in FIFO and bag), `test_p22_simulate_checks_last_state`
(a walk checks its last state for quiescence and eventuality),
`test_p23_property_cap` (a 65th property of one kind, or one of no known
kind, is refused by `cmc_run_new()`), `test_p24_replay_enabled_error`
(replay reports BFS's `cmc:engine` verdict for an action-list overflow
at its last state) and `test_p25_fair_schedule_fires_timers` (an armed
timer, and a message on another channel, are not starved by a node that
messages itself forever).  2.5 is fixed, not only documented: a drain
runs in generations and ends when it returns to a world it started a
generation from, so the timers that follow it fire.  The verdict is
still about that one schedule, not every fair one, and `DESIGN.md`,
`cmc(1)` and `libcmc(3)` say so.  The calibration rows did not move.

## Phase 3: process engine (`src/cmc_proc.c`, `src/cmc_preload.c`)

- **3.1 Exact state equality.**  The visited set stores the full world
  encoding and compares bytes on a fingerprint hit.  A hash is an index,
  not an identity.
- **3.2 A complete world encoding.**  The durable record of every live
  node; every polled fd (no five-entry record); epoll user data; futex
  waits keyed by the object waited on (normalised addresses that keep
  identity), associated with the waiting thread's role.  A whitebox test
  builds pairs of worlds with different futures and asserts their
  encodings differ.
  Status: done.  The seen set stores each world's encoding and compares
  bytes on a fingerprint match.  The encoding now carries the durable
  record and how much of it was read back, every polled fd with its
  events, epoll user data, a futex word by address (nodes run with
  `ADDR_NO_RANDOMIZE`, so addresses repeat across replays), a recv's
  requested length, a pending poll wake, births in flight, and the
  per-connection flags.  `test/proc/fingerprint.c` fails on `b2eab84`
  for each of those and checks a forced collision.  Two fixtures gain
  states, each a real split: restart.c 28 to 30 (a recv waiting for a
  2-byte header against one for the body), hc.c 11,545 to 14,730 (a
  participant partway through a 3-byte read).
- **3.3 Hook records are exact.**  A canonical record longer than `SNAP`
  is a model error; a partial record is kept or rejected, never dropped.
  Status: done.  `drain_pipe()` reads every record, refuses one longer
  than `SNAP`, and refuses bytes left after the last complete record.
  The longrec and partrec modes of `test/proc/unmodelled.c` were a
  truncated state and a pass on `b2eab84`.
- **3.4 Remote memory errors propagate.**  A short `process_vm_readv` or
  `process_vm_writev` is a modelled `EFAULT` or an engine error, never
  zero-filled data reported as success.
- **3.5 Preload allocator.**  `calloc` multiplication and arena capacity
  checks cannot overflow; `realloc` copies the old allocation's size.
- **3.6 Refuse what is not modelled.**  `EPOLLET`, `EPOLLONESHOT`,
  `EFD_SEMAPHORE`, finite `poll`/`ppoll`/`epoll_wait`/futex timeouts,
  and any other unsupported flag are model errors until modelled
  exactly.  The synthetic timeout byte `'T'` is documented as a
  convention distinct from a syscall timeout.
- **3.7 TCP connections, not node pairs.**  Until per-connection queues,
  FIFO order and reset state exist, a second connection between the
  same two nodes, or a reconnection after a reset, is refused.  Then
  model connections by identity with a generation number, keep stale
  descriptors until the peer closes them, and make a write to a reset
  connection fail as Linux does.  A `connect()` to a node that is not
  listening returns success and waits forever; Linux gives
  `ECONNREFUSED`, so the refusal path of a retrying client is never
  explored until this is modelled.

  Status: done.  Each connection has its own queue in each direction,
  `close()` sends a FIN or (with bytes unread) an RST, a reset leaves
  one `ECONNRESET` at each live end and then `EPIPE` with `SIGPIPE`
  decided by the writer's disposition, a crashed host's ends go dead
  without a FIN, a stale descriptor is kept until its owner closes it,
  and `connect()` to a node that is down is refused.  A live node that
  has not yet called `listen()` still accepts the dial (a shortcut,
  stated in cmc(7)).  `test/proc/crossrpc.c` gave a false violation on
  `b2eab84`, as did the refused, rstwrite and rstignore modes of
  `test/proc/unmodelled.c`.  The fix exposed two examples whose
  results came from the old model: raft's planted bug and all of hc
  (4.7, 4.8).  An independent review then found four gaps against
  Linux, each now matched: a write after the peer's FIN (EPIPE, not
  ECONNRESET), an RST that kept the bytes sent before it, an exited
  listener's backlog, and a socket dialled again after a refusal.
  `test/proc/tcpsem.c` runs each natively on loopback and under cmc,
  and requires the same result.
- **3.8 Threads.**  Until the scheduler explores every order in which
  runnable threads may proceed at the modelled preemption points (a
  futex wake, a thread birth, each intercepted syscall), a run in which a
  node has more than one thread ends INCOMPLETE ("thread schedules not
  explored"), never PASS.  Then make the choice of which runnable or
  woken thread proceeds a search action, and state the preemption
  boundary in cmc(7).
  Status: the interim rule is done.  Any `CLONE_THREAD` sets a flag that
  turns a would-be pass or unmet `eventually` into INCOMPLETE; the
  twothread rows in `test/proc_gate.sh` now expect exit 2, and the
  planted bug is still caught.  The search action is still open, and is
  deferred to its own milestone, because it is a rewrite of how the
  process engine answers system calls rather than a local fix:

  - Every owned system call of a thread in a multi-threaded node becomes
    a preemption point: the call is deferred, the thread parks, and when
    every thread is parked the search chooses which one to answer next.
    `try_pending_for()` answers most calls inline today, so each case
    must learn to defer.
  - `FUTEX_WAKE` defers the waker as well as marking the waiters, so the
    search can run a woken thread before the waker's next call (the
    wake-before-store race) as well as after it.
  - A birth must be serialised.  `clone` returns in both threads at
    once, and the parent and the newborn run user code concurrently
    until each reaches an owned call, which is a race in the checker,
    not only an unexplored schedule.  The newborn has to be held at its
    first instruction (a ptrace stop on `PTRACE_EVENT_CLONE`) until the
    search releases it.
  - The boundary stays system calls: interleavings of plain memory
    accesses between two calls are not explored, and cmc(7) must say so.

  Until all of that lands, the INCOMPLETE rule is the honest answer.
- **3.9 A root with no enabled action.**  The no-progress guard also
  reports a safety-only model whose initial state enables no action,
  instead of a pass.  (An unmet `eventually` there already fails, on
  1.3.0 too.)

## Phase 4: protocol models

Each model is either made a faithful abstraction, with the reduction
argued in its header and checked against an independent TLA+
specification, or renamed and described as the arithmetic
demonstration it is.

- **4.1 paxos2.**  Handle a node's own prepare and accept locally (no
  self-RPC); bind its values 10 to 12 onto the spec's value domain; add a
  progress property that some node decides; add a negative test with two
  conflicting decisions that the binding must report.
  Status: done.  A node serves its own requests locally, its values are
  the spec's 1 and 2 (nodes 0 and 2 contend for 1, node 1 wants 2), the
  refine model requires that some node decides, and the `-DBUG` build
  breaks the spec's agreement.  The old paxos2.c fails the progress
  property.
- **4.2 Fast Paxos.**  Recovery reads an explicit classic quorum,
  promises close the fast round, and recovery with no candidate may
  choose a fresh value.  The experiment varies the quorum inequality
  that the paper states (`Qc + 2Qf > 2N` with pairwise intersection),
  not a recovery threshold.
- **4.3 Flexible Paxos.**  Ballots, phase 1 promises and
  highest-ballot value selection, with Q1 and Q2 as parameters.
- **4.4 CASPaxos.**  A mutable register with a change function and
  versions; the safety property is that confirmed states descend from
  earlier confirmed states.
- **4.5 ZAB.**  Epochs, leader election, synchronisation from a quorum
  and a committed prefix, or the model and its docs are narrowed to the
  guarded counter it is.
- **4.6 Single-decree Paxos.**  Acceptors vote in more than one ballot,
  and safety rests on phase 1 value selection, not on write-once votes.
- **4.7 Raft election.**  raft.c campaigns by blocking on each peer's
  reply while it is outside `accept()`, so two concurrent candidates
  deadlock.  Up to 1.3.0 the double-vote bug was reported only because
  node-pair channels handed a candidate its rival's RequestVote as a
  reply.  Serve RPCs while campaigning (a poll loop, or a thread), then
  restore the planted-bug row.
  Status: done.  A candidate waits for its replies in a `poll()` loop
  over its listener and its vote connections.  The bug is caught again
  by BFS and simulation, with a counterexample in which each vote is
  read on its own connection; the pass now reaches a leader.
- **4.8 Humming Consensus.**  hc.c has the same shape, and with
  connections modelled no participant ever adopts a projection, so its
  agreement pass is vacuous and its AP-mode bug unreachable.  Rework it
  to serve while it iterates, and add an `eventually` property that some
  participant adopts.
  Status: done.  An RPC serves the participant's own store in a `poll()`
  loop while it waits, read repair no longer ends the iteration (as
  ReadRepair and Adopt are separate actions in the TLA+ spec), and
  `hc.model` requires that some participant adopts.  Four timeouts is
  the fewest at which one does; agreement holds there (36,331 states),
  and the AP-mode bug is found by seeded simulation at twelve.  Both
  take about seven minutes, so they run in `make check-examples-slow`,
  on the calibration's triggers, not on every push.

## Phase 5: qualification

- Differential tests: the abstract checker against TLC on every `.cml`
  (verdict, distinct states, depth, and liveness); the process engine's
  syscall answers against Linux for the supported subset.
- Randomised tests of the fingerprint: worlds that differ in any field
  the future depends on must not merge.
- An independent review of the changed engines before the next release.
- The next release notes list, for each item above, its gate.

## Order

Phase 0 first (it costs little and stops the harm), then Phase 1 (each
item is local and testable without process mode), then 3.1, 3.2, 3.8
and 3.6 (the process-engine false passes), then the rest of Phase 3,
Phase 2 and Phase 4.  A release before Phase 0 to 3 are done carries a
prominent "not verification evidence" notice.
