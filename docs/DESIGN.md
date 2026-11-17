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
any call on a checker-owned fd, a write to fd 3 or fd 193, or a
`clone`.  When a thread reaches one, the checker carries it out (bytes
are queued, a futex waiter is woken, a state record is written) but
holds its answer, and the thread parks (`BK_DONE`).  The checker writes
a state-hook record into the node's hook pipe itself, from the thread's
memory, rather than letting the kernel run the write, so a state-hook
write has the same semantics as every other visible operation.  The
answer to a parked recv, accept,
poll or futex wait is held the same way when the search or another
thread supplies it.  At a quiescent point the action RUN(node, thread)
sends one held answer and lets that thread alone run to its next
visible operation, so at most one thread of a node runs at a time and a
replay is deterministic.  A call on any descriptor is visible, the
kernel's (a pipe or a file two threads share) as well as the checker's.
Other calls (the clock, `getrandom`, `mmap`) are answered at once; the
clock moves only on a TIMEOUT action, when every thread is parked.

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

The boundary is the system call: interleavings of plain memory accesses
between two visible operations of different threads are not explored.

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
| env header | remaining budgets (drops, dups, crashes); per node: clients left, armed timers (u16), pending-persist count, pending wakes (u8), pending connection resets (u8) |
| network | `net_cap` entries `{dst, src, len, body, count}` |
| persists | `persist_cap` FIFO entries per node, `{len, body}` |
| replies | the ghost client log, `reply_cap` entries in emission order |

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
| `FIRE(n, t)` | node n armed timer t (`SET_TIMER`, not yet cancelled or fired) |
| `FLUSH(n)` | node n has a pending persist; the oldest completes first |
| `WAKE(n, w)` | node n emitted `WAKE(w)` |
| `CLIENT(n)` | node n has client budget left (and `clients(cfg, n)` allows it) |
| `CRASH(n, j)` | crash budget left; j = 0..k of n's k pending persists reach disk first |
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
| calibration (bridge) | the abstract-model checker against TLC on the *same* model: `cmc --emit-tla FILE.cml` must parse in TLC and give the identical verdict as `cmc --spec`, for safety and for liveness (`test/emit_gate.sh`, `make check-emit-tla`; needs nix) |

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
5. A ghost history of client requests, not only replies.
6. Symmetry reduction (a `permute` callback; a run counts only if the same
   counterexamples appear with it on and off) and an on-disk BFS queue.
7. Parallel BFS over a sharded table.  After that, if search is still the
   bottleneck, expose a core as an LTSmin PINS module (`WHY.md`).
