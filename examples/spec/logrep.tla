------------------------------ MODULE logrep ------------------------------
(* Leader-based log replication: the hand-written TLA+ twin of
   examples/spec/logrep.cml, written as a TLA+ user would write it.
   Server 0 leads; followers 1..F receive entries [idx, val] on lossy
   FIFO channels and append one only when it is the next one their log
   needs; the leader commits an entry once a quorum holds it.  BUG drops
   the follower's index check.  test/spec_calibrate.sh runs TLC on it
   with logrep_safe.cfg and logrep_bug.cfg. *)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS F, L, C, Q, BUG

Servers == 0..F
Followers == 1..F
Values == {0, 1}
Entry == [idx : 1..L, val : Values]

VARIABLES log, chan, match, commit
vars == <<log, chan, match, commit>>

TypeOK ==
  /\ log \in [Servers -> UNION {[1..n -> Values] : n \in 0..L}]
  /\ chan \in [Followers -> UNION {[1..n -> Entry] : n \in 0..C}]
  /\ match \in [Followers -> 0..L]
  /\ commit \in 0..L

IsPrefix(p, q) == Len(p) <= Len(q) /\ SubSeq(q, 1, Len(p)) = p
Holds(i) == Cardinality({s \in Servers : IsPrefix(SubSeq(log[0], 1, i), log[s])})
Acked(i) == 1 + Cardinality({f \in Followers : match[f] >= i})

Init ==
  /\ log = [s \in Servers |-> <<>>]
  /\ chan = [f \in Followers |-> <<>>]
  /\ match = [f \in Followers |-> 0]
  /\ commit = 0

Client(v) ==
  /\ Len(log[0]) < L
  /\ log' = [log EXCEPT ![0] = Append(@, v)]
  /\ UNCHANGED <<chan, match, commit>>

Send(f, i) ==
  /\ i <= Len(log[0])
  /\ Len(chan[f]) < C
  /\ chan' = [chan EXCEPT ![f] = Append(@, [idx |-> i, val |-> log[0][i]])]
  /\ UNCHANGED <<log, match, commit>>

Accept(f) ==
  /\ log' = [log EXCEPT ![f] = Append(@, Head(chan[f]).val)]
  /\ chan' = [chan EXCEPT ![f] = Tail(@)]
  /\ match' = [match EXCEPT ![f] = Len(log[f]) + 1]
  /\ UNCHANGED commit

Recv(f) ==
  /\ chan[f] # <<>>
  /\ Head(chan[f]).idx = Len(log[f]) + 1
  /\ Accept(f)

RecvBug(f) ==
  /\ BUG
  /\ chan[f] # <<>>
  /\ Len(log[f]) < L
  /\ Head(chan[f]).idx # Len(log[f]) + 1
  /\ Accept(f)

Drop(f) ==
  /\ chan[f] # <<>>
  /\ chan' = [chan EXCEPT ![f] = Tail(@)]
  /\ UNCHANGED <<log, match, commit>>

Advance(i) ==
  /\ commit < i
  /\ i <= Len(log[0])
  /\ Acked(i) >= Q
  /\ commit' = i
  /\ UNCHANGED <<log, chan, match>>

Next ==
  \/ \E v \in Values : Client(v)
  \/ \E f \in Followers, i \in 1..L : Send(f, i)
  \/ \E f \in Followers : Recv(f) \/ RecvBug(f) \/ Drop(f)
  \/ \E i \in 1..L : Advance(i)

Spec == Init /\ [][Next]_vars

Durable == Holds(commit) >= Q
LeaderAhead == \A f \in Followers : match[f] <= Len(log[0])
=============================================================================
