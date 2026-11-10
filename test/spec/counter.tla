---------------------------- MODULE counter ----------------------------
\* Hand-written TLA+ twin of test/spec/counter.cml for Milestone B
\* calibration against single-worker TLC.
EXTENDS Naturals
CONSTANT N
ASSUME N \in Nat
VARIABLE x
Init == x = 0
Inc   == x < N /\ x' = x + 1
Reset == x = N /\ x' = 0
Next == Inc \/ Reset
Spec == Init /\ [][Next]_x
InRange == x >= 0 /\ x <= N
=======================================================================
