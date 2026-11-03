# libcmc design

This describes the engine as built (`include/cmc.h`, `src/cmc.c`).  The
original design, written before the code, is
`research/01-survey-and-design.md` §4a.  The review that changed the
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
zeroed slots).  Calibration proves the HC core does.

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
`CONN_RESET` pending at each peer, and delivers `RESTART`.  Branching over
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
| `CMC_EVENTUALLY_PROP` | bounded liveness: from every reachable state, the deterministic fair schedule (no faults, no new clients; drain deliveries, flushes, wakes and resets, then fire each armed timer once per round) reaches the goal within `--live-steps` steps.  It fails if the schedule stops, a round-start world repeats, or the steps run out.  The counterexample is the shortest prefix plus the fair suffix. |

## Gates around the cores

| gate | catches |
|---|---|
| `--det` | non-determinism: every step runs twice from a copy, and both outputs and post-state are compared |
| `scripts/purity_gate.sh` | calls outside `mem*`; writable or TLS data (state kept outside the world); `rdtsc`/`rdrand`/`syscall`/`cpuid`; `asm`; writing `out->o[` directly |
| `scripts/purity_teeth.sh` | the purity gate itself: each probe in `test/probe/` must be rejected |
| `make check-msan` | uninitialised reads, which `--det` cannot see |
| planted bugs | `#if` sites in shipped core source; each must be caught with the expected property (`test/toy_gate.sh`, `calib/hc/hc_bug_gate.sh`) |
| calibration | the engine itself, against TLC (`calib/hc/calibrate.sh`) |

## Limits

`CMC_MAX_NODES` 8, `CMC_MSG_MAX` 32 bytes per message or record,
`CMC_OUT_MAX` 16 outputs per step, 2048 enabled actions per state, 16
timers and 8 wakes per node.  Going over any limit, or over a `--*-cap`,
is reported as an engine violation, never truncated.

## Roadmap

Ordered by what the first user (pg_chain's protocol cores) needs:

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
