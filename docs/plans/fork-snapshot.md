---
title: "Plan: fork-from-snapshot (--fast)"
parent: Reference
nav_order: 10
permalink: /reference/plan-fork-snapshot/
lede: >-
  Why --fast stays experimental: the snapshot mechanic is proven, but a Linux seccomp-notify quirk (read in the filter plus fork injection) breaks the resumed accept, and the current per-thread engine needs a major re-port before --fast could even run.
---

This is a status-and-decision plan, not built code.  It records why
fork-from-snapshot (`--fast`) stays an experimental branch
(`m9-fast-wip`), what is proven, what is blocked, and what a future
attempt would have to do.  The sound BFS replay stays the default and the
oracle; nothing in the north-star milestones depends on `--fast`.

## What `--fast` is for

The process front end explores each state by replaying from launch:
start all N node processes again, re-drive the whole action prefix.  An
exhaustive search is O(states x depth) node-steps.  Fork-from-snapshot
would make expanding a state O(1) node-steps -- freeze every node at a
quiescent point, fork a copy per enabled action, apply the action, drive
to the next quiescence -- turning the search into O(states) node-steps.

The snapshot must be a true `fork(2)` (the kernel copies the address
space correctly, copy-on-write), never a hand-rolled memory capture that
could miss a page and give a silent wrong verdict.  The obstacle: a
quiescent node is suspended in the kernel on a seccomp user-notification,
so it cannot call `fork()` itself; the checker injects one by ptrace
register rewrite.

## What is proven

The full fork-inject-resume mechanic works on Linux x86-64 with
`ptrace_scope=1`.  `experiments/fork-snapshot/` has the evidence:

- `probe.c`: a kernel-suspended USER_NOTIF task can be brought to a
  ptrace stop, its registers rewritten to call `fork`, single-stepped,
  and the duplicate recovered -- an independent COW copy.
- `chain.c`: a frozen master forks exactly once into (worker, next
  master) and the DFS chains forward, one frozen master per stack level,
  no master re-forked.
- `fdtest.c`: with the engine's exact virtual-fd node pattern (intercepted
  socket -> bind -> listen -> accept, answered with virtual fds), the
  resumed worker correctly re-raises its blocked `accept`.

The engine integration on `m9-fast-wip` (`src/cmc_proc.c`, off `main`,
off by default) wires a snapshot-DFS: `freeze_master`, `fork_master`,
`resume_world`, `dfs`, with a fingerprint self-check that refuses a
verdict if a resumed world does not match its snapshot.  The two-phase
resume (fork all next-masters first, then service each worker) fixed an
earlier livelock.

## The blocker, isolated (2026-11-16)

On the `repl` fixture `--fast` stops at the self-check:
`--fast: a resumed world did not match its snapshot (resume is not
faithful)`.  The sound guard doing its job: a resumed node runs its
protocol to completion instead of re-blocking at the snapshot's accept.

A bisection from `fdtest.c` toward the engine isolated the single
breaking element (full table and probes in
`experiments/fork-snapshot/README.md`, Part 7):

**The seccomp USER_NOTIF filter intercepting `read` (x86-64 syscall
number 0) breaks the fork-resumed `accept` re-trap.**  A fork-injected
worker resumed to re-issue its blocked accept does not re-trap to the
listener; the accept returns an error and the node proceeds to
`read(fd=-1)`.  Remove `read` from the filter (or replace it with any
other syscall: write, close, recvfrom) and the accept re-traps correctly.

The break needs BOTH `read` in the filter AND the fork injection; neither
alone triggers it, and no register-restore or resume strategy (rewind,
exact-restart, single-step, PTRACE_SYSCALL) avoids it.  It holds at full
engine filter scale.  `seccomp_unotify(2)` documents the matching error
path: a USER_NOTIF syscall whose notification cannot be delivered fails
with ENOSYS.  Why syscall 0 specifically triggers this listener-invalid
state for a COW-forked worker, when no other intercepted syscall does, is
a Linux-kernel-internal question this effort did not settle.

## Why it cannot ship now

Two independent reasons, either sufficient.

**1. The cause is isolated but not fixed, and the fix is unknown.**  The
engine MUST intercept `read` (it mediates every connection byte), so
"drop read from the filter" is not an option.  A real fix needs a
kernel-level understanding of the syscall-0 restart path, or a redesign
that does not fork-inject a task blocked in a notification whose filter
intercepts `read`.  That is a bounded research step, not a bounded
engineering step.

**2. The engine has outgrown `--fast`'s single-threaded assumption.**
`m9-fast-wip`'s `--fast` snapshots one `pid` per node with one saved
register set.  `origin/main` now models a node as up to `MAXTHREAD`
threads, each with its own kernel tid, blocking point, and futex-wake
serialisation, plus a persistent per-epfd epoll interest list and
`clone`/`clone3` birth tracking.  A correct snapshot must freeze and fork
every thread of every node at a consistent point and restore the per-thread
slots, the births-in-flight, and the woken-futex release order, all
byte-identically.  Porting `--fast` onto that is a major re-port, and it
inherits the unsolved `read`-escape on top.

## The bar `--fast` must clear before it ships

Unchanged from the original plan, and all still required:

- resolve the `read`-escape so a resumed connection-mediated node
  re-traps its blocked syscall faithfully;
- re-port onto the per-thread engine: snapshot every thread, restore the
  full per-thread checker state;
- manage a tree of frozen snapshots, reap without zombies or fd leaks,
  drain the shared fd-3/fd-4 canon pipes when switching copies;
- gate it byte-identical against BFS: same distinct-state counts and the
  same counterexamples on every `test/proc/` fixture, and measurably
  faster.  BFS stays the oracle it is checked against.

Until all of that holds, `--fast` must not give a verdict on any fixture.

## Alternatives that do not need fork-injection

If the goal is only search speed (milestone G), a replay-prefix cache
(memoise the driven-prefix state so siblings skip re-driving the shared
prefix) would cut the dominant cost without any ptrace/seccomp mechanic,
and stays trivially sound because it changes only how a state is reached,
not what is explored.  That is the recommended direction if `--fast`
itself stays blocked.
