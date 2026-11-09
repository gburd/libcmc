# Humming Consensus / chain replication

Chain replication, checked by libcmc as it ships.  This is the protocol
shape libcmc was built for: a replicated log kept by a chain of nodes,
where the one safety property is that **an acknowledged write is never
lost** -- even when a node crashes or the chain is reconfigured.

Three nodes run as ordinary processes over blocking TCP, with no libcmc
header.  For epoch 1 the chain is head 0 -> tail 1, with node 2 a spare;
a reconfiguration (driven by a timeout at the spare) moves to epoch 2
with chain head 0 -> tail 2, inheriting the committed value so a failed
tail loses nothing.  A client write is committed -- and only then
acknowledged -- once the tail has stored it.

## Reference

- **Chain replication**: Robbert van Renesse and Fred B. Schneider,
  *Chain Replication for Supporting High Throughput and Availability*,
  OSDI 2004.  <https://www.cs.cornell.edu/home/rvr/papers/OSDI04.pdf>
  The safety argument is the update-propagation invariant: an
  acknowledged update is held by every server from the tail back.
- **Humming Consensus** is the Machi chain-management protocol by Scott
  Lystig Fritchie; its high-level specification is in the Machi project
  (`basho/machi`, `doc/src.high-level/high-level-chain-mgr.tex`).
- There is no single canonical TLA+ specification of chain replication;
  the OSDI 2004 paper is the formal reference, and `no_lost_ack` below is
  its acknowledged-update-is-durable property.

## See it fail, animated

Marc Brooker (AWS) has an animation of exactly this failure -- a Raft
cluster losing an acknowledged write because durability was not really
achieved -- in *Animations for System Builders*:
<https://brooker.co.za/blog/2026/10/23/animations.html> (the
`lying-fsync` animation).  libcmc catches the same failure class in the
`-DBUG` build below, and prints the shortest schedule that causes it.

## Check it

```sh
cc -no-pie -o hc examples/hc/hc.c
cmc --model-file examples/hc/hc.model --binary ./hc --crashes 1
```

```
Model checking completed. No error has been found.
524 states generated, 285 distinct states found, 0 states left on queue.
```

cmc runs all three processes and tries every order of the write
replicating down the chain, the reconfiguration, and a crash of any
node, and finds no acknowledged write that is lost.

## The planted bug

Built with `-DBUG`, the head acknowledges the client as soon as the
write is in its own memory, before the tail has committed it.  If the
head then crashes before the tail stored the write, the acknowledged
value is on no node's durable state.

```sh
cc -no-pie -DBUG -o hc-bug examples/hc/hc.c
cmc --model-file examples/hc/hc.model --binary ./hc-bug --crashes 1
```

```
Error: Invariant no_lost_ack is violated.
Error: The behavior up to this point is (3 actions):
    1. accept client->n0
    2. deliver client->n0
    3. crash n0
  node states: node[0]=1,0 node[1]=1,0 node[2]=1,0
  replies: (n0,7)
```

The client holds an acknowledgement for 7, and value 7 is on no node --
the acknowledged write was lost to the crash.  Simulation finds it
within a couple of random walks too.

## What this shows

The commit rule -- acknowledge only after the tail has it -- is the whole
protocol, and it is executed, not transcribed.  The correct build obeys
it and loses nothing; the `-DBUG` build acknowledges early and loses an
acknowledged write to a crash, the failure Brooker's animation shows.
