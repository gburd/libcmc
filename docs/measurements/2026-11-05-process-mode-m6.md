---
title: "Process mode M6: crash loses volatile, keeps durable"
parent: Background
nav_order: 12
permalink: /background/process-mode-m6/
lede: >-
  The checker may crash a node -- losing its volatile state, keeping what it made durable -- or drop a message, and it catches a committed write lost to a crash.
---

Date: 2026-11-05.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-05-process-mode-m6-raw.txt` (same directory).  Follows M5b;
roadmap in `experiments/process-mode/DESIGN.md`.

## Why

A consensus protocol's whole job is to not lose a committed decision when
a node crashes.  To check that, the model checker must be able to crash a
node at a chosen moment and model exactly what a crash destroys (volatile
memory) versus what it preserves (what was `fsync`'d).  It must also be
able to lose messages -- a partition.  M6 adds both as budgeted
environment actions, mapping the in-process engine's crash/partition
model onto real processes.

## The mechanism

**Durable versus volatile, declared not intercepted.**  The target
already emits its canonical state on fd 3 after each change (M2).  M6 adds
a second stream, fd 193: the *durable* canonical encoding, which the
target emits only after it has made that state durable (its `fsync`
point).  A CRASH action kills the node and keeps only the last fd-193
emit, discarding the fd-3 (volatile) state.  This is the same opt-in seam
as M2 -- the target declares durability by *when* it emits, so no
`fsync`/`pwrite` interception is needed, and the engine's
lose-volatile-keep-durable model holds on a real process.

**Two new budgeted actions.**  At each quiescent point the checker may:

- CRASH any live node (budget `crashes`);
- DROP any buffered message (budget `drops`), the partition/loss
  primitive.

Both are bounded in the model file, the same discipline the timeouts and
the original engine's crashes use, so the search stays finite.

> fd note: the durable stream is fd 193, not fd 4.  dup'ing a pipe onto
> fd 4 before `execve` breaks `LD_PRELOAD` on glibc 2.44 -- the dynamic
> loader uses low fds while handling the preload -- so the durable fd is
> chosen high (but below the 500 threshold that marks an inter-node
> channel).

## What was run

`demo/durlog.c`: a two-node replicated log, ordinary blocking sockets, no
libcmc header.  A client write to the primary (node 0) is *committed* once
the primary acks it; the backup (node 1) stores a forwarded value durably
and acks.

- Correct: the primary forwards, waits for the backup's durable-ack,
  makes the write durable locally (fd 193), and only **then** acks the
  client.  A crash after the ack cannot lose the write -- it is durable on
  both nodes.
- `-DBUG`: the primary acks the client as soon as it has forwarded, before
  anything is durable.

`durlog.model` asserts `no_lost_commit`:
`all r in replies : r.val in node[0] or r.val in node[1]` -- every acked
value is in some node's (post-crash, durable) state.

## Result

| target | verdict | exit |
|---|---|---|
| `durlog` (correct) | `no_lost_commit` holds over 9 states (2 merges) | 0 |
| `durlog-bug` | violated: reply `(n0,7)`, `node[0]=` `node[1]=` empty | 1 |

The checker finds the bug on the schedule "ack the client, then crash the
primary": value 7 was committed (the ghost reply log has `(n0,7)`) but it
is in neither node's durable state, so the crash lost a committed write.
That is the M6 kill criterion -- *a crash that loses a committed entry is
caught*.  Stable 10/10 runs.

## Still out

A dedicated partition demo (the DROP action is in and exercised, but no
example turns on it alone yet); M7 scale, honestly stated; and M8 lifting
process mode into `src/`.  Only after those do the consensus examples
(Paxos, Raft, Humming Consensus, ZAB) arrive, as real implementations --
M6 is the last environment capability they need.
