# Plan: per-thread scheduling in libcmc process mode (Tier 3, ask 8)

Status: shipped in 1.3.0 as a single schedule.  The engine runs one
order of a node's threads, so the soundness plan (item 3.8) makes any
multi-threaded search end INCOMPLETE until the choice of thread is a
search action.

## The problem

The process front end (`src/cmc_proc.c`) models one node as one process
with a single deferred slot (`Node.blocked`, `Node.block_kind`,
`Node.block_id`, ...).  A node is quiescent when that one slot is filled
(a deferred recv/accept/poll) or the node has exited.

A multi-threaded target breaks this.  A loop thread parked in poll and a
device/compute thread parked in a futex are two independent blocking
points in one process.  With one slot:

- once the loop thread's poll is deferred, `try_pending_for` stops reading
  the listener, so the device thread's owned syscalls (a `write` that
  reports a result) are never received; and
- a thread parked in `futex` makes no owned call, so after `LULL_MS`
  (10 s) `drive_quiescent` declares "node made no checked syscall ...
  (busy, or blocked in a call the checker does not model)" and dies.

So the checker cannot model a node that uses more than one thread.

## The facts the kernel gives us (verified on the box)

A probe (`/scratch/libcmc/probe/mt_probe.c`, kernel 7.0, glibc 2.39)
confirmed:

1. A thread created with clone shares the seccomp filter; its
   notifications arrive on the **same listener** fd as the main thread's.
2. `seccomp_notif.pid` is the **calling thread's TID**, distinct per
   thread.
3. The listener delivers a notification from thread B **while thread A's
   notification id is still outstanding** (unanswered): a deferred tid
   does not block the listener for other tids.
4. `poll(listener, POLLIN)` is readable while any notification waits.

A strace of a glibc condvar handoff showed the real shape:

- `clone3` with `CLONE_THREAD|CLONE_CHILD_CLEARTID`, child_tid in
  `clone_args.child_tid`;
- the new thread first runs `rseq` and `set_robust_list` (ALLOWed, no
  model effect), then `futex(cond_word, FUTEX_WAIT_BITSET_PRIVATE, 0,
  ...)`;
- the signaller does `futex(cond_word, FUTEX_WAKE_PRIVATE, 1)` on the
  **same address**;
- `pthread_cond_wait` tests its predicate under a mutex, so a thread only
  futex-waits when it genuinely has to block (the `*uaddr == val` check
  is load-bearing);
- `pthread_join` waits with `futex(child_tid_word, FUTEX_WAIT_BITSET,
  tid, ...)`, woken by the kernel's `CHILD_CLEARTID` on thread exit.

## The model

### Per-thread deferred state

Replace the single `Node.blocked`/`block_*` with a fixed table of thread
slots keyed by tid (`MAXTHREAD = 8` per node).  A notification is routed
to its tid's slot by `seccomp_notif.pid`.  A tid is *blocked* when its
slot holds a deferred syscall (BK_RECV/BK_ACCEPT/BK_POLL/BK_FUTEX),
independent of other tids.  The single-threaded case is exactly one slot
in use, so the behaviour degenerates to today's.

A new tid is allocated a slot on first sight of its notification.  A
thread exit (SYS_exit of that tid) frees its slot.

### clone / clone3: know a thread exists before it blocks

The checker must know a thread is *running* so it does not declare
quiescence while a just-spawned thread is still computing toward its
first blocking point.  On a `clone`/`clone3` whose flags carry
`CLONE_THREAD`, record a pending birth (`th_pending`) and capture the
child's clear-tid address (`clone_args.child_tid` for clone3, arg 3 for
clone), then answer CONTINUE so the real thread is created.  A node is
not quiescent while `th_pending > 0`.  The first notification from a new
tid allocates its slot and clears one pending birth.

### futex: a parked thread is quiescent, a computing thread is not

Intercept `futex`.  A `FUTEX_WAIT` (all wait variants) reads `*uaddr`:

- if `*uaddr != val`, the real futex would return EAGAIN at once, so we
  answer EAGAIN and the thread keeps running (sound: we never park a
  thread the kernel would not have parked);
- else the thread parks in slot `BK_FUTEX` on address `uaddr`
  (deferred, not answered).

A `FUTEX_WAKE(uaddr, n)` is a real event from the waker's execution: it
wakes up to `n` threads parked in `BK_FUTEX` on `uaddr`, answering each
woken futex with 0, and answers the waker's futex with the count woken.
We do **not** invent wakes; a wake happens only when the program issues
one (or when a thread exits, see below).

On thread exit (`SYS_exit`) we clear the slot and wake any futex waiter
on the exiting thread's clear-tid address (the kernel's
`CHILD_CLEARTID`), so `pthread_join` completes.

Determinism and soundness of wake ordering: when `n >= waiters` (wake
all, the condvar-broadcast and single-waiter cases) or `waiters == 1`
(the single-waiter signal case) there is no ambiguity.  When a
`FUTEX_WAKE(n)` with `n < waiters` would wake a strict subset of several
same-address waiters, *which* subset wakes is a schedule this model does
not enumerate; rather than silently pick one and risk hiding a bug, the
checker errors.  That keeps the verdict honest and covers the
loop-plus-one-worker shape the eval asks for; many-waiters-on-one-word
contention (a blocking pool fighting for one lock) is the documented
boundary.

### Quiescence rule

A node is quiescent when it has no pending thread births and every live
thread is parked in a deferred owned call (recv/accept/poll) or in futex.
The whole world is a search state when every node is quiescent or exited.
A thread that is genuinely computing (not in any slot) still trips
`LULL_MS`; a compute thread must eventually block or exit, and a spinning
one is a real error.

### Fingerprint

The per-thread blocked state goes into `world_fp`, canonicalised so a raw
tid never leaks in.  Each live thread contributes its `(block_kind,
what-it-waits-on)` to a **sorted multiset** per node: thread identity is
the role/state, not the kernel tid, so two runs that assign different
tids to the same roles produce the same fingerprint.  A BK_FUTEX slot
contributes a canonical tag for its wait address (an index into the
node's seen-futex-word table, not the raw pointer), so two runs with
different ASLR'd condvar addresses still match.

## Red test

`test/proc/twothread.c`: a node whose main thread runs an accept/recv
loop and whose worker thread futex-waits for a job, then on wake does a
`write` that reports a value; plus a client node that sends the job.  An
invariant over the reported value holds for the correct build; `-DBUG`
drops data across the thread handoff and the checker catches it.  The
fixture dies with `LULL_MS` on today's engine (the red), checks on the
new one, and its distinct-state count is stable across two runs
(canonical fingerprint).

## Out of scope / boundaries

- `n < waiters` partial futex wakes on one address: an error, not a
  silent pick (above).
- `FUTEX_LOCK_PI`/`FUTEX_CMP_REQUEUE` and other PI/requeue ops: not
  modelled; an error if a target uses them.
- A thread that blocks in an unmodelled syscall (e.g. `pause`) still
  trips `LULL_MS`, as before.
