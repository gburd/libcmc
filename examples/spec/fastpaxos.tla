---------------------------- MODULE fastpaxos ----------------------------
\* Hand-written TLA+ twin of examples/spec/fastpaxos.cml + consensus.cml,
\* for the calibration against single-worker TLC (test/spec_calibrate.sh).
\*
\* Reference: Leslie Lamport, "Fast Paxos", Distributed Computing 19(2),
\*   2006 (MSR-TR-2005-112): the coordinator's rule in Figure 2 and the
\*   Quorum Requirement of section 3.1.  Round 1 is fast (acceptors vote
\*   for values clients send them), round 2 is classic, and its
\*   coordinator picks a value from the promises of a classic quorum Q:
\*   with k the highest round a member voted in and VS the values voted
\*   in round k, any value if k = 0; the one value in VS if there is one;
\*   else a w in VS for which O4(w) holds (some fast quorum R has every
\*   member of R \cap Q voting w in round k); else any value.
\*
\* It is written the way paxos.tla is: a set msgs of the messages sent
\* (round 2 promises "1b" with their sender and last vote, round 2's "2a"
\* and the "2b" votes of both rounds) and quorums as sets of acceptors.
\* The .cml keeps the round 2 promises only as counts per reported value
\* (cnt1b), since the coordinator's rule needs no more and no acceptor
\* promises twice.  This module keeps the sender, so it has more distinct
\* states; the configuration fastpaxos_view.cfg sets VIEW to Abstract,
\* which forgets the sender, and with it TLC's distinct-state count is
\* the .cml's.  USE_BUG turns on ShrinkFastQuorum, which runs the whole
\* protocol with fast quorums of QFBUG acceptors (Qc + 2*Qf = 2*N).
EXTENDS Integers, FiniteSets
CONSTANTS V, N, QC, QF, QFBUG, USE_BUG

Value == 1 .. V
Acceptor == 0 .. N-1
QuorumOf(n) == {S \in SUBSET Acceptor : Cardinality(S) >= n}

VARIABLES chosen, qf, rnd, vrnd, vval, msgs
vars == <<chosen, qf, rnd, vrnd, vval, msgs>>

Init == /\ chosen = [v \in Value |-> 0]
        /\ qf = QF
        /\ rnd = [a \in Acceptor |-> 0]
        /\ vrnd = [a \in Acceptor |-> 0]
        /\ vval = [a \in Acceptor |-> 0]
        /\ msgs = {}

Send(m) == msgs' = msgs \cup {m}
Voted(r, v) == {m.acc : m \in {mm \in msgs : mm.type = "2b" /\ mm.rnd = r /\ mm.val = v}}
Proposal == {m \in msgs : m.type = "2a"}

FastVote(a, v) ==
  /\ rnd[a] = 0
  /\ rnd' = [rnd EXCEPT ![a] = 1]
  /\ vrnd' = [vrnd EXCEPT ![a] = 1]
  /\ vval' = [vval EXCEPT ![a] = v]
  /\ Send([type |-> "2b", acc |-> a, rnd |-> 1, val |-> v])
  /\ UNCHANGED <<chosen, qf>>

Phase1b(a) ==
  /\ rnd[a] < 2
  /\ rnd' = [rnd EXCEPT ![a] = 2]
  /\ Send([type |-> "1b", acc |-> a, vrnd |-> vrnd[a], vval |-> vval[a]])
  /\ UNCHANGED <<chosen, qf, vrnd, vval>>

\* Figure 2: the value the coordinator may pick from Q's promises P
Pickable(P, Q, v) ==
  LET k == CHOOSE r \in {m.vrnd : m \in P} : \A m \in P : m.vrnd <= r
      VS == {m.vval : m \in {mm \in P : mm.vrnd = k}}
      O4(w) == \E R \in QuorumOf(qf) :
                 \A a \in R \cap Q : \E m \in P : m.acc = a /\ m.vrnd = k /\ m.vval = w
  IN  \/ k = 0
      \/ VS = {v}
      \/ Cardinality(VS) > 1 /\ v \in VS /\ O4(v)
      \/ Cardinality(VS) > 1 /\ \A w \in VS : ~ O4(w)

Phase2a(v) ==
  /\ Proposal = {}
  /\ \E Q \in QuorumOf(QC) :
        LET P == {m \in msgs : m.type = "1b" /\ m.acc \in Q}
        IN  /\ \A a \in Q : \E m \in P : m.acc = a
            /\ Pickable(P, Q, v)
  /\ Send([type |-> "2a", val |-> v])
  /\ UNCHANGED <<chosen, qf, rnd, vrnd, vval>>

Phase2b(a) ==
  \E m \in Proposal :
    /\ vrnd[a] < 2
    /\ rnd' = [rnd EXCEPT ![a] = 2]
    /\ vrnd' = [vrnd EXCEPT ![a] = 2]
    /\ vval' = [vval EXCEPT ![a] = m.val]
    /\ Send([type |-> "2b", acc |-> a, rnd |-> 2, val |-> m.val])
    /\ UNCHANGED <<chosen, qf>>

Learn(v) ==
  /\ \/ Cardinality(Voted(1, v)) >= qf
     \/ Cardinality(Voted(2, v)) >= QC
  /\ chosen' = [chosen EXCEPT ![v] = 1]
  /\ UNCHANGED <<qf, rnd, vrnd, vval, msgs>>

ShrinkFastQuorum ==
  /\ qf = QF /\ \A a \in Acceptor : rnd[a] = 0
  /\ qf' = QFBUG
  /\ UNCHANGED <<chosen, rnd, vrnd, vval, msgs>>

Next ==
  \/ \E a \in Acceptor, v \in Value : FastVote(a, v)
  \/ \E a \in Acceptor : Phase1b(a)
  \/ \E v \in Value : Phase2a(v)
  \/ \E a \in Acceptor : Phase2b(a)
  \/ \E v \in Value : Learn(v)
  \/ USE_BUG /\ ShrinkFastQuorum

Spec == Init /\ [][Next]_vars

Agreement == Cardinality({v \in Value : chosen[v] = 1}) <= 1

\* the .cml's state: round 2 promises counted by reported value, not sender
Abstract ==
  <<chosen, qf, rnd, vrnd, vval, {m \in msgs : m.type /= "1b"},
    [w \in 0 .. V |-> Cardinality({m \in msgs : m.type = "1b" /\ m.vval = w})]>>
=======================================================================
