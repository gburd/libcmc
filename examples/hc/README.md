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
6026 states generated, 2694 distinct states found, 0 states left on queue.
```

**This pass is vacuous.**  In every state cmc reaches, no participant
has adopted any projection, so agreement holds trivially; an invariant
that no participant ever adopts holds too.  A participant's iteration
dials its peers while they are doing the same, and each one waits for a
reply from a peer that is itself waiting outside `accept()`.  Up to
1.3.0 the engine kept one channel per node pair and handed those waiting
participants each other's requests as replies, which is how the
iterations ever finished; with connections modelled one by one
(soundness plan 3.7) they do not.  The example needs to serve requests
while it iterates before it shows anything (plan 4.8).

## The planted bug (withdrawn)

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

Up to 1.3.0 this run reported the violation, but only through the
cross-connection replies described above, so it was not a Linux run.
With connections modelled, no participant adopts and the bug is out of
reach; the gate row is withdrawn until the example is reworked.

## What this shows

As it stands, nothing: the program never reaches the adoption rule it
was written to check.  The finding is itself the point of the soundness
work.  A checker that answers a read with bytes from the wrong
connection can make a deadlocked program look like a working protocol,
and a pass over such a program says only that no state was bad, not
that any interesting state was reached.  Pair a safety invariant with an
`eventually` property the program must reach, as `test/proc_gate.sh`
does for its fixtures.
