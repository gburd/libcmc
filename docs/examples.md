---
title: Examples
nav_order: 4
has_children: false
permalink: /examples/
lede: >-
  Real protocols, checked as they ship.
---

Each example is an ordinary program -- blocking TCP, no libcmc header,
no rewrite for the checker -- plus a model file and a short write-up that
cites the protocol's formal reference.  `cmc --model-file` runs the
program as it ships and checks it.  They live in
[`examples/`](https://codeberg.org/gregburd/libcmc/src/branch/main/examples)
in the source tree, and `make check` builds and checks each one.

- **[Single-decree Paxos](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/paxos)**
  -- two proposers and three acceptors; the safety property is agreement.
  Reference: Lamport's `Paxos.tla`.  A **second** implementation
  (`paxos2.c`, three symmetric nodes, a different hook layout) refines
  the *same* abstract model `examples/spec/paxos.cml` through its own
  binding -- the "one model, many implementations" claim, demonstrated.
- **[Raft leader election](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/raft)**
  -- three nodes; Election Safety (at most one leader per term), with and
  without a crash.  Reference: Ongaro's `raft.tla`.
- **[Humming Consensus](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/hc)**
  -- a generic agreement protocol (like Paxos/Raft/ZAB): three
  participants agree on a configuration per epoch over write-once
  register stores; agreement holds even under a network partition.
  Reference: Fritchie, PaPoC 2016.
- **[ZooKeeper Atomic Broadcast](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/zab)**
  -- a leader and two followers; a committed transaction is never lost.
  Reference: Apache ZooKeeper's `Zab.tla`.

Each example cites its canonical TLA+ specification (or paper) and the
safety property the libcmc invariant corresponds to, so a run of the
real binary cross-checks the property the formal model states.

## Abstract models

Several protocols have abstract `.cml` models under `examples/spec/`,
checked three ways that agree -- `cmc --spec`, TLC
(`make check-emit-tla`) and Apalache (`make check-emit-apalache`):

- `paxos.cml` -- single-decree Paxos, extending the shared `consensus`
  base; Agreement (at most one value chosen).  Reference: Lamport's
  `Paxos.tla`.
- `fpaxos.cml` -- Flexible Paxos, extending `consensus`; Agreement holds
  iff every Phase 1 quorum intersects every Phase 2 quorum, for which
  `Q1 + Q2 > A` suffices (Howard's theorem).  Reference: Howard, Malkhi,
  Spiegelman, OPODIS 2016.
- `raft.cml` -- Raft leader election; Election Safety (at most one
  leader per term).  Reference: Ongaro's `raft.tla`.
- `hc.cml` -- Humming Consensus over write-once epoch register stores,
  extending `consensus`; Agreement per epoch.  Reference: Machi.
- `caspaxos.cml` -- CASPaxos, single-decree Paxos on a register via a
  client change function, extending `consensus`; Agreement per register
  version, from the client's quorum read.  Reference: Rystsov, 2018.
- `zab.cml` -- ZAB commit-durability: a committed transaction is held by
  a quorum, so it survives a leader change.  Reference: ZooKeeper's
  `Zab.tla`.
- `twophase.cml` -- two-phase commit, the canonical non-consensus
  example; Consistency (atomicity): a committed and an aborted resource
  manager never coexist.  Reference: Lamport's `TwoPhase.tla`.

Each also ships a `--define BUG` build that breaks the safety rule (a
double vote, a revote, an overwrite), and the checker finds the shortest
counterexample.  Paxos also demonstrates "one model, many
implementations" (`paxos.c` and `paxos2.c` refine the same model).

## The failure, animated

Marc Brooker (AWS) has an animation of the failure the chain-replication
and ZAB examples catch -- a replicated cluster losing an acknowledged
write because durability was not truly achieved -- in *Animations for
System Builders*:
<https://brooker.co.za/blog/2026/10/23/animations.html>.  Where libcmc
prints the shortest schedule that loses the write, his animation shows
the same class of failure in motion.
