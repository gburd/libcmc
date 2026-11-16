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
other two with RequestVote.  A node grants its vote at most once per
term, to a candidate whose term is at least its own; a candidate with a
majority (2 of 3) becomes leader.  An RPC carrying a higher term makes
the receiver step down and adopt that term.

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
249 states generated, 182 distinct states found, 0 states left on queue.
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
1706 states generated, 1117 distinct states found, 0 states left on queue.
```

(about six seconds, single-threaded).

## The planted bug (withdrawn)

Built with `-DBUG`, a node grants a vote without recording whom it voted
for, so it can vote for two different candidates in the same term.  Both
candidates could then reach a majority and become leader in that term,
the classic Raft election bug.

Up to 1.3.0 cmc reported this violation, but the counterexample is not
a Linux run.  The engine then kept one channel per node pair, so a
candidate waiting on the connection it dialled was handed the other
candidate's RequestVote, sent on a different connection, as its reply.
With connections modelled one by one (soundness plan 3.7) two candidates
deadlock instead: each waits for a reply from a node that is itself
waiting outside `accept()`, so neither ever counts the second vote, and
the bug is not reachable with this program.  The gate row is withdrawn
until raft.c serves RPCs while it campaigns (plan 4.7).

The pass above is the same kind of result: it holds for the schedules
this program can reach, and in all of them at most one candidate
collects its votes.

## What this shows

The election logic is executed, not transcribed.  The same source,
built two ways, is correct Raft and the double-vote bug.  Writing the
example, the checker also caught a mistake in the state hook (the
crash-durable record had a different byte layout, so a crashed node was
misread as a leader) -- the kind of thing a declarative model hides and
a run of the real code does not.
