---
title: "Qualification"
parent: Reference
nav_order: 8
permalink: /reference/qualification/
lede: >-
  What each engine answers exactly, what it approximates, and what it
  refuses, with the gate that checks each claim.
---

# Qualification

This page lists, for each of libcmc's three engines, every construct it
handles and how: **exact** (it answers as Linux or TLA+ would, and a
gate checks that), **approximate** (it answers a stated subset or
abstraction; the limit says which), or **refused** (the check ends with
exit 3 or INCOMPLETE rather than guess).  A construct not listed is not
qualified.

A row cites the gate that pins it.  `proc_gate` is
`test/proc_gate.sh`, `spec_gate` is `test/spec_gate.sh`, `emit_gate`
and `apalache_gate` are the TLA+ bridge gates, and `test_cmc` is the
in-process engine's unit suite.  The status as of the soundness
remediation is in [the plan]({{ '/reference/plan-soundness/' | relative_url }}).

**A pass is evidence for the instance checked, not a proof.**  Each
engine searches a bounded instance: the nodes, budgets and depth the
model gives.  None of this page makes a pass mean more than that.

## Process engine (`cmc --model-file`)

The engine runs each node as a real process under seccomp user
notification and answers the system calls below.  Every other system
call runs in the kernel, untouched.

### Sockets and connections

| construct | status | limit | gate |
|---|---|---|---|
| `socket`, `bind`, `listen`, `setsockopt` | exact enough | node i listens on 127.0.0.1:20000+i; options are accepted and ignored | proc_gate verdicts |
| `connect` to a live node | approximate | always succeeds and waits at the listener, even before the peer calls `listen()` | proc_gate verdicts |
| `connect` to a node that is down | exact | `ECONNREFUSED`; later reads return 0, writes `EPIPE` | proc_gate "a dead node refuses the connection" |
| `accept`, `accept4` | exact | which waiting dialler is a search choice; per dialler, oldest first; the sockaddr out-parameter is not filled | proc_gate verdicts |
| non-blocking `accept` with nothing waiting | exact | `EAGAIN` | proc_gate pollloop, epollloop |
| one queue per connection direction | exact | FIFO; two connections between one node pair never share bytes | proc_gate crossrpc, fifo |
| `read`, `recvfrom` | approximate | one read takes at most one write's bytes; a short read leaves the rest queued | proc_gate verdicts (the short read: code only, no fixture yet) |
| `write`, `sendto` | approximate | at most `[target] max_write` bytes (default 64, cap 4096) per call; a longer write is exit 1 | proc_gate bigmsg |
| `close` | exact | FIN, or RST when bytes were left unread | proc_gate rstwrite, rstignore |
| a reset connection | exact | one `ECONNRESET`, then reads 0 and writes `EPIPE` | proc_gate rstwrite, rstignore, reset |
| `SIGPIPE` on a write into a reset connection | exact for ignored and default | default kills the node; ignored leaves `EPIPE` | proc_gate rstwrite, rstignore |
| `SIGPIPE` with a handler, or blocked | refused | exit 3 | proc_gate "a SIGPIPE handler is refused" |
| a crashed peer | approximate | no FIN; a reader may notice (EOF) once drained, standing for a timeout | proc_gate durlog, restart |
| a buffer the node does not own | exact | `EFAULT` for fixed-size arguments; a partial transfer is exit 3 | proc_gate "a bad write buffer gets EFAULT" |

### Event loops

| construct | status | limit | gate |
|---|---|---|---|
| `poll`, `ppoll` with no or zero timeout | exact | readiness of a channel or listener is a search choice (WAKE), never automatic | proc_gate pollloop, noprogress |
| a finite `poll`, `ppoll`, `epoll_wait` timeout | refused | exit 3 | proc_gate "a finite poll timeout is refused" |
| `epoll_create`, `epoll_ctl`, `epoll_wait`, `epoll_pwait` | exact for level-triggered `EPOLLIN`, `EPOLLOUT` | interest list up to 16 fds | proc_gate epollloop |
| `EPOLLET`, `EPOLLONESHOT`, other epoll flags | refused | exit 3 | proc_gate "EPOLLET is refused" |
| `eventfd2` | exact without `EFD_SEMAPHORE` | `EFD_NONBLOCK` honoured | proc_gate pollloop |
| `EFD_SEMAPHORE` | refused | exit 3 | proc_gate "EFD_SEMAPHORE is refused" |
| `fcntl` `F_GETFL`/`F_SETFL` (`O_NONBLOCK`), `F_GETFD`/`F_SETFD` | exact | any other command is exit 1 | proc_gate pollloop, guarded |

### Time, randomness and memory

| construct | status | limit | gate |
|---|---|---|---|
| `clock_gettime`, `gettimeofday`, `time` | approximate | a logical clock that advances one second per fired timeout | proc_gate idle, clocks, lease |
| `getrandom` | approximate | a fixed pattern (zero bytes), not a search choice | proc_gate clocks |
| `malloc` and the aligned allocators | exact enough | a fixed-address arena (64 MiB by default); `free` is a no-op | proc_gate "the preload allocator checks sizes and copies exactly" |
| address layout | exact | nodes run without ASLR, so a replay lays memory out alike | proc_gate fingerprint |

### Threads

| construct | status | limit | gate |
|---|---|---|---|
| `clone`, `clone3` (threads), `futex` wait and wake, `exit` | approximate | one order of a node's threads is run, not every order | proc_gate twothread, twothread_join |
| a multi-threaded search that would pass | refused | INCOMPLETE ("thread schedules not explored"); a violation still counts | proc_gate twothread |
| a timed futex wait | refused | exit 3 | (code; no fixture yet) |
| a `FUTEX_WAKE` of a strict subset of several waiters | refused | exit 1 | (code; no fixture yet) |

### State, search and verdicts

| construct | status | limit | gate |
|---|---|---|---|
| the state hook (fd 3, fd 193) | exact | a record of up to 64 bytes; a longer or unfinished record is exit 3 | proc_gate longrec, partrec |
| deduplication | exact | the full world encoding is compared on a fingerprint match | proc_gate fingerprint |
| crash and restart | approximate | a crash keeps the fd-193 record; a restart hands it back on the first read of fd 193 | proc_gate durlog, restart |
| `eventually P` | exact for an exhaustive search | unmet after a full search is a violation; a bounded or simulated run is INCOMPLETE | proc_gate twothread reach |
| a run that never progressed | refused | INCOMPLETE without an `eventually` property | proc_gate noprogress |
| `--refine` (C1) and `--refine-steps` (C2) | exact for the bound variables | C2 needs every spec variable bound; checks init, gated actions, exact states | proc_gate mono, spec_gate C2 rows |

## Abstract checker (`cmc --spec`)

| construct | status | limit | gate |
|---|---|---|---|
| safety verdicts, distinct states, depth | exact | agrees with TLC on every `.cml` model | spec_gate, emit_gate, apalache_gate |
| `eventually P`, `A ~> B` under `fairness weak` or none | exact | TLA+ semantics with stuttering; checked against TLC on each probe | spec_gate liveness rows, emit_gate |
| liveness under `symmetry` | refused | exit 3 | (code; no fixture yet) |
| integers | exact | 64-bit; an overflowing literal or operation is exit 3; `/` and `mod` refuse negative operands | spec_gate overflow row |
| declared domains | exact | init and every successor must lie in each variable's domain | spec_gate boundpass, succbound |
| text that does not fit a buffer | refused | exit 3 | spec_gate truncate |
| a primed variable read, an init reading an unset variable, a variable assigned twice, a redeclared bound name | refused | exit 3 | spec_gate primed, init_order, duplicate, quantifier |
| memory safety | checked | ASan and UBSan over the spec gate | `make check-asan` |

## In-process engine (`cmc --model`, `libcmc(3)`)

| construct | status | limit | gate |
|---|---|---|---|
| the search on the HC calibration core | exact at the instances run | 22 rows against TLC: verdicts and distinct-state counts | `make check-calibration`, `check-hc-bugs` |
| determinism of a core | checked | `--det` runs every step twice | toy_gate |
| uninitialised reads in a core | checked | MemorySanitizer | `make check-msan` |
| crash, simulation, replay, the fair schedule | under remediation | see Phase 2 of the plan | test_cmc |

## Examples

| example | status |
|---|---|
| paxos | pass and planted bug, with and without faults (proc_gate) |
| raftlog, zab | pass and planted bug (proc_gate) |
| raft | pass only; the planted bug was reachable only through the old pair-keyed channels (plan 4.7) |
| hc | vacuous: no participant ever adopts (plan 4.8) |
| paxos2 | refines `paxos.cml` with a progress property; its planted bug breaks the spec's agreement (proc_gate) |
