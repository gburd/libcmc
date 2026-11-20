---
title: "Apalache past length 10"
parent: History and provenance
nav_order: 15
permalink: /background/apalache-length/
lede: >-
  The Apalache gate checks the emitted .cml models to --length 10, short
  of most models' depth.  How far a local run gets, and what it costs,
  with Apalache 0.63.0 on werk.
---

Date: 2026-11-19 and 20.  Raw output: `2026-11-20-apalache-length-raw.txt`
(same directory).  The gate is `test/apalache_gate.sh`
(`make check-emit-apalache`).

## Why it matters

Apalache checks a bounded run: `--length N` means every behaviour of at
most N steps.  A model whose depth (in TLC's and cmc's sense, counting
the initial state as 1) is D has every reachable state at most D - 1
steps from the start, so a run with N >= D - 1 that finds no error has
covered every reachable state.  A shorter run is "no violation within N
steps", and nothing more.  At length 10, the gate covers counter, raft,
hc and twophase completely, and paxos (depth 16), fpaxos (20), caspaxos
(17), zab (26) and fastpaxos (12) only in part.

## What was run

- cmc built from `pr/2.2-spec` at 9bf1fea (clang, the Makefile's
  `-O2 -g`); `cmc --version` says 2.1.0.  Each module is the output of
  `cmc --emit-tla examples/spec/<model>.cml`, renamed to its file, as the
  gate does.  sha256 of the binary and of each module are in the raw
  file.
- Apalache 0.63.0 (the gate's pinned version), on OpenJDK 21.0.12.1
  from `nix shell nixpkgs#jdk21`.
- Command, per model and length, from the model's directory:
  `timeout CAP apalache-mc check --no-deadlock --cinit=CInit --inv=INV
  --length=N MODEL.tla`, with the gate's invariant for each model (zab:
  `committed_prefix`).  A run that reaches CAP is killed by `timeout`
  (exit 124) and reports nothing.
- Host: werk, 48 x Xeon Platinum 8375C, 185 GiB, Linux 6.18.60.  Load
  average 7 to 9 from other work during the runs.  Runs shared the host:
  five at once in the first batch (CAP 1200 s), four in the second (CAP
  3600 s), two in the third.  Each Apalache run is one JVM and one Z3,
  so the wall times are of that run with other load present, not of an
  idle host.

## Result

Every run that finished reported `NoError` with exit code OK [measured].
Wall times in seconds:

| model | depth | full at | 10 | 11 | 12 | 13 | 14 | 15 | 16 |
|---|---|---|---|---|---|---|---|---|---|
| paxos | 16 | 15 | 33 | | 303 | 821 | 2304 | | |
| fpaxos | 20 | 19 | 29 | | 410 | 984 | >1200 | | |
| caspaxos | 17 | 16 | 32 | | 194 | 807 | >1200 | | |
| zab | 26 | 25 | 40 | | | 442 | 767 | 1430 | 2758 |
| fastpaxos | 12 | 11 | 35 | 83 | | | | | |

">1200" is a run killed at the 1200 s cap; paxos 14 and zab 16 were
also killed at 1200 s in the first batch and finished in the second.

So, on this host:

- fastpaxos is checked completely at length 11, in under a minute and a
  half.
- paxos holds up to 14 steps, one short of its full depth.  fpaxos and
  caspaxos hold up to 13, zab up to 16 of its 25.  These are "no
  violation within N steps", not passes of the model.
- Each extra step costs about 2 to 3 times the one before [measured on
  the rows above].  At that rate paxos at 15 would take one and a half to
  two hours, and fpaxos at 19 or zab at 25 days [estimate].  The full
  depths of fpaxos, caspaxos and zab are out of reach for bounded model
  checking this way; `cmc --spec` and TLC cover them exhaustively in
  seconds.

## What the gate does with it

The CI job that runs the gate (`bridge` in `.github/workflows/ci.yml`)
has 60 minutes for TLC and Apalache together.  The gate with the change
below ran here in 29 minutes [measured, 19:03 to 19:33 on 2026-11-19,
load average 6 to 7]: the pass rows at length 10 take 3 to 40 s each,
and the violation rows, which must reach their counterexample, take the
rest (zab's at length 13 took 14 minutes, paxos's and fpaxos's at 12
three and five).  CI runners are smaller than werk.

So one row changes: fastpaxos's pass row runs at length 11, which
covers its whole state space for 66 s.  The other pass rows stay at 10.
Length 12 would add 3 to 7 minutes for each of paxos, fpaxos and
caspaxos, and length 13 for zab about 7, which would put the job past
its 60 minutes.  The gate's row comment says which pass rows are
complete and which are bounded.
