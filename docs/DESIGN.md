---
title: "Design"
parent: Reference
nav_order: 1
permalink: /reference/design/
lede: >-
  The engine as built: the world encoding, where actions come from, durability, search, properties and gates.
---

This describes the engine as built.  It has two front ends.

- **The process front end** (`src/cmc_proc.c`, `cmc --model-file`) is
  how a product is checked: each node is the shipped program, run as a
  process under seccomp user notification, and the checker is the
  network, the clock and the crash between them.  It is described in
  `cmc(7)` and `docs/model-file.md`; its milestones, measurements and
  the probes that pin its verdicts are in
  `experiments/process-mode/DESIGN.md` and `test/proc/`.
- **In-process cores** (`include/cmc.h`, `src/cmc.c`, `cmc --model`) are
  pure C step functions.  They are the engine's calibration path against
  TLC.  The rest of this page describes them.

The two share the CLI, the exit statuses, TLC's output wording and the
rules in `docs/steering/project.md`, but not yet a search loop: the
in-process engine stores worlds and expands them, while a process cannot
be restored from bytes, so the process front end stores action
sequences and replays them from a fresh launch.  Joining them needs a
restorable process (fork from a snapshot); the mechanic is proven
(`experiments/fork-snapshot/chain.c`) and a `--fast` snapshot search is
in progress, with breadth-first replay the sound default.

## How the checker drives an unmodified binary

The process front end needs to *be* the kernel for the program under
check: hand it the bytes of a message when the checker schedules one,
make a clock read return logical time, decide when a syscall blocks.
It does this with **seccomp user notification** (`SECCOMP_FILTER_FLAG_
NEW_LISTENER`, Linux >= 5.0), not eBPF.  The target installs a seccomp
filter that, for the syscalls the checker owns (`socket`, `bind`,
`accept`, `sendto`/`recvfrom`, `clock_gettime`, ...), **suspends** the
target and hands the checker a notification on a listener descriptor.
The checker reads the syscall and its arguments, reads or writes the
target's memory with `process_vm_readv`/`process_vm_writev`, and writes
back the result the target sees.  `ptrace` (`PTRACE_SEIZE`) is used to
read registers and, for the snapshot search, to freeze and fork a node.

A node may run more than one thread.  A thread's notifications arrive on
the same listener (a cloned thread shares the filter), tagged with the
thread's kernel id, so the checker keeps a deferred slot per thread
rather than one per node.  It intercepts `clone`/`clone3` (to know a
thread exists before it blocks) and `futex`: a thread parked in `futex`
(a mutex, a condition variable, or a `pthread_join` on another thread's
exit) is blocked, exactly like one parked in `recv`, `poll` or
`epoll_wait`.  The
world is a search state only when every thread of every node is blocked
or has exited.  A `FUTEX_WAKE` wakes waiters only when the program
issues one, as a step of the waking thread; the checker invents no
wakes.  A single-threaded node reduces to exactly one slot, the
pre-thread behaviour.

In a node with more than one thread, the order of the threads' visible
operations is a search choice.  A visible operation is a `futex` call,
any call on a checker-owned fd (or one that makes one), a write to fd 3
or fd 193 or a read of fd 193, a lock acquisition or an atomic (both
through a marker, below), a `clone`, or a thread's exit, and nothing
else is a choice point.  When a thread reaches one, the checker
carries it out (bytes are queued, a futex waiter is woken, a state
record is written) but holds its answer, and the thread parks
(`BK_DONE`).  The checker writes
a state-hook record into the node's hook pipe itself, from the thread's
memory, rather than letting the kernel run the write, so a state-hook
write has the same semantics as every other visible operation.  The
answer to a parked recv, accept, poll or futex wait is held the same
way when the search or another thread supplies it.  At a quiescent
point the action RUN(node, thread) sends one held answer and lets that
thread alone run to its next visible operation, so at most one thread
of a node runs at a time and a replay is deterministic.  In such a node
a call on a kernel fd (a pipe or a file the threads may share) other
than a write to stdout or stderr is refused before it runs, since its
contents and offset are kernel state the world does not hold.  Other
calls (the clock, `getrandom`, `mmap`, a write to stdout or stderr) are
answered at once; the clock moves only on a TIMEOUT action, when every
thread is parked.  That includes `lseek`, `fsync` and the other calls
on an open file, which the filter intercepts.

The file system is read-only to a node.  Its durable state goes through
fd 193, which the world holds; a file it wrote would survive a crash
and every later replay outside the world.  The filter intercepts every
open (`open`, `openat`, `openat2`, `creat`, `open_by_handle_at`): a
read-only one continues to the kernel, and one that could change a file
ends the check with the path, which `fs_path()` reads from the node a
page at a time.  The calls that change the namespace or metadata, and
`io_uring_setup` (its operations reach the kernel with no system call),
are refused.  `lseek`, `fsync` and the rest answer a checker-owned fd as
Linux answers a socket.  The filter grew past a hundred tests; each
`OWN()` is a self-contained equality test with jump offsets 0 and 1, so
its length never reaches classic BPF's 8-bit jump limit, and proc_gate
has a row per refused call to show each one routes.

`sendmsg` on a kernel fd needs a second filter.  The launch child itself
sends the listener to the checker with `sendmsg` on a socketpair, after
the first filter is in and before anyone holds the listener to answer
a notification, so the first filter notifies `sendmsg` only at fd 500
and above.  The checker seizes the child with `PTRACE_O_TRACESECCOMP`
before it installs any filter, and the child waits for a byte until it
has.  It then installs the first filter and a second one that returns
`SECCOMP_RET_TRACE` for `sendmsg` below fd 500.  The kernel takes the
most severe answer of all the filters, and `USER_NOTIF` ranks above
`TRACE`, so an owned fd is still notified.  The handoff `sendmsg` is the
first trace stop, which the checker waits for at launch (so a node whose
second filter is missing never starts), and every later one is a
`PTRACE_EVENT_SECCOMP` stop that `reap()` finds between waits: a node
with one thread is resumed (`PTRACE_CONT`, the call reaches the kernel),
and a node with more is refused, as for every other kernel fd.

Nodes run one at a time.  `drive_node()` serves one node's
notifications until every thread of it is parked or it has exited, or
until one of its calls wakes a node that was parked (a close whose RST
answers a peer's recv); every other node meanwhile sits in a
notification nobody has read.  When exactly one node can run,
`settle()` drives it with no choice, which is the case after every
DELIVER, ACCEPT or RUN.  When two or more can (at launch, after a
RESTART, after such a wake), the search chooses with STEP(node), so a
call whose answer reads another node's state (a connect to a node that
may have exited, a write to a peer that may have closed) is answered
in each order.  2.0.0 served all running nodes at once, and the answer
depended on the kernel's scheduling: the same action sequence reached
different worlds on different runs.  Worlds between STEPs are not
judged by the invariants, not counted, and not part of the depth; they
are merged in the seen set only while every running node is fresh
(launched and not yet served a call), and searched as a tree once a
woken node has run partway, since its record does not hold the answer
it ran on.

A birth is held the same way.  Nodes are seized with
`PTRACE_O_TRACECLONE`, so a `clone` stops the parent at
`PTRACE_EVENT_CLONE` and starts the newborn in a ptrace stop.  Both stay
stopped (`BK_CLONED`, `BK_NEWBORN`) until a RUN releases each with
`PTRACE_CONT`, so the newborn's first visible operation can come before
or after the parent's next one.  Every traced thread is reaped by its
own tid (`waitpid(tid, __WALL)`): an exited thread at once, and a killed
node's threads from `/proc/PID/task` before the leader, whose exit the
kernel withholds until they are reaped.

A thread's record in the world encoding holds its held answer (the
result and every byte the call wrote into its memory) and, in a
multi-threaded node, the call
it is parked in, with its arguments and instruction and stack pointers,
since two threads can hold the same answer at different points of their
code.  Records are sorted, so the key is the multiset of thread states,
not which tid holds which.

The encoding does not hold a thread's memory, and two points of a run
can share a record: two iterations of a loop park at the same call, and
glibc gives a new thread the stack of one already joined.  So a world in
which a live node has run more than one thread is never merged: the
search neither looks it up in the seen set nor adds it, and follows
every order of the threads' visible operations as a tree.  The flag is
per process, so it clears when the node dies or restarts.  The tree is
bounded by `--max-states`, or 1,000,000 such worlds by default, and a
search that reaches the bound is INCOMPLETE.  Worlds whose nodes have
each run one thread are still deduplicated.

Memory interleavings are covered by reduction, not enumeration.  The
preload wraps every acquiring synchronisation call (a lock, a semaphore
wait, a barrier, a once; pthread and C11) and makes a marker call, a
`sched_yield` with a magic first argument, before the real one; the
checker answers it and, in a multi-threaded node, treats it as a visible
operation.  Releases get no marker.  In a program free of data races
every schedule is equivalent to one that switches threads only before
acquisitions (a release moves left past race-free accesses, an
acquisition right), so these points and the system calls are enough,
for acquisitions that wait until they succeed.  A try or timed
acquisition that fails does not commute with the release that would
have let it succeed, and a semaphore with count left over admits a
second holder inside the first one's section; the runtime tells the
checker when either runs between threads (`RACE_TRY`), and the pass
becomes INCOMPLETE.  Marking every release instead was measured: the
twothread tree goes past 600 s from 235 s, raceemit from 2,437 worlds to
10,228 (docs/research/04-review-race-runtime.md).

Freedom from data races is what the race runtime (`src/cmc_race.c`,
`libcmc-race.a`) checks.  A node built with `-fsanitize=thread` and
linked against it instead of the sanitizer's runtime calls into it on
every instrumented access and atomic.  It keeps vector clocks (per
thread, per synchronisation object, edges from create and join, the
wrapped calls and atomics by memory order; a rwlock keeps a writers'
clock and a readers' clock, a barrier one clock per round by parity,
and a condition signal adds no edge) and a per-byte record of the
last write and the reads since, in tables mapped at fixed addresses so
the node's own layout does not move.  An unordered conflicting pair is
reported through a marker and is a violation.  Atomics are markers too,
so their order is searched.  The checker turns a would-be pass into
INCOMPLETE when a node ran threads without the runtime (it announces
itself at `__tsan_init`) or an atomic weaker than `seq_cst` ran, since
the search is sequentially consistent.  The runtime's wrappers come
first in symbol lookup (they are in the executable); each calls the
next definition with `dlsym(RTLD_NEXT)`, which is the preload's, which
makes the marker and calls the C library.

The seccomp *filter* is classic BPF -- the bytecode `seccomp(2)` has
always taken -- but there is no eBPF program, no `bpf(2)`, no kernel
probe.  Observation tools built on eBPF uprobes/kprobes can watch
syscalls but cannot cleanly substitute a result and hold the process at
the boundary; seccomp user notification is the primitive that lets the
checker stand in for the network, the clock and the disk of an
otherwise unmodified, as-shipped binary.

The original design of the in-process engine, written before the code,
is `research/01-survey-and-design.md` §4a.  The review that changed the
actor mode is `research/02-review-actor-mode.md`.

## The model

A **model** is a core plus properties.  A **core** is a `CmcCore`
vtable over a fixed-size, pointer-free, per-node state.  The engine
treats that state as opaque bytes.

A **world** is the concatenation of:

| part | contents |
|---|---|
| node states | `nnodes` x `state_size` bytes; the first `durable_size` of each are durable |
| env header | remaining budgets (drops, dups, crashes); per node: clients left, armed-timer count, pending-wake count, pending-persist count, pending connection resets (u8); then the reference time and each node's clock (u64) |
| network | `net_cap` entries `{dst, src, len, body, count}` |
| persists | `persist_cap` FIFO entries per node, `{len, body}` |
| replies | the ghost client log, `reply_cap` entries in emission order |
| timers | `timer_cap` entries per node, `{id, arg2, deadline}`, sorted by `(id, arg2)`, one per pair; deadline 0 = untimed |
| wakes | `wake_cap` entries per node, `{id, arg2}`, sorted, one per pair |
| requests | `CmcModel.requests` only: one slot per (node taking clients, request number), `{answered, answer, precedes}`; the precedence bits say which requests were answered before this one was invoked |
| history | `CmcModel.history_size` bytes, updated by `CmcModel.history()` after each step |

Every part is canonical: sorted where order carries no meaning, with
unused entries zeroed.  The world is zero-padded to 8 bytes.  The bytes
are the encoding, so two worlds are the same state exactly when their
bytes are equal.  A core keeps its own state canonical (sorted sets,
zeroed slots).  Calibration checks that the HC core does, at the
instances it runs.

**Direct mode** is for calibration only.  A core that ports a
shared-register TLA+ spec sets `direct_actions`.  The world is then one
state blob, and the actions are whatever that function returns: the TLA+
`Next` disjuncts.  Actor-mode cores never do this.

## Actions come only from emitted effects

This is the rule the engine exists for (libxtc's simulator broke it and
hid 8 lost-wake bugs).  In actor mode, `cmc_enabled()` reads only the
environment part of the world:

| action | enabled when |
|---|---|
| `DELIVER` | a message is in the network (FIFO: only a channel's head) |
| `DROP` | drop budget left (FIFO: resets the connection, and both ends get `CONN_RESET`) |
| `DUP` | dup budget left; bag mode only |
| `FIRE(n, t, a, c)` | node n armed timer t with argument a (`SET_TIMER`, not yet cancelled or fired); the action carries both, so the same id with two arguments is two actions.  A timed timer also needs its deadline to be n's earliest, and c names the drift variant (see Time) |
| `FLUSH(n)` | node n has a pending persist; the oldest completes first |
| `WAKE(n, w, a)` | node n emitted `WAKE(w)` with argument a |
| `CLIENT(n)` | node n has client budget left (and `clients(cfg, n)` allows it) |
| `CRASH(n, j)` | crash budget left (and `crashable(cfg, n)` allows it); j = 0..k of n's k pending persists reach disk first |
| `RESET(n, p)` | a crash or drop left node n a pending `CONN_RESET` from peer p |

Applying an action makes one `step()` call on one node with the matching
event.  The engine then processes that step's outputs: it queues sends,
arms or cancels timers, queues persists and wakes, and appends replies.

## Durability

The engine owns the durable bytes.  `FLUSH` calls
`apply_persist(cfg, node, durable, record)` and then delivers `FLUSHED`
to `step()`.  `step()` must leave the durable bytes unchanged on every
event, and the engine compares them before and after.

`CRASH(n, j)` applies the first j pending records, discards their
outputs (the process is dead), zeroes the volatile bytes and timers,
wakes and pending persists, empties every channel to and from n, marks
`CONN_RESET` pending at each peer, and delivers `RESTART`.  In bag mode it
drops only the messages n sent itself, which never left the process, and
marks no resets.  Branching over
j covers a write that reached disk before its completion was observed.

## Time

The environment holds a reference time and a clock per node, all u64
in the core's own unit and 0 at the start.  Every event carries the
node's clock in `CmcEvent.now`, so `step()` stays pure; properties read
`cmc_clock(w, node)` and `cmc_now(w)`.

`SET_TIMER` with `dur` 0 arms an untimed timer, which fires at any
point, as before 2.4.  With `dur` > 0 the deadline is the node's clock
plus `dur`, and the timer fires only when it is the node's earliest
timed deadline (ties fire in either order).  Firing moves that node's
clock by D to the deadline, and the reference time by D.  The other
clocks move by D too, or, with `--drift R` (per thousand, up to 500),
one other node j's clock moves at the slowest (D(1000 - R)/1000, rounded
up) or fastest (1000D/(1000 - R)) rate the drift allows: the variants c
= 1 + 2j and c = 2 + 2j of the `FIRE` action.  A fire whose clock moves
would carry another node past its own earliest deadline is not offered,
since that deadline comes first.  These are process mode's semantics
(`drift_advance()` and `expire_ok()` in `src/cmc_proc.c`): rates in
between, and two clocks off the common rate at once, are not explored.

Only timed fires move time.  `DELIVER`, `CLIENT`, `FLUSH`, `WAKE`,
`RESET` and `CRASH` take no time, as in process mode, so a message can
arrive no later than the next deadline, and a core cannot measure how
long a delivery took.  Wakes take no duration.

Clocks only grow, so a model that arms timers with durations has
infinitely many states unless something else ends it.  `--max-time T`
is the horizon: a world whose reference time passed T is not expanded,
and a run that left one unexpanded ends INCOMPLETE (exit 2), never
PASS.  Without `--max-time`, such a model must stop arming timers on its
own (the lease core in `test/test_cmc.c` does) or the run ends at
`--max-states`.  Declared time fields (plan item 2b), which store times
relative to the reference clock, are what will remove the need for a
horizon.

## Network

- **FIFO** (default): one ordered channel per (src, dst), as with TCP or
  libpq.  It never duplicates.
- **Bag** (`--net bag`): an unordered multiset with optional duplication,
  for transports with no ordering.  For safety it is a superset of FIFO,
  so a bag counterexample may be spurious for a protocol that relies on
  FIFO.

## Search

| mode | semantics |
|---|---|
| BFS (default) | TLC semantics: shortest counterexample, the depth counts the initial state as 1, and exact distinct and generated counts |
| DFS `--dfs D` | depth-bounded; a witness not reached is a failure |
| simulate | seeded random walks; `--seed S --walks 1` reproduces a failing walk |
| replay | runs a replay file through the real cores; model, cfg and budgets come from the file, and a conflict is refused |

The visited set is an open-addressing table of 128-bit fingerprints (two
independent 64-bit mixes) plus each state's parent fingerprint and
action, so traces are rebuilt from parents.  The collision estimate
n²/2¹²⁹ is printed, as TLC does.

### Symmetry reduction

A core may declare interchangeable nodes: `CmcCore.symmetry(cfg, cls,
max)` gives each node a class (0 = fixed), and the group is every
permutation that maps each class onto itself.  `permute(cfg, perm,
node, state)` rewrites the node ids inside one node's state and
`permute_msg(cfg, perm, body, len)` those inside a message or persist
body; the engine moves everything it owns itself (node order, budgets,
clocks, timers, wakes, persists, connection resets, the network renamed
and sorted again, replies, the request log), and the model's
`permute_history()` moves a history.  With `--symmetry on`, BFS and DFS
key the visited table by the fingerprint of the world's canonical form,
the smallest image under the group in memcmp order, found by trying
every permutation (at most 8! = 40320, since a run has at most 8 nodes).
The queue keeps the actual world, and a table entry the action that
reached the actual world first queued for its orbit, so the parent chain
replays from the initial world: a counterexample is a real execution
and no replay matching is needed.  Counts are of orbits.

Soundness rests on two obligations the engine cannot prove: the core
commutes with the permutations (`step(perm(s), perm(e)) = perm(step(s,
e))`), and every property gives the same verdict on a world and on its
images.  `--sym-check` tests both on every expanded world, for every
permutation: each property on the world and its image, and the
successors' images against the image's successors, one for one, with
each action property agreeing on the paired steps.  A wrong `permute()`
or an asymmetric property is an engine violation.  Refused outright:
interchangeable nodes that `clients()` or `crashable()` tells apart,
`CMC_EVENTUALLY` (the fair schedule runs in node order), and a history
without `permute_history()`.  Node ids in a reply's `arg` or a timer's
`arg2` are not rewritten; `--sym-check` catches a core that puts them
there.

## Properties

| kind | checked |
|---|---|
| `CMC_INVARIANT` | every reachable state |
| `CMC_ACTION_PROPERTY` | every transition, including transitions to already-seen states |
| `CMC_WITNESS_PROP` | must hold in some reachable state (a vacuity check) |
| `CMC_QUIESCENT_PROP` | every state with no enabled action, or whose only actions are timer fires that return the same world |
| `CMC_EVENTUALLY_PROP` | bounded liveness: from every reachable state, the deterministic fair schedule (no faults, no new clients; drain deliveries, flushes, wakes and resets, then fire each armed timer once per round) reaches the goal within `--live-steps` steps.  A drain runs in generations, each taking once what was enabled at its start, so no channel waits behind another; a drain that returns to a generation-start world ends there, so cores that keep messaging each other in a cycle cannot postpone an armed timer.  A drain that does not come back to a world within `--live-steps` steps (a counter in the messages, say) is reported as not reaching the goal, never as reaching it.  The verdict is about this one schedule, not every fair one.  It fails if the schedule stops, a round-start world repeats, or the steps run out.  The counterexample is the shortest prefix plus the fair suffix. |

## Gates around the cores

| gate | catches |
|---|---|
| `--det` | non-determinism: every step runs twice from a copy, and both outputs and post-state are compared |
| `scripts/purity_gate.sh` | calls outside `mem*`; writable or TLS data (state kept outside the world); `rdtsc`/`rdrand`/`syscall`/`cpuid`; `asm`; writing `out->o[` directly |
| `scripts/purity_teeth.sh` | the purity gate itself: each probe in `test/probe/` must be rejected |
| `make check-msan` | uninitialised reads, which `--det` cannot see |
| `make check-asan` | out-of-bounds access, leaks and undefined behaviour in the abstract checker, over every `.cml` model and soundness probe |
| planted bugs | `#if` sites in shipped core source; each must be caught with the expected property (`test/toy_gate.sh`, `calib/hc/hc_bug_gate.sh`) |
| calibration (engine) | the search itself, against TLC: the step-function HC core vs a hand-written `HummingConsensus.tla`, row by row (`calib/hc/calibrate.sh`) |
| calibration (bridge) | the abstract-model checker against TLC on the *same* model: `cmc --emit-tla FILE.cml` with the `.cfg` from `cmc --emit-cfg` must parse in TLC and give the identical verdict, distinct states and depth as `cmc --spec`, for safety, deadlock, constraints and liveness (`test/emit_gate.sh`, `make check-emit-tla`; needs nix) |

There are two calibration tracks because there are two things to pin.
The **engine** track (`calib/hc`) checks that the in-process
step-function search (`cmc --model`, `src/cmc.c`, the documented
`libcmc(3)` path) agrees with TLC, row by row, at the instances it runs.  It leans on a hand-written `HummingConsensus.tla` kept
beside a hand-written HC core -- a duplicate that can drift -- but it
calibrates a rich model: several HC variants (paper and code, CP and AP
mode) against many named properties (`S1_Agreement`,
`S2r_NoStaleRead`, `S3_NoLostAck`, `S3a_NoLostAckActive`,
`W_AckAfterReconfig`), with exact distinct-state counts into the tens of
millions.  The **bridge** track checks that the abstract checker (`cmc
--spec`) agrees with TLC on each model, with no hand-kept twin: the
same `.cml` is both checked by `cmc --spec` and emitted to TLC by `cmc
--emit-tla` (and to Apalache by the same bridge).

Milestone D envisaged retiring the engine track in favour of the bridge.
That has not happened, and the plan was over-optimistic about it.
`examples/spec/hc.cml` is a minimal single-property HC model (write-once Agreement at three
participants, 41 states); it does **not** reproduce the `calib/hc` rows,
which pin many properties and variants at up to tens of millions of
states.  A full-fidelity HC `.cml` that reproduced those rows through
the bridge would be a large undertaking.  More to the point, the
in-process engine is a shipped, documented path in its own right, not a
scaffold to be discarded, so its real-protocol calibration has standing
value.  `calib/hc` therefore stays: retiring it would delete the only
evidence that the in-process search agrees with TLC on a real consensus
protocol, and `hc.cml` does not replace it.  Neither track says
anything about the process engine (`cmc --model-file`), which has its
own search loop and is checked only by `test/proc_gate.sh`.

## Limits

`CMC_MAX_NODES` 8, `CMC_MSG_MAX` 32 bytes per message or record,
`CMC_OUT_MAX` 16 outputs per step, 2048 enabled actions per state, 16
timers and 8 wakes per node.  Going over any limit, or over a `--*-cap`,
is reported as an engine violation, never truncated.

## Roadmap

The north star past 0.3.0 is
[TLA+ scope over the shipped code]({{ '/reference/plan-tlaplus-scope/' | relative_url }}):
an abstract model plus a binding to the production binary, checked by
refinement, so one model validates many implementations and a category
of protocols shares a common base.  The milestones below are the
tactical steps the engine needs regardless of that framing.

For the process front end, next: fork from a snapshot instead of
replaying from launch (the speed that larger models need), then the
consensus examples -- Paxos, Raft, Humming Consensus, ZAB -- as programs
written as they ship (`experiments/process-mode/DESIGN.md`).

For in-process cores, ordered by what replication and consensus cores
need first:

1. Clocks: per-node monotonic time with drift bounded by ρ, a `TICK`
   action and a time-carrying `CLOCK` event (needed for leases).
2. Partitions: a `LINKS` action over link sets, including asymmetric ones.
3. An `APPLY` completion like `FLUSH`, lost on crash.
4. Several cores per node, sharing a crash.
5. A ghost history of client requests, not only replies (2.4: the
   request log and history variables).
6. Symmetry reduction (2.4: `CmcCore.symmetry`, `--symmetry`,
   `--sym-check`) and an on-disk BFS queue.
7. Parallel BFS over a sharded table.  After that, if search is still the
   bottleneck, expose a core as an LTSmin PINS module (`WHY.md`).
