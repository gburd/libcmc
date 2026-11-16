# Raft leader election

Raft's leader election, checked by libcmc as it ships.  Three nodes run
as ordinary processes over blocking TCP, with no libcmc header and no
rewrite for the checker.  cmc owns the election timers and the order of
the RPCs, so it explores every election race -- simultaneous candidates,
split votes, a candidate that crashes mid-election -- and checks the one
theorem leader election must satisfy: **Election Safety**, at most one
leader per term.

(This is the election half of Raft.  Log replication is a separate
concern; this example is complete and real for what it covers.)  The
companion [`raftlog.c`](raftlog.c) is the log-replication half: it checks
that a committed entry is never lost, the failure Brooker animates.

## Reference

- **Canonical TLA+**: Diego Ongaro's `raft.tla`,
  <https://github.com/ongardie/raft.tla>.  The five Raft safety
  properties -- Election Safety, Leader Append-Only, Log Matching,
  Leader Completeness, State Machine Safety -- are proved against it in
  Appendix B of Ongaro's PhD dissertation.  The `election_safety`
  invariant checked below is the first of them: at most one leader per
  term.
- **Visualizations**: *The Secret Lives of Data*,
  <https://thesecretlivesofdata.com/raft/> (narrated, animated); and
  RaftScope, the interactive visualization on the official site,
  <https://raft.github.io/>.
- **See a lost write, animated**: Marc Brooker (AWS) animates a Raft
  cluster losing an acknowledged write when durability is not truly
  achieved, in *Animations for System Builders*:
  <https://brooker.co.za/blog/2026/10/23/animations.html>.  The
  companion `raftlog.c` here shows exactly that failure: a leader that
  commits and acknowledges a client before a quorum has the entry, then
  crashes, loses a committed entry (State Machine Safety), and cmc
  prints the shortest schedule that does it.  The chain-replication
  ([`examples/hc`](../hc)) and ZAB ([`examples/zab`](../zab)) examples
  catch the same failure class.

## The protocol

`raft.c` is a Raft election, reduced: no logs, so the up-to-date check
on a vote is absent.  Each node listens on 127.0.0.1
port 20000 + its id and sits in `accept()`, idle.  The checker drives
time: a node whose election timer fires (a timeout cmc chooses) becomes
a candidate -- it increments its term, votes for itself, and dials the
other two with RequestVote.  It then waits for the replies in a `poll()`
loop that also serves any RPC arriving on its listener, since another
candidate may be asking for its vote at the same moment.  A node grants
its vote at most once per term, to a candidate whose term is at least
its own; a candidate with a majority (2 of 3) becomes leader.  An RPC
carrying a higher term makes the receiver step down and adopt that
term.

Persistent Raft state -- `currentTerm` and `votedFor` -- is written on
descriptor 193, so it survives a crash; a recovering node comes back a
follower.  The volatile role is on descriptor 3.

## Check it

```sh
cc -no-pie -o raft examples/raft/raft.c
cmc --model-file examples/raft/raft.model --binary ./raft
```

```
Model checking completed. No error has been found.
9460 states generated, 4090 distinct states found, 0 states left on queue.
```

Two nodes may time out and stand for election; cmc tries every
interleaving of the two election rounds across the three nodes and finds
no term with two leaders.

Allowing a crash as well (a candidate or leader can die at any point,
keeping only its persistent state):

```sh
cmc --model-file examples/raft/raft.model --binary ./raft --crashes 1
```

```
Model checking completed. No error has been found.
41272 states generated, 14062 distinct states found, 0 states left on queue.
```

(about six seconds, single-threaded).

## The planted bug

Built with `-DBUG`, a node grants a vote without recording whom it voted
for, so it can vote for two different candidates in the same term.  Both
candidates then reach a majority and become leader in that term -- the
classic Raft election bug.

```sh
cc -no-pie -DBUG -o raft-bug examples/raft/raft.c
cmc --model-file examples/raft/raft.model --binary ./raft-bug
```

```
Error: Invariant election_safety is violated.
Error: The behavior up to this point is (10 actions):
    1. timeout n0
    2. accept n0->n1
    3. deliver n0->n1
    4. wake n1->n0
    5. deliver n1->n0
    6. timeout n2
    7. accept n2->n1
    8. deliver n2->n1
    9. wake n1->n2
    10. deliver n1->n2
  node states: node[0]=2,1,1 node[1]=0,1,0 node[2]=2,1,3
```

Node 1 grants node 0 in term 1 (steps 2 to 5), then grants node 2 in
the same term (steps 7 to 10), and both become leader.  Simulation finds
it within a few random walks too.

Up to 1.3.0 a candidate waited for each reply in turn, outside
`accept()`.  The checker then kept one channel per node pair and handed
a waiting candidate its rival's RequestVote as the reply, which is how
two candidates ever finished an election; with connections modelled one
by one they deadlocked, and the bug was out of reach.  Serving RPCs
while campaigning, as a real Raft node must, is the fix.

## What this shows

The election logic is executed, not transcribed.  The same source,
built two ways, is correct Raft and the double-vote bug.  Writing the
example, the checker also caught a mistake in the state hook (the
crash-durable record had a different byte layout, so a crashed node was
misread as a leader) -- the kind of thing a declarative model hides and
a run of the real code does not.
