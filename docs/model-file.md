---
title: "The model file"
parent: Reference
nav_order: 5
permalink: /reference/model-file/
lede: >-
  A declarative file that points libcmc at the binary you ship, the environment to explore, and the invariants to check.
---

A model file tells libcmc what to check without reimplementing it.  It
names the binary to run, how many nodes, how to read a node's state, the
environment budget to explore, and the invariants.  It carries no
behaviour: the protocol is the running binary, not anything written here.

## Format

INI-style sections of `key = value`.  Comments start with `#`.  Three
sections: `[target]`, `[environment]`, `[invariants]`.

```
[target]
binary = ./replog          # the program to check, run under interception
nodes  = 3                 # number of node processes
state  = canon             # how to read a node's state (see below)

[environment]
clients  = 1               # client requests per node (0-255)
drops    = 1               # messages the network may lose
crashes  = 1               # node crashes (partial-durability branching)
dups     = 0               # duplications (needs net = bag)
net      = fifo            # fifo (default) or bag
net_cap  = 8               # messages in flight at once
persist_cap = 2            # pending disk writes per node
reply_cap   = 8            # client replies recorded
live_steps  = 1000         # bound for eventually-properties

[invariants]
# name = expression    (see "Invariant language")
no_lost_ack = "all r in replies: r.val in node[r.src].durable"
agreement   = "all i,j in nodes: committed[i]==0 or committed[j]==0 or committed[i]==committed[j]"
```

### `[target].state`

How the checker reads one node's state for dedup and for invariants:

- `canon` -- the node emits a canonical encoding of its logical state on
  file descriptor 3 after each step, as a self-delimiting record.  This is
  the recommended form; the encoding is order-independent, so two
  interleavings that reach the same logical state dedup correctly.  See
  "The state hook".
- `region:SYMBOL` -- the checker reads a fixed byte region named by an ELF
  symbol (the target must be `-no-pie`).  Simpler, but the raw bytes must
  already be canonical (no order-dependent layout, no uninitialised
  padding), so it suits only flat, pointer-free state.

## The state hook

With `state = canon`, the target exports its logical state like this
(about eight lines, no protocol logic):

```c
/* called after each handled event; emits [count][bytes...] on fd 3 */
static void cmc_emit(void)
{
    unsigned char buf[256];
    size_t n = serialize_logical_state(buf + 1, sizeof buf - 1);  /* your encoding */
    buf[0] = (unsigned char) n;
    (void) !write(3, buf, n + 1);
}
```

The encoding must be **canonical** (the same logical state always
produces the same bytes: sort sets, omit addresses) and **complete**
(every byte that affects future behaviour is included; anything omitted
can let the checker merge two states that differ and miss a bug).
Completeness is the one property the operator is trusted to get right;
treat it like the durable/volatile split in a protocol core.

## Invariant language

A small expression language, evaluated over each node's canonical state
and the ghost reply log.  It is deliberately not Turing-complete: no
loops beyond the quantifiers, no function definitions.

Values: integers, byte arrays (the canonical state), and the reply log.

- `node[i]` -- node `i`'s canonical state (a byte array), `0 <= i < nodes`.
- `node[i].NAME` -- a named field, if the model file declares a layout
  (future; for now the state is addressed by byte offset `node[i][k]`).
- `replies` -- the ghost log; each entry has `.src` (node) and `.val`.
- `committed[i]`, `durable[i]` -- named views the model file maps onto the
  canonical bytes (future; initially byte accessors).

Quantifiers and operators:

```
all V in SET : EXPR          # holds for every binding
some V in SET : EXPR         # holds for at least one
EXPR and EXPR    EXPR or EXPR    not EXPR
A == B   A != B   A < B   A <= B   A in B
```

An invariant holds in a state when its expression is true there.  A
violation prints the shortest sequence of environment actions that
reaches the failing state, and writes a replay file.

### C-predicate escape hatch

For a property the DSL cannot express, the target may export
`int cmc_check_NAME(void)` returning non-zero on violation, and the model
file references it:

```
[invariants]
hard_property = c:cmc_check_hard_property
```

The predicate runs in the node process over its real state.  It is a small
amount of code that can carry a bug, so prefer the DSL; the hatch exists
so a hard property never forces a protocol rewrite.

## Running

```
cmc --model-file replog.model          # exhaustive BFS
cmc --model-file replog.model --simulate --seed 7 --walks 100000
cmc --model-file replog.model --trace-out bug.replay
cmc --replay bug.replay
```

Exit status is the engine's: 0 pass, 1 violation, 2 incomplete (a limit
stopped the search), 3 a usage or model-file error.

## Status

The schema is fixed here; the implementation lands incrementally
(`experiments/process-mode/`, then `src/`).  The first milestone is a
single `[target]` node with `state = canon`, `[environment]` budgets, and
byte-offset invariants; named fields, multi-node and the C escape hatch
follow.
