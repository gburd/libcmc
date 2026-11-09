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

## Still to prove before touching `src/`

`refork.c` tests the step the DFS search needs: re-forking **one** frozen
snapshot repeatedly, resuming each copy to re-issue its blocked syscall.
It surfaced two mechanics the single-snapshot `probe.c` did not:

- A snapshot made by fork injection is left **already ptrace-stopped and
  attached** (`waitpid` reports `PTRACE_EVENT_STOP`, `GETREGSET`
  succeeds).  So to re-fork it you read its registers directly -- a
  second `PTRACE_INTERRUPT` waits for a stop that never comes and hangs.
- Re-forking that snapshot a *second* time currently fails: the first
  re-fork drives the snapshot through a `fork` syscall and restores its
  registers, but it does not return to the same re-forkable stopped
  state, so the next `GETREGSET`/inject does not take.  And a resumed
  copy, detached and let run, did not raise a serviceable fresh
  notification on the shared listener in the test harness.

So the DFS engine design -- freeze a quiescent world once and re-fork it
once per enabled action -- is not yet mechanically sound.  Resolving it
(the exact ptrace state a re-forkable snapshot must be left in, and the
listener/notif handshake for a resumed copy) is the gate before any of
this goes into `src/cmc_proc.c`.  The single freeze + resume is proven
(`probe.c`); repeated re-fork from one snapshot is not.

And then, once the mechanics hold:

- manage a tree of frozen snapshots and reap them without zombies or fd
  leaks; drain the shared fd-3/fd-4 canon pipes when switching copies
  (forked copies inherit one pipe, confirmed), with one live descendant
  at a time so the single seccomp listener fd stays valid;
- gate it against the replay engine: a fork-snapshot run must give
  byte-identical distinct-state counts and the same counterexamples as
  stateless replay on every `test/proc/` fixture, and be measurably
  faster, before it becomes the default.  (The replay engine stays as
  the oracle it is checked against.)
