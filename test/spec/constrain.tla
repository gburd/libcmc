---------------------------- MODULE constrain ----------------------------
\* Hand-written TLA+ twin of test/spec/constrain.cml and the files that
\* extend it, for the calibration against single-worker TLC: the .cfg
\* picks the variant.
EXTENDS Naturals
CONSTANTS N
VARIABLES x, y
vars == <<x, y>>
Init == x = 0 /\ y = 0
IncX == x < N /\ x' = x + 1 /\ UNCHANGED y
IncY == y < N /\ y' = y + 1 /\ UNCHANGED x
Drop == x > 0 /\ y > 0 /\ x' = x - 1 /\ y' = y - 1
Next == IncX \/ IncY \/ Drop
Spec == Init /\ [][Next]_vars
InRange == x + y <= 2 * N
NotThree == x + y # 3
YFollows == y <= x
Low == x + y <= 2
Follow == y' <= x' \/ y' < y
=======================================================================
