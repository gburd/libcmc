---- MODULE vardom ----
\* An x \in T conjunct whose T reads a variable is no type: cmc's reader
\* checks TypeOK as an ordinary invariant, as TLC does.
EXTENDS Integers
VARIABLES x, y
Init == x = 0 /\ y = 0
Next == y < 2 /\ y' = y + 1 /\ UNCHANGED x
TypeOK == x \in y..5 /\ y \in 0..5
====
