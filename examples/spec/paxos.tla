---------------------------- MODULE paxos ----------------------------
\* Hand-written TLA+ twin of examples/spec/paxos.cml + consensus.cml, for
\* the Milestone B calibration against single-worker TLC.  Same small
\* instance: 3 acceptors, 2 ballots, 2 values; votes encoded value*10 +
\* ballot.  USE_BUG turns on the unsafe double-vote action.
EXTENDS Naturals, FiniteSets
CONSTANTS V, A, B, Q, USE_BUG
ASSUME V \in Nat /\ A \in Nat /\ B \in Nat /\ Q \in Nat /\ USE_BUG \in BOOLEAN

VARIABLES chosen, voted
vars == <<chosen, voted>>

Values == 1 .. V
Accs == 0 .. A-1
Ballots == 1 .. B

Init == chosen = [v \in Values |-> 0] /\ voted = [a \in Accs |-> 0]

ValOf(code) == (code - (code % 10)) \div 10
BalOf(code) == code % 10

Vote(a, v, b) ==
  /\ voted[a] = 0
  /\ Cardinality({c \in Accs : voted[c] # 0 /\ BalOf(voted[c]) >= b /\ ValOf(voted[c]) # v}) = 0
  /\ voted' = [voted EXCEPT ![a] = v*10 + b]
  /\ UNCHANGED chosen

VoteBug(a, v, b) ==
  /\ BalOf(voted[a]) # b
  /\ voted' = [voted EXCEPT ![a] = v*10 + b]
  /\ UNCHANGED chosen

Choose(v, b) ==
  /\ Cardinality({a \in Accs : voted[a] = v*10 + b}) >= Q
  /\ chosen' = [chosen EXCEPT ![v] = 1]
  /\ UNCHANGED voted

Next ==
  \/ \E a \in Accs, v \in Values, b \in Ballots : Vote(a, v, b)
  \/ \E a \in Accs, v \in Values, b \in Ballots : (USE_BUG /\ VoteBug(a, v, b))
  \/ \E v \in Values, b \in Ballots : Choose(v, b)

Spec == Init /\ [][Next]_vars

Agreement == Cardinality({v \in Values : chosen[v] = 1}) <= 1
=======================================================================
