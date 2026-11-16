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
- `fpaxos.cml` -- Flexible Paxos, extending `consensus`: the ballots
  and promises of `paxos.cml` with a phase 1 quorum `Q1` and a phase 2
  quorum `Q2`.  Agreement holds at `A = 4`, `Q1 = 3`, `Q2 = 2`, where
  two phase 2 quorums can be disjoint but `Q1 + Q2 > A`.  Reference:
  Howard, Malkhi, Spiegelman, OPODIS 2016.
- `fastpaxos.cml` -- Fast Paxos, extending `consensus`: a fast round,
  then a classic round whose coordinator reads a classic quorum of
  promises and applies the paper's rule for a collision.  Agreement
  holds when the Quorum Requirement `Qc + 2*Qf > 2*N` does (here N = 3,
  Qc = 2, Qf = 3), and the BUG build's `Qf = 2` breaks it.  Reference:
  Lamport, Fast Paxos, 2006.
- `raft.cml` -- Raft leader election; Election Safety (at most one
  leader per term).  Reference: Ongaro's `raft.tla`.
- `hc.cml` -- Humming Consensus over write-once epoch register stores,
  extending `consensus`; Agreement per epoch.  Reference: Machi.
- `caspaxos.cml` -- CASPaxos: Paxos rounds on a rewritable register,
  each applying a change function to the highest-ballot state a quorum
  of promises reports.  Its invariant is the paper's: a confirmed state
  descends from every earlier confirmed state, tracked by a ghost parent
  per round.  Two rounds.  Reference: Rystsov, 2018.
- `zab.cml` -- Zab with epochs: discovery takes a new leader's initial
  history from the most up-to-date member of a quorum, followers
  synchronise to it, and the leader broadcasts and commits a prefix.
  Of two epochs' committed prefixes one extends the other.  Three
  servers, two epochs, histories of at most two transactions, no
  crashes.  Reference: Junqueira, Reed, Serafini, DSN 2011, and
  ZooKeeper's `Zab.tla`.
- `twophase.cml` -- two-phase commit, the canonical non-consensus
  example; Consistency (atomicity): a committed and an aborted resource
  manager never coexist.  Reference: Lamport's `TwoPhase.tla`.

Each also ships a `--define BUG` build that breaks the safety rule (a
leader that ignores its promises, a revote, an overwrite), and the
checker finds the shortest counterexample.

These models are small instances (three or four processes, two
ballots, rounds or epochs at most) with no message loss and no crashes.
`cmc --spec`, TLC and Apalache agreeing on one shows the encoded rules
were executed as written, at that instance; it is not a proof of the
protocol.  Each file's header names the phases it encodes and what it
leaves out, and the remediation plan (Phase 4) records what each engine
reported.  `raft.cml` and `hc.cml` were not part of that rework: they
remain one-term and one-epoch vote tallies, which state Election Safety
and per-epoch Agreement but do not model elections across terms or
epochs.

## The failure, animated

Marc Brooker (AWS) has an animation of the failure the chain-replication
and ZAB examples catch -- a replicated cluster losing an acknowledged
write because durability was not truly achieved -- in *Animations for
System Builders*:
<https://brooker.co.za/blog/2026/10/23/animations.html>.  Where libcmc
prints the shortest schedule that loses the write, his animation shows
the same class of failure in motion.
