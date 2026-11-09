---
title: Examples
nav_order: 4
has_children: false
permalink: /examples/
lede: >-
  Real protocols, checked as they ship.
---

Each example is an ordinary program -- blocking TCP, no libcmc header,
no rewrite for the checker -- plus a model file and a short write-up.
`cmc --model-file` runs the program as it ships and checks it.  They
live in [`examples/`](https://codeberg.org/gregburd/libcmc/src/branch/main/examples)
in the source tree, and `make check` builds and checks each one.

- **[Single-decree Paxos](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/paxos)**
  -- two proposers and three acceptors as five processes; the safety
  property is agreement.  The correct protocol passes; a build whose
  proposer ignores the promised values decides two different values, and
  cmc prints the shortest schedule that reaches it.

- **[Raft leader election](https://codeberg.org/gregburd/libcmc/src/branch/main/examples/raft)**
  -- three nodes; the checker owns the election timers and explores every
  race, checking Election Safety (at most one leader per term).  The
  correct protocol holds, with and without a crash; a build that grants a
  vote twice in a term elects two leaders, and cmc finds it.

More consensus protocols (Humming Consensus, ZAB) follow, each a real
implementation checked for agreement and no lost committed entry.
