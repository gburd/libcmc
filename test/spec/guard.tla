---------------------------- MODULE guard ----------------------------
\* Hand-written TLA+ twin of test/spec/guard.cml for the calibration
\* against single-worker TLC.  TLC evaluates /\ and \/ left to right and
\* stops once the left operand decides, so f[x] at x = N and N \div x at
\* x = 0 are never evaluated.  USE_BUG turns on Skip.
EXTENDS Naturals
CONSTANTS N, USE_BUG
VARIABLES x, f
vars == <<x, f>>
Init == x = 0 /\ f = [i \in 0 .. N-1 |-> 0]
Mark == x < N /\ f[x] = 0 /\ f' = [f EXCEPT ![x] = 1] /\ UNCHANGED x
Next1 == x < N /\ f[x] = 1 /\ x' = x + 1 /\ UNCHANGED f
Skip == USE_BUG /\ x < N /\ f[x] = 0 /\ x' = x + 1 /\ UNCHANGED f
Next == Mark \/ Next1 \/ Skip
Spec == Init /\ [][Next]_vars
Ratio == x = 0 \/ N \div x >= 1
Marked == x = 0 \/ f[x-1] = 1
=======================================================================
