---
title: "Review of the race runtime"
parent: History and provenance
nav_order: 4
permalink: /background/race-review/
lede: >-
  Every happens-before edge cmc_race.c adds, checked against POSIX and
  C11, and the acquisition-only reduction checked against fixtures.
---

Date: 2026-11-19.  Subject: `src/cmc_race.c` and the choice points of
`src/cmc_preload.c` as released in 2.1.0 (`v2.1.0`, 9908acb), with the
design as `man/man7/cmc.7` (DESCRIPTION, the threads paragraphs) and
`CHANGELOG.md` 2.1.0 state it.  Reviewer: the 2.2 race worker, who did
not write the 2.1 runtime.  Host: floki, x86-64, glibc 2.44, gcc 16.2,
clang 21.1.

The question is the project's usual one: could cmc report PASS for a
program that has a data race, or that can reach a state the invariant
forbids?  Two parts of the design carry that answer.  (a) The runtime
must add exactly the happens-before edges POSIX and C11 promise.  An
edge they do not promise hides a race; an edge they promise and the
runtime lacks gives a false alarm.  (b) The search switches threads
only before acquisitions (a lock, a semaphore wait, a barrier, a once)
and atomics, never before a release, on the argument that in a
race-free program a release can move earlier and an acquisition later.

Every finding below comes with a fixture result.  A fixture is a small
program built with `-fsanitize=thread` and linked against
`libcmc-race.a`, run under `cmc --model-file` with an invariant
`phase < 2 or value == 1` and `eventually phase == 2`.  The ones kept
as gates are modes of `test/proc/syncedge.c`; the others are probes in
the review's scratch directory, described in enough detail here to
rebuild.  "2.1.0" means the runtime and preload of that tag; "fixed"
means `pr/2.2-race` after the commit that carries this file.

## Verdict

Part (a) had six edges that POSIX and C11 do not promise, each of which
hid a real race, and three calls whose accesses the runtime did not see
at all.  Part (b) is right for acquisitions that wait until they
succeed and wrong for two others: a try or timed acquisition that
fails, and a semaphore that admits more than one holder.  All of these
gave a PASS on 2.1.0 for a program with a race or a reachable bad
state.  Each is fixed (or, for part (b), made INCOMPLETE) with a gate
row that was red first.  Three smaller gaps remain and are listed at
the end.

| # | finding | 2.1.0 | fixed | gate |
|---|---|---|---|---|
| A1 | rwlock: a read unlock orders the next read lock | PASS (race missed) | data race | syncedge rwread |
| A2 | barrier: one round orders the next | PASS (race missed) | data race | syncedge bargen |
| A3 | failed compare-exchange acquires by the success order | PASS (race missed) | data race | syncedge casfail; casok holds |
| A4 | condition signal orders the woken wait | caught only by luck | no edge | race_rt |
| A5 | a seq_cst store acquires | caught only by luck | no edge | race_rt |
| A6 | `pthread_exit`, `thrd_exit` leave no exit clock | false alarm | edge | threadexit |
| A7 | `memcpy`, `memset`, `memmove` (gcc; any compiler with `_FORTIFY_SOURCE`) unseen | PASS (race missed) | data race | memcopy, memcopy-chk |
| A8 | bytes moved by `read`, `recv` and the like unseen | PASS (race missed) | data race | syncedge readbuf |
| A9 | `*_clocklock`, `*_clockwait`, `sem_clockwait` unwrapped | false alarm | edge | race_rt |
| A10 | `pthread_once` inside an initialiser loses the outer edge | false alarm | edge | race_rt |
| B1 | a failing trylock is never reached | PASS (outcome missed) | INCOMPLETE | syncedge trymutex, trysem, tryrw |
| B2 | a semaphore of count two: holders never overlap | PASS (race missed) | data race (and INCOMPLETE had it passed) | syncedge sem2 |

## The edges, one by one

POSIX.1-2017 XBD 4.12 (Memory Synchronization) lists the functions that
"synchronize memory with respect to other threads"; C11 7.17 and 7.26
give the atomics' and the C11 threads' synchronizes-with relations.
The runtime models each as a release into a per-object vector clock and
an acquire from it.  [source: src/cmc_race.c]

**Mutex, spinlock, C11 `mtx_*`.**  Unlock releases, a successful lock
or trylock acquires, a failed trylock adds nothing.  That is the
standard edge.  race_rt's scenarios "a mutex orders a write and a
read", "a failed trylock adds no edge" and the C11 twins pass.

**A1, rwlock.**  2.1.0 used one clock per rwlock: every unlock released
into it and every lock, read or write, acquired from it.  So a reader's
unlock ordered a later reader's lock.  POSIX promises no such edge:
two readers may hold the lock at once, and ThreadSanitizer keeps a
separate read clock for exactly this reason.  syncedge rwread has two
threads take the read lock; one writes x under it (the bug), the other
reads x.  2.1.0: PASS, 159 distinct states.  Fixed: the lock keeps a
writers' clock and a readers' clock; a write unlock releases into the
first, a read unlock into the second; a read lock acquires the first,
a write lock both.  An unlock does not say which hold it ends, so each
thread remembers the locks it holds for writing (up to eight; a ninth
is a table-full error, not a truncation).  Result: data race.

**Semaphore.**  `sem_post` releases, a successful wait acquires.  POSIX
lists both.  Correct as an edge; see B2 for the schedule.

**A4, condition variable.**  2.1.0 made `pthread_cond_signal` and
`pthread_cond_broadcast` (and the C11 forms) releases into the condition
variable, and every wait acquire from it after reacquiring the mutex.
POSIX does list the condition functions, but a wait may return without
any signal (a spurious wake-up, XSH `pthread_cond_wait`), so a correct
program can rely only on what it reads under the mutex.  An edge from
the signal hides a race on data written outside the mutex and read
after the wait.  ThreadSanitizer models no signal edge for the same
reason.  The probe (two threads wait under the mutex for a flag main
sets before a broadcast; once woken, each unlocks, one writes x and the
other reads it, and each then signals) was reported as a race on
2.1.0, on a schedule where neither signal came before the other's
access.  So the extra edge cost no gate here, but race_rt's scenario "a
condition signal orders a waiter" asserted it as correct.  Fixed: no edge from signal or broadcast; the wait's
edges are the mutex's.  race_rt now asserts that a signal alone does
not order, and that a write under the mutex before the signal does.

**A2, barrier.**  2.1.0 released into one clock per barrier at every
arrival and acquired from it at every return.  The search runs a woken
thread later, so a thread returning from round one acquired the clock
of an arrival at round two by a thread that had already run on.
syncedge bargen: two threads, a barrier of two used twice, and between
the rounds one writes x and the other reads it.  2.1.0: PASS, 5,188
distinct states in 76 s.  Fixed: `pthread_barrier_init` records the
count, an arrival belongs to round (arrivals / count), and each round
has its own clock by parity (round three cannot start before every
thread has arrived at round two, so after every return from round
one).  A barrier the runtime did not see initialised keeps one clock.
Result: data race.

**Once.**  The initialiser's completion releases, every caller
acquires; the standard edge.  **A10**: the initialiser's function and
object were kept in two thread-local variables, which a `pthread_once`
inside the initialiser overwrote, so the outer object's release went to
the inner object.  A false alarm, not a missed race (probe: an
initialiser of o1 that calls `pthread_once(o2)` then writes y; another
thread calls `pthread_once(o1)` and reads y; 2.1.0 reports a race).
Fixed by reading both into locals before the call.

**Create and join.**  `pthread_create` gives the new thread its
creator's clock; a join takes the joined thread's clock.  **A6**: the
clock was saved only when the start function returned, so a thread
ending by `pthread_exit` or `thrd_exit` left none, and the joiner's
read of its writes was reported as a race (threadexit modes exit, thrd
and nested all reported a race on 2.1.0).  The same fixture hit a
second obstacle: glibc's first `pthread_exit` loads libgcc_s for the
unwinder, an open and a read of a kernel fd that the checker refuses in
a multi-threaded node.  Fixed (b767841): a join takes the thread's
final clock from the thread table, and `__tsan_init` loads libgcc_s
while the node has one thread.  A detached thread's writes stay
unordered with everything after, as they should (probe: a detached
writer, then main reads after creating and joining another thread;
reported as a race on both versions).

A join that matches a `pthread_t` takes the clock of every table entry
with that id, and glibc reuses a joined thread's descriptor for the
next thread.  So joining the second thread also takes the first's
clock.  That clock is one the joiner already holds (it joined the
first), so no extra edge results; the probe (join T1, create T2 with
T1's id, create T3, join T2, read what T3 wrote) reports the race on
both versions.

**Atomics.**  Every atomic runs as seq_cst on the real machine and in
the search, and an order weaker than seq_cst makes a pass INCOMPLETE,
so a weak order can only cost a verdict, never fake one.  Within
seq_cst, C11 gives synchronizes-with from a release store (or RMW) to
an acquire load (or RMW) that reads it, and release sequences extend
it.  The runtime releases into a per-address clock on every write and
acquires on every read, which over-approximates release sequences (a
plain store by a third thread breaks one; the runtime keeps the earlier
release in the clock).  The probe (A stores x then f = 1, B stores f =
2, C reads f == 2 then x) is still reported, since another schedule
shows the race, but the over-approximation is real and is listed under
what remains.  **A5**: 2.1.0 also made a store acquire.  A store reads
nothing, so it acquires nothing; the probe (A: x = 1, store f; B: store
f, read x) was caught on another schedule.  Fixed: stores do not
acquire.  **A3**: a compare-exchange that fails is a load with the
failure order (C11 7.17.7.4), but 2.1.0 ignored the failure order and
acquired by the success order whether or not the exchange succeeded,
and did not report a relaxed failure order as weak.  syncedge casfail:
the writer stores the payload, then sets the flag; the reader's
compare-exchange (seq_cst success, relaxed failure) fails on the flag
and the reader reads the payload.  2.1.0: PASS.  Fixed: a failed
exchange acquires by its failure order, and a failure order below
seq_cst is reported weak.  Result: data race; casok (seq_cst failure)
holds.

**A9, the clock forms.**  `pthread_mutex_clocklock`,
`pthread_rwlock_clockrdlock`, `pthread_rwlock_clockwrlock`,
`sem_clockwait` and `pthread_cond_clockwait` (POSIX 2024, glibc 2.30)
were not wrapped, so the edge their lock carries was missing: a false
alarm (probe: a clocklock-protected write and a lock-protected read,
reported as a race on 2.1.0).  Fixed: wrapped like their timed forms.
They are timed acquisitions, so they also make a pass INCOMPLETE under
B1.

**A7 and A8, accesses the runtime did not see.**  These are not edges
but they decide the same question.  The instrumentation turns a plain
access into a `__tsan_read*` or `__tsan_write*` call, but gcc leaves
`memcpy`, `memset` and `memmove` as plain calls, and with
`_FORTIFY_SOURCE` (on by default in nixpkgs, Fedora and Ubuntu
compilers) both gcc and clang emit `__memcpy_chk` and its siblings.
memcopy: a worker copies into a buffer main reads unlocked.  2.1.0:
PASS with gcc, and PASS with clang when fortified; only clang without
fortify reported it.  Fixed (a679188).  Likewise the bytes a `read`
stores into a buffer: syncedge readbuf has one thread read an eventfd
into a buffer the other reads with a plain load.  2.1.0: PASS, 60
states.  Fixed: the runtime defines `read`, `write`, `pread`, `pwrite`,
`recv`, `send`, `recvfrom`, `sendto`, `readv`, `writev`, `recvmsg`,
`sendmsg`, `poll` and `epoll_wait` in the executable, with the
fortified `__read_chk`, `__pread_chk`, `__recv_chk`, `__recvfrom_chk`
and `__poll_chk` that `_FORTIFY_SOURCE` makes of them (the first clang
run of the gate missed the race because clang had emitted
`__read_chk`), and records the bytes each moved.

## The acquisition-only reduction

The argument, from `src/cmc_preload.c` and cmc(7): in a program free of
data races, a release commutes left past the accesses before it and an
acquisition right past those after it (Lipton's movers), so every
schedule is equivalent to one that switches threads only before
acquisitions.  The race runtime checks freedom from races on every
explored path, so the reduction's premise is checked, not assumed.

The movers argument holds for an acquisition that waits until it
succeeds: it is a right mover because nothing another thread does can
change its outcome, only delay it.  It fails for two kinds of call.

**B1, an acquisition that can fail.**  `pthread_mutex_trylock` returns
EBUSY when another thread holds the lock.  That outcome needs the other
thread to be between its lock and its unlock, and the search never
switches threads there: the holder's section runs from its lock's
choice point to its next one without a switch.  A failed trylock does
not commute with the release that would have let it succeed.  syncedge
trymutex: A locks and unlocks; B trylocks and records whether it
failed; the invariant says it never fails, which a real host refutes.
2.1.0: PASS, 159 states, with the race runtime (the uninstrumented
build was INCOMPLETE by the existing rule).  trysem (`sem_trywait`)
and tryrw (`pthread_rwlock_tryrdlock` against a writer) behave the
same.  Timed acquisitions fail the same way, by timing out.

**B2, a semaphore that admits two.**  With a count of two, both threads
can be inside `sem_wait ... sem_post` at once.  The search runs one
thread from its wait to its next choice point, which is past its post,
so the two sections never overlap.  syncedge sem2 (each thread waits,
touches x, posts; one writes and one reads): 2.1.0 PASS.  The race
runtime cannot see the race on the schedules searched, because on each
of them the later thread's wait acquires the earlier's post.

Two fixes were measured.  Marking every release as a choice point (the
preload wrapping unlock and post) catches both, but the trees grow:
twothread went from 9,362 worlds in 235 s to past 600 s, twothread_join
from 3,301 worlds in 60 s to past 600 s, raceemit from 2,437 to 10,228,
syncpoint from 56 to 876 [measured, floki, load about 35 on 48 cores].
The fix taken instead keeps the choice points and gives up the verdict
where the reduction does not hold: the runtime tells the checker
(`RACE_TRY`) the first time a try, timed or clock acquisition runs in
a node with more than one thread, or a semaphore wait succeeds and
leaves the count above zero; the checker then turns a would-be pass
into INCOMPLETE.  A violation found is still a violation.  The marker
is a `sched_yield` the checker answers, so in a node with more than one
thread it is itself a choice point: the first semaphore wait that
leaves room is followed by a switch, the second holder runs inside the
first one's section, and sem2's race is reported (6 actions).
trymutex, trysem and tryrw are INCOMPLETE.  The existing fixtures' counts are
unchanged (twothread 9,362, twothread_join 3,301, raceemit 2,437,
raceborn 412, syncpoint 56, racewake 78).

The rest of the argument holds.  A condition wait reacquires its mutex
inside the C library with no marker, but it returns only after a futex
wake, which the search orders.  A join waits on a futex the exiting
thread's clear-tid wakes, and the exit is a choice point.  `pthread_once`
blocks a second caller until the first's initialiser finishes, so it
behaves as a waiting acquisition.  A barrier blocks until the round is
full.

## What remains

- Spurious wake-ups are not explored: the checker invents no futex
  wakes (cmc(7)), so a condition wait without a predicate loop, which
  Linux may wake early, is run only as woken by a signal.  The probe for
  it spins and makes the tree endless, so it has no fixture.  This is a
  scheduler limit, not a runtime one.
- Release sequences are over-approximated: a release stays in an
  atomic's clock after a later plain store by another thread, which C11
  says ends the sequence.  It can hide a race only in a program whose
  atomics another schedule does not expose; no fixture found one.
- C library routines other than the memory, string and format writers
  the runtime defines (`memccpy`, `qsort`, `fread`, `strtok` and the
  like) are not seen, and a format's `%s` arguments are not recorded as
  reads.  gcc expands a `strcpy` or `memcpy` of a short constant into
  plain stores it does not instrument, even at `-O0` and with
  `-fno-builtin`; clang instruments them.  Stock libtsan with gcc misses
  the same race.
- Other system calls that write program memory (`ppoll`, `epoll_pwait`,
  `getsockopt`, `accept`'s address, `clock_gettime`) are not recorded.
  Their buffers are rarely shared between threads, but an access to
  them is still unseen.
