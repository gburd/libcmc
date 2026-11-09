# ZooKeeper Atomic Broadcast (ZAB)

ZAB, the primary-backup atomic broadcast at the heart of ZooKeeper,
checked by libcmc as it ships.  A leader proposes a transaction, the
followers store it, and the leader commits -- and only then
acknowledges the client -- once a quorum has it.  The safety property
checked here is that a committed (acknowledged) transaction is never
lost, even if the leader crashes.

Three nodes run as ordinary processes over blocking TCP, no libcmc
header.  Node 0 is the leader, 1 and 2 the followers; a quorum is two of
three.  A client submits one transaction.

## Reference

- **ZAB**: Flavio Junqueira, Benjamin Reed, Marco Serafini, *Zab:
  High-performance broadcast for primary-backup systems*, DSN 2011.
- **Canonical TLA+**: Apache ZooKeeper's `Zab.tla`,
  <https://github.com/apache/zookeeper/blob/master/zookeeper-specifications/protocol-spec/Zab.tla>.
  Its named invariants include `Leadership` (at most one leader per
  epoch), `PrefixConsistency`, `Agreement` (if one process delivers a
  transaction, every process delivers it or a later one) and
  `TotalOrder`.  The `no_lost_commit` property checked below is the
  commit-durability that underlies `Agreement`: a delivered transaction
  survives.
- Background on the protocol: the ZooKeeper internals documentation,
  <https://zookeeper.apache.org/doc/current/zookeeperInternals.html>.

## See it fail, animated

Marc Brooker (AWS) animates this failure class -- a replicated system
losing an acknowledged write because durability was not truly achieved
-- in *Animations for System Builders*:
<https://brooker.co.za/blog/2026/10/23/animations.html>.  libcmc catches
it in the `-DBUG` build and prints the shortest schedule that causes it.

## Check it

```sh
cc -no-pie -o zab examples/zab/zab.c
cmc --model-file examples/zab/zab.model --binary ./zab --crashes 1
```

```
Model checking completed. No error has been found.
62 states generated, 45 distinct states found, 0 states left on queue.
```

## The planted bug

Built with `-DBUG`, the leader acknowledges the client as soon as the
transaction is in its own memory, before any follower has stored it.  A
crash of the leader then loses a committed transaction.

```sh
cc -no-pie -DBUG -o zab-bug examples/zab/zab.c
cmc --model-file examples/zab/zab.model --binary ./zab-bug --crashes 1
```

```
Error: Invariant no_lost_commit is violated.
Error: The behavior up to this point is (3 actions):
    1. accept client->n0
    2. deliver client->n0
    3. crash n0
  node states: node[0]=1,0 node[1]=1,0 node[2]=1,0
  replies: (n0,7)
```

The client holds an acknowledgement for transaction 7, and no node has
it.  Simulation finds it too.

## What this shows

The commit rule -- acknowledge only after a quorum has the transaction
-- is the protocol, executed rather than transcribed.  The correct build
obeys it and loses nothing; the `-DBUG` build commits early and loses an
acknowledged transaction to a crash.
