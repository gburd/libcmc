# Humming Consensus

[Humming Consensus][hc] (HC), by Scott Lystig Fritchie, checked by
libcmc as it ships.  HC is a **generic agreement protocol** -- in the
same family as Paxos, Raft and ZAB -- for agreeing on a *sequence of
configurations*, one per epoch.  The configuration is an opaque,
system-specific value; the paper's examples are "a list of replicas of a
key-value store or a flat ASCII file".  Chain replication (Machi's use
of HC) is just *one* such configuration -- HC itself knows nothing about
chains.  This example agrees on an opaque one-byte configuration, to
show HC as the agreement protocol it is.

Three participants run as ordinary processes over blocking TCP, with no
libcmc header.  Each holds a **write-once epoch register store**, keyed
by epoch, readable by all; **every participant communicates only
indirectly, through reads and writes of these stores** (paper section
3).  A participant takes one iteration of the algorithm when the
environment fires a timeout at it.

An iteration (the paper's Figure 2): read every participant's latest
projection; let `E_max` be the greatest epoch seen; if a store has no
value at `E_max`, repair it (write-once, so an existing value is never
changed); else if a **quorum majority** agrees on the value at `E_max`
and it is newer than what we hold, **adopt** it; else abandon the
dissenting epoch and converge on a single new suggestion at `E_max + 1`.

## Reference

- **Humming Consensus**: Scott Lystig Fritchie, *Coordinating Distributed
  System Configuration Changes with Humming Consensus*, PaPoC 2016
  (an EuroSys workshop).
  <http://snookles.com/scott/publications/papoc2016-slf.pdf>.  The
  algorithm is Figure 2; the write-once epoch register store is Figure 1.
- **Reference implementation**: the only implementation is inside
  [Machi][machi], a distributed blob store.  The agreement logic -- the
  projection ranking and the unanimity rule this example mirrors -- is in
  [`machi_chain_manager1.erl`][mgr] (`rank_projection`,
  `rank_and_sort_projections_with_extra`).  Research code toward a
  standalone implementation is in Fritchie's
  [`humming-consensus`][hclite] repo (branch `hc-lite/base-work`).
- **Safety property.**  HC guarantees safety, not liveness (the paper is
  explicit: "an infinitely fast jerk can make liveness impossible").
  Its safety property is **agreement**: at most one configuration is
  adopted per epoch.  The paper rests this on write-once registers plus
  read-all unanimity; in strongly-consistent mode (section 6.3) a
  quorum majority of reads is required, and quorums intersect.  The
  `agreement` invariant below is exactly that property.
- **TLA+ specification**: [`HummingConsensus.tla`](HummingConsensus.tla)
  in this directory is a full TLA+ model of HC (write-once public/private
  stores, read-repair, ranking, unanimity, chain data).  It is the model
  libcmc's calibration core (`calib/hc/`) is checked against, row by row,
  with single-worker TLC.  Note it models the Machi chain-oriented HC in
  detail, which is richer than the opaque-config example here (`hc.c`);
  it is included as the authoritative formal reference and the
  calibration oracle, not a line-by-line spec of this example's C.  The
  planned abstract-model + binding work (`docs/plans/tlaplus-scope.md`)
  will close that gap, so one abstract model checks both the spec and the
  shipped code.

[hc]: https://www.snookles.com/scott/presentations/papoc2016-slf.slides.pdf
[machi]: https://github.com/basho/machi
[mgr]: https://github.com/basho/machi/blob/master/src/machi_chain_manager1.erl
[hclite]: https://github.com/slfritchie/humming-consensus/tree/hc-lite/base-work

## How the invariant maps to the safety property

```
agreement = "all i in nodes : all j in nodes :
             node[i].epoch == 0 or node[j].epoch == 0
             or node[i].epoch != node[j].epoch
             or node[i].config == node[j].config"
```

"No two participants have adopted different configurations for the same
epoch" -- HC's agreement property, written over all pairs (a participant
that has adopted nothing has epoch 0).  This is the direct statement of
"at most one configuration is adopted per epoch".

## See it, interactively

- The paper's [slides][hc] walk the algorithm and the network-partition
  cases.
- Fritchie's RICON 2015 talk, *Managing Chain Replication Metadata with
  Humming Consensus*, has a video: <https://www.youtube.com/watch?v=yR5kHL1bu1Q>.
- Marc Brooker (AWS), *Animations for System Builders*, animates how
  distributed agreement goes wrong when durability is not real:
  <https://brooker.co.za/blog/2026/10/23/animations.html>.

## Check it

```sh
cc -no-pie -o hc examples/hc/hc.c
cmc --model-file examples/hc/hc.model --binary ./hc
```

```
Model checking completed. No error has been found.
15202 states generated, 6393 distinct states found, 0 states left on queue.
```

cmc runs all three participants and tries every order of their
iterations and every pattern of a dropped message (an asymmetric network
partition), and finds that no two ever adopt different configurations at
one epoch -- **agreement holds even under a partition**, because adoption
needs a quorum majority and quorums intersect.  That is the strong,
positive result: the shipping code is proven, not just tested.

## The planted bug

Built with `-DBUG`, a participant runs in eventual-consistency ("AP")
mode: it adopts a projection it sees as unanimous **without** requiring a
quorum majority, so a participant isolated with a minority of stores
adopts anyway.  Under an asymmetric partition two participants each adopt
from a different minority and end up with different configurations at the
same epoch -- the conflict the paper notes is invisible until the
partition heals.

```sh
cc -no-pie -DBUG -o hc-bug examples/hc/hc.c
cmc --model-file examples/hc/hc.model --binary ./hc-bug \
    --timeouts 6 --drops 1 --simulate --seed 3
```

```
Error: Invariant agreement is violated.
```

(The violation needs a long partition schedule, so simulation reaches it
faster than the exhaustive search; a fixed seed makes it reproducible.)

## What this shows

HC is a general agreement protocol, and its agreement rule -- adopt only
a projection a quorum agrees on, over write-once stores -- is executed,
not transcribed.  The correct build keeps agreement even under a network
partition; the `-DBUG` build relaxes the quorum requirement and loses it,
and cmc prints a schedule that does.
