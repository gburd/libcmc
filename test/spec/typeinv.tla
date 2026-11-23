---- MODULE typeinv ----
\* Several invariants of the x \in T form on one variable.  cmc's reader
\* takes the variables' types from one (TypeOK, else the first listed)
\* and checks every other as an ordinary invariant.
EXTENDS Integers
VARIABLES x, y
Init == x = 0 /\ y = 0
Next == x < 4 /\ x' = x + 1 /\ y' = y
TypeOK == x \in 0..5 /\ y \in 0..1
Loose == x \in 0..5
Tight == x \in 0..2
====
