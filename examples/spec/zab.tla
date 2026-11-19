---------------------------- MODULE zab ----------------------------
\* Hand-written TLA+ twin of examples/spec/zab.cml, for the calibration
\* against single-worker TLC (test/spec_calibrate.sh).
\*
\* Reference: Flavio Junqueira, Benjamin Reed, Marco Serafini, "Zab:
\*   High-performance broadcast for primary-backup systems", DSN 2011,
\*   and Apache ZooKeeper's Zab.tla (zookeeper-specifications).  The
\*   phases are the paper's: discovery (ACKEPOCH from a quorum; the new
\*   leader takes the history of the most up-to-date member, by current
\*   epoch, then last zxid), synchronisation (followers adopt it and send
\*   ACK-LD; a quorum establishes the epoch and commits that history),
\*   and broadcast (propose, acknowledge, commit a prefix a quorum has
\*   acknowledged).
\*
\* Unlike the .cml, histories are sequences of transactions, each named
\* by the epoch that proposed it, and a zxid is <<epoch, counter>> as in
\* the paper; "most up-to-date" compares (currentEpoch, lastZxid), not
\* the .cml's integer encoding of a history.  ACKEPOCH and ACK-LD
\* messages are sets of records; the last history each follower
\* acknowledged in each epoch is a function ackH.  The simplifications are the .cml's: 3 servers, 2 epochs,
\* histories of at most two transactions, no election messages, no
\* crashes, no loss, and synchronisation as one step.  USE_BUG turns on
\* NewLeaderBug, which takes the history of any quorum member.
EXTENDS Integers, Sequences, FiniteSets
CONSTANTS S, E, Q, MAXH, USE_BUG

Server == 0 .. S-1
Epoch == 1 .. E
Quorum == {Qs \in SUBSET Server : Cardinality(Qs) >= Q}
Hist == UNION {[1 .. n -> Epoch] : n \in 0 .. MAXH}

Prefix(p, q) == Len(p) <= Len(q) /\ SubSeq(q, 1, Len(p)) = p

\* the zxid of a history's last transaction: <<epoch, counter in epoch>>
LastZxid(hs) ==
  IF Len(hs) = 0 THEN <<0, 0>>
  ELSE <<hs[Len(hs)], Cardinality({i \in 1 .. Len(hs) : hs[i] = hs[Len(hs)]})>>
\* (currentEpoch, lastZxid) of an ACKEPOCH, ordered lexicographically
Key(m) == <<m.ce, LastZxid(m.hist)[1], LastZxid(m.hist)[2]>>
LexLeq(a, b) ==
  \/ a[1] < b[1]
  \/ a[1] = b[1] /\ a[2] < b[2]
  \/ a[1] = b[1] /\ a[2] = b[2] /\ a[3] <= b[3]

VARIABLES aE, cE, h, ackepoch, ackld, ackH, lst, lh, cm
vars == <<aE, cE, h, ackepoch, ackld, ackH, lst, lh, cm>>

Init == /\ aE = [s \in Server |-> 0]
        /\ cE = [s \in Server |-> 0]
        /\ h = [s \in Server |-> <<>>]
        /\ ackepoch = {}
        /\ ackld = {}
        /\ ackH = [p \in Epoch \X Server |-> <<>>]
        /\ lst = [e \in Epoch |-> 0]
        /\ lh = [e \in Epoch |-> <<>>]
        /\ cm = [e \in Epoch |-> <<>>]

AckEpoch(s, e) ==
  /\ e > aE[s]
  /\ aE' = [aE EXCEPT ![s] = e]
  /\ ackepoch' = ackepoch \cup {[from |-> s, epoch |-> e, ce |-> cE[s], hist |-> h[s]]}
  /\ UNCHANGED <<cE, h, ackld, ackH, lst, lh, cm>>

\* the leader of epoch e takes the history of m, the most up-to-date
\* member of a quorum of ACKEPOCHs
NewLeader(e) ==
  /\ lst[e] = 0
  /\ \E m \in ackepoch, Qs \in Quorum :
        /\ m.epoch = e /\ m.from \in Qs
        /\ \A n \in Qs : \E mm \in ackepoch :
              mm.from = n /\ mm.epoch = e /\ LexLeq(Key(mm), Key(m))
        /\ lh' = [lh EXCEPT ![e] = m.hist]
  /\ lst' = [lst EXCEPT ![e] = 1]
  /\ UNCHANGED <<aE, cE, h, ackepoch, ackld, ackH, cm>>

NewLeaderBug(e) ==
  /\ lst[e] = 0
  /\ \E m \in ackepoch, Qs \in Quorum :
        /\ m.epoch = e
        /\ \A n \in Qs : \E mm \in ackepoch : mm.from = n /\ mm.epoch = e
        /\ lh' = [lh EXCEPT ![e] = m.hist]
  /\ lst' = [lst EXCEPT ![e] = 1]
  /\ UNCHANGED <<aE, cE, h, ackepoch, ackld, ackH, cm>>

AckLD(s, e) ==
  /\ lst[e] >= 1 /\ aE[s] = e /\ cE[s] < e
  /\ cE' = [cE EXCEPT ![s] = e]
  /\ h' = [h EXCEPT ![s] = lh[e]]
  /\ ackld' = ackld \cup {[from |-> s, epoch |-> e]}
  /\ ackH' = [ackH EXCEPT ![<<e, s>>] = lh[e]]
  /\ UNCHANGED <<aE, ackepoch, lst, lh, cm>>

Establish(e) ==
  /\ lst[e] = 1
  /\ \E Qs \in Quorum : \A s \in Qs : [from |-> s, epoch |-> e] \in ackld
  /\ lst' = [lst EXCEPT ![e] = 2]
  /\ cm' = [cm EXCEPT ![e] = lh[e]]
  /\ UNCHANGED <<aE, cE, h, ackepoch, ackld, ackH, lh>>

Propose(e) ==
  /\ lst[e] = 2 /\ Len(lh[e]) < MAXH
  /\ lh' = [lh EXCEPT ![e] = Append(lh[e], e)]
  /\ UNCHANGED <<aE, cE, h, ackepoch, ackld, ackH, lst, cm>>

Ack(s, e) ==
  /\ lst[e] = 2 /\ aE[s] = e /\ cE[s] = e /\ h[s] /= lh[e]
  /\ h' = [h EXCEPT ![s] = lh[e]]
  /\ ackH' = [ackH EXCEPT ![<<e, s>>] = lh[e]]
  /\ UNCHANGED <<aE, cE, ackepoch, ackld, lst, lh, cm>>

Commit(e, p) ==
  /\ lst[e] = 2 /\ p /= <<>> /\ p /= cm[e]
  /\ Prefix(cm[e], p) /\ Prefix(p, lh[e])
  /\ \E Qs \in Quorum : \A s \in Qs : Prefix(p, ackH[<<e, s>>])
  /\ cm' = [cm EXCEPT ![e] = p]
  /\ UNCHANGED <<aE, cE, h, ackepoch, ackld, ackH, lst, lh>>

Next ==
  \/ \E s \in Server, e \in Epoch : AckEpoch(s, e)
  \/ \E e \in Epoch : NewLeader(e)
  \/ \E e \in Epoch : (USE_BUG /\ NewLeaderBug(e))
  \/ \E s \in Server, e \in Epoch : AckLD(s, e)
  \/ \E e \in Epoch : Establish(e)
  \/ \E e \in Epoch : Propose(e)
  \/ \E s \in Server, e \in Epoch : Ack(s, e)
  \/ \E e \in Epoch, p \in Hist : Commit(e, p)

Spec == Init /\ [][Next]_vars

CommittedPrefix == \A x, y \in Epoch : Prefix(cm[x], cm[y]) \/ Prefix(cm[y], cm[x])
InitialHistory == \A x, y \in Epoch : (x < y /\ lst[y] >= 1) => Prefix(cm[x], lh[y])
=======================================================================
