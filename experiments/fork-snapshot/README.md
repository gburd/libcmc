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

- manage a tree of frozen snapshots and reap them without zombies or fd
  leaks; drain the shared fd-3/fd-4 canon pipes when switching copies
  (forked copies inherit one pipe, confirmed), with one live descendant
  at a time so the single seccomp listener fd stays valid;
- gate it against the replay engine: a fork-snapshot run must give
  byte-identical distinct-state counts and the same counterexamples as
  stateless replay on every `test/proc/` fixture, and be measurably
  faster, before it becomes the default.  (The replay engine stays as
  the oracle it is checked against.)
