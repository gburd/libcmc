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
  interface: cores, events, effects, properties, running a check.
- [cmc(7)]({{ '/reference/man/cmc.7/' | relative_url }}): the model and
  the rules that keep a pass meaningful.

After `make install` these are on your system as `man cmc`,
`man libcmc` and `man 7 cmc`.  The header,
[`include/cmc.h`](https://codeberg.org/gregburd/libcmc/src/branch/main/include/cmc.h),
is the whole public API.
