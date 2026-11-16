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
  Reference: Lamport's `Paxos.tla`.  A second, structurally different
  implementation, `paxos2.c`, is checked against the same abstract
  model through its own binding; its planted bug breaks the spec's
  agreement.
- **[Raft leader election](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/raft)**
  -- three nodes; Election Safety (at most one leader per term), with and
  without a crash.  Reference: Ongaro's `raft.tla`.  A candidate serves
  RPCs while it campaigns; before it did, its planted double-vote bug
  was caught only through a channel model that mixed connections
  (remediation plan, 3.7 and 4.7).
- **[Humming Consensus](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/hc)**
  -- a generic agreement protocol (like Paxos/Raft/ZAB): three
  participants agree on a configuration per epoch over write-once
  register stores; agreement holds and a participant adopts.  Before a
  participant served requests while it waited on its own, it never
  adopted at all under a faithful channel model (remediation plan, 3.7
  and 4.8).  Reference: Fritchie, PaPoC 2016.
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
  base; Agreement (at most one value chosen).  Acceptors promise and vote
  in more than one ballot, and a leader must propose the value of the
  highest-ballot vote its quorum of promises reports.  Reference:
  Lamport's `Paxos.tla`, whose variables and actions it follows; a
  hand-written twin in that style (`paxos.tla`) gives TLC the same
  6,144 distinct states.
- `fpaxos.cml` -- Flexible Paxos, extending `consensus`; Agreement holds
  iff every Phase 1 quorum intersects every Phase 2 quorum, for which
  `Q1 + Q2 > A` suffices (Howard's theorem).  Reference: Howard, Malkhi,
  Spiegelman, OPODIS 2016.
- `fastpaxos.cml` -- Fast Paxos, extending `consensus`; a fast round may
  collide, so its quorum must exceed `3A/4` for a classic recovery round
  to resolve the collision (two rounds modelled).  Reference: Lamport,
  Fast Paxos, 2006.
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
leader that ignores its promises, a revote, an overwrite), and the
checker finds the shortest counterexample.

These models are small safety demonstrations, each a few guarded
actions over a tally.  `cmc --spec`, TLC and Apalache agreeing on one
shows the encoded rules were executed as written; it does not show the
rules are the named protocol.  Fast Paxos recovery does not read a
quorum, Flexible Paxos has no ballots or phase 1, CASPaxos models a
write-once register rather than a mutable one, and ZAB is a guarded
counter with no epochs or recovery.  The remediation plan (Phase 4)
either makes each a faithful abstraction or renames it.

## The failure, animated

Marc Brooker (AWS) has an animation of the failure the chain-replication
and ZAB examples catch -- a replicated cluster losing an acknowledged
write because durability was not truly achieved -- in *Animations for
System Builders*:
<https://brooker.co.za/blog/2026/10/23/animations.html>.  Where libcmc
prints the shortest schedule that loses the write, his animation shows
the same class of failure in motion.
