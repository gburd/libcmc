---------------------------- MODULE caspaxos ----------------------------
\* Hand-written TLA+ twin of examples/spec/caspaxos.cml, for the
\* calibration against single-worker TLC (test/spec_calibrate.sh).
\*
\* Reference: Denis Rystsov, "CASPaxos: Replicated State Machines without
\*   logs", 2018, arXiv:1802.07000, section 2.2 and the proof in its
\*   appendix.  Each round with ballot b reads the register's state from
\*   a quorum of promises (the highest-ballot accepted state, or the
\*   empty state), applies the change function, and writes the new state
\*   to a quorum of acceptors.  The change function here is increment.
\*
\* It is written the way paxos.tla is: a set msgs of the messages sent
\* (prepare replies "1b", the round's accept request "2a" and the
\* acceptors' "2b" replies) and the read quantified over quorums, not
\* counted.  Ballots are 1 .. B and 0 is none.  The 2a message carries
\* the ballot whose state the round read (parent, a ghost field), which
\* the descends invariant reads.  The simplifications are the .cml's:
\* no 1a messages, a bounded ballot range, no conflict replies.  The
\* .cml encodes the same states as function variables, so equal
\* distinct-state counts check its encoding.  USE_BUG turns on AcceptBug,
\* an acceptor accepting a ballot below its promise.
EXTENDS Integers, FiniteSets
CONSTANTS A, B, Q, USE_BUG

Acceptor == 0 .. A-1
Ballot == 1 .. B
Quorum == {S \in SUBSET Acceptor : Cardinality(S) >= Q}

VARIABLES promised, accBal, accVal, msgs, confirmed
vars == <<promised, accBal, accVal, msgs, confirmed>>

Init == /\ promised = [a \in Acceptor |-> 0]
        /\ accBal = [a \in Acceptor |-> 0]
        /\ accVal = [a \in Acceptor |-> 0]
        /\ msgs = {}
        /\ confirmed = [b \in Ballot |-> 0]

Send(m) == msgs' = msgs \cup {m}
Proposal(b) == {m \in msgs : m.type = "2a" /\ m.bal = b}

Prepare(a, b) ==
  /\ b > promised[a]
  /\ promised' = [promised EXCEPT ![a] = b]
  /\ Send([type |-> "1b", acc |-> a, bal |-> b, mbal |-> accBal[a], mval |-> accVal[a]])
  /\ UNCHANGED <<accBal, accVal, confirmed>>

\* the round reads the state of the highest-ballot reply in a quorum of
\* promises, or the empty state 0 with parent 0 when none reports one
Propose(b) ==
  /\ Proposal(b) = {}
  /\ \E S \in Quorum :
        LET P == {m \in msgs : m.type = "1b" /\ m.acc \in S /\ m.bal = b}
            Pv == {m \in P : m.mbal > 0}
        IN  /\ \A a \in S : \E m \in P : m.acc = a
            /\ \/ /\ Pv = {}
                  /\ Send([type |-> "2a", bal |-> b, val |-> 1, parent |-> 0])
               \/ \E m \in Pv :
                     /\ \A mm \in Pv : m.mbal >= mm.mbal
                     /\ Send([type |-> "2a", bal |-> b, val |-> m.mval + 1, parent |-> m.mbal])
  /\ UNCHANGED <<promised, accBal, accVal, confirmed>>

Accept(a, b) ==
  \E m \in Proposal(b) :
    /\ b >= promised[a]
    /\ promised' = [promised EXCEPT ![a] = b]
    /\ accBal' = [accBal EXCEPT ![a] = b]
    /\ accVal' = [accVal EXCEPT ![a] = m.val]
    /\ Send([type |-> "2b", acc |-> a, bal |-> b])
    /\ UNCHANGED confirmed

AcceptBug(a, b) ==
  \E m \in Proposal(b) :
    /\ b < promised[a]
    /\ accBal' = [accBal EXCEPT ![a] = b]
    /\ accVal' = [accVal EXCEPT ![a] = m.val]
    /\ Send([type |-> "2b", acc |-> a, bal |-> b])
    /\ UNCHANGED <<promised, confirmed>>

Confirm(b) ==
  /\ Proposal(b) /= {}
  /\ confirmed[b] = 0
  /\ \E S \in Quorum : \A a \in S : [type |-> "2b", acc |-> a, bal |-> b] \in msgs
  /\ confirmed' = [confirmed EXCEPT ![b] = 1]
  /\ UNCHANGED <<promised, accBal, accVal, msgs>>

Next ==
  \/ \E a \in Acceptor, b \in Ballot : Prepare(a, b)
  \/ \E b \in Ballot : Propose(b)
  \/ \E a \in Acceptor, b \in Ballot : Accept(a, b)
  \/ \E a \in Acceptor, b \in Ballot : (USE_BUG /\ AcceptBug(a, b))
  \/ \E b \in Ballot : Confirm(b)

Spec == Init /\ [][Next]_vars

\* of two confirmed rounds x < y, round y read the state of round x
Descends ==
  \A x, y \in Ballot :
    (x < y /\ confirmed[x] = 1 /\ confirmed[y] = 1) =>
      \A m \in Proposal(y) : m.parent = x
=======================================================================
