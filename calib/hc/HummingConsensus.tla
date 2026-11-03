------------------------------ MODULE HummingConsensus ------------------------------
(***************************************************************************)
(* Machi "humming consensus" (HC) as described in                          *)
(* machi/doc/src.high-level/high-level-chain-mgr.tex, plus the parts of    *)
(* machi_chain_manager1.erl that the paper leaves vague (calc_projection2, *)
(* projection_transition_is_sane, B10 relevance, A21 unanimity).           *)
(*                                                                         *)
(* Each node n has                                                         *)
(*   pub[n][e]  public projection store, write-once register per epoch     *)
(*   priv[n][e] private store (only n writes); cur[n] = latest adopted     *)
(* A manager (Propose) reads the public stores it can reach, computes a    *)
(* projection for epoch max(seen)+1 from its own cur and its reachability  *)
(* view and writes it to its own store; Deliver/ReadRepair copy            *)
(* projections into other reachable stores (write-once).  Adopt reads all  *)
(* reachable stores and adopts the latest epoch's projection iff it is     *)
(* unanimous and the transition cur -> new is "sane".                      *)
(*                                                                         *)
(* Chain data is abstract: hist[n] is the set of writes n stores;          *)
(* stamp[n] records <<w, P>> when n stored w as a chain member of P.  A    *)
(* write is acked once every member of P (UPI ++ repairing) stored it      *)
(* under P.  Repair copies the UPI tail's history to repairing members.    *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    N,              \* number of nodes; nodes are 1..N (ordered: rank, sort)
    MaxEpoch,       \* epochs 1..MaxEpoch; epoch 1 is the initial projection
    Writes,         \* set of client write ids
    CPMode,         \* TRUE: CP mode (|UPI ++ repairing| >= majority), FALSE: AP mode
    UnanimityRule,  \* "paper": all reachable stores hold P at the latest epoch
                    \* "code" : A21 rule, stores at the latest epoch agree and
                    \*          every UPI/repairing member answered with P
    Confirm,        \* CP code only: stay wedged until every UPI member's private
                    \* store holds the same latest projection (annotation,
                    \* poll_private_proj_is_upi_unanimous, erl:2417-2538)
    Zerf            \* CP code only: make_zerf (erl:2871-2985): compute from the
                    \* newest confirmed projection read from a majority of Up

Nodes  == 1..N
Epochs == 1..MaxEpoch
Maj    == (N \div 2) + 1

\* Unwritten register value.  Real projections have e >= 1.
NoProj == [e |-> 0, a |-> 0, upi |-> <<>>, rep |-> <<>>]

Range(s)   == {s[i] : i \in DOMAIN s}
Max(S)     == CHOOSE x \in S : \A y \in S : x >= y
Min(S)     == CHOOSE x \in S : \A y \in S : x <= y
Last(s)    == s[Len(s)]
RECURSIVE SetToSeq(_)
SetToSeq(S) == IF S = {} THEN <<>> ELSE LET m == Min(S) IN <<m>> \o SetToSeq(S \ {m})
Members(P) == Range(P.upi) \cup Range(P.rep)
Chain(P)   == P.upi \o P.rep          \* machi mutation_flus(): UPI ++ Repairing

Pairs == {p \in SUBSET Nodes : Cardinality(p) = 2}

P1 == [e |-> 1, a |-> 1, upi |-> SetToSeq(Nodes), rep |-> <<>>]

VARIABLES
    net,    \* set of connected unordered pairs (non-transitive => "asymmetric" views)
    pub,    \* pub[n][e]
    priv,   \* priv[n][e]
    cur,    \* cur[n]: P_current, the latest projection in priv[n]
    props,  \* every projection ever suggested (messages may be delayed forever)
    hist,   \* hist[n] \subseteq Writes
    stamp,  \* stamp[n]: set of <<w, P>> stored by n as a member of P
    acked,  \* set of <<w, e>>: w acknowledged to the client in epoch e
    conf    \* conf[n]: epochs n has confirmed (annotated) in its private store

vars == <<net, pub, priv, cur, props, hist, stamp, acked, conf>>

Reach(a, b) == a = b \/ {a, b} \in net
Up(n)       == {m \in Nodes : Reach(n, m)}

WrittenAt(m) == {e \in Epochs : pub[m][e] /= NoProj}
LatestE(n)   == Max(UNION {WrittenAt(m) : m \in Up(n)})

\* Wedged: the node's public store holds an epoch newer than the one it uses
\* (machi_projection_store:do_proj_write4 wedges on such a public write).
Wedged(n) == \/ \E e \in Epochs : e > cur[n].e /\ pub[n][e] /= NoProj
             \/ (Confirm /\ cur[n].e \notin conf[n])

Unanimous(n, P) ==
    LET E == LatestE(n) IN
    /\ P.e = E
    /\ IF UnanimityRule = "paper"
         THEN \A m \in Up(n) : pub[m][E] = P
         ELSE /\ \A m \in Up(n) : pub[m][E] \in {NoProj, P}
              /\ Members(P) \subseteq {m \in Up(n) : pub[m][E] = P}

Rank(P) == IF P.upi = <<>> THEN 0 ELSE 1 + N*N*N*Len(P.upi) + N*Len(P.rep)

(***************************************************************************)
(* calc_projection2 (machi_chain_manager1.erl:772-942), witnesses omitted. *)
(***************************************************************************)
NoneP(n) == [e |-> cur[n].e, a |-> n, upi |-> <<>>, rep |-> <<>>]
ZerfCands(n) == UNION {{priv[m][e] : e \in conf[m]} : m \in Up(n)}
ZerfOK(P) == Cardinality(Members(P)) >= Maj
\* P_current_calc: the base projection for calc_projection (A29).
Base(n) ==
    IF ~(CPMode /\ Zerf) THEN cur[n]
    ELSE IF Cardinality(Up(n)) < Maj THEN NoneP(n)
    ELSE LET C == {P \in ZerfCands(n) : ZerfOK(P)}
         IN IF C = {} THEN NoneP(n)
            ELSE CHOOSE P \in C : \A Q \in C : Q.e <= P.e

Calc(n, E) ==
    LET old    == Base(n)
        U      == Up(n)
        upi1   == SelectSeq(old.upi, LAMBDA x : x \in U)
        rep1   == SelectSeq(old.rep, LAMBDA x : x \in U)
        newUp  == U \ Members(old)
        \* the UPI tail promotes repairing members once repair is done
        promote == /\ newUp = {}
                   /\ rep1 /= <<>>
                   /\ old.upi /= <<>>
                   /\ n = Last(old.upi)
                   /\ \A r \in Range(rep1) : hist[n] \subseteq hist[r]
        upi2   == IF promote THEN upi1 \o rep1 ELSE upi1
        rep2   == (IF promote THEN <<>> ELSE rep1) \o SetToSeq(newUp)
        \* AP mode: empty UPI => first repairing member becomes the UPI
        apFix  == ~CPMode /\ upi2 = <<>> /\ rep2 /= <<>>
        upi3   == IF apFix THEN <<Head(rep2)>> ELSE upi2
        rep3   == IF apFix THEN Tail(rep2) ELSE rep2
        ok     == ~CPMode \/ Len(upi3) + Len(rep3) >= Maj
    IN IF ok THEN [e |-> E, a |-> n, upi |-> upi3, rep |-> rep3]
             ELSE [e |-> E, a |-> n, upi |-> <<>>, rep |-> <<>>]   \* none projection

(***************************************************************************)
(* projection_transition_is_sane (erl:2161-2415, 2743-2820), simplified.   *)
(***************************************************************************)
IsSubseqOf(s, t) == s = SelectSeq(t, LAMBDA x : x \in Range(s))

SimpleSane(old, new) ==
    LET kept == SelectSeq(old.upi, LAMBDA x : x \in Range(new.upi))
        k    == Len(kept)
    IN /\ SubSeq(new.upi, 1, k) = kept
       /\ LET added == SubSeq(new.upi, k + 1, Len(new.upi))
          IN /\ IsSubseqOf(added, old.rep)
             /\ added /= <<>> => (old.upi /= <<>> /\ new.a = Last(old.upi))

ChainSane(old, new, n) ==
    IF new.upi = <<new.a>> THEN TRUE
    ELSE IF Range(old.upi) \cap Range(new.upi) = {} THEN n \notin Range(new.upi)
    ELSE SimpleSane(old, new)

\* CP final review: old and new UPI must share a member.  old.upi = <<>> is
\* let through, standing in for the code's zerf_backstop escape hatch.
FinalReview(old, new) ==
    \/ ~CPMode \/ new.upi = <<>> \/ (Zerf /\ old.upi = <<>>)
    \/ Range(old.upi) \cap Range(new.upi) /= {}

MajOK(P) == ~CPMode \/ P.upi = <<>> \/ Len(P.upi) + Len(P.rep) >= Maj

Sane(old, new, n) ==
    /\ new.e > old.e
    /\ MajOK(new)
    /\ ChainSane(old, new, n)
    /\ FinalReview(old, new)

\* react_to_env_B10 IsRelevantToMe_p
Relevant(n, P) ==
    IF P.a = n THEN TRUE
    ELSE IF P.upi = <<>> THEN FALSE
    ELSE IF n \notin Members(P) THEN FALSE
    ELSE IF n \in Range(P.rep)
         THEN ~(n \in Range(cur[n].upi) /\ Len(cur[n].upi) > Len(P.upi))
    ELSE TRUE

-----------------------------------------------------------------------------
Init ==
    /\ net   = Pairs
    /\ pub   = [n \in Nodes |-> [e \in Epochs |-> IF e = 1 THEN P1 ELSE NoProj]]
    /\ priv  = [n \in Nodes |-> [e \in Epochs |-> IF e = 1 THEN P1 ELSE NoProj]]
    /\ cur   = [n \in Nodes |-> P1]
    /\ props = {}
    /\ hist  = [n \in Nodes |-> {}]
    /\ stamp = [n \in Nodes |-> {}]
    /\ acked = {}
    /\ conf  = [n \in Nodes |-> {1}]

\* Network changes arbitrarily: partitions start, heal, and are non-transitive.
NetChange ==
    /\ \E s \in SUBSET Pairs : s /= net /\ net' = s
    /\ UNCHANGED <<pub, priv, cur, props, hist, stamp, acked, conf>>

\* Column A/C300: compute P_newprop at max(seen)+1, write own public store.
Propose(n) ==
    LET E == Max({cur[n].e, LatestE(n)}) + 1 IN
    /\ E <= MaxEpoch
    /\ LET P == Calc(n, E) IN
       \* no reason to act: same chain as P_current and P_current is latest+unanimous
       /\ ~(P.upi = cur[n].upi /\ P.rep = cur[n].rep /\ Unanimous(n, cur[n]))
       /\ props' = props \cup {P}
       /\ pub'   = [pub EXCEPT ![n][E] = P]
    /\ UNCHANGED <<net, priv, cur, hist, stamp, acked, conf>>

\* A (possibly delayed) public write from the author reaches store m.
Deliver(P, m) ==
    /\ Reach(P.a, m)
    /\ pub[m][P.e] = NoProj
    /\ pub' = [pub EXCEPT ![m][P.e] = P]
    /\ UNCHANGED <<net, priv, cur, props, hist, stamp, acked, conf>>

\* Read repair: fill an unwritten reachable store at the latest epoch with
\* the highest-ranked value seen there.
ReadRepair(n, m) ==
    LET E    == LatestE(n)
        Vals == {pub[k][E] : k \in Up(n)} \ {NoProj}
    IN /\ m \in Up(n)
       /\ pub[m][E] = NoProj
       /\ \E V \in Vals :
            /\ \A V2 \in Vals : Rank(V2) <= Rank(V)
            /\ pub' = [pub EXCEPT ![m][E] = V]
       /\ UNCHANGED <<net, priv, cur, props, hist, stamp, acked, conf>>

\* C100/C110/C120: adopt P_latest if unanimous and the transition is sane.
Adopt(n) ==
    \E P \in {pub[m][LatestE(n)] : m \in Up(n)} :
       /\ Unanimous(n, P)
       /\ P.a \in Up(n)                       \* author not down
       /\ Relevant(n, P)
       \* C100: in CP mode the check is against P_current_calc with the
       \* epoch faked down to min(current, calc) (erl:1835-1858)
       /\ LET B == Base(n) IN
          Sane([B EXCEPT !.e = IF B.e < cur[n].e THEN B.e ELSE cur[n].e], P, n)
       /\ P.e > cur[n].e
       /\ priv[n][P.e] = NoProj
       /\ priv' = [priv EXCEPT ![n][P.e] = P]
       /\ cur'  = [cur EXCEPT ![n] = P]
       /\ UNCHANGED <<net, pub, props, hist, stamp, acked, conf>>

-----------------------------------------------------------------------------
(* Chain replication data path (client-driven, as machi_cr_client).        *)
IsHead(n)  == cur[n].upi /= <<>> /\ n = Head(cur[n].upi)
IsTail(n)  == cur[n].upi /= <<>> /\ n = Last(cur[n].upi)
InUPI(n)   == n \in Range(cur[n].upi)
Serving(n) == InUPI(n) /\ ~Wedged(n)

ClientWrite(h, w) ==
    /\ IsHead(h) /\ ~Wedged(h)
    /\ <<w, cur[h]>> \notin stamp[h]
    /\ hist'  = [hist EXCEPT ![h] = @ \cup {w}]
    /\ stamp' = [stamp EXCEPT ![h] = @ \cup {<<w, cur[h]>>}]
    /\ UNCHANGED <<net, pub, priv, cur, props, acked, conf>>

\* Client sends w (stored by n under P) to the next member of P's chain;
\* that member accepts only if it is in P and not wedged (epoch check).
Forward(n, m, w) ==
    \E P \in {x[2] : x \in {y \in stamp[n] : y[1] = w}} :
       LET c == Chain(P) IN
       /\ \E i \in 1..(Len(c) - 1) : c[i] = n /\ c[i + 1] = m
       /\ cur[m] = P /\ ~Wedged(m)
       /\ <<w, P>> \notin stamp[m]
       /\ hist'  = [hist EXCEPT ![m] = @ \cup {w}]
       /\ stamp' = [stamp EXCEPT ![m] = @ \cup {<<w, P>>}]
       /\ UNCHANGED <<net, pub, priv, cur, props, acked, conf>>

Ack(w, P) ==
    /\ P.upi /= <<>>
    /\ \A m \in Members(P) : <<w, P>> \in stamp[m]
    /\ <<w, P>> \notin acked
    /\ acked' = acked \cup {<<w, P>>}
    /\ UNCHANGED <<net, pub, priv, cur, props, hist, stamp, conf>>

\* The UPI tail copies its history to a repairing member in the same projection.
Repair(t, r) ==
    /\ IsTail(t)
    /\ r \in Range(cur[t].rep)
    /\ cur[r] = cur[t] /\ Reach(t, r)
    /\ ~(hist[t] \subseteq hist[r])
    /\ hist' = [hist EXCEPT ![r] = @ \cup hist[t]]
    /\ UNCHANGED <<net, pub, priv, cur, props, stamp, acked, conf>>

\* CP annotation: every UPI member is reachable and uses the same projection.
DoConfirm(n) ==
    /\ Confirm
    /\ cur[n].upi /= <<>>
    /\ cur[n].e \notin conf[n]
    /\ \A m \in Range(cur[n].upi) : Reach(n, m) /\ cur[m] = cur[n]
    /\ conf' = [conf EXCEPT ![n] = @ \cup {cur[n].e}]
    /\ UNCHANGED <<net, pub, priv, cur, props, hist, stamp, acked>>

AllStamped == UNION {stamp[n] : n \in Nodes}

Next ==
    \/ NetChange
    \/ \E n \in Nodes : Propose(n) \/ Adopt(n) \/ DoConfirm(n)
    \/ \E P \in props, m \in Nodes : Deliver(P, m)
    \/ \E n, m \in Nodes : ReadRepair(n, m) \/ Repair(n, m)
    \/ \E h \in Nodes, w \in Writes : ClientWrite(h, w)
    \/ \E n, m \in Nodes, w \in Writes : Forward(n, m, w)
    \/ \E x \in AllStamped : Ack(x[1], x[2])

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Safety properties                                                       *)

\* S1: no two private stores hold different projections for one epoch.
S1_Agreement ==
    \A a, b \in Nodes, e \in Epochs :
        (priv[a][e] /= NoProj /\ priv[b][e] /= NoProj) => priv[a][e] = priv[b][e]

\* S1 restricted to projections with a non-empty UPI (ignores "none" projections,
\* which differ only by author).
S1_AgreementNonNone ==
    \A a, b \in Nodes, e \in Epochs :
        (priv[a][e].upi /= <<>> /\ priv[b][e].upi /= <<>>) => priv[a][e] = priv[b][e]

\* S2 (as stated): two unwedged UPI members acting as head or tail are in
\* the same epoch.
ServesHT(n) == Serving(n) /\ (IsHead(n) \/ IsTail(n))
S2_NoSplitBrainServing ==
    \A a, b \in Nodes : (ServesHT(a) /\ ServesHT(b)) => cur[a].e = cur[b].e

\* S2w: no two different projections can both complete (ack) writes now.
AckCapable(n) == cur[n].upi /= <<>> /\ \A m \in Members(cur[n]) : cur[m] = cur[n] /\ ~Wedged(m)
S2w_NoConcurrentWriteChains ==
    \A a, b \in Nodes : (AckCapable(a) /\ AckCapable(b)) => cur[a] = cur[b]

\* S2r: an unwedged tail (read server) holds every acknowledged write.
S2r_NoStaleRead ==
    \A t \in Nodes : (IsTail(t) /\ ~Wedged(t)) => \A x \in acked : x[1] \in hist[t]

\* S3: an acked write is held by every UPI member of the projection it was
\* acked under and of every later adopted projection.  (hist only grows, so
\* checking cur in every state covers every adopted projection.)
S3_NoLostAck ==
    \A x \in acked, n \in Nodes :
        (InUPI(n) /\ (cur[n].e > x[2].e \/ cur[n] = x[2])) => x[1] \in hist[n]

\* S3a: the same, but only for projections that are active (every member uses
\* it), i.e. ones that can ack writes or have a fully-joined tail.
Active(P) == P.upi /= <<>> /\ \A m \in Members(P) : cur[m] = P
S3a_NoLostAckActive ==
    \A x \in acked, n \in Nodes :
        (InUPI(n) /\ Active(cur[n]) /\ cur[n].e > x[2].e) => x[1] \in hist[n]

\* S3 as first written: also counts a divergent projection at the ack's epoch.
S3_NoLostAckSameEpoch ==
    \A x \in acked, n \in Nodes : (InUPI(n) /\ cur[n].e >= x[2].e) => x[1] \in hist[n]

\* S4: adopted epochs are monotone; private registers are write-once.
S4_EpochMonotone == [][\A n \in Nodes : cur'[n].e >= cur[n].e]_vars
S4_PrivWriteOnce ==
    [][\A n \in Nodes, e \in Epochs : priv[n][e] /= NoProj => priv'[n][e] = priv[n][e]]_vars

\* Vacuity witnesses: each must be VIOLATED (proves the behaviour is reachable).
W_AckAfterReconfig == ~(\E x \in acked : x[2].e >= 2)
W_RepairJoinsUPI   == ~(\E n \in Nodes : cur[n].e >= 3 /\ Len(cur[n].upi) = N /\ acked /= {})

TypeOK ==
    /\ net \subseteq Pairs
    /\ \A n \in Nodes : cur[n] = priv[n][Max({e \in Epochs : priv[n][e] /= NoProj})]
    /\ \A x \in acked : x[1] \in Writes /\ x[2] \in props \cup {P1}
=============================================================================
