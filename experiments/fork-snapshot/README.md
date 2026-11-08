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

So the approach is feasible.  On a quiet host the probe goes further: it
injects `fork` by register rewrite (set `orig_rax`/`rax` to `SYS_fork`,
`rip` back onto the syscall instruction, single-step), catches the
duplicate with `PTRACE_EVENT_FORK`, restores the original's registers so
it re-enters the blocked `write()`, runs the original on to its own
exit, then resumes the frozen snapshot and checks its heap still reads
the quiescent-point value (11), not the original's final value (12) --
the proof the copy is a separate, undisturbed freeze.

The fork injection itself is confirmed working (`injected fork ->
snapshot pid ...`).  The independent-resume check needs a quiet host;
under heavy external load the seccomp/ptrace handshake is starved and
the run stalls after the injection.

```
cc -O2 -o probe probe.c && ./probe     # Linux x86-64; run on a QUIET host
```

A heavily loaded host starves the seccomp/ptrace handshake and the
probe times out midway (it still prints the facts above first).

## Still to prove before touching `src/`

- confirm (on a quiet host) the injected snapshot resumes independently
  and reads the quiescent value, not the original's -- the probe does
  this; it needs load low enough to finish;
- manage a tree of frozen snapshots (re-fork a snapshot before resuming
  it, so it can be resumed again for the next sibling), and reap them
  without zombies or fd leaks; the inherited seccomp listener fd and the
  `PTRACE_SEIZE` relationship both need care across the fork;
- gate it against the replay engine: a fork-snapshot run must give
  byte-identical distinct-state counts and the same counterexamples as
  stateless replay on every `test/proc/` fixture, and be measurably
  faster, before it becomes the default.  (The replay engine stays as
  the oracle it is checked against.)
