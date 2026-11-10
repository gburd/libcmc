---------------------------- MODULE mutex ----------------------------
\* Hand-written TLA+ twin of test/spec/mutex.cml, for the Milestone B
\* calibration: TLC must give the same verdict and shortest-counterexample
\* depth as `cmc --spec`.  The BUG action is included; set USE_BUG in the
\* cfg to turn it on.
EXTENDS Naturals, FiniteSets
CONSTANTS N, USE_BUG
ASSUME N \in Nat /\ USE_BUG \in BOOLEAN

VARIABLES pc, held
vars == <<pc, held>>

Procs == 0 .. N-1

Init == pc = [i \in Procs |-> 0] /\ held = 0

Try(i)   == pc[i] = 0 /\ pc' = [pc EXCEPT ![i] = 1] /\ UNCHANGED held
Enter(i) == pc[i] = 1 /\ held = 0 /\ pc' = [pc EXCEPT ![i] = 2] /\ held' = 1
EnterBug(i) == pc[i] = 1 /\ pc' = [pc EXCEPT ![i] = 2] /\ held' = 1
Leave(i) == pc[i] = 2 /\ pc' = [pc EXCEPT ![i] = 0] /\ held' = 0

Next ==
  \E i \in Procs :
    \/ Try(i)
    \/ Enter(i)
    \/ (USE_BUG /\ EnterBug(i))
    \/ Leave(i)

Spec == Init /\ [][Next]_vars

MutualExclusion == Cardinality({i \in Procs : pc[i] = 2}) <= 1
=======================================================================
