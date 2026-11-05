---
title: Your first model
parent: Guide
nav_order: 2
permalink: /guide/02-first-model/
lede: >-
  A server written as it ships, a model file that says what to check, a planted bug, and the counterexample libcmc prints for it.
---

1. TOC
{:toc}

---

This page checks a small replicated server exactly as it would ship:
ordinary C, blocking TCP, no libcmc header, no rewrite for the checker.
A client writes a value; the primary forwards it to a backup and
acknowledges once both have made it durable.  The property is that a
write the client saw acknowledged survives a crash.  Then it plants the
classic bug, acknowledging too early, and shows libcmc finding it.

The server is `docs/_includes/snippets/kvserver.c` and the model file
is `docs/_includes/snippets/kvserver.model`.  `make check` builds the
server, runs every command on this page, and checks that the output is
what the page says.  This needs Linux on x86-64.

## The server

Two nodes run the same program; the first argument is the node number.
Node 0 is the primary, node 1 the backup.  Node `i` listens on port
20000 + `i`, which is how libcmc routes a connection: it is the network,
and no real socket ever forms.

{% include snippet.html file="kvserver.c" region="server" %}

## The state hook

libcmc runs the real program, so it needs the program to say what its
state is.  That is the only code it asks for:

{% include snippet.html file="kvserver.c" region="hook" %}

After each change the server reports its logical state as a length byte
and the bytes: what it holds in memory on descriptor 3, what it has
made durable on descriptor 193.  A crash keeps only the durable report.

The report must be **canonical**, the same bytes for the same logical
state, and **complete**: everything the node's future depends on.  If a
field is left out, two states that differ look the same, libcmc merges
them, and it can miss a bug.  For this server, the one value is all of
it.

## The model file

{% include snippet.html file="kvserver.model" %}

It names the program and the number of nodes, the environment to
explore, and the invariant.  It carries no behaviour; the protocol is
the program.  `replies` is the log of acknowledgements a client
received, and `node[i]` is what node `i` last reported, or after a
crash, what it last made durable.  An unknown or unsupported key is an
error, so a typo cannot silently drop a crash from the search.

## Run it

```sh
cc -no-pie -o kvserver docs/_includes/snippets/kvserver.c
cp docs/_includes/snippets/kvserver.model .
build/cmc --model-file kvserver.model
```

```
cmc: model file kvserver.model, binary kvserver, 2 nodes, breadth-first search
  environment: client_write 7, timeouts 0, crashes 1, drops 0
Model checking completed. No error has been found.
21 states generated, 16 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 6.
```

libcmc ran both nodes as real processes, tried every order of the
connection, the messages and one crash of either node at every point,
and found no state that breaks `no_lost_ack`.  The exit status is 0.
`-no-pie` is required: the checker reads a fixed-address heap.

## Plant the bug

Built with `-DKV_BUG`, the primary acknowledges as soon as the value is
in its memory, before anyone has made it durable.  Check that build
against the same model file.  Without a crash nothing goes wrong:

```sh
cc -no-pie -DKV_BUG -o kvserver-bug docs/_includes/snippets/kvserver.c
build/cmc --model-file kvserver.model --binary ./kvserver-bug --crashes 0
```

```
Model checking completed. No error has been found.
```

With the model's one crash, libcmc finds it:

```sh
build/cmc --model-file kvserver.model --binary ./kvserver-bug
```

```
Error: Invariant no_lost_ack is violated.
Error: The behavior up to this point is (3 actions):
    1. accept client->n0
    2. deliver client->n0
    3. crash n0
  node states: node[0]=() node[1]=()
  replies: (n0,7)
```

Three events: the primary accepts the client's connection, reads the
write and acknowledges it, then crashes before the value is durable
anywhere.  The client holds an acknowledgement for 7, and no node has
it.  Breadth-first search finds the shortest such sequence; the exit
status is 1.

Simulation finds it too:

```sh
build/cmc --model-file kvserver.model --binary ./kvserver-bug --simulate --seed 1 --walks 200
```

A simulation that finds nothing exits 2, "no violation found", never 0:
random walks are not exhaustive.

## What to take from this

- The bug needs a crash to show.  A pass means "no violation within
  these budgets"; raise `crashes`, `drops` and `timeouts` until your
  planted bugs are caught.
- Check that the interesting states are reached.  An invariant over
  `replies` passes trivially if no acknowledgement is ever sent; `make
  check` confirms that one is.
- The planted bug lives in the shipped source behind `#ifdef`, and the
  same model file checks every build.  The
  [next page]({{ '/guide/03-in-your-project/' | relative_url }}) turns
  that into a gate.
