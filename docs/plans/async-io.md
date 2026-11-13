# Plan: async-I/O support in libcmc for event-loop runtimes (libxtc)

Status: in progress.  Target release: v1.1.0.  The libcmc side is
done: `poll`/`ppoll`, `fcntl(F_GETFL/F_SETFL)` for `O_NONBLOCK`, and
`eventfd2` are intercepted with complete virtual-fd semantics; a `BK_POLL`
quiescence point and a WAKE search action drive the poll loop without
auto-delivering (M5); the fingerprint carries the poll set, the nonblock
flags and the eventfd counters; and `test/proc/pollloop.c` is gated in
`proc_gate` with a `-DBUG` the checker catches.  `epoll` (item 5) is
deferred -- libxtc's poll backend makes it unnecessary for v1.1.0.  The
libxtc side and the build glue remain.

## The problem

libcmc process mode intercepts a blocking-socket syscall vocabulary
(socket, setsockopt, bind, listen, connect, accept/accept4,
read/write/recvfrom/sendto, close, clock_gettime, gettimeofday,
getrandom; `src/cmc_proc.c` ~630) and recognises a node as quiescent
only when it is parked in a deferred recv or accept.  An event-loop
runtime (libxtc) waits in `poll`/`epoll_wait`/`io_uring_enter`, sets
sockets `O_NONBLOCK` with `fcntl`, and wakes its loop with `eventfd`.
None of that is modelled, so `drive_quiescent` hits the LULL_MS branch
and dies with "node made no checked syscall ... (blocked in a call the
checker does not model)", or the program misbehaves on the virtual fds.

The gap is readiness-based async I/O, not a soundness defect in what is
already modelled.  The fix is additive: extend the intercept vocabulary
to the readiness model, reusing the pending-data and deferred-syscall
core.

## Scope decision (what libcmc models, what it does not)

- **Model in libcmc:** `poll`, `ppoll`, `fcntl(F_GETFL/F_SETFL)` for the
  `O_NONBLOCK` flag, and `eventfd`/`eventfd2` as a virtual channel.
  Optionally `epoll_create1`/`epoll_ctl`/`epoll_wait` (close to poll).
- **Do NOT model in libcmc:** `io_uring` (a shared-memory SQ/CQ protocol,
  far more surface; modelling it would bloat the intercept set and its
  soundness story).  The blocking-thread pool (opaque work, not a
  protocol interleaving; it would break replay determinism).
- **libxtc adapts** to the subset libcmc models: a `poll` backend, a
  single loop, no blocking-pool use, `-no-pie`, and the fd-3 state hook.
  This is the two-sided contract; libcmc does not chase an arbitrary
  async runtime.

The soundness rule governs every addition: each newly intercepted
syscall must have **complete** semantics for the virtual-fd world.  A
half-modelled `poll` that sometimes falls through to `SECCOMP_RET_ALLOW`
would let a real interleaving escape the search -- the one thing libcmc
must never do.  So the readiness model lands as one unit with its own
calibration (a planted-bug example the checker must catch), not
piecemeal.

## libcmc work items

1. **Readiness query: `poll`/`ppoll`.**  Intercept both.  Read the
   `pollfd` array from the node (`cr_read`).  For each fd, decide
   readiness from the SAME bookkeeping that answers a deferred recv:
   a channel fd is POLLIN-ready iff the bag has a message for this node
   on that peer, or a timeout is being delivered; a listener fd is
   POLLIN-ready iff a connection is pending (`npend`); an eventfd is
   ready iff its counter is non-zero.  POLLOUT is always ready (the
   model never blocks a write).  If any fd is ready, write the revents
   back and return the count.  If none is ready, the node is quiescent:
   DEFER with a new `BK_POLL` block kind recording the polled fd set, so
   the search can make it ready (deliver a message, fire a timeout) and
   then answer the poll.  A zero timeout (`poll(..., 0)`) returns
   immediately with whatever is ready (0 allowed) -- it does not become a
   quiescence point.

2. **`BK_POLL` servicing.**  Where the search delivers to a `BK_RECV`
   node, teach it to also satisfy a `BK_POLL` node: when a message or
   timeout is delivered to a channel the node is polling, answer the
   poll with that fd marked POLLIN-ready.  The fingerprint must capture a
   `BK_POLL` node's polled set so two differently-polling states are
   distinct.

3. **`fcntl(F_GETFL/F_SETFL)` and `O_NONBLOCK`.**  Intercept `fcntl`.
   Track an `O_NONBLOCK` flag per virtual fd.  A `read`/`recvfrom` on a
   non-blocking fd with no message ready returns `-1/EAGAIN` immediately
   instead of deferring as `BK_RECV`.  This is what lets an event loop
   loop instead of blocking.  `F_GETFL` returns the stored flags;
   unknown `fcntl` commands on a virtual fd are an error (die), not a
   silent pass, to keep semantics complete.

4. **`eventfd`/`eventfd2` as a virtual channel.**  `eventfd` returns a
   virtual fd with a counter (initial value from the arg).  A `write`
   adds to the counter and makes it POLLIN-ready; a `read` returns the
   counter and clears it (or decrements, per `EFD_SEMAPHORE`).  This
   makes the loop's self-wakeup path visible rather than passed through.

5. **(Optional) `epoll_create1`/`epoll_ctl`/`epoll_wait`.**  An epoll set
   is a registered fd list; `epoll_wait` answers readiness from the same
   bookkeeping as `poll`, deferring as `BK_POLL` when nothing is ready.
   Decide during implementation whether libxtc's poll backend makes this
   unnecessary for v1.1.0; if so, defer it.

6. **Filter + dispatch.**  Add the new numbers to the `OWN(...)` list in
   the seccomp filter (~630) and a case to `try_pending_for` for each,
   with complete semantics.  Update the world-layout comment, the
   fingerprint, and any block-kind enum and quiescence logic.

7. **Gate + calibration.**  A new process-mode example that uses the
   poll/nonblock/eventfd path: a replicated register over a poll-driven
   loop, model file with a `no_lost_ack` invariant and a `-DBUG` flag the
   checker must catch (mirroring `raftlog`).  Wire it into `proc_gate`
   (so it is covered by `check-examples`).  The red-test-first
   discipline from the libcmc-change skill applies: write the example
   and see the LULL_MS death first, then implement until it checks.

## libxtc work items (the other side of the contract)

- A `poll` backend selectable for the model-checkable build (no
  epoll/io_uring, no blocking-pool, one loop).
- `-no-pie` build, and an fd-3 canonical state hook.
- `configure --with-libcmc=auto` (`AC_PATH_PROG cmc` + locate the
  preload), `make check-cmc` that SKIPs when cmc is absent (libcmc is an
  optional dev dependency, as TLC/Apalache are for libcmc).
- A model-checkable example: a replicated register over `xtc_net` with a
  planted-bug flag and a `no_lost_ack` invariant.

## Sequencing

1. `fcntl`/`O_NONBLOCK` + `eventfd` first (small, local, independently
   testable with a tiny hand-written poll-loop fixture).
2. `poll`/`ppoll` + `BK_POLL` (the keystone), with the fingerprint and
   quiescence changes.
3. The gated poll-loop example with the planted bug.
4. Calibrate: the example's correct build checks clean, the `-DBUG`
   build is caught, and (if the model maps to an abstract spec) the
   usual three-way agreement.
5. Then the libxtc side and the build glue, and a worked libxtc example.
6. Qualify and cut v1.1.0.

## Soundness checklist (per the fit docs and the change skill)

- Every intercepted syscall has complete virtual-fd semantics; none
  falls through to ALLOW in a way that could hide an interleaving.
- `poll` with nothing ready is a genuine quiescence point, serviced by
  the same delivery the search already drives; it never auto-delivers.
- Non-blocking reads return EAGAIN deterministically; the search still
  owns when a message arrives.
- The new example's planted bug is caught, proving the readiness model
  exposes a real fault, not just that the correct program passes.
