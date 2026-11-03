---
title: Your first model
parent: Guide
nav_order: 2
permalink: /guide/02-first-model/
lede: >-
  A write that must survive a crash, a planted bug, and the counterexample libcmc prints for it.
---

1. TOC
{:toc}

---

This page builds a complete model in one C file: a client writes a
value to a server, the server makes it durable and then acknowledges it,
and the property is that a write the client saw acknowledged survives a
crash.  Then it plants the classic bug, acknowledging before the write
is durable, and shows libcmc finding it.

Every code block here is a region of
`docs/_includes/snippets/quickstart.c`.  `make check` builds that file
and checks that it behaves as this page says.

## The state

A node's state is a fixed-size struct of plain bytes, with no pointers.
The engine copies, compares and hashes it as bytes, so unused fields
must stay zero.  Both nodes use the same layout; the first
`durable_size` bytes are what survives a crash.

{% include snippet.html file="quickstart.c" region="state" %}

## The step function

`step()` handles one event at one node.  It reads only its arguments:
no clock, no randomness, no globals, no I/O.  It changes the state in
place and emits effects through `cmc_out_push()`.

The client sends a write when it gets a client request, and reports the
acknowledgement as a reply.  The server asks for its write to be made
durable (`CMC_OUT_PERSIST`) and acknowledges when the engine tells it
the write reached disk (`CMC_EV_FLUSHED`).

{% include snippet.html file="quickstart.c" region="step" %}

`step()` never writes the durable byte itself.  The engine owns the
durable part of the state: when a persist completes it calls
`apply_persist()`, and it checks after every step that the durable
bytes did not change.  That is what lets it model a crash honestly: the
volatile bytes are lost, and of the persists still pending, any prefix
may have reached disk.

## The core

The vtable ties it together.  The engine calls `cfg_set` for each
`--cfg key=value`; this core has no settings.  `clients` says which
nodes take client requests.

{% include snippet.html file="quickstart.c" region="core" %}

## The properties

A property reads the world through accessors: `cmc_node()` for a
node's state, `cmc_nreplies()` and `cmc_reply()` for the ghost log of
replies sent to clients.

{% include snippet.html file="quickstart.c" region="props" %}

`Acked` is a witness: some reachable state must satisfy it.  Without
it, a model in which the client never gets an acknowledgement would
pass `NoLostAck` trivially, and so would a model whose budgets are too
small to reach the interesting states.

## main()

{% include snippet.html file="quickstart.c" region="main" %}

`cmc_main()` is the whole `cmc(1)` command line.  Your program carries
your models and is your checker.

## Run it

```sh
cc -std=c11 -Iinclude docs/_includes/snippets/quickstart.c \
    build/libcmc.a -o quickstart
./quickstart --model quickstart --all-props --crashes 1
```

```
cmc: model quickstart, cfg "", breadth-first search, world 488 bytes
Model checking completed. No error has been found.
  Estimated probability of a 128-bit fingerprint collision (n^2/2^129): 1.32e-36
Witness Acked: reached
  Acked first reached at depth 5
37 states generated, 30 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 7.
Finished in 0.00s
```

libcmc tried every order of the client request, the two messages, the
disk write and one crash of either node, and found no state that breaks
`NoLostAck`.  The exit status is 0.

## Plant the bug

Built with `-DQUICKSTART_BUG`, the server acknowledges as soon as it
asks for the write, before the write is durable.  Without a crash,
nothing goes wrong:

```sh
cc -std=c11 -DQUICKSTART_BUG -Iinclude docs/_includes/snippets/quickstart.c \
    build/libcmc.a -o quickstart-bug
./quickstart-bug --model quickstart --all-props
```

```
Model checking completed. No error has been found.
```

Allow one crash, and libcmc finds it:

```sh
./quickstart-bug --model quickstart --all-props --crashes 1 --trace-out bug.replay
```

```
Error: Invariant NoLostAck is violated.
  write 1 was acknowledged, but the server restarted with value 0
Error: The behavior up to this point is:
State 1: <Initial predicate>
  node 0: server value=0 restarted=0
  node 1: client
  net:
  ...
State 2: <CLIENT node 1>
  node 0: server value=0 restarted=0
  node 1: client
  net: [1->0 x1 WRITE(1)]
  ...
State 3: <DELIVER 1->0 #0>
  node 0: server value=0 restarted=0
  node 1: client
  net: [0->1 x1 ACK(1)]
  env: ... | n0 ... persists=1 ...
State 4: <DELIVER 0->1 #0>
  ...
  replies: (n1,1)

State 5: <CRASH node 0>
  node 0: server value=0 restarted=1
  ...
  replies: (n1,1)
Replay file written to bug.replay (4 actions).
```

Four events: the client asks, the server receives the write and
acknowledges it with the persist still pending, the client gets the
acknowledgement, and the server crashes before the persist completes.
Breadth-first search finds the shortest such sequence.  The exit status
is 1.

The replay file runs the same four events through the real step code,
printing every state:

```sh
./quickstart-bug --replay bug.replay --all-props
```

Simulation finds it too, and prints the seed that reproduces it:

```sh
./quickstart-bug --model quickstart --all-props --crashes 1 --simulate --walks 1000
```

## What to take from this

- The bug needs a crash to show.  A pass means "no violation within
  these budgets"; raise `--crashes`, `--drops` and `--clients` until
  your planted bugs are caught.
- The witness guards against passing for the wrong reason.
- The planted bug lives in the shipped source behind `#ifdef`.  Your
  gate builds each variant and requires each one to be caught.  That is
  the subject of the
  [next page]({{ '/guide/03-in-your-project/' | relative_url }}).
