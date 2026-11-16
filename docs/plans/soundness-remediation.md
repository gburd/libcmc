---
title: "Plan: soundness remediation"
parent: Reference
nav_order: 10
permalink: /reference/plan-soundness/
lede: >-
  The audit of 1.3.0 found false passes, gates that report success after a tool failed, and claims the evidence does not support.  This plan fixes them in order of risk.
---

# Plan: soundness remediation

Status: Phases 0 to 4 done, except the thread scheduler of 3.8 (the
INCOMPLETE rule stands in for it) and Phase 5's randomised fingerprint
tests.  Audited commit: `b2eab84` (1.3.0).  Branch: `pr/soundness`.

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
  Status: done for the flags and timeouts named here, and widened after
  the independent review: `accept4` flags, `socket` types other than a
  TCP stream, `recvfrom` and `sendto` flags, and the socket calls the
  model does not answer (`writev`, `recvmsg`, `shutdown`, `dup` and the
  rest; see docs/qualification.md) on a checker-owned fd are refused.
  `sendmsg` is matched in the filter on its fd, since the launch path
  itself sends the notify fd with it on a low fd.
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

  The design being built (`wip/threads-sched`):

  - **Visible operations.**  In a node with more than one thread (or a
    birth in flight), a thread's step ends at a visible operation: a
    `futex` call, any call on a checker-owned fd, a write to fd 3 or fd
    193, `clone`, `exit`.  Other calls (`mmap`, `rseq`, the clock) are
    answered at once, since they touch nothing another thread can see
    except through memory, and memory between two calls is not
    explored.
  - **Execute, then park.**  A visible operation is carried out when the
    thread reaches it (so its effect, such as waking a futex waiter or
    queueing bytes, is in the world at once), but its answer is held and
    the thread parks (BK_DONE).  A deferring call parks as before
    (recv, accept, poll, futex wait).  A call the kernel performs
    (CONTINUE: an fd-3 write, `clone`) is held before it runs instead.
  - **A search action.**  When every thread of the node is parked, the
    actions include RUN(node, thread) for each thread with a held answer
    and each woken futex waiter.  RUN sends the answer and lets that one
    thread run to its next visible operation.  At most one thread of a
    node runs at a time, so a replay is deterministic.
  - **Births.**  The main thread is seized with PTRACE_O_TRACECLONE, so
    a newborn starts in a ptrace stop and the parent stops at
    PTRACE_EVENT_CLONE.  The parent continues to its next visible
    operation; the newborn stays stopped until a RUN releases it.
    Reaping waits on every traced thread (`__WALL`).
  - **Encoding.**  A held answer is part of a thread's record (the call
    and its result), so worlds that differ in a pending result split.
  - **The boundary.**  Interleavings of memory accesses between two
    visible operations of different threads are not explored; cmc(7)
    states it.  With the scheduler complete, the INCOMPLETE rule is
    removed.
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
  Status: done.  `fastpaxos.cml` has a fast round 1 and a classic
  round 2.  A round 2 promise closes round 1 for its acceptor and
  reports its vote; the coordinator picks a value by Figure 2's rule
  over a classic quorum of promises, including O4 for a collision and a
  fresh value when the quorum reports no vote.  Quorums are sizes QC and
  QF.  With N = 3, QC = 2 and QF = 3 (2 + 6 > 6) it has 1,976 distinct
  states and depth 12 in cmc and in TLC; Apalache reports no error up to
  length 10.  The `BUG` sets the fast quorum to 2 before any acceptor
  acts, so 2 + 4 = 2N; all three engines find a fast-chosen value
  overridden, cmc and TLC in 11 steps.  Still abstracted: two rounds,
  one coordinator, no 1a messages, no recovery from 2b messages, no
  loss, and both values always proposed.  At N = 5 the same model has
  222,460 states, too many for the per-push gate.
- **4.3 Flexible Paxos.**  Ballots, phase 1 promises and
  highest-ballot value selection, with Q1 and Q2 as parameters.
  Status: done.  `fpaxos.cml` is the Paxos of 4.6 with the quorum split:
  Phase2a needs Q1 promises, a value is chosen by Q2 votes at one
  ballot.  With 4 acceptors, Q1 = 3 and Q2 = 2 (two phase 2 quorums can
  be disjoint) it has 36,756 distinct states and depth 20 in cmc and in
  TLC; Apalache reports no error up to length 10.  The `BUG` Phase2a
  runs on 2 promises, so Q1 + Q2 = A; all three engines find the
  violation, cmc and TLC in 12 steps.  The abstractions are those of
  4.6.  There is no hand-written TLA+ twin; TLC checks the emitted
  module.
- **4.4 CASPaxos.**  A mutable register with a change function and
  versions; the safety property is that confirmed states descend from
  earlier confirmed states.
  Status: done.  `caspaxos.cml` runs prepare, propose (increment the
  highest-ballot state a quorum of promises reports), accept and
  confirm per ballot, with a ghost parent per round; the invariant is
  that of two confirmed rounds the earlier is the later's parent.  It
  no longer extends `consensus`, since a register takes many values.
  With 3 acceptors and 2 rounds it has 3,280 distinct states and depth
  17 in cmc and in TLC; Apalache reports no error up to length 10.  The
  `BUG` accept ignores the acceptor's promise; all three engines find
  the fork, cmc and TLC in 12 steps.  Still abstracted: two rounds (so
  the ancestor check is one step), one change function, no 1a messages,
  no conflict replies, and no membership change or deletion.
- **4.5 ZAB.**  Epochs, leader election, synchronisation from a quorum
  and a committed prefix, or the model and its docs are narrowed to the
  guarded counter it is.
  Status: done, as a model of the protocol.  `zab.cml` has accepted and
  current epochs and a history per server, discovery (ACKEPOCH from a
  quorum, the leader taking the most up-to-date member's history),
  synchronisation (NEWLEADER, ACK-LD, a quorum establishes the epoch),
  broadcast and commit of a quorum-acknowledged prefix.  A history of
  at most two transactions is one integer.  The invariants are
  committed_prefix (of two epochs' committed prefixes one extends the
  other) and initial_history (a new leader's initial history extends
  every earlier committed prefix).  With 3 servers and 2 epochs it has
  27,913 distinct states and depth 26 in cmc and in TLC; Apalache
  reports no error for either invariant up to length 10.  The `BUG`
  discovery takes any quorum member's history; cmc and TLC find
  initial_history violated in 13 steps (committed_prefix in 20), and
  Apalache finds it at length 13 in about 17 minutes, the slowest row
  in the gate.  Still abstracted: no leader election messages (at most
  one leader per epoch, by construction), no crashes or restarts, no
  loss, synchronisation as one step, and no delivery to the
  application.
- **4.6 Single-decree Paxos.**  Acceptors vote in more than one ballot,
  and safety rests on phase 1 value selection, not on write-once votes.
  Status: done.  `paxos.cml` has Paxos.tla's maxBal, maxVBal and maxVal
  per acceptor, the 1b, 2a and 2b messages as function variables, and
  Phase1b, Phase2a (the highest-ballot vote among a quorum's promises,
  or any value if none) and Phase2b.  With 3 acceptors, 2 ballots and 2
  values it has 6,144 distinct states and depth 16 in cmc and in TLC,
  both on the emitted module and on a hand-written twin (`paxos.tla`)
  that quantifies over quorums and keeps a message set as Paxos.tla
  does.  Apalache reports no error up to length 10.  The `BUG` 2a
  ignores the votes the promises report; all three engines find the
  violation, cmc and TLC in 12 steps.  Still abstracted: no 1a messages
  (a promise may name any higher ballot), no loss or duplication, two
  ballots, and an explicit learn action.  The process engine's refine
  rows for paxos.c and paxos2.c still bind only `chosen`, and pass.
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

Status: in part.  The abstract checker's differential test against TLC
is the emit gate, which now covers every `.cml` model and the five
liveness probes, and runs in CI before a tag.  The process engine's
differential test against Linux covers close and reset
(`test/proc/tcpsem.c`, run natively and under cmc by proc_gate).
`test/proc/fingerprint.c` checks named pairs of worlds, and
`test/proc/fpfuzz.c` random ones: 3,000 seeded worlds, each with 22
kinds of mutation that must change the encoding and three changes
(thread order and tids, bag slots, connection slots) that must not.
Dropping any of three encoded fields, or leaking a tid, fails it.  An independent
review of the engine changes at `21fd57d` found six process-engine
false passes and a hash-only seen set in the abstract checker; each has
a fix and a red test (`f9e5370`, `eebaaa9`, `ce59d01`, `1c3ae30`).  The
review covered Phases 1 to 3, not the Phase 4 models, which rest on
three-engine agreement (cmc, TLC, Apalache) and, for paxos, a separately
written TLA+ twin.  The CHANGELOG's Unreleased section lists the
changes; it does not yet name each item's gate.

## Order

Phase 0 first (it costs little and stops the harm), then Phase 1 (each
item is local and testable without process mode), then 3.1, 3.2, 3.8
and 3.6 (the process-engine false passes), then the rest of Phase 3,
Phase 2 and Phase 4.  A release before Phase 0 to 3 are done carries a
prominent "not verification evidence" notice.
