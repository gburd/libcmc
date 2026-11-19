# Changelog

libcmc follows semantic versioning.  Before 1.0.0 a minor release may
change the API in `include/cmc.h`; this file says when it does.

## Unreleased

A crash can fall between any two system calls of a single-threaded node
that reach outside the process, not only between its steps.  While
crash budget is left, a node stops after each write or send on a
checker-owned fd, each close of one, and each durable write, and the
search may crash it there.  Before, a node that acked a client and then
wrote its durable record, with no blocking call between, passed.  This
changes verdicts: a fixture whose backup acks from memory now fails
(guarded with `--crashes 1`), and state counts with crashes grow (the
guide's server: 15 to 26 distinct states).  No invariant is checked at
such a mid-step point, since a program reports its state after its
effects.  Gates: proc_gate rows durack-single and ackfirst, the guarded
row, and the guide's trace in docs_gate.

The race runtime provides the 16-byte atomics (`__tsan_atomic128_*`)
that gcc and clang emit for `__int128` atomics under `-mcx16`.  A
program that used them failed to link against `libcmc-race.a`.  Gate:
proc_gate rows atomic128 (a seq_cst handoff holds, a relaxed one is a
race, fetch_add and compare-exchange compute) and race_rt.

A thread that ends by `pthread_exit` or `thrd_exit` gives its join the
same happens-before edge as one that returns from its start function.
The race runtime took the joined thread's clock only on a return, so
the joiner's reads of what such a thread wrote were reported as data
races; and glibc's first `pthread_exit` loads libgcc_s, a read of a
kernel fd the checker refused in a multi-threaded node.  The runtime
now loads libgcc_s at `__tsan_init` and the join takes the thread's
final clock.  Gate: proc_gate rows threadexit (exit, thrd, nested, and
a read before the join that is still a race).

## 2.1.0 (2026-11-19)

Closes the four gaps 2.0.0 documented in the process engine (a
`sendmsg` on a kernel fd, the file system, memory interleavings between
threads, and the untested tree bound) and adds a coverage gate holding
function and branch coverage of every source file at 90% or more.
Writing the tests found more than twenty other places where cmc
checked less than it was asked or answered unlike Linux; each is fixed
with a test that fails on 2.0.0.  As in 2.0.0, results can change for
the same input: a multi-threaded node built without the race runtime,
which could pass, is now INCOMPLETE, and several constructs that ran
unseen are refused.

Process engine (`cmc --model-file`):

- Every lock acquisition is a choice point between threads, including
  one taken on its uncontended path with no system call: the preload
  wraps the pthread mutex, rwlock, spinlock, semaphore, barrier and once
  calls and C11 `mtx_lock`, `mtx_trylock`, `mtx_timedlock` and
  `call_once`.  On 2.0.0 a thread switch there was never explored.
  [proc_gate: syncpoint, pthread and C11]
- A race runtime, `libcmc-race.a`: a node compiled with
  `-fsanitize=thread` (gcc or clang) and linked against it in place of
  the sanitizer's runtime has every instrumented access and atomic
  checked against happens-before.  A data race is a violation, printed
  with both code addresses and the schedule.  Atomics are choice points.
  [proc_gate: memrace unlocked and relaxed, raceemit]
- A search in which a node ran more than one thread without the race
  runtime cannot pass: it ends INCOMPLETE, and so does an unmet
  `eventually`, since memory interleavings between choice points were
  not checked.  An atomic weaker than `seq_cst` does the same, since the
  search runs a sequentially consistent machine.  On 2.0.0 such a node
  could pass with a lost update reachable.  [proc_gate: memrace
  unlocked, relaxedadd; twothread]
- `sendmsg` on a kernel fd (below 500) is seen.  A second seccomp
  filter stops it for the checker, which sends it to the kernel in a
  node with one thread and refuses it (exit 3) in a node with more, like
  every other call on a kernel fd.  On 2.0.0 the launch child's own
  `sendmsg` of the listener kept the filter from matching it, so a
  worker could send on a socket another thread read, unseen.  The
  launch socketpair is now close-on-exec.  [proc_gate: sendmsg worker,
  sendmsg alone]
- Which node runs first is a search choice.  The engine runs one node
  at a time, until it parks, exits or wakes a parked node, and when two
  or more can run (at launch, after a restart, after such a wake) it
  offers `step nN` for each.  On 2.0.0 every running node was served at
  once, so a call whose answer depends on another node's progress (a
  `connect` to a node that may already have exited) got whichever answer
  the kernel's scheduling gave: a model could give different verdicts
  on different runs, and one outcome could never be seen.  STEPs are not
  checked, counted, or part of the depth; a counterexample lists them
  unnumbered.  Distinct-state counts and depths of every model whose
  nodes run one thread are unchanged; "states generated" grows by the
  worlds the STEPs reach (paxos 964 to 983).  Two-node models with a
  threaded node double (twothread 515 to 1,028, twothread_join 275 to
  549, raceborn 278 to 556), since the start order of the nodes is now
  part of each threaded tree.
  [proc_gate: dialexit, which needs both outcomes; badptr and dialexit
  each run 300 times and must give one verdict and count]
- The file system is read-only to a node.  Every open is intercepted:
  a read-only one goes to the kernel, and one with write access,
  `O_CREAT`, `O_TRUNC`, `O_APPEND` or `O_TMPFILE` ends the check (exit
  3) naming the path, except `/dev/null`.  The calls that change the
  namespace or metadata are refused, as are `ftruncate`, `fallocate`,
  `flock` and `io_uring_setup`.  On 2.0.0 a node that kept its state in
  a file passed a crash test on bytes that outlived the crash outside
  the world, and a worker's `lseek` on a shared fd was not seen.
  `lseek`, `fsync`, `fdatasync`, `ftruncate` and `sync_file_range` on a
  checker-owned fd get Linux's answers for a socket, and `fsync` of fd
  193 succeeds.  A node built with `--coverage` is refused: it writes
  its counters to files.  [proc_gate: files persist, lseek, config,
  devnull, badpath, durfsync, sockerr (against a native run), and one
  row for each refused call]
- The default bound on a threaded program's tree search (1,000,000
  worlds) can be set at build time with `-DCMC_TREE_CAP=n`, so a gate
  reaches it: the search stops there INCOMPLETE (exit 2) and
  `--max-states` still takes precedence.  On 2.0.0 no gate reached the
  default.  One run to the real default is recorded in
  `docs/measurements/2026-11-17-tree-cap.md`.  [proc_gate: tree bound
  rows; docs_gate: the default cmc(1) and cmc(7) state]

- Each checker-owned fd keeps its own flags.  On 2.0.0 a `fcntl` on an
  epoll fd or a socket not yet dialled read and wrote the listener's,
  so a client made non-blocking before `connect` made the node's
  listener non-blocking and itself blocking, and an eventfd's
  `FD_CLOEXEC` read back 0.  A non-blocking `connect` (EINPROGRESS)
  ends the check (exit 3).  `poll` and `epoll_wait` no longer call a
  listening socket or an epoll instance writable, report a socket never
  dialled with `POLLOUT | POLLHUP`, and report a closed fd with
  `POLLNVAL` at once instead of parking.  [proc_gate: syscorner eventfd,
  poll, nbepoll, nbclient, pollclosed, each against a native run]
- A socket call on the wrong kind of fd gets Linux's errno.  On 2.0.0
  `accept` on a connected socket, a plain socket or an eventfd took the
  listener's next connection, `listen` on an eventfd made it the
  listener, `connect`, `bind` and `setsockopt` on an eventfd or an
  epoll instance succeeded, and `sendto` and `recvfrom` on an eventfd
  worked as a write and a read: a program with such a slip passed.
  Now ENOTSOCK, EINVAL, EISCONN, EBADF, ENOTCONN or EPIPE, as Linux
  gives.  [proc_gate: syscorner wrongfd, notconn, against a native run]
- A node's channels to other processes end the check (exit 3): a
  second `execve`, SysV shared memory, message queues and semaphores
  and POSIX message queues (the SysV objects outlive the process, so
  their contents survived a crash outside the world), `process_vm_readv`
  and `process_vm_writev`, and a signal or pidfd to any other process,
  the checker's process group included.  A signal to the node itself
  still goes to the kernel.  On 2.0.0 each went unseen.  [proc_gate:
  edges exec, shm, vmread, mq, killparent, killgroup, pidfd, killself]
- `epoll_wait` into memory the checker cannot write returns EFAULT, as
  Linux does, instead of ending the check.  [proc_gate: syscorner robuf]
- A refusal of `dup2`, `dup3`, `splice` or `sendfile` with a checker-owned
  fd at either end names the owned fd, not the call's first one.
  [proc_gate: edges dup2, dup3, splice, sendfile]

Model files:

- An invariant that mixes `and` and `or` at one level without
  parentheses is a parse error.  They associated left to right with
  equal precedence, so `a or b and c` meant `(a or b) and c`, which a
  reader who expects `and` to bind tighter takes for a different
  invariant.  Every shipped model file still parses.  [proc_gate:
  inv-mixed, inv-grouped]

Coverage:

- `make coverage` builds with gcc `--coverage`, runs every gate of
  `make check`, and fails unless function and branch coverage of each
  `src/*.c` file and overall are at least 90%; CI runs it.  The node
  side (the preload and the race runtime) is measured in process
  (`test/proc/preload_alloc.c`, `test/proc/race_rt.c`).  Each
  `GCOVR_EXCL` marker sits on a check that cannot fail in a test, or on
  the node launcher's code past the point its counts can be written,
  with its reason; the report lists them.  At release: functions
  98.2%, branches 91.5% (cmc.c 98.7% and 91.5%, cmc_cli.c 100% and
  99.0%, cmc_preload.c 100% and 100%, cmc_proc.c 96.2% and 90.2%,
  cmc_race.c 100% and 100%, cmc_spec.c 100% and 90.1%).  At 2.0.0:
  96.7% and 69.6%.

Command line, API and the abstract checker:

- A number given to a `cmc` option must be a whole decimal number in
  the option's range: `--crashes x` used to read as 0 and pass a bug
  that needs a crash.  `--walks 0` and, with `--model-file`, `--depth 0`
  are refused, as is `--refine` without `--model-file`.
  [test_cmc: test_cli_input]
- API: `cmc_run_new()` returns NULL for a `CMC_SIMULATE` search with
  `walks` of 0, which looked at no state and passed.
  [test_cmc: test_min4_negative_bounds]
- API: `cmc_run_new()` returns NULL for a `search` that is none of
  `CMC_BFS`, `CMC_DFS`, `CMC_SIMULATE` and `CMC_REPLAY`: `cmc_check()`
  ran no search for it and reported `CMC_PASS` with 0 states.  It also
  returns NULL for `CMC_REPLAY` with no `replay_in`, which would open a
  NULL path.  [test_cmc: test_min4_negative_bounds]
- API: a BFS stopped by `stop_on_witness` is `CMC_INCOMPLETE`, not
  `CMC_PASS`: it had not searched the rest of the graph, so toy-bug1's
  Mutex violation passed with it set.  [test_cmc: test_stop_on_witness]
- `cmc --spec` checks the type of every operand: `not {1}` held, and
  `x < {1}` compared x with 0, so an invariant with a type slip passed
  vacuously.  Arithmetic, orders, ranges, set elements and function
  entries take integers; `and`, `or`, `not`, quantifier bodies, guards,
  invariants and goals take booleans; `==` and `!=` refuse two types.
  Each breach is a model error, exit 3.  No shipped spec changed its
  counts or verdict.  [spec_gate: "the evaluator refuses an operand of
  the wrong type"]
- A `--bind` file is read whole.  A second binding of a variable used
  to replace the first, so `x = "node[0][0]"` then `x = "0"` hid the
  observable and accepted a step the spec forbids.  A variable bound
  twice, an unknown section or variable, a line with no `=`, an index
  on a scalar variable or none on a function, and a `[fields]` offset
  that is not 0 to 254 are errors now.  [spec_gate: "a variable bound
  twice is an error" and the rows after it]
- A `symmetry` declaration that cannot be evaluated, or that names
  more than 8 values, is an error; such a spec was searched without the
  reduction and no word said so.  [spec_gate: "symmetry x is an error"]
- `cmc --refine` checks each bound variable of the observed abstract
  state against its declared domain.  An implementation whose observed
  value left the spec's state space (x = 7 for `var x : 0 .. 2`) used
  to refine whenever the invariant held there; it is an error now
  (exit 3).  [spec_gate: "C1: an observed value outside its domain is
  refused"]
- `cmc --emit-tla` translates `not`, `in`, `some` and `all` inside a
  `count` body, which it copied as .cml words, so TLC refused the
  module.  [emit_gate: countops; spec_gate: "emit-tla: not, in, some
  and all in a count body"]
- `--define TAG` that gates no action in the spec is an error, so a
  mistyped tag no longer checks the spec without its planted bug.
  [spec_gate: "a --define that gates no action is an error"]
- `--tlc-cfg` reads TLC's one-line forms (`INVARIANT TypeOK`,
  `CONSTANT N = 3`), which it used to drop, and refuses a keyword it
  does not implement (`SYMMETRY`, `CONSTRAINT`, ...).  Core settings
  longer than the buffer are an error, not cut off.
  [test_cmc: test_cli_input]

## 2.0.0 (2026-11-17)

A soundness release, from an audit of 1.3.0
([docs/plans/soundness-remediation.md](docs/plans/soundness-remediation.md)).
The audit found ways for every engine to report a pass where a bug
existed, gates that passed after their tool failed, and documentation
that claimed more than the gates showed.  Each fix below has a test that
fails on 1.3.0, named in brackets after it (a gate script and its row
or fixture), unless the bracket says otherwise.  [docs/qualification.md](docs/qualification.md) lists what
each engine answers exactly, approximates, or refuses, with its gate.

The major version changes because results change for the same input.  A
model that passed on 1.3.0 may now fail, end INCOMPLETE (exit 2), or be
refused (exit 3), and state counts move.  Where an example's 1.3.0
result was an artifact of a bug, its README says so.

Abstract checker (`cmc --spec`):

- Liveness follows TLA+ semantics.  Stuttering counts as a behaviour,
  every fair bad component is checked, and the backward search for
  `A ~> B` stops at B.  The verdicts agree with TLC on five probes
  where 1.3.0 disagreed, and a graph with many edges per state is
  checked whole.  [spec_gate: sound/leads, leadscross, once, unfair,
  dead, edges; emit_gate runs the first five through TLC]
- The seen set compares states exactly, under symmetry as well: a
  fingerprint match is confirmed against the stored canonical state.
  [no failing test: a 64-bit collision cannot be produced at gate
  sizes; spec_gate's symmetry and count rows show no count moved]
- Model errors are errors.  These used to change the model silently and
  now exit 3: text that overflows a buffer; a non-set domain; a value
  outside its declared domain; integer overflow; `/` or `mod` on a
  negative; a primed variable read; an init that reads an unset
  variable; a variable assigned twice; a bound name that shadows another.
  [spec_gate: sound/truncate, typepass, boundpass, succbound, overflow,
  primed, init_order, duplicate, quantifier]
- Liveness under `symmetry` is refused.  [spec_gate: sound/symlive]
- `--refine-steps` (C2) checks the first state against init, skips
  actions gated off by `--define`, and compares states exactly.
  [spec_gate: sound/c2gate, c2init, scalar.bind]
- The spec gate also runs under ASan and UBSan.  [make check-asan]

Process engine (`cmc --model-file`):

- Each TCP connection has its own queues.  On 1.3.0, two connections
  between the same two nodes shared bytes.  [proc_gate: crossrpc,
  twoconn]
- `close()` sends a FIN, or an RST when bytes are left unread; a reset
  keeps the bytes already sent; an exited listener resets its backlog;
  a refused socket reads `ENOTCONN` and can be dialled again.  After a
  reset the next call reports `ECONNRESET`, then writes fail with
  `EPIPE` and the `SIGPIPE` the node's disposition decides.  Each case
  matches Linux loopback, which the gate runs natively beside cmc.
  [proc_gate: tcpsem finwrite, rstdata, backlog, redial]
- A `connect()` to a node that is down is refused.  [proc_gate:
  unmodelled refused]
- A node starts with every signal at its default and none blocked,
  whatever cmc inherited.  [proc_gate: the SIGPIPE row under `trap ''
  PIPE`]
- Worlds are deduplicated by their exact encoding, not a hash.  The
  encoding gains the durable record and its read offset, every polled
  fd, epoll user data, the futex word, a recv's requested length, and
  the connection flags.  Nodes run without ASLR, so addresses repeat
  across replays.  [proc_gate: fingerprint; fpfuzz, 3000 random worlds
  with mutations that must change the encoding and changes it must
  hide]
- Threads are scheduled by the search.  In a node with more than one
  thread, each `futex` call, call on a checker-owned fd, state-hook
  write, `clone` and thread exit ends the thread's step, and the search
  chooses which thread runs next (`run n1 t1`), births included: a new
  thread and its parent are held at the clone's return.  A world with
  such a node is never merged with another, so the search over it is a
  tree, INCOMPLETE after `--max-states` or 1,000,000 such worlds.  A
  call on a kernel fd (a pipe, a file) from such a node is refused,
  except writes to stdout and stderr.  Interleavings of memory accesses
  between two visible calls are not explored.  A single-threaded node
  is unchanged.  On 1.3.0 a multi-threaded node ran in one order.
  [proc_gate: racewake, raceemit, raceborn, threadpos, durack,
  kernelfd, racepipe, twothread, twothread_join; twothread_join run
  1000 times on two loaded CPUs]
- State-hook writes are carried out by the checker, so a crash can fall
  between a durable write and the next call.  [proc_gate: durack]
- A node that aborts (a failed `assert`) or faults is reported as a
  violation, and a thread blocked in a call the checker does not see
  ends the check after 10 s; both used to hang it.  [proc_gate: fault,
  kernelfd]
- These are refused (exit 3): a finite `poll`, `ppoll` or `epoll_wait`
  timeout; a timed futex wait; `EPOLLET`, `EPOLLONESHOT` and other epoll
  flags; `EFD_SEMAPHORE`; a state record longer than 64 bytes or left
  unfinished; a `SIGPIPE` handler or blocked `SIGPIPE` on a write into
  a reset connection; `fork`, `vfork` and a `clone` that is not a
  thread; a socket other than `AF_INET` `SOCK_STREAM`; `accept4`,
  `recvfrom` and `sendto` flags or addresses the model does not
  follow; and `readv`, `writev`, `pread`, `pwrite`, `recvmsg`,
  `sendmsg`, `shutdown`, `dup`, `ioctl`, `splice`, `sendfile` and the
  `*sockopt`/`*name` queries on a checker-owned fd.  [proc_gate:
  unmodelled pollto, epollet, semaphore, longrec, partrec, rsthandler,
  fork, udp, nbaccept, peek, writev, sendmsg]
- A write from a buffer the node does not own gets `EFAULT`, and a
  partial transfer of a call's arguments ends the check.  On 1.3.0 it
  was a zero-filled success.  The preload allocator checks sizes and
  copies exactly on `realloc`.  [proc_gate: unmodelled badptr,
  preload_alloc]
- A safety-only model whose initial state enables no action is
  INCOMPLETE, not a pass.  [proc_gate: noprogress]

In-process engine (`cmc --model`, `libcmc(3)`):

- A crash clears the node's messages to itself.  [test_cmc]
- Simulation and replay check every state, including the last.
  [test_cmc]
- `cmc_run_new()` refuses more than 64 properties of one kind, and a
  property of an unknown kind.  [test_cmc]
- The fair schedule cannot postpone an armed timer forever.  [test_cmc]

Examples:

- raft.c and hc.c served no requests while waiting on their own.  Under
  1.3.0's shared channels, a waiting node was handed its peer's request
  as the reply.  Both now serve while they wait.  raft's double-vote bug
  is caught again, through real schedules.  [make check-examples,
  check-examples-slow]
- hc's pass on 1.3.0 was vacuous: no participant ever adopted.  Now a
  participant adopts, and the model requires it.  [make
  check-examples-slow]
- paxos2 decides, and its planted bug breaks the shared spec's
  agreement.  [make check-examples: paxos2-refine]
- The Paxos family follows Lamport's Paxos.tla: `paxos.cml` (with a TLA+
  twin), and new `fpaxos.cml` (Flexible Paxos quorums), `caspaxos.cml`,
  `zab.cml` (epochs, discovery and the committed prefix) and
  `fastpaxos.cml`.  cmc's counts agree with TLC, and Apalache agrees.
  [emit_gate, apalache_gate]

Gates and release:

- The Apalache gate fails on a tool error.  A missing tool fails the
  gate under `CMC_REQUIRE_TOOLS=1`.  [apalache_gate --self-test]
- CI runs the TLA+ bridge gates (TLC and Apalache), the spec gate under
  ASan and UBSan (`make check-asan`), and the slow example rows (`make
  check-examples-slow`) before a tag.

## 1.3.0 (2026-11-14)

Tier 3 of the davros evaluation: the process front end now models the
runtime machinery a production event-loop server actually uses, so it
can be checked as it ships.  A node may run multiple threads
(`clone`/`futex`, with a search-driven schedule); a crashed node can be
relaunched (a budgeted RESTART with durable recovery); and `epoll` (not
just `poll`) is modelled, which is libxtc's default Linux backend.

Per-thread scheduling in the process front end (`cmc --model-file`,
Tier 3 of the davros evaluation): a node may now run more than one
thread.  The checker intercepts `clone`/`clone3` and `futex`, keeps a
deferred slot per thread (keyed by the kernel thread id), and treats a
thread parked in `futex` (a mutex, a condition variable, or a
`pthread_join`) as blocked.  The world is a search state only once every
thread of every node is blocked or has exited, so a loop thread in
`poll`/`accept` and a worker thread that waits on a condition variable
are both driven by the search rather than tripping the LULL timeout or
racing unmodelled.  A `FUTEX_WAKE` wakes waiters only when the program
issues it, as part of the waking thread's step; woken threads run one at
a time so the kernel's atomic futex and mutex handoffs are never torn.
The fingerprint carries each thread's blocked state as a canonical
multiset keyed by role, not by the raw (arbitrary) kernel tid, so two
runs are deduplicated identically.  A single-threaded node behaves
exactly as before.  A `FUTEX_WAKE` that would wake a strict subset of
several waiters on one word (a lock contended by a thread pool) is an
error rather than a silently chosen schedule; PI and requeue futex
operations are likewise refused.  Example: `test/proc/twothread.c` (the
handoff holds; `-DBUG` drops the data across it and the checker prints
the two-action counterexample) and `test/proc/twothread_join.c` (the
exit and join path).

Relaunch a crashed node in the process front end (Tier 3 of the davros
evaluation): a new `[environment] restarts = N` budget (default 0)
enables a RESTART action that relaunches a crashed node as a fresh
process.  A crash still keeps only the durable encoding (descriptor
193); a RESTART hands that surviving record back to the new process, so
its first read of descriptor 193 returns what it had made durable before
the crash (a process that never crashed reads EOF there) and a server
that reads its own log on startup recovers.  The relaunched process
starts with no connections and re-dials and re-listens as its code does;
a peer that still held a connection to the crashed process reads EOF on
it.  The relaunch is a pure function of the surviving durable record, so
stateless replay stays deterministic, and a restarted node is driven to
quiescence like any launch.  With no restart budget a crash stays final,
exactly as before.  Example: `test/proc/restart.c` (crash-and-recover
holds; `-DBUG` forgets to recover and the checker prints the
`crash` then `restart` counterexample; without the budget the crash is
final and the bug is not explorable).

Model `epoll` in the process front end (Tier 3 of the davros
evaluation, ask 9): the checker intercepts
`epoll_create1`/`epoll_create`/`epoll_ctl`/`epoll_wait`/`epoll_pwait` with
complete virtual-fd semantics.  An epoll instance holds a persistent
interest list built by `epoll_ctl` (`ADD`/`MOD` capture the requested
events and the opaque `epoll_event.data`; `DEL` removes; an unknown op is
an error); `epoll_wait` answers local readiness (an eventfd counter, a
requested `EPOLLOUT`) at once and otherwise defers as the same `BK_POLL`
quiescence point `poll` uses, so the search drives the loop and epoll
readiness never auto-delivers (M5).  When the search makes a watched fd
ready, `epoll_wait` returns the `epoll_event.data` the loop registered
for that fd verbatim (a `struct epoll_event`, not a `pollfd`), because
that data is how an event loop routes to the fd's handler; the data is
opaque (an ASLR pointer in a real loop) and stays out of the fingerprint,
while the interest list itself goes in, canonicalised by each fd's
semantic role.  This matters because libxtc's default Linux backend is
epoll, so without the model the checked binary ran the loop on real
kernel fds and the pass was a fiction.  Level-triggered only; `EPOLLET`
(edge-triggered) is not modelled.  Example: `test/proc/epollloop.c`, the
epoll twin of `pollloop` (the correct loop holds; `-DBUG` stores the
wrong byte and the checker prints the counterexample naming the epoll
wake).

## 1.2.0 (2026-11-14)

Event-loop runtime support hardened from the davros evaluation: the
process front end now models the socket setup, non-blocking accept and
TCP reset semantics a real event loop needs, and no longer passes a
program that never advances past its initial state.

Process front end (`cmc --model-file`):

- Event-loop socket setup is modelled: `fcntl` now handles `F_GETFD` and
  `F_SETFD` (the `FD_CLOEXEC` flag, stored and returned, inert in the
  model) instead of failing, so a runtime that sets close-on-exec on
  every socket gets past `fcntl`.
- `accept` on an `O_NONBLOCK` listener with nothing queued returns
  `EAGAIN`, so an event loop that drains the backlog (accept until
  `EAGAIN`) makes progress instead of parking on the second accept.
- `listen` records the listener fd; the poll wake matches the listener
  by that fd and ignores negative fds, instead of treating any polled fd
  that is neither a connection nor an eventfd as the listener (which
  wrongly woke a kernel self-pipe or an unused `fd -1` slot).
- A no-progress self-loop is reported, not passed vacuously: if the
  search is exhaustive and every invariant holds but no action ever
  changed the world (every step was a stutter), the program never
  advanced past its initial state, so `cmc` reports INCOMPLETE (exit 2)
  and advises pairing the safety invariant with an `eventually` property,
  rather than a clean pass.  `test/proc/noprogress.c` is the guard.
- `[target] max_write` raises the per-write limit (default 64) up to a
  4 KiB cap, for real payloads larger than a tiny control message.
- `[target] args = ...` passes extra argv after the node id, so a target
  that takes flags runs under `--model-file` without a wrapper.
- A drop models a connection reset, not a lost byte.  TCP never loses a
  byte from a live stream, so losing one message from the middle of a
  channel was a counterexample the network cannot produce.  A drop now
  discards every queued message on the connection in both directions and
  both ends read EOF (as after `ECONNRESET`); a write into a reset
  connection reaches no one.  The reset set is part of the fingerprint.
  `test/proc/reset.model` is the regression guard: with the old silent
  loss it reported a spurious violation, with the reset it holds.
- `time()` and the aligned allocators (`posix_memalign`, `aligned_alloc`,
  `memalign`) go through the preload.  A target that seeded from
  `time(NULL)` read the real wall clock, and an aligned buffer came from
  glibc's ASLR'd heap, either of which broke replay determinism.  `time()`
  now reads the logical clock and the aligned allocators carve from the
  fixed-address arena.  `CMC_ARENA_MB` sets the arena size in MiB
  (default 64) for a larger target.
- The checker sets `CMC_STATE_FD` (the volatile fd, 3) and
  `CMC_DURABLE_FD` (the durable fd, 193) for every node, so a program
  that keeps its state hook in the build it ships can guard every emit on
  `CMC_STATE_FD` and never write a stray descriptor outside a check.
- `test/proc/guarded.c` exercises the guarded hook and the preload's
  determinism; it is gated in `proc_gate`.
- cmc(1), cmc(7) and the model-file guide describe the process front
  end's drop (a reset), crash (final, no restart yet) and determinism
  contract accurately.

## 1.1.0 (2026-11-13)

Async-I/O support for event-loop runtimes, so a program that waits in
`poll` with non-blocking sockets and `eventfd` wakeups (rather than
blocking sockets) can be model-checked as it ships.  Plus the Fast Paxos
certified model and the automated Codeberg release from 1.0.0's tail.

- Async-I/O support for event-loop runtimes: the process front end
  intercepts `poll`/`ppoll`, `fcntl(F_GETFL/F_SETFL)` for the
  `O_NONBLOCK` flag, and `eventfd2`.  A `poll` with nothing locally
  ready (only a requested `POLLOUT` or a non-zero eventfd answer at
  once) is a quiescence point: the node defers, and a new WAKE action
  marks one polled fd ready so the node accepts or reads it and defers
  again, which the existing ACCEPT/DELIVER/TIMEOUT actions service.
  Poll readiness is a search decision, never automatic, so the M5
  no-auto-delivery property holds for event loops as it does for
  blocking recv.  A non-blocking read with nothing deliverable returns
  EAGAIN; an `eventfd` is a counter a write bumps and a read drains.
  The world fingerprint carries each connection's `O_NONBLOCK` flag, a
  polling node's fd set, and the eventfd counters.  A new example,
  `test/proc/pollloop.c`, checks a poll-driven server and is caught by
  `-DBUG` when it stores the wrong byte (gated in `proc_gate`).

- The Codeberg release is published by `make codeberg-release VER=X.Y.Z`
  (`scripts/codeberg-release.sh`), run after the GitHub release for the
  tag publishes.  Codeberg Actions are disabled, so the forgejo release
  workflow does not run; the script creates the Codeberg release through
  the API and attaches the same tarballs the GitHub workflow built, so
  both forges carry identical artifacts.  It is idempotent.  The release
  procedure is written up in `RELEASING.md`.

- A certified abstract model for Fast Paxos
  (`examples/spec/fastpaxos.cml`).  A fast round lets clients send
  values directly to acceptors and so may collide; its distinctive
  safety result is that the fast quorum must exceed `3A/4` for a classic
  recovery round to resolve a collision.  The model has two rounds (a
  fast round, then a classic recovery) and tracks the bound faithfully:
  with `A=5, F=4` (> 3.75) it holds, and with the correct recovery rule
  but `F=3` (<= 3A/4) Agreement breaks, as Lamport's analysis says.  The
  `--define BUG` lets recovery use a classic-majority threshold where a
  fast quorum is needed.  Certified against TLC and Apalache; the
  Apalache gate's bound is raised to length 10 so the seven-step Fast
  Paxos counterexample is reachable.

## 1.0.0 (2026-11-12)

The first stable release.  The public interface in `include/cmc.h`, the
command line, the model file and the abstract `.cml` language are now
stable under semantic versioning.  The engine checks the program you
ship, with the TLA+-scope milestones A through F in place (a value
model, the abstract `.cml` language with modules, the binding and
refinement checker with action-level refinement, the TLA+ bridge to TLC
and Apalache, symmetry reduction and weak-fairness liveness), and a
library of certified abstract models spanning the consensus family and
beyond.  Milestone G (scale) continues past 1.0 as a performance track:
the fork-from-snapshot search is parked on a branch and the default
breadth-first engine is the sound path.

Since 0.5.0:


- A certified abstract model for two-phase commit
  (`examples/spec/twophase.cml`), the canonical non-consensus
  distributed-systems example (Lamport's `TwoPhase.tla`).  Consistency
  (atomicity) -- a committed and an aborted resource manager never
  coexist -- holds because a resource manager commits only on a
  unanimous prepared vote; the `--define BUG` commits without that
  check.  Certified against TLC and Apalache.  This broadens the example
  set beyond the quorum-based consensus family to an all-or-nothing
  atomic-commit protocol.

- A certified abstract model for CASPaxos (`examples/spec/caspaxos.cml`),
  single-decree Paxos on a register via a client change function.
  Agreement (one value per register version) holds because the client
  reads from a quorum before committing, so its read intersects any
  prior commit; the `--define BUG` stale read overwrites an acceptor and
  two values commit for one version (a lost update).  Certified against
  TLC and Apalache.
- Running every shipped example is now an explicit release gate: the
  `check-examples` target builds and checks every process-mode example
  (`examples/*/`) and every abstract model (`examples/spec/*.cml`), and
  `make check` (which the release workflow runs on the unpacked tarball)
  depends on it.  Previously the example gates ran inside `check-unit`
  as a side effect; naming the dependency makes a release unable to ship
  an example that no longer runs.
- `calib/hc` is kept, not retired.  Milestone D envisaged retiring the
  hand-kept `HummingConsensus.tla` once a certified HC `.cml` reproduced
  its rows through the bridge; `examples/spec/hc.cml` is certified but a
  minimal single-property model that does not reproduce those rows (many
  properties and variants at up to tens of millions of states), and the
  in-process engine it calibrates is a documented, shipped path.  The
  design notes and plan are corrected to say so.

- A certified abstract model for Flexible Paxos
  (`examples/spec/fpaxos.cml`), the flagship of the Paxos-variant test
  matrix and a direct expression of Heidi Howard's theorem: Agreement
  holds iff every Phase 1 quorum intersects every Phase 2 quorum, for
  which `Q1 + Q2 > A` suffices.  With `A=5, Q1=4, Q2=2` it holds; the
  `--define BUG` uses the Phase 2 quorum size for the Phase 1 guard, so
  the cross-phase intersection fails and two values are chosen.  The
  model tracks the inequality faithfully: with the correct guard but
  `Q1=3` (so `Q1 + Q2 = A`, not `> A`) Agreement also breaks.  Certified
  against TLC and Apalache like the other specs.
- `docs/fit.md` ("When libcmc fits"): where an explicit-state checker of
  the shipping code is the right tool (crash-fault agreement and
  replication at a small instance, the quorum-intersection knob, commit
  durability, refinement of a real binary) and where it is the wrong one
  (liveness and partial synchrony, real-time and timing, Byzantine and
  cryptography, performance, state spaces no bounded search reaches).
- `docs/research/03-paxos-variants-and-howard.md`: a survey of Heidi
  Howard's consensus research and the Paxos family, with the one safety
  invariant and the one planted-bug knob for each variant, and a fit
  assessment for explicit-state bounded checking.

## 0.5.0 (2026-11-11)

- Certified abstract models for Raft leader election, Humming Consensus
  and ZAB (`examples/spec/raft.cml`, `hc.cml`, `zab.cml`), joining the
  existing Paxos model, so the whole consensus family now has one.  Each
  is checked three ways that agree: `cmc --spec`, TLC
  (`make check-emit-tla`), and Apalache (`make check-emit-apalache`).
  Raft's model is the vote-and-win core (each server grants one vote per
  term, a candidate wins on a quorum) and its property is Election
  Safety (at most one leader per term, following Ongaro's raft.tla); the
  `--define BUG` revote lets two candidates win.  HC's model is
  agreement over write-once epoch register stores with a quorum
  (following Machi's Humming Consensus) and inherits the shared
  `consensus` Agreement; the `--define BUG` overwrite breaks write-once
  so two values are chosen.  ZAB's model is commit-durability (following
  Apache ZooKeeper's Zab.tla): a committed transaction is held by a
  quorum, so it survives a leader change whose recovery quorum must
  intersect; the `--define BUG` early ack commits before a quorum
  stored, so a crash loses the committed transaction.  The emit gates
  now set `CHECK_DEADLOCK FALSE` (TLC) and `--no-deadlock` (Apalache),
  since a terminal state is not a safety fault.

- The TLA+ bridge reaches Apalache (Milestone D): `cmc --emit-tla` now
  writes Apalache `@type` annotations (every constant and scalar
  variable is `Int`, a function variable is `Int -> Int`, the `vars`
  tuple is annotated so Apalache reads it as a tuple, not a sequence)
  and a `CInit` operator binding the constants to their `.cml` values.
  Apalache's symbolic, Z3-backed checker then accepts the generated
  module directly: `apalache-mc check --cinit=CInit --inv=INV
  MODULE.tla`.  The annotations are `\*` comments, so TLC still accepts
  the same module unchanged.  `make check-emit-apalache` verifies that
  the generated TLA+ type-checks in Apalache and agrees with `cmc
  --spec` (counter and Paxos hold, the planted Paxos bug violates).
  Apalache is an optional external tool, like TLC; the gate fetches it
  once to `$CMC_SCRATCH` or uses `$APALACHE_MC`, and needs Java 21+.

- Action-level refinement with stuttering (Milestone C2): `cmc --refine
  SPEC --bind B --refine-steps` checks that each implementation *step*
  maps, under the binding, to either an abstract action step or a
  stutter (the abstraction unchanged), not only that each reached state
  satisfies the invariant (C1).  This catches an implementation that
  reaches only in-range states but gets there by a transition the spec
  forbids, which the state-level check cannot see.  It reuses the same
  successor enumeration and canonical fingerprint the `--spec` search
  uses and TLC calibrates, so a matched step is an exact abstract
  successor.  It requires every spec variable to have a binding and
  refuses otherwise, since matching an action step needs the whole
  abstract state.  `test/proc/mono.c` (a counter the spec allows to step
  up only by one) is the gate: C1 passes a by-two build, C2 catches it.

- One model, many implementations (Milestone C gate): a second,
  structurally different single-decree Paxos (`examples/paxos/paxos2.c`
  -- three symmetric nodes, each both proposer and acceptor, a different
  wire protocol and a different state-hook layout) is checked to refine
  the *same* abstract model `examples/spec/paxos.cml`, through its own
  binding `examples/spec/paxos2.bind` (the spec unchanged).  Both Paxos
  binaries now refine one specification, making "one spec, many
  implementations, all correct" a demonstrated claim.  Gated in
  `test/proc_gate.sh`.

## 0.4.0 (2026-11-10)

- Liveness with weak fairness for the abstract checker (Milestone F): a
  `.cml` can declare `fairness weak` and temporal properties
  `property P: eventually E` (`<>E`) and `property P: A ~> B`
  (leads-to).  After the safety search, the checker builds the reachable
  state graph and finds a fair violating lasso -- a cycle, all of whose
  states miss the goal, that weak fairness does not forbid (no action
  enabled throughout the cycle yet never taken) -- via SCC analysis on
  the sub-graph restricted to goal-free states.  `examples/spec/ticker.cml`
  (a counter that must reach its max) holds under fairness; built with a
  `reset` action (`--define RESET`) a fair cycle avoids the max and the
  property fails.  Cross-checked against TLC: `cmc --emit-tla` now emits
  `WF_vars(...)` fairness and the temporal property, and TLC agrees on
  both verdicts.  This is the in-process-engine liveness the plan scoped
  for the abstract side; process-side liveness still waits on
  fork-from-snapshot.
- Symmetry reduction for the abstract checker (Milestone E): a
  `symmetry SET` declaration in a `.cml` names interchangeable values
  (node ids, say); the checker canonicalises each state to the
  least image under permutations of SET before deduping, collapsing
  states that differ only by a permutation.  `examples/spec/token.cml`
  (a token among N interchangeable nodes) shrinks from 10 distinct
  states to 3, with the same verdict -- the soundness check for the
  declaration.  Obligation (as with TLA+ model values): the symmetric
  values must be a dedicated range, interchangeable everywhere they
  appear and disjoint from other integer uses, or the reduction is
  unsound; the gate checks the verdict is identical with symmetry on and
  off.  Invariants are evaluated on the real reached state, not the
  canonical representative, so a specific-id property is still checked
  correctly.
- The TLA+ bridge, `cmc --emit-tla FILE.cml` (Milestone D): translate an
  abstract `.cml` module to a TLA+ module on stdout, so the same model
  reaches TLC (explicit), Apalache (symbolic), and TLAPS (proof) -- the
  proof and large-domain engines the plan delegates to.  Verified
  against live TLC (`make check-emit-tla`): the generated module parses
  and gives the same verdict as `cmc --spec` -- counter and Paxos hold,
  Paxos with the planted bug violates, at the same depth.  `[TAG]`-gated
  actions are emitted only when `--define`d, and operators are rewritten
  faithfully (`and`->`/\`, `count`->`Cardinality({...})`,
  `[f except ...]`->`[f EXCEPT ![...] ...]`, `/`->`\div`).  This is how
  the hand-kept `calib/hc` TLA+ is, in time, superseded by a mechanical
  bridge.
- Refinement checking, `cmc --refine SPEC.cml --bind FILE.bind --binary
  PROG` (Milestone C, the heart of the plan): the shipping binary is
  checked against the *reusable abstract model* -- the same `.cml` that
  `cmc --spec` checks on its own and TLC calibrates.  A `.bind` file maps
  the implementation's observables (state-hook fields, the reply log) to
  the abstract variables; at each reached implementation state the
  abstract state is built from the binding and the abstract invariants
  are checked.  Demonstrated on Paxos: the shipping
  `examples/paxos/paxos.c` refines `examples/spec/paxos.cml` (561 states,
  no error), and the planted-bug build does not -- the abstract
  Agreement is violated, sourced from the reusable model, not a
  hand-written implementation-side invariant.  This is "one model, many
  implementations": point the same spec and binding at any Paxos binary.
  The binding may reference only observables, never internals (the
  no-omniscient-oracle rule), so the refinement is as honest as the
  state hook is complete.
- Abstract-model modules: a `.cml` can `extends` (or `import`) another,
  merging its constants, variables, actions and invariants, so a shared
  base is written once.  `examples/spec/consensus.cml` is that base for
  the consensus family -- it declares the agreed-value record and the
  Agreement property (at most one value chosen) -- and
  `examples/spec/paxos.cml` extends it: single-decree Paxos in the
  Voting style (acceptors vote per ballot; a value is chosen on a
  quorum) inherits Agreement unchanged.  Calibrated against single-worker
  TLC (examples/spec/paxos.tla): 117 distinct states, depth 5, identical
  verdict; the planted double-vote bug breaks Agreement at the same
  6-action depth as TLC.  This is the shared-base half of "a category of
  protocols models its common requirements the same way".
- The abstract-model front end, `cmc --spec FILE.cml` (Milestone B of the
  TLA+-scope plan): a small, TLC-like explicit-state checker for a
  declarative specification on its own, no implementation.  A `.cml`
  module has `const`, `var` (integer, set and function/record
  variables), `init`, named guarded `action`s (with finite parameters,
  so one action covers a family of transitions), and `invariant`s; the
  search is breadth-first, so a violation is the shortest action
  sequence.  Calibrated against single-worker TLC: on the counter and
  mutex fixtures `cmc --spec` gives the same verdict, distinct-state
  count and shortest-counterexample depth as TLC on the hand-written
  TLA+ twin (test/spec/*.tla; `make check-spec-calibration`).  This is
  the abstract half of "one model, many implementations" -- the model an
  implementation will be checked to refine (Milestone C).

- The invariant language gained integer arithmetic (`+`, `-`, `*`,
  `mod`) and `count V in SET : PRED` (cardinality), so a quorum property
  -- a committed value is held by a majority -- is now expressible; it
  was not before.  This is the first increment of the value model
  (Milestone A of the TLA+-scope plan), kept inside the SMT-encodable
  fragment.  A `quorum` fixture pins it: the count-based invariant holds
  when the coordinator commits only on a majority of stores, and catches
  a sub-quorum commit.

## 0.3.0 (2026-11-09)

- The Humming Consensus example (`hc/`) is now the GENERIC agreement
  protocol it is -- like Paxos, Raft and ZAB -- not chain replication.
  Three participants agree on an opaque configuration per epoch over
  write-once register stores, communicating only indirectly through
  reads and writes of those stores (Fritchie, PaPoC 2016, Figure 2).
  The checked property is HC's safety property, agreement: at most one
  configuration is adopted per epoch.  The correct (CP-mode) build holds
  agreement EVEN UNDER A NETWORK PARTITION -- a quorum majority is
  required to adopt, and quorums intersect -- proven exhaustively; the
  `-DBUG` build runs in eventual-consistency mode (adopt without a
  quorum) and loses agreement under a partition.  The README cites the
  paper, Machi's reference implementation, and the invariant's mapping
  to the stated safety property.  (Supersedes the earlier chain-
  replication framing, which was one use of HC, not HC itself.)
- The Paxos, Raft and ZAB invariants are restated with the new
  node-quantifiers and named fields, so each reads as the safety
  property of its canonical specification -- Paxos agreement over all
  proposer pairs, Raft Election Safety over all node pairs -- rather
  than a hand-expanded list of pairs.
- The invariant language gained the expressiveness to state a protocol's
  real safety property instead of a proxy.  `all i in nodes : EXPR` and
  `some i in nodes : EXPR` quantify over node indices, so agreement over
  all pairs reads like its TLA+ mirror rather than a hand-expanded list.
  An optional `[fields]` section names byte offsets, so an invariant
  says `node[i].epoch`, not `node[i][7]`, and does not depend on the
  hook's layout.  An invariant prefixed `eventually` is a bounded
  reachability check: some reached state must satisfy it, reported at
  the end of an exhaustive search (not fairness-based liveness, which
  the safety engine does not do).
- Two more worked examples, checked as they ship.  `zab/`: ZooKeeper
  Atomic Broadcast (a leader and two followers) checked for
  no-lost-commit; an early-ack bug loses a committed transaction to a
  leader crash.  `raft/raftlog.c`: Raft log replication, checked for
  State Machine Safety (a committed entry is never lost); the leader
  that commits before a quorum has the entry loses it on a crash -- the
  acknowledged-write-lost failure Marc Brooker (AWS) animates in
  "Animations for System Builders".  (The `hc/` example is rewritten as
  generic Humming Consensus, above.)
- Each example cites its canonical formal reference -- Paxos (Lamport's
  `Paxos.tla`), Raft (Ongaro's `raft.tla`), Humming Consensus (Fritchie,
  PaPoC 2016), ZAB (Apache ZooKeeper's `Zab.tla`) -- and the safety
  property the libcmc invariant corresponds to, plus links to
  animated/interactive visualizations.

## 0.2.0 (2026-11-09)

Checking the program you ship.  `cmc --model-file` runs an unmodified
binary as processes under interception and checks it; the in-process
`CmcCore` interface becomes the engine's calibration path.  Two real
consensus protocols ship as worked examples.

- `cmc --model-file FILE` checks a program as it ships: each node is the
  shipped binary, run unmodified as a process under seccomp user
  notification (Linux x86-64), and the checker is the network, the
  clock and the crash between them.  A model file names the program,
  the environment (a client write, timeouts, crashes, drops, a depth
  bound) and the invariants, in a small expression language over each
  node's reported state and the replies clients received.  Breadth-first
  search gives the shortest counterexample, named action by action;
  `--simulate` takes seeded random walks.  `--binary` checks another
  build against the same file.  This is now the way to use libcmc; the
  in-process `CmcCore` interface is the engine's calibration path.
- The loader refuses every model-file section, key or value it does not
  implement, and every invariant it cannot parse whole, instead of
  ignoring it.
- Worked examples under `examples/`, checked as they ship.  `paxos/`:
  single-decree Paxos (two proposers, three acceptors, five processes)
  checked for agreement, with a planted promised-values bug caught by
  search and by simulation.  `raft/`: Raft leader election (three nodes)
  checked for Election Safety with and without a crash, with a
  double-vote bug that elects two leaders in a term.  `make check`
  builds and checks both.
- A timeout can now fire at a node idle in `accept()`, not only one
  blocked in `recv`: the node accepts a synthetic timer connection and
  reads `T` from it.  A server that waits for a peer to connect can thus
  detect an election timeout, which real leader election needs.  Pinned
  by `test/proc/accept_timeout.c`.
- Every intercepted syscall is pinned by a fixture with teeth
  (`test/proc/clocks.c`).  This caught a bug: `gettimeofday` wrote the
  logical clock to the wrong argument (the `tz` pointer, not the `tv`
  buffer), so a target reading wall time through it got zero instead of
  the checker's clock; fixed.
- `make install` installs the `cmc` binary and `lib/libcmc/cmc-preload.so`,
  the deterministic heap and logical clock a checked program runs under;
  an installed `cmc` finds it from its own location, or from
  `CMC_PRELOAD`.
- No change to `include/cmc.h`.  `cmc_main()` accepts `--model-file`.

## 0.1.0 (2026-11-03)

The first release: an explicit-state model checker for protocol code
written as pure C step functions, linked from the same object files the
product ships.

- Search: breadth-first with TLC semantics (shortest counterexamples,
  exact distinct and generated counts, depth counting the initial state
  as 1), depth-first to a bound, seeded random walks, and replay of a
  saved counterexample through the real cores.
- Environment, enabled only from what the cores emit and from budgets:
  FIFO connections that reset on a drop or crash, or an unordered bag
  with duplication; timers; persists that complete in order; crashes
  that keep any prefix of the pending persists; wakes; client requests.
- Properties: invariants, action properties, witnesses, quiescence
  properties, and bounded liveness under a fair schedule.
- Gates: a determinism gate (`--det`), a purity gate over core objects
  and sources and the probes that prove it rejects impurity, a
  MemorySanitizer gate, planted-bug gates for the toy and calibration
  cores, and a documentation gate.
- Calibration: on 22 configurations of HummingConsensus, the same
  verdicts, counterexample depths and distinct-state counts as
  single-worker TLC.
- `libcmc.a`, `cmc.h` (`CMC_VERSION` is "0.1.0"), and the command line
  as the library function `cmc_main()`, with `cmc --version`.
- Man pages `cmc(1)`, `libcmc(3)` and `cmc(7)`; `make install`; a Nix
  flake; documentation at <https://gregburd.codeberg.page/libcmc/>.
