# Paxos variants and Heidi Howard's research: a survey for the test matrix

This survey grounds the libcmc Paxos-family test matrix.  For each
protocol it pins the one safety invariant a checker should assert and
the one knob whose corruption is the classic bug, because those two
facts are exactly what the `.cml` idiom needs: a pigeonhole-over-quorums
invariant plus a `[BUG]` action that drops the guard (as in
`examples/spec/paxos.cml`, `raft.cml`, `hc.cml`, `zab.cml`).

Citations to primary sources are given with authors, year and venue;
arXiv identifiers and the `tlaplus/Examples` repository paths are worth a
confirming click before being quoted downstream.

## Part 1: Heidi Howard's consensus research

Howard's work concerns the foundations of consensus: what the quorum
requirement in Paxos actually is, how far it can be relaxed without
breaking safety, and whether the apparent zoo of consensus algorithms is
one algorithm in disguise.  The through-line is that Paxos is
over-constrained as usually stated, and that loosening it carefully both
speeds it up and clarifies it.

### 1.1 Flexible Paxos (FPaxos)

Heidi Howard, Dahlia Malkhi, Alexander Spiegelman, "Flexible Paxos:
Quorum Intersection Revisited," OPODIS 2016; arXiv:1608.06696.

Classical Paxos requires every quorum to be a majority so any two
quorums intersect.  FPaxos shows this is stronger than necessary.  Paxos
runs in two phases (Phase 1 prepare/promise, Phase 2 accept/accepted).
The only intersection the safety argument uses is between a Phase 1
quorum of one leader and the Phase 2 quorums of earlier leaders.  So the
real requirement is: every Phase 1 quorum must intersect every Phase 2
quorum.  Phase 1 quorums need not intersect each other, and Phase 2
quorums need not intersect each other.

The theorem: let Q1 be the allowed Phase 1 quorums and Q2 the allowed
Phase 2 quorums.  Paxos is safe provided, for all Q1 in Q1 and Q2 in Q2,
Q1 and Q2 intersect.  A sufficient instance with N acceptors is any
|Q1| and |Q2| with |Q1| + |Q2| > N (for example N=5, |Q2|=2 for cheap
steady-state replication, |Q1|=4 for a rarer, costlier election).
Majority is the special case |Q1| = |Q2| = floor(N/2)+1.  Grid quorums (a
row for Q1, a column for Q2) also satisfy it while letting two rows, or
two columns, be disjoint.

Safety property: Agreement (at most one value chosen).  The knob: the
cross-phase intersection.  The classic bug is to shrink Phase 2 quorums
without enlarging Phase 1 to compensate, i.e. |Q1| + |Q2| <= N; then a
new leader's Phase 1 quorum can miss a chosen value and two values are
chosen.  This is the most instructive planted bug in the family: a
correct generalization that becomes unsafe at exactly one inequality.

### 1.2 DPhil thesis: "Distributed consensus revised"

Heidi Howard, *Distributed consensus revised*, PhD thesis, University of
Cambridge Computer Laboratory, 2019 (UCAM-CL-TR-935).

The thesis re-derives Paxos from first principles and then proves a
sequence of safe relaxations of its quorum and value-selection rules.
The unifying device separates state that must be immutable once a quorum
has observed it from state that may be revised.  The relaxations it
proves safe include: the FPaxos cross-phase intersection; quorums that
vary across proposers and epochs (so reconfiguration needs no separate
protocol, provided cross-phase intersection holds for the relevant
pairs); and weakened value-selection rules that preserve the
anchored-value invariant.

Safety property: non-triviality plus Agreement.  The knob: a chosen
value stays anchored, which breaks two ways: (a) weaken intersection
past the cross-phase condition (FPaxos above); (b) keep intersection but
corrupt value selection so a new leader proposes a fresh value though an
intersecting acceptor reported an accepted one.  Two distinct bugs.

### 1.3 "Paxos vs Raft: have we reached consensus on distributed consensus?"

Heidi Howard, Richard Mortier, PaPoC 2020 (EuroSys workshop);
arXiv:2004.05074.

Multi-Paxos and Raft are the same core algorithm, differing mainly in
how a new leader learns which entries are already committed and what it
may safely propose.  Both elect a leader by a quorum, both replicate to
a quorum, both guarantee state-machine safety.  Raft's "leader with the
most up-to-date log wins" is one concrete instantiation of Paxos's
Phase 1 value recovery.  The visible differences (strong leader,
contiguous log, terms vs ballots) are engineering choices, not a
different safety core.

Safety property: State-Machine Safety / Log Matching (no committed entry
lost or overwritten).  The knob: the election restriction that
guarantees a new leader's log is at least as current as any committed
entry (Raft's vote predicate; Paxos's Phase 1 recovery).  The point for
libcmc: `paxos.cml`'s Phase 1 guard and `raft.cml`'s one-vote-per-term
guard are the same constraint in different clothes.

### 1.4 Reconfiguration and Byzantine work

Matchmaker Paxos (Michael Whittaker, Neil Giridharan, Adriana Szekeres,
Joseph M. Hellerstein, Heidi Howard, Faisal Nawab, Ion Stoica, 2020/2021;
arXiv:2007.09468) makes reconfiguration first-class: matchmaker nodes
store the configuration history, and a new leader queries a matchmaker
quorum to learn every prior configuration whose quorums could hold a
chosen value, then intersects all of them.  Safety: a value chosen in an
old configuration is never lost across a change.  Knob: completeness of
the matchmaker log and reading it from a matchmaker quorum; drop a prior
configuration and a value chosen there is lost.  This is the
cross-epoch intersection knob of the thesis.

Howard also co-authored Byzantine/confidential consensus work in the
Confidential Consortium Framework (CCF).  That safety core rests on
cryptographic non-equivocation, a poor fit for an explicit-state
crash-fault checker (Part 3).

### 1.5 Formal methods

"Smart casual verification of the Confidential Consortium Framework"
(NSDI 2025, Howard among the authors) uses TLA+ as a separate
specification whose actions are checked against traces emitted by the
production C++ (trace validation in TLC), plus model checking and
simulation.  This is the same "model and code must not diverge" problem
libcmc addresses, solved there by trace-linking; libcmc's `--refine`
couples them more tightly by running the shipped code as the transition
relation.  Community TLA+ specifications of FPaxos exist.

## Part 2: the Paxos family and real-world use

For each: canonical reference; the invariant a model checker asserts;
the knob whose corruption is the classic bug; whether a well-known TLA+
spec exists.

- **Single-decree (Synod) Paxos.**  Lamport, "The Part-Time Parliament"
  (TOCS 1998) and "Paxos Made Simple" (2001).  Invariant: Agreement.
  Knob: per-ballot write-once discipline and the majority quorum.  TLA+:
  yes, Lamport's `Paxos.tla` / `Voting.tla` (tlaplus/Examples).  libcmc
  mirrors this in `examples/spec/paxos.cml`.
- **Multi-Paxos.**  van Renesse, Altinbuken, "Paxos Made Moderately
  Complex" (CSUR 2015).  Invariant: State-Machine Safety.  Knob: the
  leader's recovery of already-chosen slots in Phase 1.  TLA+: yes.
- **Fast Paxos.**  Lamport (Distributed Computing 2006).  Invariant:
  Agreement under fast rounds.  Knob: the fast-quorum size (fast rounds
  need |Qfast| > 3N/4); a plain majority for a fast round lets a collided
  round choose two values.  TLA+: yes.  Needs N >= 5 for the bug to be
  reachable.
- **Cheap Paxos.**  Lamport, Massa (DSN 2004).  Invariant: Agreement with
  auxiliary acceptors used only on reconfiguration.  Knob: counting an
  auxiliary in a steady-state quorum.  TLA+: no well-known standalone.
- **Generalized Paxos.**  Lamport (MSR-TR-2005-33).  Invariant:
  compatibility of learned command structures.  Knob: the
  commutativity relation on commands.  TLA+: partial.
- **Egalitarian Paxos (EPaxos).**  Moraru, Andersen, Kaminsky (SOSP
  2013).  Invariant: conflicting commands execute in the same order at
  all replicas.  Knob: dependency tracking (an incomplete or asymmetric
  dependency set).  TLA+: yes; specs later found recovery flaws.
- **Mencius.**  Mao, Junqueira, Marzullo (OSDI 2008).  Invariant:
  State-Machine Safety with rotating slot ownership.  Knob: the skip
  (no-op) rule for an absent owner's slot.  TLA+: no well-known.
- **Vertical Paxos.**  Lamport, Malkhi, Zhou (PODC 2009).  Invariant:
  Agreement across reconfiguration.  Knob: the hand-off ordering (old
  read quorum intersects the write quorum that chose).  TLA+: partial.
- **Stoppable Paxos.**  Lamport, Malkhi, Zhou (2008).  Invariant:
  Agreement plus: after a stop command is chosen, no later slot is
  chosen.  Knob: the no-proposal-beyond-stop rule.  TLA+: no well-known.
- **Flexible Paxos.**  Part 1.1.  Invariant: Agreement.  Knob:
  |Q1| + |Q2| > N.  TLA+: yes.
- **Ring Paxos.**  Marandi, Primi, Schiper, Pedone (DSN 2010).
  Invariant: atomic broadcast total order.  Knob: the ring ack
  aggregation.  TLA+: no well-known; safety core is classic Paxos.
- **WPaxos.**  Ailijiang, Charapko, Demirbas, Kosar (IEEE TPDS 2020;
  arXiv:1703.08905).  Invariant: Agreement with object stealing.  Knob:
  FPaxos cross-phase intersection plus a monotone object ballot on a
  steal.  TLA+: a spec exists.
- **CASPaxos.**  Rystsov (2018; arXiv:1802.07000).  Invariant: Agreement
  on a register updated by a client change function.  Knob: Phase 1 /
  Phase 2 intersection per CAS; applying on a non-quorum read.  TLA+: a
  spec exists.
- **Matchmaker Paxos.**  Part 1.4.  Invariant: Agreement across
  reconfiguration.  Knob: matchmaker-log completeness.  TLA+: in the
  paper's materials.

Real-world use: Chubby (Multi-Paxos), Spanner (Multi-Paxos per group),
etcd / Consul / TiKV / CockroachDB (Raft), ZooKeeper (ZAB), Cassandra and
ScyllaDB LWT (single-decree Paxos per key), FoundationDB (disk-Paxos
lineage for coordination), Neon (Paxos-derived WAL safekeepers), Azure
CCF (Raft, Byzantine-hardened).

## Part 3: fit for an explicit-state bounded model checker

libcmc is explicit-state, breadth-first, exhaustive to a bound, over
finite small domains (`CMC_MAX_NODES` 8, bounded messages), with a
TLA+/Apalache bridge and refinement of real C.  Its liveness is bounded
and under a fixed fair schedule.  The idiom that works: reduce safety to
a pigeonhole-over-quorums invariant (a function-var tally plus
`count ... >= Q`) and plant the classic bug as a `[BUG]` action that
drops the guard.  The decisive question for each protocol: does a small
finite instance expose the bug, and is the safety property a state (or
step) invariant over a tally?

### Strong fit

These reduce to a single-slot / single-register / single-term pigeonhole
invariant, the idiom already proven in-repo.  The bug fires at N=3, Q=2,
or N=5 for the quorum-split variants.

| Variant | Minimal instance | Planted-bug knob |
|---|---|---|
| Single-decree Paxos (done) | 3 acceptors, 2 ballots, 2 values | double vote without the safety guard |
| Flexible Paxos | 5 acceptors, 2 values | set `Q1 + Q2 <= A` |
| Raft election (done) | 3 servers, one term | grant without spending the one vote |
| ZAB commit-durability (done) | 3 participants | commit without a quorum (early ack) |
| Humming Consensus (done) | 3 participants, 2 values | overwrite a write-once register |
| CASPaxos | 3 acceptors, 1-2 CAS steps | apply on a non-quorum read |
| Vertical Paxos (two configs) | old + new, 3+3 overlap | activate new config before old read quorum |
| Matchmaker Paxos (two configs) | 2 configs, 3 matchmakers | drop a prior config |
| WPaxos (one steal) | 5 nodes, 1 object, 1 steal | disjoint phase quorums, or a steal that drops the value |
| Mencius (two slots) | 3 servers, 2 slots | skip a slot without the quorum |
| Fast Paxos (collision) | 5 acceptors, 2 values, 1 fast round | plain majority for the fast round |

Fast Paxos and FPaxos both need N >= 5 for the bug to be reachable (a
majority must be strictly smaller than the quorum the protocol actually
requires); at N=3 the bug cannot appear.

### Partial fit (a reduction that loses something)

Multi-Paxos / Raft-log / Generalized-Paxos as multi-slot logs: the
distinctive safety (a committed prefix survives arbitrary leader changes
across many slots) is not directly expressible in the bounded DSL.  The
proven reduction is the single-contested-slot / single-transaction
collapse (the `raftlog.c` and `zab.cml` approach): model one slot and its
quorum-intersection guarantee, which is genuinely distinct from plain
Agreement (it is durability: committed implies quorum-backed).  This
catches "stale leader overwrites a committed entry" at the one slot but
does not check log matching across slots, contiguity, or commit-index
monotonicity.  State that limit.

### Poor fit (wrong tool)

- **EPaxos full execution consistency.**  The safety that matters is
  "all replicas execute conflicting commands in the same order," a
  property of the dependency graph over a set of commands.  Exposing the
  recovery bug needs enough commands and conflicts to form a cycle; the
  dependency domain grows fast and symmetry does not rescue it (commands
  are not interchangeable once they conflict asymmetrically).  A stretch
  goal at best, single conflict pair, and a PASS is "no violation in N
  states," not a proof.
- **Byzantine / CCF consensus.**  Safety rests on cryptographic
  non-forgeability and non-equivocation.  An explicit-state crash-fault
  checker has no model of forged-but-valid messages; encoding crypto as
  finite bytes either trivializes it or explodes it.  Wrong tool.
- **Liveness / FLP-bounded properties.**  "Eventually a value is
  chosen," leader stabilization, progress under partial synchrony.
  libcmc's liveness is bounded and under a fixed fair schedule; it can
  check "under this fair schedule the goal is reached in <= L steps" but
  cannot establish termination under adversarial-but-fair scheduling, and
  does not model real-time timeouts or leases as timing.  Paxos safety is
  in scope; Paxos liveness is mostly not.
- **Performance properties.**  Latency, throughput and message count, the
  point of the Fast / Ring / WPaxos optimizations, are not checkable
  here.  Those optimizations change safety only through their quorum
  knobs, which the strong-fit rows capture; the speedup itself is not a
  property a model checker evaluates.
- **Very large configuration spaces.**  Reconfiguration over many
  configurations, EPaxos over many commands, multi-slot logs over many
  slots: state spaces grow past the bound even with symmetry, because the
  interesting objects are not interchangeable.  The answer is the
  reduction (single slot, two configs, one steal) with its limit stated,
  not a claim that the full protocol is verified.

### Guidance for the matrix

Build the matrix around the quorum-intersection knob, the one thing this
tool checks well and the single knob behind Paxos, FPaxos, Raft, ZAB, HC,
CASPaxos, and the reconfiguration variants.  Each becomes a function-var
tally, a `count ... >= Q` guard, a one-line invariant, and a `[BUG]`
action, cross-checked by `cmc --spec`, TLC and Apalache, and (where a C
implementation exists) `--refine`d.  FPaxos is the headline new test: its
correct and unsafe forms differ by one inequality, and it is the direct
expression of Howard's central theorem.
