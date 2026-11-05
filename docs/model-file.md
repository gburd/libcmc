---
title: "The model file"
parent: Reference
nav_order: 5
permalink: /reference/model-file/
lede: >-
  A declarative file that points libcmc at the binary you ship, the environment to explore, and the invariants to check.
---

A model file tells libcmc what to check without reimplementing it.  It
names the program to run and how many nodes, the environment to explore,
and the invariants.  It carries no behaviour: the protocol is the
running program, not anything written here.

```
cmc --model-file FILE [--binary PATH] [--bfs | --simulate [--seed N] [--walks K]]
    [--depth D] [--max-states N] [--time S] [--crashes N] [--drops N] [--timeouts N]
```

[cmc(1)]({{ '/reference/man/cmc.1/' | relative_url }}) has the options,
and [cmc(7)]({{ '/reference/man/cmc.7/' | relative_url }}) the model the
checker explores.  The [guide]({{ '/guide/02-first-model/' | relative_url }})
checks a complete example.

## Format

INI-style sections of `key = value`.  A `#` starts a comment.  A value
may be quoted with `"`.  There are three sections, and these are all the
keys:

```
[target]
binary = ./replog          # the program to check; relative to this file
nodes  = 3                 # node processes, 1 to 8 (required)
state  = canon             # the only state: the fd-3 hook (optional)

[environment]
client_write = 5           # a client writes this value (0-255) to node 0
timeouts = 1               # reads the environment may time out
crashes  = 1               # node crashes
drops    = 1               # messages the network may lose
depth    = 40              # bound the search to this many actions

[invariants]
no_lost_ack = "all r in replies : r.val in node[0] or r.val in node[1]"
```

Any other section or key is an error, and so is a value out of range.
A mistyped `crashs = 1` must not quietly check the model with no
crashes, so the loader refuses it and names the line.

### `[target]`

- `binary`: the program, built `-no-pie`.  `--binary PATH` overrides it,
  so one model file checks every build, planted bugs included.  One of
  the two is required.
- `nodes`: how many copies of the program run.  Node `i` gets `i` as its
  first argument and listens on 127.0.0.1 port 20000 + `i`.
- `state`: `canon`, the default and the only choice.  (Reading a fixed
  ELF symbol as the state is not implemented.)

### `[environment]`

Each budget counts over the whole run, from 0 (the default) to 255.

- `client_write = V`: a client connects to node 0 and sends the two
  bytes `W V`.  Every write back on that connection that starts with `A`
  is logged in `replies`, with the sender as `.src` and the second byte
  as `.val`.  Absent: no client.
- `timeouts`: a node blocked reading a connection may get the one byte
  `T` instead of a message, and the logical clock advances one second.
  This is how a heartbeat timeout or a lease expiry happens.
- `crashes`: a node dies at any quiescent point.  Its state becomes what
  it last reported on descriptor 193 (durable); what it reported on
  descriptor 3 is lost, and its peers read EOF once their connections
  to it are drained.
- `drops`: the oldest message on a connection is lost.
- `depth`: a search that reaches this many actions stops and is
  reported as incomplete (exit 2), never as a pass.  `--depth`
  overrides it.  Absent: unbounded.

`--crashes`, `--drops` and `--timeouts` override the budgets.

## The state hook

The checker reads a node's state from what the node reports.  After
each change the program writes a record, a length byte and then that
many bytes, to descriptor 3; what it has made durable (its `fsync`
point) it writes the same way to descriptor 193.  About eight lines:

```c
static void
report(int fd, const unsigned char *state, unsigned char n)
{
    unsigned char rec[256];

    rec[0] = n;
    memcpy(rec + 1, state, n);
    (void) !write(fd, rec, n + 1u);
}
```

The encoding must be **canonical**, the same logical state always
producing the same bytes (sort sets, leave out addresses), and
**complete**: every byte the node's future depends on, its position in
its own code included.  Anything left out can let the checker merge two
states that differ and miss a bug.  Completeness is the one property you
are trusted to get right; treat it like the durable/volatile split in
the protocol.

## Invariant language

A small expression language over each node's reported state and the
reply log.  It is deliberately not Turing-complete.

- `node[i]`: node `i`'s state, a byte array (`0 <= i < nodes`); after a
  crash, its durable state.
- `node[i][k]`: byte `k` of it, or -1 when the node has reported fewer
  bytes (a crashed node that made nothing durable, for one).
- `replies`: the reply log; each entry `r` has `r.src` and `r.val`.
- integers.

```
all V in SET : EXPR        some V in SET : EXPR     (SET: node[i] or replies)
EXPR and EXPR    EXPR or EXPR    not EXPR    ( EXPR )
A == B   A != B   A < B   A <= B   A > B   A >= B   A in node[i]
```

`and` and `or` associate left to right with equal precedence; use
parentheses.  Every invariant is parsed when the file is loaded, and a
parse error, text after a complete expression, or an unclosed
parenthesis is an error, even in a part no state reaches.

An invariant holds in a state when its expression is true there.  A
violation prints the shortest sequence of actions that reaches the
failing state, by name, and each node's state there.

## Exit status

0: no violation, and the search was complete.  1: a violation, or an
engine rule was broken (a cap, a write longer than 64 bytes, a node
silent in a call the checker does not model).  2: incomplete; a depth,
state or time bound stopped the search, or `--simulate` found nothing.
3: a usage, model-file or setup error.

## Not yet

Named fields for the state, a C predicate for a property the language
cannot express, replay files for a process run, duplication, and a
connect that fails.  Each is an error today, not ignored.
