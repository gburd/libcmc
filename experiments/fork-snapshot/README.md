# Fork-from-snapshot (feasibility)

The process front end (`src/cmc_proc.c`) explores each state by
**replaying from launch**: it starts all N node processes again and
re-drives them through the whole action prefix.  Measured on the Paxos
fixture with one crash and one drop, launch is about a third of the time
and re-driving the prefix the other two thirds -- so a faster launch
alone caps the win at ~1.5x.  The real cost is re-running node code over
the prefix for every state; an exhaustive search is O(states x depth)
node-steps.

**Fork-from-snapshot** would make expanding a state O(1) node-steps:
at a quiescent point, freeze a copy of every node process; to explore an
action, resume a copy, apply the action, drive to the next quiescence;
to explore a sibling action, resume another copy of the frozen point.
The search becomes O(states) node-steps.

The copy must be a true copy, not a hand-rolled memory capture: if the
checker tried to snapshot "all writable memory" of a node and missed a
page, a sibling exploration would see stale or mutated bytes and the
checker could give the wrong verdict.  That is the silent unsoundness
the project forbids.  So the snapshot is a `fork()` -- the kernel copies
the whole address space, correctly, copy-on-write.

The obstacle: a quiescent node is suspended **in the kernel** on a
seccomp user-notification (its `read`/`accept` unanswered), so it cannot
call `fork()` itself.  The checker must inject one.

## What `probe.c` establishes

`probe.c` is a standalone, libc-only test of the one mechanism this
rests on.  A child installs a seccomp user-notification filter on
`write(2)`, keeps a counter in a fixed-address heap cell, and blocks in
`write()` at each step; the parent is the checker.  It shows, on this
kernel (Linux 6.x, x86-64):

- a node blocked in `SECCOMP_RET_USER_NOTIF` can be read with
  `process_vm_readv`, and its heap cell holds the quiescent-point value
  (11), as the real checker already relies on;
- **`PTRACE_INTERRUPT` brings that kernel-suspended node to a ptrace
  group-stop** (`WIFSTOPPED`, `SIGTRAP`), which was the open question;
- at that stop the registers read cleanly: `orig_rax = 1` (the `write`
  it is blocked in), `rax = -ERESTARTSYS`.  That is a clean instruction
  boundary from which to rewrite the registers to call `fork(2)`,
  single-step, recover the new pid, and restore the registers so the
  original is back at the blocked `write()` and the fork is a frozen
  duplicate at the same point.

So the approach is feasible.  The probe runs the whole cycle on a quiet
host (`exit 0`, the `OK:` line): it injects `fork` by register rewrite
(`orig_rax`/`rax` = `SYS_fork`, `rip` back onto the syscall
instruction, single-step), catches the duplicate with
`PTRACE_EVENT_FORK`, reads both heaps (each 11), freezes the snapshot,
runs the original on, and confirms the snapshot's heap is still 11 --
an independent copy, undisturbed by the original.

**The engine finding from the probe:** once the node is driven through
the ptrace excursion, its original `write()` notification is
*abandoned* -- answering it later gives `ENOENT`.  So a resumed node
does not keep its in-flight notification; it **re-issues** the blocked
syscall and raises a *fresh* notification, which the checker picks up
normally.  That is cleaner than preserving an in-flight notif, and it
sets the engine's resume contract: to resume a snapshot, restore its
registers to the syscall-instruction boundary (`saved.rip - 2`, the
saved `write` arguments) and let it re-run, then `NOTIF_RECV` the new
notification.

```
cc -O2 -o probe probe.c && ./probe     # Linux x86-64; run on a QUIET host
```

A heavily loaded host starves the seccomp/ptrace handshake and the
probe times out midway (it still prints the facts above first).

## The snapshot chain works (`chain.c`)

`refork.c` found that re-forking **one** snapshot repeatedly did not
work.  `chain.c` resolves it with the right model, proven on Linux
x86-64: a frozen master is forked **exactly once**, yielding a worker
(the parent, resumed for this action) and a next frozen master (the
child).  The DFS holds one frozen master per stack level and chains
forward; no master is re-forked.  Three mechanics the single-snapshot
probe did not show:

1. Fork once -> (worker, next master).  The child is auto-attached
   (`PTRACE_O_TRACEFORK`) and left stopped at the syscall boundary -- a
   pristine snapshot, re-forkable in turn.
2. Resume a worker with `PTRACE_CONT`, **not** `PTRACE_DETACH`: it must
   stay our tracee or `process_vm_readv` gives `EPERM` under yama
   `ptrace_scope = 1` (a detached grandchild is not readable).
3. Zero the `seccomp_notif` buffer before **every** `NOTIF_RECV`, or the
   ioctl gives `EINVAL`.

Each worker re-issues its blocked `write()` and raises a fresh
notification the checker services; every chained master stays pristine.
`chain.c` prints `OK` for three workers.  This is the gate that was
missing; the engine integration follows it.

## The open bug, diagnosed precisely (2026-11-10)

The `--fast` snapshot-DFS is wired in (`src/cmc_proc.c`, off `main`, off
by default).  On the `repl` fixture (two nodes, each blocked in
`accept()` at the root) it fails: after `resume_world` forks a snapshot
and `PTRACE_CONT`s the worker, the worker does **not** re-trap its
blocked `accept()` to the seccomp listener.  Tracing every
`NOTIF_RECV` shows the worker instead running a real `read()` on a real
fd (~500000, a pid-range number), which the checker's connection map
(`cfd`/`cpeer`) does not know -> `die("node 0 reads fd N, which is no
connection")`.  The restored node shows `nc=0`, `next_fd=501`,
`block_kind=BK_ACCEPT`, `saved.orig_rax=43` (accept), `saved.rax=-512`
(ERESTARTSYS) -- so the snapshot point is correct, but the resume does
not re-raise the accept notification.

`chain.c` resumes a worker blocked in `write(777)` and it DOES re-trap;
the engine's `accept()` case does not.  The difference to chase: a
re-issued `accept()` that was mid-flight (ERESTARTSYS) at snapshot time
behaves differently under the fork-injection + register restore than a
clean `write()` -- the resumed worker proceeds into the kernel instead
of trapping to the filter's `USER_NOTIF`.  Next step: a minimal probe
that freezes a node blocked in `accept()` (not `write()`), forks it, and
checks whether the re-issued `accept()` notifies the held listener;
reproduce chain.c's success for `accept` before touching the engine.

Until that probe is green, `--fast` must not ship; BFS replay stays the
sound default and the oracle.  Note the north-star work (the abstract
model + binding + refinement checker, Milestones A-C in
`docs/plans/tlaplus-scope.md`) does NOT depend on `--fast`; only liveness
(F) and scale (G) do.

## Still to do in `src/`

The `--fast` snapshot-DFS is wired into `src/cmc_proc.c` on the branch
`m9-fast-wip` (off `main`, off by default, not sound yet).  Two results
from integrating it:

- **Resume is by the kernel's syscall-restart, not a hand rewind.**  The
  single-snapshot probe suggested rewinding `rip` to the syscall
  instruction.  In the engine that makes the resumed task take `SIGSYS`
  and die.  The correct resume restores the *exact* ptrace-stop
  registers (`rip` just past the syscall, `rax = -ERESTARTSYS`) and lets
  the kernel re-issue the interrupted syscall itself; it then raises the
  fresh notification.  Verified: a one-node `accept()`-blocked worker,
  resumed, re-traps to the notification (process state S) instead of
  running on.
- **Open bug: connection fds across a resume.**  Through the full
  `dfs()`, a resumed node ends up reading a *real* fd (~75000) that the
  checker's connection map does not know, instead of a checker-assigned
  virtual fd (>= 500).  The re-issued syscall of a connection-mediated
  node is escaping interception / fd bookkeeping in the forked copy.
  This must be resolved before `--fast` gives a verdict on any
  multi-connection fixture.

Then, before `--fast` can ship (still off by default):

- manage a tree of frozen snapshots and reap them without zombies or fd
  leaks; drain the shared fd-3/fd-4 canon pipes when switching copies
  (forked copies inherit one pipe, confirmed), with one live descendant
  at a time so the single seccomp listener fd stays valid;
- gate it against the replay engine: a fork-snapshot run must give
  byte-identical distinct-state counts and the same counterexamples as
  stateless replay on every `test/proc/` fixture, and be measurably
  faster.  (The replay engine stays as the oracle it is checked
  against.)

## The --fast hang, diagnosed further (2026-11-10, v0.4.0 branch)

With the resume mechanic aligned to acctest.c (rewind rip-2, rax =
orig_rax), the "reads a real fd" error is gone, but `--fast` now
LIVELOCKS in drive_quiescent on the repl fixture.  The precise finding:

- freeze_master captures the right boundary: rip at the `accept`
  syscall, orig_rax = 43, rax = -512 (ERESTARTSYS).
- Just before PTRACE_CONT, the resumed worker's registers are PERFECT:
  rip points at the `0f 05` (syscall) instruction, rax = 43, orig = 43.
  So the worker is positioned to re-issue exactly `accept`.
- Yet after CONT, the checker's listener receives the node's ENTIRE
  startup sequence again -- read/close, getrandom, the preload
  constructor's clock_gettime magic (nr=228 a0=0x434d43), write(fd3),
  socket/bind/listen/accept.  The preload CONSTRUCTOR re-running means a
  process is starting from the top, not resuming mid-accept.

So: correct registers on the worker, but the notifications servicing
drive_quiescent come from a process running main() from entry.  The
contradiction (right regs, wrong execution) points at a process-identity
confusion -- the worker whose regs we set is not the one whose notifs
arrive on the listener, OR a fresh launch is happening (run_seq?)
concurrently.  acctest.c (a trivial accept loop) does not hit this; repl
(a long startup before the snapshot) does.

RESOLVED the identity question: the post-resume notifs DO come from the
worker pid (notif.pid == N.pid == worker), and no stray relaunch
happens.  The sharper finding: the worker, after resume, raises nr=0
(read), NOT nr=43 (accept) -- so it is executing PAST the accept it was
frozen in, into the node's next syscall (the read of an accepted
connection).  So the register rewind re-issues accept, the accept is
somehow satisfied immediately (not deferred as BK_ACCEPT), and the node
runs on to read, then the drive_quiescent loop churns (b1->b0) because
the restored N[].pend keeps a connection the re-issued accept consumes.

So the defect is the interaction between the re-issued accept and the
RESTORED connection/pending state (N[].pend, N[].cfd): after a COW fork
the worker's in-kernel/socket state and the checker's restored
bookkeeping are not consistent for a connection-mediated node the way
they are for acctest.c's standalone accept loop.  NEXT: make resume
re-establish the blocked state by RE-DEFERRING the re-issued accept
(ignore restored pend until the node re-traps), i.e. treat the first
post-resume syscall of each worker as the fresh boundary and rebuild
N[].pend/cfd from what the worker actually re-issues, rather than
trusting the snapshot's restored connection maps.


## Sharpest diagnosis (2026-11-10, part 2)

Instrumented the post-resume path precisely:
- The initial run_seq drive reaches the accept boundary correctly (both
  nodes block BK_ACCEPT, drive_quiescent breaks).
- After resume, the worker's registers at PTRACE_CONT are perfect (rip on
  the `0f 05` syscall instruction, rax = orig_rax = 43 = accept).
- Yet the FIRST notification the worker raises post-resume is
  `nr=0` read with `a0 = 0xffffffffffffffff` (read(-1)) -- serviced
  immediately after CONT (acctest-style) gives the same.  So the worker
  is already PAST accept, reading fd -1 (accept returned -1).
- answer() is never called post-resume and the accept-defer branch never
  runs post-resume -- so the re-issued accept was NOT deferred as
  BK_ACCEPT and NOT answered by the checker.  It appears to have run and
  returned -1 (as a kernel accept on the virtual, non-real fd 500 would).

So: perfect registers, but the re-issued accept is not intercepted/
deferred the way acctest.c's is -- it returns -1 and the node proceeds to
read(-1).  acctest.c (filter traps only accept; node loops accept) does
NOT reproduce this; repl (socket/bind/listen then accept, all intercepted
to virtual fds) does.  The leading hypothesis: the forked worker's
re-issued accept is reaching the KERNEL (not the seccomp USER_NOTIF
listener) -- i.e. the COW fork does not preserve the seccomp-filter ->
our-listener association for this process the way a fresh notif would, OR
the fork-injection excursion perturbs the filter state.  This contradicts
acctest.c, so the real variable is the richer syscall history / virtual
fds.  NEXT: compare the seccomp filter/notify state of a forked worker
that re-issues accept in the repl setup vs acctest (e.g. SECCOMP_
IOCTL_NOTIF_ID_VALID on the id; check /proc/worker/status Seccomp field;
confirm the worker's accept(500) is or isn't producing a USER_NOTIF).

STATUS: this is a deterministic logic/sequencing bug, not host noise -- a
quiet or EC2 host does not change it.  It remains parked off main; the
north-star milestones (A-F, shipped in v0.4.0) do not depend on --fast.


## BREAKTHROUGH (2026-11-10, part 3): the mechanic works -- it's an engine bug

fdtest.c settles the question.  It reproduces the engine's EXACT node
pattern -- intercepted socket -> bind -> listen -> accept, all answered
with virtual fds (socket = 500; the accept deferred and never answered,
as the engine does) -- then freezes the accept-blocked node and
fork-resumes it exactly as the engine does.  On a quiet Linux x86-64 box
with ptrace_scope=1, the resumed worker correctly re-raises nr=43
(accept).

So the full mechanic works even with the rich virtual-fd syscall history
that distinguishes repl from acctest.c.  The engine's --fast livelock
(worker ends at read(-1), its accept neither deferred nor answered) is
NOT a ptrace/seccomp limitation -- it is an ENGINE SEQUENCING bug.  The
difference from fdtest.c / acctest.c: both service a worker's re-issued
notification IMMEDIATELY after its PTRACE_CONT, whereas resume_world in
the engine CONTs ALL workers and THEN calls drive_quiescent.  Between
CONT(n0) and the RECV, the engine forks n1 (fork_master does
PTRACE_CONT + waitpid on n1's master), which very likely consumes or
races n0 worker's ptrace/notification event, so n0's re-issued accept is
not received and deferred -- it escapes to the kernel and returns -1.

THE FIX (bounded, next step): in resume_world, for each node, CONT its
worker and immediately poll+RECV+defer its re-issued blocked syscall
(probe-style) BEFORE forking the next node -- or fork all next-masters
first, then CONT and service workers one at a time.  Do not interleave a
fork_master's ptrace excursion with a live, un-serviced worker.  With
that, --fast should reach a verdict; then gate it byte-identical vs BFS
on every fixture before it ships (still off by default).


## Part 4 (2026-11-10): two-phase resume fixes the hang; now a protocol-state mismatch

Applied the fdtest.c fix to resume_world: Phase 1 forks ALL next-masters
first (no worker live during a fork_master excursion); Phase 2 then
PTRACE_CONTs each worker and services its re-issued notification
immediately (poll + try_pending_for), one at a time.  Result on repl:
the LIVELOCK/HANG is GONE -- resume now reaches the fingerprint
self-check, which correctly reports "a resumed world did not match its
snapshot".  So the engine-sequencing bug is fixed; the trajectory is
hang -> clean self-check failure (the sound guard doing its job).

The remaining mismatch: at the self-check the resumed nodes are
alive=0 (exited, code 0) while the snapshot had them alive=1 blocked in
accept.  So each worker, after resume, runs its protocol to COMPLETION
(main returns 0) instead of re-blocking at the snapshot's accept.  The
canon bytes match (0 vs 0); only aliveness differs.  So the worker's
re-issued accept is being ANSWERED (not left deferred) and the node runs
its whole accept->read->...->accept loop until accept returns <=0 and it
breaks out of main.

NEXT: find why Phase 2's try_pending_for answers (rather than only
defers) the worker's re-issued accept -- likely it is draining a stale
queued notification (a pre-snapshot read/close) or the restored N[].pend
feeds an auto-accept.  The fix is to service EXACTLY the one re-issued
blocked syscall per worker and stop (leave it deferred, blocked=1),
matching the snapshot's blocked state, rather than pumping
try_pending_for until the node advances.  The mechanic and the
sequencing are now sound; this is the last bookkeeping step.


## Part 5 (2026-11-10): the remaining divergence from fdtest.c

With two-phase resume, Phase 2 services each worker's first post-resume
notification -- but it is `nr=0 read(fd=-1)`, NOT `nr=43 accept`, even
though freeze_master captured orig_rax=43 (accept) and the worker's regs
at CONT were rewound to re-issue accept.  So in the ENGINE the resumed
worker is at read(-1) (accept already returned -1), while fdtest.c -- the
same socket/bind/listen/accept-to-virtual-fds pattern, isolated -- has
its resumed worker correctly re-raise accept.

So the divergence is now localized to freeze_master/the engine's
snapshot of a node with a long prior history vs fdtest.c's node frozen
cleanly right after its accept notif.  Hypotheses to check next:
- Does freeze_master's PTRACE_INTERRUPT catch the engine node at the
  true accept boundary, or mid-glibc-wrapper?  Compare the exact rip and
  the bytes at rip-2 for the engine node vs fdtest.c (fdtest.c's rip-2 is
  0f05 = syscall; confirm the engine's is too AND that it is the ACCEPT
  syscall site, not a read site inside glibc's accept).
- a0=0xffffffff(-1) as a read fd strongly suggests the node's accept
  returned -1 BEFORE the snapshot, i.e. the snapshot boundary is actually
  a read, not the accept -- check whether block_kind=BK_ACCEPT is stale
  (set at an earlier accept) while the node had already moved to a read.
The two-phase sequencing fix stands (committed); this is a freeze-boundary
question, isolated and concrete.


## Part 6 (2026-11-10): freeze boundary is CORRECT; divergence is engine-state, not mechanic

Confirmed the freeze boundary is right and identical to fdtest.c's:
engine node at freeze = block_kind=BK_ACCEPT, orig_rax=43, rax=-512
(ERESTARTSYS), rip-2 = 0f05 (syscall), rdi=500 (the listen fd).
fdtest.c's frozen node is byte-identical (orig_rax=43 rax=-512), and ITS
resumed worker re-raises accept correctly, while the engine's resumed
worker's first notification is read(fd=-1) (accept already returned -1,
so it was not intercepted).

Ruled out: (a) stale block_kind -- the boundary is genuinely accept;
(b) syscall-restart-skips-seccomp -- fdtest has the SAME ERESTARTSYS
restart state and works; (c) PTRACE_SINGLESTEP-for-fresh-entry before
CONT -- tried, no change.

So with identical freeze registers and an identical fork_master/resume
mechanic, fdtest.c re-traps accept but the ENGINE does not.  The
remaining variable is the engine's surrounding state that fdtest.c lacks:
the shared fd-3/fd-4 canon-pipe draining in resume_world, the restored
checker globals (bag, N[].pend with the client connection, cfd/next_fd),
or the two-node interaction.  The worker's accept returning -1 (not
trapping) despite the forked worker HAVING the seccomp filter
(Seccomp: 2, Seccomp_filters: 1) is the specific unexplained fact.

STATUS: the mechanic is proven achievable (fdtest.c); the hang is fixed
(two-phase resume, committed); the divergence is localized to engine
state but its last cause is not yet pinned after extensive probing.
This has consumed many sessions.  Recommendation: keep --fast as a
documented experimental branch; do not block anything on it.  A fresh
attack should bisect by making fdtest.c incrementally more like the
engine (add a second node; add the canon pipes; restore a client-pending
and see which addition breaks the accept re-trap) -- that will isolate
the exact engine-state element responsible.


## Part 7 (2026-11-16): the accept-escape ISOLATED to `read` (syscall 0) in the filter

A fresh attack ran the README's recommended bisection: start from
fdtest.c (which re-traps accept correctly after fork-resume) and make it
incrementally more engine-like until the accept re-trap breaks.  The
break was isolated to a single element, and it is not any of the ones the
prior parts guessed (second node, canon pipes, restored client-pending).

**The isolated cause: the seccomp USER_NOTIF filter intercepting `read`
(x86-64 syscall number 0) breaks the fork-resumed `accept` re-trap.**
With `read` in the filter, a fork-injected worker resumed to re-issue its
blocked `accept` does NOT re-trap to the listener: the accept returns an
error and the node proceeds to `read(fd=-1)` -- exactly the engine's
`--fast` divergence.  Remove `read` from the filter (or replace it with
any other syscall) and the same mechanic re-traps accept correctly.

### The bisection (every run `timeout 30`, `pkill -9` after; quiet EC2 box,
### Linux 7.0.0-1014-aws x86-64, ptrace_scope=1)

The node is always the same: intercepted socket -> bind -> listen ->
accept (socket answered 500, accept deferred), frozen at the accept,
fork-injected and resumed exactly as the engine does.  Only the filter
and the resume path vary.

| probe | filter USER_NOTIF set | fork? | resume path | resumed worker raises |
|-------|-----------------------|-------|-------------|-----------------------|
| fdtest.c | socket,bind,listen,accept,accept4 | yes | rewind rip-2 + CONT | **nr=43 accept (PASS)** |
| b5 | same 5 (no read) | yes | rewind rip-2 + CONT | **nr=43 accept (PASS)** |
| b9 | +close (6, no read) | yes | rewind rip-2 + CONT | **nr=43 accept (PASS)** |
| b11 | +write (nr 1) | yes | rewind rip-2 + CONT | **nr=43 accept (PASS)** |
| b12 | +recvfrom (nr 45) | yes | rewind rip-2 + CONT | **nr=43 accept (PASS)** |
| b4 | +read (nr 0) | yes | rewind rip-2 + CONT | nr=0 read(-1) (**FAIL**) |
| b13 | +read, placed FIRST | yes | rewind rip-2 + CONT | nr=0 read(-1) (**FAIL**) |
| b7 | +read (nr 0) | **no** | rewind rip-2 + CONT | **nr=43 accept (PASS)** |
| b8 STRAT 0-3 | +read | yes | rewind / exact-restart / singlestep / PTRACE_SYSCALL | all nr=0 read(-1) (**FAIL**) |
| b16 WITHREAD=1 | full engine set (26 incl read) | yes | rewind rip-2 + CONT | nr=0 read(-1) (**FAIL**) |
| b16 WITHREAD=0 | full engine set MINUS read | yes | rewind rip-2 + CONT | **nr=43 accept (PASS)** |

Reading the table:

- **Presence of `read` in the filter is the single discriminator.**  Every
  passing row lacks `read`; every failing row has it.  Adding any other
  syscall (close, write, recvfrom) does not break it, so it is not "one
  more intercepted syscall" or filter size.  Position is irrelevant
  (b13: read first in the filter fails identically).
- **It needs BOTH `read` in the filter AND the fork injection.**  b7 keeps
  `read` in the filter but skips the fork (just INTERRUPT, rewind, CONT);
  the accept re-traps.  So neither element alone breaks it.
- **No register-restore or resume strategy avoids it** (b8): rewinding to
  the syscall instruction, restoring the exact frozen registers and
  letting the kernel's own restart re-issue accept, single-stepping once
  first, or PTRACE_SYSCALL to the entry stop -- all four leave the accept
  returning an error.  This confirms Part 6's "singlestep tried, no
  change" and extends it: the problem is structural, not register tuning.
- **It holds at engine scale** (b16): with the full 26-syscall engine
  filter the accept fails with `read` present and passes with it removed.

### The mechanism, as far as it is pinned

b15 (PTRACE_SYSCALL to the resumed worker's syscall-entry stop) shows
`orig_rax=43` -- the kernel's restart IS re-issuing accept, not read.  But
the accept does not trap to the USER_NOTIF listener: it stops at a normal
ptrace syscall-entry (not a notification), runs, returns -1, and the node
moves to `read(-1)`.  So the re-issued accept's seccomp USER_NOTIF action
is bypassed/failed for the forked worker.

`seccomp_unotify(2)` documents the matching kernel behaviour: when the
supervisor is gone (the listener no longer services the notification),
"the kernel causes the target process's system call to fail with the
error ENOSYS".  A single-stepped accept in b6/b10 returns exactly -38
(ENOSYS), confirming that error path (though single-step itself disables
the notify, so that observation is only corroborating, not the CONT-path
mechanism).

Why `read` (syscall 0) specifically triggers this listener-invalid state
for a COW-forked worker, when no other intercepted syscall does, is a
Linux-kernel-internal question this effort did NOT settle.  It would need
reading the x86-64 syscall-restart path and the seccomp-notify
per-filter listener bookkeeping to see how a forked task's restarted
accept resolves the notification when filter index 0 is `read`.  Pinning
that is the next step for anyone who wants to fix (rather than document)
`--fast`.

### Reproducing

The committed probes map to the table rows (short labels above are the
session names):

| table row | committed source | expect |
|-----------|------------------|--------|
| fdtest.c | `fdtest.c` | PASS (accept) |
| b5 | `bisect_noread_fork.c` | PASS |
| b9 | `bisect_close_fork.c` | PASS |
| b11 | `bisect_write_fork.c` | PASS |
| b4 | `bisect_read_fork.c` | FAIL (read(-1)) |
| b13 | `bisect_read_first.c` | FAIL |
| b7 | `bisect_read_nofork.c` | PASS |
| b16 | `bisect_engine_filter.c` (arg 1/0) | FAIL / PASS |

```
# the two decisive cases
cc -O2 -o rf bisect_read_fork.c   && timeout 30 ./rf     # -> nr=0 read(-1)  FAIL
cc -O2 -o nf bisect_noread_fork.c && timeout 30 ./nf     # -> nr=43 accept   PASS

# engine-scale filter with read toggled (1 = include read, 0 = exclude)
cc -O2 -o ef bisect_engine_filter.c
timeout 30 ./ef 1        # full engine filter WITH read    -> nr=0 read(-1)  FAIL
timeout 30 ./ef 0        # full engine filter WITHOUT read  -> nr=43 accept   PASS
```

Run every probe under `timeout 30` with `pkill -9 -f <prog>` after: the
fork/ptrace handshake hangs without a timeout on a loaded host.

### Verdict: keep `--fast` experimental; do NOT ship it

Two independent reasons, either sufficient:

1. **The cause is isolated but not fixed, and the fix is unknown.**  We
   know the exact trigger (`read` in the USER_NOTIF filter + fork
   injection) and that no resume-register strategy avoids it.  A real fix
   needs either a kernel-level understanding of the syscall-0 restart
   path or a redesign that does not fork-inject a task blocked in a
   notification whose filter intercepts `read`.  The engine MUST intercept
   `read` (it mediates every connection byte), so "just drop read from the
   filter" is not available.  This is a bounded research step, not a
   bounded engineering step.

2. **The current engine has moved past `--fast`'s single-threaded
   assumption.**  `m9-fast-wip`'s `--fast` snapshots one `pid` per node
   with one saved register set (`WorldSnap.saved[MAXNODES]`).  `origin/main`
   (baa91a6) now models a node as up to `MAXTHREAD` threads, each with its
   own kernel tid, blocking point, and futex-wake serialisation
   (`Thread th[MAXTHREAD]`, `BK_FUTEX`, `release_one_woken`), plus a
   persistent per-epfd epoll interest list and `clone`/`clone3` birth
   tracking.  A correct snapshot must now freeze and fork EVERY thread of
   every node at a consistent point and restore the checker's per-thread
   slots, `th_pending` births-in-flight, and the woken-futex release order
   -- byte-identically.  Porting `--fast` onto that is a major re-port, not
   a rebase, and it inherits the unsolved `read`-escape on top.

The sound BFS replay stays the default and the oracle.  Nothing in the
north-star milestones depends on `--fast`; only scale (G) and liveness (F)
would benefit, and both can wait for a sound snapshot engine or a
different acceleration (e.g. replay-prefix caching) that does not need
fork-injection.
