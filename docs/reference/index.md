---
title: Reference
nav_order: 3
has_children: true
permalink: /reference/
lede: >-
  The manual pages and the design of the engine.
---

- [Design]({{ '/reference/design/' | relative_url }}): the world
  encoding, where actions come from, durability, search, properties,
  gates and limits.
- [cmc(1)]({{ '/reference/man/cmc.1/' | relative_url }}): the command
  line.
- [libcmc(3)]({{ '/reference/man/libcmc.3/' | relative_url }}): the C
  interface for in-process models, the engine's calibration path: cores,
  events, effects, properties, running a check.
- [cmc(7)]({{ '/reference/man/cmc.7/' | relative_url }}): the model and
  the rules that keep a pass meaningful.
- [The model file]({{ '/reference/model-file/' | relative_url }}): the
  declarative file that points libcmc at the binary you ship, the
  environment to explore, and the invariants to check (the reasoning is
  in [the decision record]({{ '/background/decision-model-file/' | relative_url }})).
- [The abstract model]({{ '/reference/abstract-model/' | relative_url }}): a
  declarative `.cml` specification libcmc checks on its own
  (`cmc --spec`), bridges to TLA+ (`cmc --emit-tla`), and checks the
  shipping binary against by refinement (`cmc --refine`).
- [When libcmc fits]({{ '/fit/' | relative_url }}): where an
  explicit-state checker of the shipping code is the right tool, and
  where it is the wrong one.
- [Qualification]({{ '/reference/qualification/' | relative_url }}): what
  each engine answers exactly, approximates, or refuses, and the gate
  that checks it.
- [Soundness remediation]({{ '/reference/plan-soundness/' | relative_url }}):
  the false passes an audit of 1.3.0 found, and how 2.0.0 fixes each
  one.  A pass is evidence only for what the qualification table lists
  as modelled.
- [The library engine for pg_chain (2.4)]({{ '/reference/plan-2.4/' | relative_url }}):
  time, crashable nodes, timer arguments, history, scale.
- [For TLA+ users (2.3)]({{ '/reference/plan-2.3/' | relative_url }}): TLC's
  options, records and sequences, traces, TLA+ input, proof hooks.
- [In place (2.2 and after)]({{ '/reference/plan-2.2/' | relative_url }}): the
  road from a checker of small C services to one that runs PostgreSQL.
- [2.1 boundaries]({{ '/reference/plan-2.1/' | relative_url }}): the
  four gaps 2.0.0 documented (kernel `sendmsg`, the file system, memory
  interleavings, the tree bound) and the coverage target.

After `make install` these are on your system as `man cmc`,
`man libcmc` and `man 7 cmc`.  The header,
[`include/cmc.h`](https://codeberg.org/gregburd/libcmc/src/branch/main/include/cmc.h),
is the whole public API.
