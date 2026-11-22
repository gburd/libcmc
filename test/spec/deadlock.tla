---------------------------- MODULE deadlock ----------------------------
\* Hand-written TLA+ twin of test/spec/deadlock.cml and the files that
\* extend it, for the calibration against single-worker TLC: the .cfg
\* picks the variant (deadlock_off.cfg turns the check off,
\* deadlock_c.cfg adds the state constraint, deadlock_ac.cfg the action
\* constraint).
EXTENDS Naturals
CONSTANTS N
VARIABLES x, y
vars == <<x, y>>
Init == x = 0 /\ y = 0
Inc == x < N /\ x' = x + 1 /\ UNCHANGED y
Flip == x < N /\ y = 0 /\ y' = 1 /\ UNCHANGED x
Idle == x = N /\ y = 1 /\ y' = y /\ UNCHANGED x
Next == Inc \/ Flip \/ Idle
Spec == Init /\ [][Next]_vars
InRange == x <= N
Below == x < N
NoTop == x' < N
=======================================================================
