# Single-decree Paxos

A worked example: single-decree Paxos, checked by libcmc as it ships.
Two proposers and three acceptors run as five ordinary processes over
blocking TCP, with no libcmc header and no rewrite for the checker --
only the eight-line state hook every target has.  cmc is the network
between them, and explores every order in which the acceptors take the
proposers' requests.

## The protocol

`paxos.c` is textbook single-decree Paxos.  Nodes 0 and 1 are proposers,
2, 3 and 4 acceptors; node `i` listens on 127.0.0.1 port 20000 + `i`.
Each request is its own connection: a proposer dials an acceptor, sends,
and waits for the reply; an acceptor accepts, reads, persists, replies,
and goes back to accept.

- Proposer `i` uses ballot `i+1` and proposes value `i+1`.
- Phase 1: send PREPARE to the acceptors until a majority promise.
- Phase 2: send ACCEPT(ballot, v) to those that promised, where `v` is
  the value of the highest-ballot acceptance seen among the promises, or
  its own if none.  Decide `v` once a majority accept.

An acceptor persists its state (on descriptor 193) before it replies, as
Paxos requires; a proposer persists its decision.  A crash keeps only
that durable state.

The safety property is **agreement**: the two proposers never decide
different values.

## Check it

```sh
cc -no-pie -o paxos examples/paxos/paxos.c
cmc --model-file examples/paxos/paxos.model --binary ./paxos
```

```
Model checking completed. No error has been found.
964 states generated, 561 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 28.
```

cmc ran all five processes, tried every interleaving of the two
proposers' request rounds against the three acceptors, and found no
state where the proposers disagree.

Searching harder, with one crash and one lost message allowed:

```sh
cmc --model-file examples/paxos/paxos.model --binary ./paxos --crashes 1 --drops 1
```

```
Model checking completed. No error has been found.
68947 states generated, 30075 distinct states found, 0 states left on queue.
```

(about three and a half minutes, single-threaded; each state is a fresh
run of all five processes).

## The planted bug

Built with `-DBUG`, the proposer skips the heart of Paxos: it ignores
the values in the promises and always proposes its own.  Two proposers
can then drive different values to acceptance.

```sh
cc -no-pie -DBUG -o paxos-bug examples/paxos/paxos.c
cmc --model-file examples/paxos/paxos.model --binary ./paxos-bug
```

```
Error: Invariant agreement is violated.
Error: The behavior up to this point is (24 actions):
  ...
```

cmc prints the shortest sequence of accepts and deliveries that reaches
two different decisions.  Simulation finds it too, from the first few
hundred random walks:

```sh
cmc --model-file examples/paxos/paxos.model --binary ./paxos-bug --simulate --seed 1 --walks 2000
```

## What this shows

The protocol code is executed, not transcribed into a model.  The same
source, built two ways, is the correct protocol and the bug; the model
file holds only the environment to explore and the one invariant.  A
pass covers the budgets given -- raise `--crashes` and `--drops` until
your own planted bugs are caught.
