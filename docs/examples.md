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

More consensus protocols (Raft, Humming Consensus, ZAB) follow, each as
a real implementation checked for agreement and no lost committed entry.
