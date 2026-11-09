---
title: "Process mode M2: a real malloc heap, and the wall it hits"
parent: History and provenance
nav_order: 6
permalink: /background/process-mode-m2/
lede: >-
  The milestone that finds the honest boundary: a real heap canonicalises along one schedule, but a reconverging search needs a small opt-in hook.
---

Date: 2026-11-04.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-04-process-mode-m2-raw.txt` (same directory).  Follows
`-m0.md` and `-m1.md`.

## Why

M1 hashed a fixed BSS array, canonical because the bytes *are* the state
at a fixed address.  The milestone question is whether process mode can
check code whose protocol state is a **dynamic, pointer-linked structure
built with `malloc`** -- the common case.  M2 is built to find the honest
boundary, not to paper over it.

## What was run

`demo/list.c`: an ordinary one-connection TCP server, no libcmc header,
no step function, keeping an ordered set as a `malloc`'d linked list.  The
client inserts keys 1-3.  `search2.c` does a breadth-first search over
insert orders by stateless replay, with the target run under a
deterministic arena allocator (`demo/detmalloc.c`) that the checker
`LD_PRELOAD`s.

- Host: Linux 6.18.60, x86_64, glibc 2.44, `-no-pie` targets.
- `experiments/process-mode/run.sh` (M0, M1, and the three M2 cases).

## Result

| step | what | outcome |
|---|---|---|
| deterministic arena | `malloc` from an `mmap MAP_FIXED` arena; `free` a no-op | the Nth allocation lands at a fixed address every run; ASLR defeated |
| raw-heap hashing | hash `arena[0..window)` | **the wall**: the set `{1,2}` built as insert-1-2 and as insert-2-1 has the two nodes in opposite arena slots, so the bytes differ and the orders do **not** merge (16 raw states where the logical count is 8) |
| canonical emit | target emits sorted keys on fd 3; hash that | the orders merge (8 distinct states), the search terminates, and the planted lost-key bug is caught with the shortest prefix `INS-2 INS-1` |

**Verdict: M2 passes, and it draws the boundary.**  A deterministic arena
is necessary and works -- it removes address nondeterminism, so one
schedule is reproducible and dedups.  But it does **not** remove
*allocation-order* nondeterminism, which is exactly what two interleavings
of commuting operations differ in.  Raw heap bytes therefore cannot
canonicalise a logical state that is independent of allocation order.
This is the wall CMC (OSDI 2002) paid 165-185 lines per program to climb:
it did not hash memory, it walked the live object graph from roots in a
deterministic order.

## The fix is a small opt-in hook, not a rewrite

The target, when built for checking, emits a canonical encoding of its
logical state on a declared side channel (fd 3) after each change -- for
the list, the keys in sorted order, about eight lines.  The checker hashes
that instead of the raw arena; absent it, it falls back to the raw arena
and says so.  With the hook the reordered inserts merge and the invariant
"every inserted key is still present" becomes checkable (raw bytes are not
parseable).  The hook leaves the protocol code as it ships; it is not the
step-function rewrite the project exists to avoid.

The concrete bytes, from the raw run (first 24 bytes of the arena, a node
being `[key:8][next:8]`):

```
INS-1,INS-2 -> 01 .. next=0x600000000010 .. then 02 ..
INS-2,INS-1 -> 02 ..                        .. then 01 ..
```

Same set, different bytes -- including the embedded `next` pointer, which
*does* canonicalise (fixed arena base) while the order does not.

## What this means for libcmc

Process mode checks arbitrary heap-using C **along a fixed schedule**
(replay, simulation, a DAG-shaped search) with no change to the target.
A **reconverging exhaustive search** needs the small canonical-emit hook
-- far less than rewriting the logic into pure step functions, and
optional: without it, process mode still runs, it just counts
reordered-equal states separately.

## Still out

Several nodes; the clock and the vDSO; threads with a controlled
scheduler; PIE (the arena and the `-no-pie` target both assume fixed
addresses); and wiring libcmc's own engine (properties, replay,
`--simulate`, the TLC calibration) onto the process frontier.  Whether the
canonical-emit hook becomes the standard way to use process mode, or the
fixed-schedule forms suffice for the first real target, is the decision
the next milestone turns on.

## Honest limits

- `free` is a no-op, sound only for a bounded run; a long run would
  exhaust the arena.  A real allocator would need a canonicalising free
  list, which reintroduces order effects -- another reason the hook, not
  raw bytes, is the scalable path.
- The raw window is a fixed size; a state larger than it would be
  truncated (an engine-style cap would report that rather than silently
  truncate).
- The arena base is a fixed constant; two targets sharing process mode in
  one run would need distinct bases.
- Everything M1 noted (stateless replay re-executes per state; `-no-pie`
  only; the vDSO and threads unhandled) still holds.
