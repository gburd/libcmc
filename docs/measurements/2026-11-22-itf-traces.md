---
title: "ITF counterexamples against Apalache"
parent: History and provenance
nav_order: 16
permalink: /background/itf-traces/
lede: >-
  cmc --spec --trace-out writes ITF.  Its files compared with the ones
  Apalache writes for the same planted bugs, and read with itf-py.
---

# ITF counterexamples against Apalache (2026-11-22)

`cmc --spec --trace-out FILE` writes a counterexample in ITF, the
Informal Trace Format of Apalache's ADR-015.  This note records how the
file was checked against what Apalache itself writes.

Host: EC2 c7i-class, Fedora 44, idle.  cmc from branch pr/2.3-trace
(gcc 16), Apalache 0.63.0 (build 67b8ec6) on OpenJDK 25, itf-py 0.5.0.
[measured]

## Against Apalache's own file

For each VIOLATION row of `test/apalache_gate.sh` (paxos, raft, hc,
zab, fpaxos, caspaxos, twophase, fastpaxos, each with `--define BUG`),
the gate runs `apalache-mc check` on the module `cmc --emit-tla`
writes, then compares cmc's ITF file with Apalache's
`violation.itf.json` using `test/trace_check.py apalache`.  All eight
agree [measured] on:

- `#meta.format` (`ITF`) and `#meta.varTypes` (`Int` and
  `(Int -> Int)`, the text Apalache writes);
- `params` (the constants) and `vars`, as sets;
- the number of states (5, 7, 13, 14, 13, 13, 4 and 12 for raft, hc,
  paxos, zab, fpaxos, caspaxos, twophase and fastpaxos);
- in every state, the same keys (`#meta`, each constant, each
  variable) and the same value forms: `{"#bigint": "N"}` for an
  integer, and `{"#map": [[k, v], ...]}` with the same key forms for a
  function.

The values differ where the two tools pick different counterexamples of
the same length, so the gate compares structure, not values.  Apalache
leaves the action out of `#meta`; cmc adds `"action"` to every state but
the first, which the ADR allows (`#meta` holds free-form data).

The comparison has teeth: replacing one function value of a paxos state
with an integer fails it ("maxBal has another form than Apalache's"),
and adding a key fails it on the state's keys. [measured]

## With an ITF reader

itf-py 0.5.0 (`itf_py.trace_from_json`) reads cmc's file and Apalache's
for all eight models; both decode to the same number of states and the
same set of names in the first state.  For mutex with `--define BUG`
(`test/spec/mutex.cml`), the last state decodes as `{'N': 2, 'pc':
{0: 2, 1: 2}, 'held': 1}` from both files. [measured]

Apalache's own `tracee` command could not serve as a reader here: in
0.63.0 it fails on any input with "Tried to load state from an invalid
source: mutex.tla", reading the trace length from the specification
file rather than from the trace. [measured; source:
mod-tool/src/main/scala/at/forsyte/apalache/tla/tooling/opt/TraceeCmd.scala]

## Not covered

Records, tuples and sequences (`{"#tup": ...}`, ITF records as JSON
objects, sequences as arrays) are not values `.cml` has yet; the writer
handles integers and integer functions, and `trace_value()` in
`src/cmc_spec.c` is where the new forms go.
