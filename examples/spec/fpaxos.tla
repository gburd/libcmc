---------------------------- MODULE fpaxos ----------------------------
\* Hand-written TLA+ twin of examples/spec/fpaxos.cml + consensus.cml,
\* for the calibration against single-worker TLC (test/spec_calibrate.sh).
\*
\* Reference: Heidi Howard, Dahlia Malkhi, Alexander Spiegelman,
\*   "Flexible Paxos: Quorum Intersection Revisited", OPODIS 2016,
\*   arXiv:1608.06696.  Flexible Paxos is single-decree Paxos with the
\*   majority quorum split in two: phase 1 needs Q1 acceptors, phase 2
\*   needs Q2, and safety needs only Q1 + Q2 > A.
\*
\* It is paxos.tla (Lamport's Paxos.tla style: a set msgs of the messages
\* sent, ballots from 0 with -1 for none, Phase2a quantified over
\* quorums) with Quorum1 in Phase2a and Quorum2 in Learn.  The
\* simplifications are those of paxos.tla and the .cml: no 1a messages,
\* a bounded ballot range, and a Learn action that records a chosen
\* value.  USE_BUG turns on Phase2aBug, the Phase2a rule run on a phase 1
\* quorum of Q1BUG acceptors, with Q1BUG + Q2 = A.
EXTENDS Integers, FiniteSets
CONSTANTS V, A, B, Q1, Q2, Q1BUG, USE_BUG

Value == 1 .. V
Acceptor == 0 .. A-1
Ballot == 0 .. B-1
QuorumOf(n) == {S \in SUBSET Acceptor : Cardinality(S) >= n}
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

\* the Phase2a rule of Paxos.tla on a phase 1 quorum of n acceptors
Phase2aOn(n, b, v) ==
  /\ ~ \E m \in msgs : m.type = "2a" /\ m.bal = b
  /\ \E S \in QuorumOf(n) :
        LET Q1b == {m \in msgs : m.type = "1b" /\ m.acc \in S /\ m.bal = b}
            Q1bv == {m \in Q1b : m.mbal >= 0}
        IN  /\ \A a \in S : \E m \in Q1b : m.acc = a
            /\ \/ Q1bv = {}
               \/ \E m \in Q1bv : m.mval = v /\ \A mm \in Q1bv : m.mbal >= mm.mbal
  /\ Send([type |-> "2a", bal |-> b, val |-> v])
  /\ UNCHANGED <<chosen, maxBal, maxVBal, maxVal>>

Phase2a(b, v) == Phase2aOn(Q1, b, v)
Phase2aBug(b, v) == Phase2aOn(Q1BUG, b, v)

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
  /\ \E S \in QuorumOf(Q2) : \A a \in S : [type |-> "2b", acc |-> a, bal |-> b, val |-> v] \in msgs
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
