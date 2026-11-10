---
title: "Process mode M4: multi-node, the checker owns the network"
parent: History and provenance
nav_order: 9
permalink: /background/process-mode-m4/
lede: >-
  Two real node processes under one checker, which chooses the delivery order and catches an acked write the backup never got.
---

Date: 2026-11-05.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-05-process-mode-m4-raw.txt` (same directory).  First step toward
the consensus examples (`experiments/process-mode/DESIGN.md`, roadmap).

## Why

M0-M3 checked a single node.  A replicated or consensus protocol is
several nodes exchanging messages, where the bug is in *which order* the
messages land.  M4 is the first multi-node step: the checker runs N node
processes and owns the network between them, so message delivery order is
an environment choice it explores.

## What was run

`demo/repl.c`: a two-node replicated register, ordinary blocking sockets,
no libcmc header.  Node 0 (primary) takes a client WRITE, forwards it to
node 1 (backup), and -- when correct -- acks the client only after the
backup has stored it.  Built `-DBUG`, the primary stores and acks the
client *before* the backup confirms.

`mc2.c` runs both as real processes under seccomp user-notification and
is the network between them: it intercepts `socket`/`connect`/`accept`/
`send`/`recv`, pairs each connection fd to a peer by the port a `connect`
targets (node i listens on `BASE+i`), and buffers inter-node messages in
a bag.  A `recv` on an empty channel is a **deferred** notification -- the
node blocks in the kernel, unanswered, until the search chooses to
deliver a message to it.  When every live node is blocked on `recv` or has
exited, the world is quiescent: snapshot each node's canonical state,
check the invariants, and the deliverable messages are the next actions.
The search is stateless replay over delivery orders.

`repl.model`:

```
[target]
nodes = 2
[environment]
client_val = 5
depth = 6
[invariants]
agree_on_ack = "all r in replies : r.val in node[1]"
```

`replies` is the ghost log of client acks (each `r` has `r.src`, the node
that acked, and `r.val`); `node[1]` is the backup's stored value.

- Host: Linux 6.18.60, x86_64, glibc 2.44, `-no-pie` targets.
- `experiments/process-mode/run.sh` (M0..M4).

## Result

| target | verdict | exit |
|---|---|---|
| `repl` (correct) | `agree_on_ack` holds over 3 distinct states | 0 |
| `repl-bug` (early ack) | violated: `node[0]=5`, `node[1]=0`, client acked 5 | 1 |

The bug is exactly the hazard the invariant names: the primary told the
client its write was committed while the backup still held 0.  Stable
over 10/10 repeat runs -- the seccomp notification timing is not flaky
here.

**Verdict: M4a passes.**  The checker runs two real processes, chooses
the inter-node delivery order, and catches an agreement violation in a
replicated system written as it ships -- no step function, no transcription.

## The mechanism that made it work

A blocked `recv` is represented by a *deferred* seccomp notification: the
supervisor simply does not answer it, and the node stays stopped in the
kernel while the supervisor services other nodes.  When the search
delivers a message, it answers the stashed notification with the bytes.
This was proved minimal first in `scratch/probe_defer.c` (two children,
one deferred while the other is serviced, then answered out of arrival
order) before building the checker on it.

## Still out

The demo's accept-peer pairing is hardcoded for a primary/backup
topology (node 0 accepts a client, others accept from node 0); general
N>2 topologies, routing through the engine's real FIFO/bag with drops,
and the remaining roadmap milestones (M5 time and randomness as
environment, M6 crash and partition in multi-node, M7 scale, M8 lift into
`src/`) come next.  Only after them do the consensus examples (Paxos,
Raft, Humming Consensus, ZAB) arrive, as real implementations.
