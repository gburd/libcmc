---------------------------- MODULE paxos ----------------------------
\* Hand-written TLA+ twin of examples/spec/paxos.cml + consensus.cml, for
\* the calibration against single-worker TLC (test/spec_calibrate.sh).
\* It is written the way Lamport's Paxos.tla is (tlaplus/Examples,
\* specifications/Paxos): a set msgs of the messages sent, ballots from
\* 0 with -1 for none, and Phase2a quantified over quorums, not counted.
\* Differences from Paxos.tla, all shared with the .cml: no 1a messages
\* (Phase1b takes any higher ballot), a bounded ballot range, and a
\* Learn action that records a chosen value.  The .cml encodes the same
\* states differently, so equal distinct-state counts check its
\* encoding.  USE_BUG turns on the 2a that ignores the promises.
EXTENDS Integers, FiniteSets
CONSTANTS V, A, B, Q, USE_BUG

Value == 1 .. V
Acceptor == 0 .. A-1
Ballot == 0 .. B-1
Quorum == {S \in SUBSET Acceptor : Cardinality(S) >= Q}
None == 0

VARIABLES chosen, maxBal, maxVBal, maxVal, msgs
vars == <<chosen, maxBal, maxVBal, maxVal, msgs>>

Init == /\ chosen = [v \in Value |-> 0]
        /\ maxBal = [a \in Acceptor |-> -1]
        /\ maxVBal = [a \in Acceptor |-> -1]
        /\ maxVal = [a \in Acceptor |-> None]
        /\ msgs = {}

Send(m) == msgs' = msgs \cup {m}

Phase1b(a, b) ==
  /\ b > maxBal[a]
  /\ maxBal' = [maxBal EXCEPT ![a] = b]
  /\ Send([type |-> "1b", acc |-> a, bal |-> b, mbal |-> maxVBal[a], mval |-> maxVal[a]])
  /\ UNCHANGED <<chosen, maxVBal, maxVal>>

Phase2a(b, v) ==
  /\ ~ \E m \in msgs : m.type = "2a" /\ m.bal = b
  /\ \E S \in Quorum :
        LET Q1b == {m \in msgs : m.type = "1b" /\ m.acc \in S /\ m.bal = b}
            Q1bv == {m \in Q1b : m.mbal >= 0}
        IN  /\ \A a \in S : \E m \in Q1b : m.acc = a
            /\ \/ Q1bv = {}
               \/ \E m \in Q1bv : m.mval = v /\ \A mm \in Q1bv : m.mbal >= mm.mbal
  /\ Send([type |-> "2a", bal |-> b, val |-> v])
  /\ UNCHANGED <<chosen, maxBal, maxVBal, maxVal>>

Phase2aBug(b, v) ==
  /\ ~ \E m \in msgs : m.type = "2a" /\ m.bal = b
  /\ \E S \in Quorum : \A a \in S : \E m \in msgs : m.type = "1b" /\ m.acc = a /\ m.bal = b
  /\ Send([type |-> "2a", bal |-> b, val |-> v])
  /\ UNCHANGED <<chosen, maxBal, maxVBal, maxVal>>

Phase2b(a) ==
  \E m \in msgs :
    /\ m.type = "2a"
    /\ m.bal >= maxBal[a]
    /\ maxBal' = [maxBal EXCEPT ![a] = m.bal]
    /\ maxVBal' = [maxVBal EXCEPT ![a] = m.bal]
    /\ maxVal' = [maxVal EXCEPT ![a] = m.val]
    /\ Send([type |-> "2b", acc |-> a, bal |-> m.bal, val |-> m.val])
    /\ UNCHANGED chosen

Learn(v, b) ==
  /\ \E S \in Quorum : \A a \in S : [type |-> "2b", acc |-> a, bal |-> b, val |-> v] \in msgs
  /\ chosen' = [chosen EXCEPT ![v] = 1]
  /\ UNCHANGED <<maxBal, maxVBal, maxVal, msgs>>

Next ==
  \/ \E a \in Acceptor, b \in Ballot : Phase1b(a, b)
  \/ \E b \in Ballot, v \in Value : Phase2a(b, v)
  \/ \E b \in Ballot, v \in Value : (USE_BUG /\ Phase2aBug(b, v))
  \/ \E a \in Acceptor : Phase2b(a)
  \/ \E v \in Value, b \in Ballot : Learn(v, b)

Spec == Init /\ [][Next]_vars

Agreement == Cardinality({v \in Value : chosen[v] = 1}) <= 1
=======================================================================
