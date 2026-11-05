---
title: "Process mode M3: a model file drives the checker"
parent: Background
nav_order: 8
permalink: /background/process-mode-m3/
lede: >-
  The decided shape, first cut: one engine, a declarative model file, the shipping binary as the protocol.
---

Date: 2026-11-05.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-05-process-mode-m3-raw.txt` (same directory).  Follows M0-M2, and
implements the shape decided in
[the model-file decision]({{ '/background/decision-model-file/' | relative_url }}).

## Why

M0-M2 were three separate checker programs with hardcoded alphabets and
invariants.  The decision record set the target: libcmc is one engine, a
declarative **model file**, and the shipping binary as the protocol.  M3
is the first cut of that -- the model file driving a real run, end to end.

## What was run

`mc.c` reads a `.model` file ([schema]({{ '/reference/model-file/' | relative_url }})):
the binary to check, the node count, how to read state (`canon`), the
environment budget, and the invariants.  It runs the target
(`demo/list.c`, an ordinary `malloc`'d ordered-set server, no libcmc
header) under the M0-M2 syscall interception, does the M2 canonical-emit
breadth-first search, and evaluates each invariant in every reached
state.  The invariants are a small expression DSL over `node[0]` (the
canonical state bytes) and `acted` (the client inputs on the path here).

`list.model` in full:

```
[target]
nodes = 1
state = canon

[environment]
clients = 3
depth   = 4

[invariants]
no_lost_key = "all k in acted : k in node[0]"
```

Nothing about the protocol is in `mc.c` or the model file.  The running
binary is the transition relation.

- Host: Linux 6.18.60, x86_64, glibc 2.44, `-no-pie` target.
- `experiments/process-mode/run.sh` (M0, M1, M2, and the two M3 cases).

## Result

| target | verdict | exit |
|---|---|---|
| `list-canon` (correct) | `no_lost_key` holds over 8 distinct states | 0 |
| `list-bug` (lost key) | `no_lost_key` violated; client inputs `2 1`; `node[0] = 1` | 1 |

The model file alone -- no code change -- selects the binary, the
environment, and the property.  The bug (inserting 1 overwrites the head,
dropping key 2) is caught with the shortest client-input sequence.

**Verdict: M3 passes.**  A declarative file drives process mode, and
invariants in the DSL catch a real safety violation in a binary written
as it ships.

## The DSL is sound, and small on purpose

Separate tests (in the raw file) confirm the evaluator checks *every*
element of a quantifier, not just the last, and that `all`, `some`, `not`,
`in`, and the comparisons behave:

- `all k in acted : k == 1` is violated as soon as `acted` holds a 2 --
  so the quantifier is not short-circuiting on the last element.
- `some k in acted : k == 3` is violated at `acted = [1]` -- `some` with
  no match is correctly false.
- `some k in node[0] : k == 9` is violated (9 is never inserted);
  `all k in node[0] : k <= 3` holds.

The language is deliberately not Turing-complete: quantifiers,
membership, comparison, boolean connectives, nothing more.  A property it
cannot express is left to the C-predicate escape hatch in the schema (not
yet implemented), so a hard property never forces a protocol rewrite.

## `acted` is honest

`acted` is the environment's own record of what it fed the target --
exactly like actor mode's ghost request log, not a read of the target's
internals.  An invariant that needs the reply log ("every acked value is
durable") waits on the next milestone, which adds the ghost reply log to
process mode.

## Still out

Several nodes (the file accepts `nodes` but `mc` handles one), crashes and
the reply log in process mode, named state fields and the C-predicate
escape hatch from the schema, and lifting `mc` out of `experiments/` into
`src/` as the engine's second front end.  The next milestone is
multi-node: several real node processes under one checker, driven by the
model file, against a small replicated server.
