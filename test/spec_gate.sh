#!/bin/sh
# spec_gate.sh BUILDDIR: the abstract-model front end (cmc --spec) gives
# the right verdict, distinct-state count and shortest-counterexample
# depth on the .cml fixtures in test/spec/.  The pinned numbers are the
# ones single-worker TLC gives on the hand-written TLA+ twins (see
# test/spec/*.tla); check-spec-calibration re-verifies them against live
# TLC.  This gate is C-only and fast.
set -u
B=$(cd "${1:?usage: spec_gate.sh BUILDDIR}" && pwd)
HERE=$(cd "$(dirname "$0")/.." && pwd)
CMC=$B/cmc
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/spec-gate
mkdir -p "$OUT"
fail=0

# expectp DESC EXIT NEEDLE PROGRAM args...: PROGRAM exits EXIT and its
# output contains NEEDLE
expectp() {
	desc=$1 want=$2 needle=$3; shift 3
	"$@" > "$OUT/out.txt" 2>&1
	rc=$?
	if [ "$rc" = "$want" ] && grep -q -- "$needle" "$OUT/out.txt"; then
		echo "ok   $desc -> exit $rc"
	else
		echo "FAIL $desc -> exit $rc (wanted $want and \"$needle\")"
		sed 's/^/     /' "$OUT/out.txt" | head -20
		fail=1
	fi
}
# expect DESC EXIT NEEDLE cmc-args...
expect() {
	desc=$1 want=$2 needle=$3; shift 3
	expectp "$desc" "$want" "$needle" "$CMC" "$@"
}

echo "-- the abstract-model checker (cmc --spec), matched to TLC"
expect "counter: in range holds"            0 "4 distinct states found" --spec "$HERE/test/spec/counter.cml"
expect "mutex: mutual exclusion holds"       0 "8 distinct states found" --spec "$HERE/test/spec/mutex.cml"
expect "mutex: depth matches TLC (4)"        0 "complete state graph search is 4" --spec "$HERE/test/spec/mutex.cml"
expect "mutex: the bug is caught"            1 "mutual_exclusion is violated" --spec "$HERE/test/spec/mutex.cml" --define BUG
expect "mutex: shortest trace is 4 actions"  1 "is (4 actions)" --spec "$HERE/test/spec/mutex.cml" --define BUG
# a mistyped tag would check the spec without its planted bug and pass
expect "a --define that gates no action is an error" 3 "names no \[BUGG\] action" --spec "$HERE/test/spec/mutex.cml" --define BUGG
expect "emit-tla: a --define that gates no action is an error" 3 "names no \[BUGG\] action" --emit-tla "$HERE/test/spec/mutex.cml" --define BUGG

echo "-- modules: a protocol spec extends a shared base"
# Single-decree Paxos (Lamport's Paxos.tla): promises, highest-ballot
# value selection in phase 2a, votes in more than one ballot.  Counts,
# depth and the 12-action trace match single-worker TLC on the emitted
# module and on the hand-written paxos.tla; the bug's 2a ignores the
# votes the promises report.
expect "paxos: extends consensus, agreement holds" 0 "6144 distinct states found" --spec "$HERE/examples/spec/paxos.cml"
expect "paxos: depth matches TLC (16)"        0 "complete state graph search is 16" --spec "$HERE/examples/spec/paxos.cml"
expect "paxos: a 2a that ignores the promises breaks agreement" 1 "agreement is violated" --spec "$HERE/examples/spec/paxos.cml" --define BUG
expect "paxos: shortest trace is 12 actions"  1 "is (12 actions)" --spec "$HERE/examples/spec/paxos.cml" --define BUG

# Raft leader election (Ongaro's raft.tla): Election Safety, at most one
# leader per term.  Counts and depth match single-worker TLC (checked by
# make check-emit-tla); the revote bug lets two candidates reach a quorum.
expect "raft: election safety holds"          0 "38 distinct states found" --spec "$HERE/examples/spec/raft.cml"
expect "raft: depth matches TLC (4)"          0 "complete state graph search is 4" --spec "$HERE/examples/spec/raft.cml"
expect "raft: the revote bug elects two leaders" 1 "election_safety is violated" --spec "$HERE/examples/spec/raft.cml" --define BUG

# Humming Consensus (Machi; generic agreement over write-once epoch
# register stores): Agreement, at most one value chosen per epoch.
# Counts and depth match single-worker TLC; the overwrite bug breaks
# write-once so two values each reach a quorum.
expect "hc: agreement holds under write-once" 0 "41 distinct states found" --spec "$HERE/examples/spec/hc.cml"
expect "hc: depth matches TLC (5)"            0 "complete state graph search is 5" --spec "$HERE/examples/spec/hc.cml"
expect "hc: the overwrite bug breaks agreement" 1 "agreement is violated" --spec "$HERE/examples/spec/hc.cml" --define BUG

# Zab (Junqueira, Reed, Serafini 2011; ZooKeeper's Zab.tla): epochs,
# discovery from a quorum, synchronisation, broadcast and a committed
# prefix per epoch.  Of two epochs' committed prefixes one extends the
# other; counts and depth match single-worker TLC on the emitted module.
# The --define BUG starts an epoch from a stale history: initial_history
# fails first, in 13 steps (TLC finds committed_prefix fail in 20).
expect "zab: committed prefixes agree"        0 "27913 distinct states found" --spec "$HERE/examples/spec/zab.cml"
expect "zab: depth matches TLC (26)"          0 "complete state graph search is 26" --spec "$HERE/examples/spec/zab.cml"
expect "zab: a stale initial history misses a commit" 1 "initial_history is violated" --spec "$HERE/examples/spec/zab.cml" --define BUG
expect "zab: shortest trace is 13 actions"    1 "is (13 actions)" --spec "$HERE/examples/spec/zab.cml" --define BUG

# Flexible Paxos (Howard, Malkhi, Spiegelman 2016): the Paxos ballots of
# paxos.cml with a phase 1 quorum Q1 and a phase 2 quorum Q2.  With A=4,
# Q1=3, Q2=2 (sum 5 > 4) agreement holds; counts and depth match
# single-worker TLC on the emitted module.  The --define BUG runs phase 2a
# on 2 promises (2 + 2 = A), so a leader can miss a chosen value.
expect "fpaxos: safe when Q1+Q2>A"           0 "36756 distinct states found" --spec "$HERE/examples/spec/fpaxos.cml"
expect "fpaxos: depth matches TLC (20)"      0 "complete state graph search is 20" --spec "$HERE/examples/spec/fpaxos.cml"
expect "fpaxos: Q1+Q2<=A breaks agreement"   1 "agreement is violated" --spec "$HERE/examples/spec/fpaxos.cml" --define BUG
expect "fpaxos: shortest trace is 12 actions" 1 "is (12 actions)" --spec "$HERE/examples/spec/fpaxos.cml" --define BUG

# CASPaxos (Rystsov 2018): Paxos rounds on a rewritable register, each
# applying a change function to the highest-ballot state a quorum
# reports.  Confirmed states descend from earlier confirmed states;
# counts and depth match single-worker TLC on the emitted module.  The
# --define BUG accepts below a promise, so two confirmed states fork.
expect "caspaxos: confirmed states descend"   0 "3280 distinct states found" --spec "$HERE/examples/spec/caspaxos.cml"
expect "caspaxos: depth matches TLC (17)"     0 "complete state graph search is 17" --spec "$HERE/examples/spec/caspaxos.cml"
expect "caspaxos: accepting below a promise forks" 1 "descends is violated" --spec "$HERE/examples/spec/caspaxos.cml" --define BUG
expect "caspaxos: shortest trace is 12 actions" 1 "is (12 actions)" --spec "$HERE/examples/spec/caspaxos.cml" --define BUG

# Fast Paxos (Lamport 2006): a fast round, then a classic round whose
# coordinator applies Figure 2's rule to a classic quorum of promises.
# With N=3, Qc=2, Qf=3 the Quorum Requirement Qc + 2*Qf > 2*N holds and
# so does agreement; counts and depth match single-worker TLC on the
# emitted module.  The --define BUG runs with Qf=2 (2 + 4 = 2*N), so a
# fast-chosen value is overridden.
expect "fastpaxos: safe when Qc+2Qf>2N"      0 "1976 distinct states found" --spec "$HERE/examples/spec/fastpaxos.cml"
expect "fastpaxos: depth matches TLC (12)"   0 "complete state graph search is 12" --spec "$HERE/examples/spec/fastpaxos.cml"
expect "fastpaxos: Qc+2Qf<=2N breaks agreement" 1 "agreement is violated" --spec "$HERE/examples/spec/fastpaxos.cml" --define BUG
expect "fastpaxos: shortest trace is 11 actions" 1 "is (11 actions)" --spec "$HERE/examples/spec/fastpaxos.cml" --define BUG

# Two-phase commit (Lamport's TwoPhase.tla): the canonical non-consensus
# example.  Consistency (atomicity) -- a committed and an aborted
# resource manager never coexist -- holds because a resource manager
# commits only on a unanimous prepared vote; the --define BUG commits
# without that check.
expect "twophase: atomicity holds"            0 "No error has been found" --spec "$HERE/examples/spec/twophase.cml"
expect "twophase: the no-unanimity bug breaks atomicity" 1 "consistency is violated" --spec "$HERE/examples/spec/twophase.cml" --define BUG

# guards that protect a division and an application (test/spec/guard.tla
# is the TLC twin; cmc 2.1 refused this spec)
expect "guard: short-circuit guards hold"   0 "7 distinct states found" --spec "$HERE/test/spec/guard.cml"
expect "guard: depth matches TLC (7)"       0 "complete state graph search is 7" --spec "$HERE/test/spec/guard.cml"
expect "guard: the unmarked advance is caught" 1 "is (1 actions)" --spec "$HERE/test/spec/guard.cml" --define BUG

echo "-- symmetry reduction: interchangeable node ids collapse"
# the token spec's node ids are fully symmetric; with symmetry declared,
# the state space shrinks (10 -> 3) with the SAME verdict -- the soundness
# check for the declaration.
expect "token: symmetry collapses the state space" 0 "3 distinct states found" --spec "$HERE/examples/spec/token.cml"
# over a function whose keys and values are the symmetric ids (both are
# remapped, and the keys re-sorted): the same verdicts as without it
sed '/^symmetry/d' "$HERE/test/spec/symfun.cml" > "$OUT/symfun-nosym.cml"
expect "symfun: symmetry over a function's keys and values" 0 "4 distinct states found"  --spec "$HERE/test/spec/symfun.cml"
expect "symfun: without symmetry"                           0 "16 distinct states found" --spec "$OUT/symfun-nosym.cml"
expect "symfun: the bug under symmetry"     1 "is (2 actions)" --spec "$HERE/test/spec/symfun.cml" --define BUG
expect "symfun: the bug without symmetry"   1 "is (2 actions)" --spec "$OUT/symfun-nosym.cml" --define BUG
# a symmetry set of sets is canonical after the permutation too
printf 'module syms\nconst N = 3\nsymmetry 1 .. N\nvar s : 0 .. 7\nvar last : 0 .. N\ninit s == 0 and last == 0\naction add(n in 1 .. N):\n  when last != n and s < 7\n  last%s = n\n  s%s = s + 1\ninvariant ok: last in {0} or last in 1 .. N\n' "'" "'" > "$OUT/syms.cml"
expect "a symmetric scalar keeps the verdict"  0 "No error has been found" --spec "$OUT/syms.cml"
# a symmetry declaration of one value, or one that is not a set, is no
# reduction: the search is the full one
printf 'module sym1\nsymmetry {1}\nvar x : 0 .. 1\ninit x == 0\naction a:\n  when x == 0\n  x%s = 1\ninvariant ok: TRUE\n' "'" > "$OUT/sym1.cml"
expect "a one-value symmetry set reduces nothing" 0 "2 distinct states found" --spec "$OUT/sym1.cml" --no-deadlock
# a declaration it cannot use is an error, not a search without it
for sym in "x" "1 .. 9" "{1, y}"; do
	printf 'module s\nsymmetry %s\nvar x : 0 .. 1\ninit x == 0\ninvariant ok: TRUE\n' "$sym" > "$OUT/symbad.cml"
	expect "symmetry $sym is an error" 3 "symmetry $sym:" --spec "$OUT/symbad.cml"
done

echo "-- TLC's checking options: deadlock, CONSTRAINT, ACTION_CONSTRAINT"
# The counts are single-worker TLC's on test/spec/deadlock.tla and
# constrain.tla (check-spec-calibration re-verifies them).  A state with
# no successor deadlocks; a step that changes nothing is a successor
# (deadlock.cml's idle), and so is a step outside the model.
T=$HERE/test/spec
expect "deadlock: a state with no action deadlocks"  1 "Error: Deadlock reached." --spec "$T/deadlock.cml"
expect "deadlock: the shortest trace is 3 actions"   1 "is (3 actions)" --spec "$T/deadlock.cml"
expect "deadlock: --no-deadlock turns it off"        0 "8 distinct states found" --spec "$T/deadlock.cml" --no-deadlock
expect "deadlock: deadlock = off turns it off"       0 "8 distinct states found" --spec "$T/deadlock_off.cml"
expect "deadlock: depth matches TLC (5)"             0 "complete state graph search is 5" --spec "$T/deadlock_off.cml"
expect "deadlock: an unexpanded state does not deadlock" 0 "6 distinct states found" --spec "$T/deadlock_c.cml"
expect "deadlock: a refused step is still a successor" 0 "6 distinct states found" --spec "$T/deadlock_ac.cml"
printf 'module m\nvar x : 0 .. 1\ninit x == 0\ninvariant i: x == 1\n' > "$OUT/dlinv.cml"
expect "deadlock: an invariant before a deadlock at the init" 1 "Invariant i is violated" --spec "$OUT/dlinv.cml"
expect "deadlock: an invariant generated first is reported first" 1 "not_three is violated" --spec "$T/dlorder.cml"
expect "deadlock: in 2 actions, as TLC"              1 "is (2 actions)" --spec "$T/dlorder.cml"
printf 'module m\nvar x : 0 .. 1\ninit x == 0\ndeadlock = off\ndeadlock = on\ninvariant i: TRUE\n' > "$OUT/dlon.cml"
expect "deadlock = on after off turns it back on"    1 "Deadlock reached" --spec "$OUT/dlon.cml"
expect "constraint: none"                            0 "16 distinct states found" --spec "$T/constrain.cml"
expect "constraint: failing states are not counted"  0 "6 distinct states found" --spec "$T/constrain_c.cml"
expect "constraint: depth matches TLC (3)"           0 "complete state graph search is 3" --spec "$T/constrain_c.cml"
expect "constraint: invariants hold on a dropped state" 1 "not_three is violated" --spec "$T/constrain_inv.cml"
expect "constraint: that trace is 3 actions"         1 "is (3 actions)" --spec "$T/constrain_inv.cml"
expect "action constraint: refused steps are not taken" 0 "10 distinct states found" --spec "$T/constrain_ac.cml"
expect "action constraint: depth matches TLC (7)"    0 "complete state graph search is 7" --spec "$T/constrain_ac.cml"
expect "action constraint: invariants hold on a refused target" 1 "is (1 actions)" --spec "$T/constrain_acinv.cml"
printf 'module m\nvar x : 0 .. 1\ninit x == 1\nconstraint c: x == 0\ninvariant i: TRUE\n' > "$OUT/cinit.cml"
expect "constraint: an initial state outside it is not counted" 0 "0 distinct states found" --spec "$OUT/cinit.cml"
printf 'module m\nvar x : 0 .. 1\ninit x == 1\nconstraint c: x == 0\ninvariant i: x == 0\n' > "$OUT/cinit.cml"
expect "constraint: an initial state outside it is still checked" 1 "Invariant i is violated" --spec "$OUT/cinit.cml"
expect "emit-tla: the constraint operators"          0 "y. <= x. \\\\/ y. < y" --emit-tla "$T/constrain_ac.cml"
expect "emit-cfg: CONSTRAINT"                        0 "CONSTRAINT low" --emit-cfg "$T/constrain_c.cml"
expect "emit-cfg: ACTION_CONSTRAINT"                 0 "ACTION_CONSTRAINT follow" --emit-cfg "$T/constrain_ac.cml"
expect "emit-cfg: deadlock = off"                    0 "CHECK_DEADLOCK FALSE" --emit-cfg "$T/deadlock_off.cml"
expect "emit-cfg: --no-deadlock"                     0 "CHECK_DEADLOCK FALSE" --emit-cfg "$T/deadlock.cml" --no-deadlock
expect "emit-tla: the header names --no-deadlock when it is off" 0 "add --no-deadlock" --emit-tla "$T/deadlock_off.cml"
# refused: a constraint the checker cannot apply is an error, not a pass
cbad() {	# cbad DESC NEEDLE BODY: a spec of module m with BODY is refused
	printf 'module m\nvar x : 0 .. 1\ninit x == 0\naction a:\n  when x == 0\n  x%s = 1\n%b\n' "'" "$3" > "$OUT/cbad.cml"
	expect "$1" 3 "$2" --spec "$OUT/cbad.cml"
}
cbad "a primed variable in a state constraint" "primed variable" "constraint c: x' == 0\ninvariant i: TRUE"
cbad "a primed non-variable in an action constraint" "primed variable" "action_constraint c: N' == 0\ninvariant i: TRUE"
cbad "a non-boolean constraint"            "expected a boolean in a constraint" "constraint c: x\ninvariant i: TRUE"
cbad "a non-boolean action constraint"     "expected a boolean in an action constraint" "action_constraint c: x'\ninvariant i: TRUE"
cbad "a constraint with no name"           "constraint needs name:" "constraint x == 0\ninvariant i: TRUE"
cbad "a deadlock line without ="           "deadlock needs = on or = off" "deadlock off\ninvariant i: TRUE"
cbad "a deadlock value that is not on or off" "deadlock is on or off, not no" "deadlock = no\ninvariant i: TRUE"
cbad "a property under a constraint"       "cannot be checked under a constraint" "constraint c: TRUE\nproperty p: eventually x == 1"
cbad "a property under an action constraint" "cannot be checked under a constraint" "action_constraint c: TRUE\nproperty p: eventually x == 1"
cbad "too many constraints" "too many constraints" "$(i=0; while [ $i -lt 17 ]; do printf 'constraint c%d: TRUE\\n' $i; i=$((i + 1)); done)invariant i: TRUE"

echo "-- liveness: temporal properties under weak fairness"
# the ticker must eventually reach its max under weak fairness (holds);
# with a reset action a fair cycle avoids the max (fails).
expect "ticker: eventually-max holds under fairness" 0 "1 property held" --spec "$HERE/examples/spec/ticker.cml"
expect "ticker: a fair reset cycle breaks it"        1 "reaches_max is violated" --spec "$HERE/examples/spec/ticker.cml" --define RESET

echo "-- the TLA+ bridge (cmc --emit-tla), structure only (check-emit-tla runs TLC)"
# the emitted module's shape (test/spec/shape.cml; emit_gate runs TLC on it)
expect "shape: holds, with both properties" 0 "9 distinct states found" --spec "$HERE/test/spec/shape.cml"
emitted() {	# emitted DESC NEEDLE: the emitted module of shape.cml contains NEEDLE
	expect "emit-tla: $1" 0 "$2" --emit-tla "$HERE/test/spec/shape.cml"
}
emitted "constants are Int and bound by CInit" "/. M = 1"
emitted "a function variable is Int -> Int"    "@type: Int -> Int;"
emitted "the vars tuple"                       "vars == <<x, f>>"
emitted "an action with no guard is TRUE"      "/. TRUE"
emitted "an unassigned variable is UNCHANGED"  "UNCHANGED <<f>>"
emitted "an except with two keys"              "EXCEPT !\[i\] = 1, !\[0\] = 1"
emitted "weak fairness for every action"       "WF_vars(tick) /. WF_vars(both) /. WF_vars(idle)"
emitted "eventually"                           "<>(x = 0)"
emitted "leads-to"                             ") ~> (f\[0\] = 1)"
# words that only begin with an operator's name stay names; count over a
# set literal, nested in parens, with a nested count, and a gated action
# that --define leaves out of Next and the fairness list
printf 'module words\nconst counted = 1\nvar inner : 0 .. 1\nvar orb : 0 .. 1\ninit inner == 0 and orb == 0\naction a:\n  when (count k in {0, 1} : (k == inner) and (count j in 0 .. 1 : j == k) == 1) >= counted\n  orb%s = 1\naction b [OFF]:\n  inner%s = 1\nfairness weak\ninvariant ok: orb + inner * 2 / 1 <= 3 and (orb mod 2) == orb\n' "'" "'" > "$OUT/words.cml"
expect "emit-tla: count over a set literal, nested"   0 "{0, 1}  :  (k = inner) /. (Cardinality({j .in 0 .. 1  :  j = k})) = 1}))" --emit-tla "$OUT/words.cml"
expect "emit-tla: a name that starts with an operator" 0 "orb' = 1" --emit-tla "$OUT/words.cml"
expect "emit-tla: a gated action is left out"         0 "WF_vars(a)\$" --emit-tla "$OUT/words.cml"
expect "emit-tla: / and mod in an invariant"          0 "orb + inner . 2 .div  1 <= 3 /. (orb % 2) = orb" --emit-tla "$OUT/words.cml"
# except with a key that is itself an application, and a value with
# spaces; and fairness over a module whose first action is gated off
printf 'module ex\nvar f : [0 .. 1 -> 0 .. 1]\nvar g : [0 .. 1 -> 0 .. 1]\ninit f == [i in 0 .. 1 -> 0] and g == [i in 0 .. 1 -> 0]\naction off [OFF]:\n  g%s = g\naction set(i in 0 .. 1):\n  f%s = [f except [g[i]] =  1 - f[i] ]\nfairness weak\ninvariant ok: TRUE\n' "'" "'" > "$OUT/ex.cml"
expect "emit-tla: an except keyed by an application" 0 "EXCEPT !\[g\[i\]\] =  1 - f\[i\] \]" --emit-tla "$OUT/ex.cml"
expect "emit-tla: fairness skips a gated action"     0 "Spec == Init /. \[\]\[Next\]_vars /. WF_vars(set)$" --emit-tla "$OUT/ex.cml"
printf 'module bare\nvar x : 0 .. 1\ninit x == 0\ninvariant ok: x == 0\n' > "$OUT/bare.cml"
# with no action, Next is FALSE, so TLC finds the deadlock cmc --spec finds
expect "emit-tla: no constants and no actions" 0 "^  FALSE$" --emit-tla "$OUT/bare.cml"
# every operator inside a count body is translated (emit_gate runs TLC on it)
expect "countops: the operators in a count body"   0 "5 distinct states found" --spec "$HERE/test/spec/countops.cml"
expect "emit-tla: not, in, some and all in a count body" 0 "~ (j .in {})" --emit-tla "$HERE/test/spec/countops.cml"
expect "emit-tla: a well-formed module header" 0 "MODULE paxos" --emit-tla "$HERE/examples/spec/paxos.cml"
expect "emit-tla: translates operators to TLA+" 0 "Cardinality" --emit-tla "$HERE/examples/spec/paxos.cml"

echo "-- the loader refuses malformed specs"
printf 'module m\nvar x : 0 .. 2\ninit x == 0\naction a:\n  when x < 2\n  x%s = x + 1\n' "'" > "$OUT/noinv.cml"
expect "a spec with no invariant is an error" 3 "no invariant" --spec "$OUT/noinv.cml"
printf 'var x : 0 .. 2\ninit x == 0\ninvariant t: x <= 2\n' > "$OUT/nomod.cml"
expect "a spec with no module is an error"    3 "no module" --spec "$OUT/nomod.cml"

# an operand of the wrong type is a model error, as TLC reports it, never
# a value: `not votes` read a set as 0 and held for every state
echo "-- the evaluator refuses an operand of the wrong type"
# typed DESC NEEDLE EXPR [GUARD]: a spec whose invariant is EXPR (and
# whose action is guarded by GUARD) is refused with NEEDLE
typed() {
	printf 'module m\nvar x : 0 .. 1\nvar f : [0 .. 1 -> 0 .. 1]\ninit x == 0 and f == [i in 0 .. 1 -> 0]\naction a:\n  when %s\n  x%s = 1\ninvariant i: %s\n' "${4:-x < 1}" "'" "$3" > "$OUT/typed.cml"
	expect "$1" 3 "$2" --spec "$OUT/typed.cml"
}
typed "not of a set"                      "expected a boolean" "not {1}"
typed "and of an integer"                 "expected a boolean" "x and TRUE"
typed "or of a function"                  "expected a boolean" "FALSE or f"
typed "an invariant that is an integer"   "expected a boolean" "x + 1"
typed "a guard that is a set"             "expected a boolean" "TRUE" "{x}"
typed "all over an integer body"          "expected a boolean" "all i in 0 .. 1 : i"
typed "count over a set body"             "expected a boolean" "(count i in 0 .. 1 : {i}) == 0"
typed "arithmetic on a set"               "expected an integer" "{0} + 1 == 1"
typed "arithmetic on a boolean"           "expected an integer" "TRUE * 2 == 2"
typed "an order on a set"                 "expected an integer" "x < {1}"
typed "a set of sets"                     "expected an integer" "{{1}} == {{1}}"
typed "a range bounded by a set"          "expected an integer" "x in 0 .. {1}"
typed "membership of a function"          "expected an integer" "f in {0}"
typed "a function of booleans"            "expected an integer" "[i in 0 .. 1 -> TRUE] == f"
typed "a function applied to a set"       "expected an integer" "f[{0}] == 0"
typed "== between a function and a set"   "compares a function with a set" "f == {0}"
typed "== between a boolean and an integer" "compares a boolean with an integer" "TRUE == 1"
typed "!= between a set and an integer"   "compares a set with an integer" "{x} != 0"
# each operator checks its second operand too, and a function compares
# by its entries
typed "* of a boolean"          "expected an integer in \*" "x * TRUE == 0"
typed "/ of a boolean"          "expected an integer in /"   "TRUE / 1 == 0"
typed "/ by a boolean"          "expected an integer in /"   "1 / TRUE == 0"
typed "mod of a boolean"        "expected an integer in mod" "TRUE mod 2 == 0"
typed "mod by a boolean"        "expected an integer in mod" "1 mod TRUE == 0"
typed "+ of a boolean"          "expected an integer in +"   "x + TRUE == 0"
typed "- of a boolean"          "expected an integer in -"   "TRUE - 1 == 0"
typed "- by a boolean"          "expected an integer in -"   "1 - TRUE == 0"
typed "an order on a boolean"   "expected an integer in >="  "TRUE >= 1"
typed "a range from a boolean"  "expected an integer in .."  "x in TRUE .. 1"
typed "a function key that is a set" "expected an integer in a function key" "[f except [{0}] = 1] == f"
typed "a function value that is a set" "expected an integer in a function value" "[f except [0] = {1}] == f"
typed "a literal over no set"   "expected a set"             "[i in x -> 0] == f"
typed "a literal whose name shadows a var" "redeclares" "[x in 0 .. 1 -> 0] == f"
typed "a broken operand of a range" "unknown identifier"     "x in y .. 1"
typed "a broken upper bound"    "unknown identifier"         "x in 0 .. y"
typed "a broken set in a quantifier" "unknown identifier"    "all i in y .. 1 : TRUE"
typed "or of a set on the right" "expected a boolean in or"  "FALSE or {1}"
typed "an and of a broken operand"  "unknown identifier"       "TRUE and y"
typed "an or of a broken operand"   "unknown identifier"       "FALSE or y"
# and/or short-circuit as TLC does: an operand the left one decides is
# still parsed (a name error or a syntax error in it is refused) but not
# evaluated, so its type and evaluation errors are not raised
typed "an unknown name past a short-circuit" "unknown identifier" "x == 0 or y == 0"
typed "a syntax error past a short-circuit"  "expected )"         "FALSE and (x == 0"
typed "a non-boolean left of a short-circuit" "expected a boolean in and" "x and FALSE"
typed "a set with text after it"  "trailing text"            "x in {0} 1"
typed "a range with a broken bound" "unknown identifier"       "x in 0 .. (y)"
typed "a literal with no -> after the set" "expected -> in function literal" "[i in 0 .. 1 0] == f"
typed "an except value that fails"  "unknown identifier"       "[f except [0] = y] == f"
typed "an except key that fails"    "unknown identifier"       "[f except [y] = 0] == f"
typed "a / with a broken divisor"   "unknown identifier"       "x / y == 0"
typed "a mod with a broken divisor" "unknown identifier"       "x mod y == 0"
typed "a function applied to a broken key" "unknown identifier" "f[y] == 0"
typed "a literal body that fails"   "unknown identifier"       "[i in 0 .. 1 -> y] == f"
typed "a literal body of a set"     "expected an integer in a function value" "[i in 0 .. 1 -> {i}] == f"
# each malformed form is refused with its reason
typed "an unclosed (" "expected )" "(x == 0"
typed "an unclosed [i in" "expected ]" "f == [i in 0 .. 1 -> 0"
typed "a literal with no ->" "expected -> in function literal" "[i in 0 .. 1 : 0] == f"
typed "[..] with neither in nor except" "expected 'in' or 'except'" "[f with [0] = 1] == f"
typed "except on a constant" "except on a non-variable" "[g except [0] = 1] == f"
typed "except on a scalar" "except on a variable that is not a function" "[x except [0] = 1] == f"
typed "except with no [" "expected \\[ in except" "[f except 0 = 1] == f"
typed "except with no ]" "expected ]" "[f except [0 = 1] == f"
typed "except with no =" "expected = in except" "[f except [0] 1] == f"
typed "except with no closing ]" "expected ] to close except" "f == [f except [0] = 1"
typed "an unclosed {" "expected }" "{0} == {0, 1"
typed "a set past its limit" "set too large" "{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24} == {0}"
typed "a range past its limit" "range too large" "x in 0 .. 24"
typed "a literal past its limit" "function too large" "[i in 0 .. 12 -> 0] == f"
typed "an except past its limit" "function too large" "[f except [2] = 0, [3] = 0, [4] = 0, [5] = 0, [6] = 0, [7] = 0, [8] = 0, [9] = 0, [10] = 0, [11] = 0, [12] = 0] == f"
typed "an unclosed application" "expected ] in application" "0 == f[0"
typed "a scalar applied" "applied a non-function" "x[0] == 0"
typed "an application outside the domain" "outside its domain" "f[2] == 0"
typed "an empty operand" "expected an expression" "x == "
typed "an unknown name" "unknown identifier" "y == 0"
typed "text after the expression" "trailing text" "x == 0 )"
typed "a quantifier with no in" "expected 'in' in quantifier" "all i : TRUE"
typed "a quantifier with no :" "expected ':' in quantifier" "all i in 0 .. 1 TRUE"
typed "a quantifier over no set" "expected a set" "all i in x : TRUE"
typed "a broken body over the empty set" "unknown identifier" "all i in {} : y"
typed "a bound name that shadows a var" "redeclares" "some x in 0 .. 1 : TRUE"
typed "nested bound names past the limit" "too many nested bound names" "all a in 0 .. 1 : all b in 0 .. 1 : all c in 0 .. 1 : all d in 0 .. 1 : all e in 0 .. 1 : TRUE"
typed "divide by zero" "divide by zero" "x / 0 == 0"
typed "a negative divisor" "/ of a negative number" "x / (0 - 1) == 0"
typed "mod by zero" "mod by zero" "x mod 0 == 0"
typed "a negative mod" "mod of a negative number" "(0 - 1) mod 2 == 1"
typed "an overflowing product" "integer overflow in \\*" "4611686018427387904 * 2 == 0"
typed "an overflowing sum" "integer overflow in +" "9223372036854775807 + 1 == 0"
typed "an overflowing difference" "integer overflow in -" "(0 - 9223372036854775807) - 2 == 0"
typed "a primed variable read" "primed variable" "x' == 0"
typed "a function literal without ->" "expected -> in function literal" "[i in {0, 1} - 1] == f"
typed "an empty-domain literal that shadows a var" "redeclares" "[x in {} -> 0] == f"
typed "a broken body over an empty domain" "expected )" "[i in {} -> (0] == f"
typed "an integer left of or" "expected a boolean in or, not an integer" "x or TRUE"
typed "a skipped quantifier still binds its name" "redeclares" "FALSE and (all x in 0 .. 1 : TRUE) or TRUE"
typed "an identifier past the name limit" "unknown identifier" "$(printf '%062d' 0 | tr 0 a) == 0"
typed "a range with one dot" "expected a set (LO .. HI" "x in 0 . 1"
typed "an unknown name with an underscore" "unknown identifier" "x_ == 0"
# the values the evaluator computes, each pinned by the verdict it gives
value() {	# value DESC WANT EXPR: invariant EXPR holds (0) or is violated (1)
	printf 'module m\nvar x : 0 .. 1\nvar f : [0 .. 1 -> 0 .. 1]\nvar g : [0 .. 1 -> 0 .. 1]\ninit x == 0 and f == [i in 0 .. 1 -> i] and g == [i in 0 .. 1 -> 0]\ninvariant i: %s\n' "$3" > "$OUT/value.cml"
	if [ "$2" = 0 ]; then expect "$1" 0 "No error has been found" --spec "$OUT/value.cml" --no-deadlock
	else expect "$1" 1 "Invariant i is violated" --spec "$OUT/value.cml" --no-deadlock; fi
}
value "division truncates"          0 "7 / 2 == 3 and 7 mod 2 == 1 and 2 * 3 - 1 == 5"
value "sets compare by members"     0 "{1, 0, 1} == {0, 1} and {0} != {1} and {} == {}"
value "a set and a longer set"      1 "{0} == {0, 1}"
value "functions compare by entries" 0 "f != g and f == [i in 0 .. 1 -> i] and [g except [1] = 1] == f"
value "a function and a shorter one" 1 "f == [i in 0 .. 0 -> 0]"
value "an except that adds a key"   1 "[f except [5] = 0] == f"
value "booleans compare"            0 "TRUE != FALSE and (x == 0) == TRUE"
value "quantifiers and count"       0 "(some i in 0 .. 1 : f[i] == 1) and not (all i in 0 .. 1 : f[i] == 1) and (count i in 0 .. 1 : f[i] == 0) == 1"
value "orders"                      0 "0 < 1 and 1 > 0 and 1 <= 1 and 1 >= 1 and not (1 < 1)"
value "membership"                  0 "x in {0} and not (x in {}) and 1 in 0 .. 1"
value "functions of one length differ in a value" 1 "f == [i in 0 .. 1 -> 0]"
value "functions of one length differ in a key"   1 "[i in 0 .. 1 -> i] == [i in 1 .. 2 -> i - 1]"
# the right operand of a decided and/or is not evaluated (cmc 2.1
# evaluated it and refused each of these)
value "or skips a division by zero"  0 "x == 0 or 1 / x == 1"
value "and skips an application outside the domain" 0 "x > 0 and f[2] == 0 or x == 0"
value "or skips a non-boolean"       0 "TRUE or f"
value "and skips a quantifier" 0 "not (FALSE and (all i in 0 .. 1 : 1 / i == 0))"
value "a quantifier over no element evaluates no body" 0 "all i in 1 .. x : 1 / (i - 1) == 0"
value "and skips an overflow"        0 "x == 1 and 9223372036854775807 + 1 == 0 or TRUE"
value "or skips a set past its limit" 0 "TRUE or x in 0 .. 99"
value "and skips a function literal" 0 "not (FALSE and [i in 0 .. 1 -> 1 / i] == f)"
value "and skips an except outside the domain" 0 "x == 1 and [x except [0] = 1] == f or TRUE"
value "an evaluated operand still errs" 1 "x == 0 and f[0] == 1"
# a skipped operand is parsed, not evaluated: no error from its values
value "and skips a literal over a set" 0 "not (FALSE and [i in {0, 1} -> 0] == f)"
value "and skips membership of a scalar" 0 "not (FALSE and x in 0)"
value "and skips a mixed comparison" 0 "FALSE and {0} == 1 or TRUE"
value "and skips each arithmetic error" 0 "FALSE and 9223372036854775807 * 2 + 1 / 0 + (0 - 1) / 1 + 1 / (0 - 2) == 0 or TRUE"
value "and skips each mod error" 0 "FALSE and 1 mod 0 + (0 - 1) mod 2 + 1 mod (0 - 2) == 0 or TRUE"
value "and skips an overflow in -" 0 "FALSE and (0 - 9223372036854775807) - 2 == 0 or TRUE"
value "and skips a two-key except" 0 "FALSE and [f except [0] = 1, [1] = 1] == f or TRUE"
# the same operators on the right types still evaluate
printf 'module m\nvar x : 0 .. 1\nvar f : [0 .. 1 -> 0 .. 1]\ninit x == 0 and f == [i in 0 .. 1 -> 0]\naction a:\n  when not (x == 1) and (TRUE or FALSE) and x in {0, 1}\n  x%s = 1\n  f%s = [f except [x] = 1]\ninvariant i: (count i in 0 .. 1 : f[i] == 1) + x * 2 <= 3 and {x} != {2} and f != [i in 0 .. 1 -> 1] and (TRUE == TRUE)\n' "'" "'" > "$OUT/typed-ok.cml"
expect "well-typed operators still evaluate" 0 "2 distinct states found" --spec "$OUT/typed-ok.cml" --no-deadlock

# every line the loader cannot use is refused with its reason, never
# skipped: a skipped `when` or invariant would check a different model
echo "-- the loader names what it cannot read"
# bad DESC NEEDLE BODY: a spec of module m with BODY (printf %b escapes)
# is refused, exit 3, with NEEDLE
bad() {
	printf 'module m\n%b\n' "$3" > "$OUT/bad.cml"
	expect "$1" 3 "$2" --spec "$OUT/bad.cml"
}
V='var x : 0 .. 1\ninit x == 0'
I='invariant i: x <= 1'
bad "a const without ="             "const needs ="            "const N 3\n$V\n$I"
bad "a const that is not a number"  "const N: bad value"       "const N = {1}\n$V\n$I"
bad "a const that fails to evaluate" "const N: bad value"      "const N = M\n$V\n$I"
bad "a const that is a boolean"     "const N: bad value"       "const N = TRUE\n$V\n$I"
bad "a var without :"               "var needs :"              "var x 0 .. 1\ninit x == 0\n$I"
bad "a function var without ->"     "function var needs ->"    "var f : [0 .. 1, 0 .. 1]\ninit f == 0\n$I"
bad "fairness other than weak"      "only 'fairness weak'"     "$V\nfairness strong\n$I"
bad "a property without a name"     "property needs name:"     "$V\nproperty x == 1\n$I"
bad "a property of no known form"   "property must be"         "$V\nproperty p: always x == 0\n$I"
bad "an invariant without a name"   "invariant needs name:"    "$V\ninvariant x <= 1"
bad "a when outside an action"      "when outside action"      "$V\nwhen x == 0\n$I"
bad "an action parameter without in" "action param needs 'in'" "$V\naction a(i):\n  x' = 1\n$I"
bad "a line that is nothing"        "cannot parse: x = 1"      "$V\nx = 1\n$I"
bad "an extends that does not load" "cannot extend nosuch"     "extends nosuch\n$V\n$I"
bad "a function var without ]"     "function var needs ]"     "var f : [0 .. 1 -> 0 .. 1\ninit f == [i in 0 .. 1 -> 0]\n$I"
bad "an action tag without ]"       "action tag needs ]"       "$V\naction a [BUG:\n  x' = 1\n$I"
bad "action parameters without )"   "action parameters need )" "$V\naction a(i in 0 .. 1:\n  x' = i\n$I"
bad "an assignment without a prime"  "cannot parse: x = 1"      "$V\naction a:\n  x = 1\n$I"
bad "a prime only after the ="      "cannot parse: x == 1 or x'" "$V\naction a:\n  x == 1 or x'\n$I"
bad "an extends of a missing .cml"  "cannot open .*nosuch.cml" "extends nosuch.cml\n$V\n$I"
bad "an extends of a missing absolute path" "cannot open /nonexistent/nosuch.cml" "extends /nonexistent/nosuch\n$V\n$I"
bad "a spec with no variables"      "no variables"             "init x == 0\n$I"
bad "a spec with no init"           "no init"                  "var x : 0 .. 1\n$I"
bad "a spec with no invariant"      "no invariant or property" "$V"
bad "an init conjunct that is not =="   "init conjunct is not var == expr" "var x : 0 .. 1\ninit x < 1\n$I"
bad "an init of an unknown variable"    "init assigns unknown var y"       "var x : 0 .. 1\ninit y == 0\n$I"
bad "an init that sets a variable twice" "init assigns x twice"            "var x : 0 .. 1\ninit x == 0 and x == 1\n$I"
bad "an init that leaves a variable unset" "init does not set y"           "$V\nvar y : 0 .. 1\n$I"
bad "an assignment to an unknown variable" "assign to unknown var y"       "$V\naction a:\n  y' = 1\n$I"
bad "a function var outside its keys"   "is not a function on"             "var f : [0 .. 1 -> 0 .. 1]\ninit f == [i in 0 .. 2 -> 0]\ninvariant i: TRUE"
bad "a function var outside its range"  "is outside its range"             "var f : [0 .. 1 -> 0 .. 1]\ninit f == [i in 0 .. 1 -> 2]\ninvariant i: TRUE"
bad "a scalar var given a set"          "x is a set, not an integer"       "var x : 0 .. 1\ninit x == {0}\ninvariant i: TRUE"
bad "a function var given an integer"   "is not a function on"             "var f : [0 .. 1 -> 0 .. 1]\ninit f == 0\ninvariant i: TRUE"
bad "a domain of non-integer bounds"    "bounds of 0 .. TRUE are not integers" "var x : 0 .. TRUE\ninit x == 0\ninvariant i: TRUE"
bad "a domain that is not a set"        "expected a set"                   "var x : x\ninit x == 0\ninvariant i: TRUE"
bad "a range domain with a broken bound" "unknown identifier"              "var x : 0 .. y\ninit x == 0\ninvariant i: TRUE"
bad "a set domain with a broken member" "unknown identifier"               "var x : {0, y}\ninit x == 0\ninvariant i: TRUE"
bad "a value outside a set domain"      "x = 1 is outside its domain {0, 2}" "var x : {0, 2}\ninit x == 1\ninvariant i: TRUE"
bad "a function range with a broken bound" "unknown identifier"            "var f : [0 .. 1 -> 0 .. y]\ninit f == [i in 0 .. 1 -> 0]\ninvariant i: TRUE"
bad "a function domain that fails"      "unknown identifier"               "var f : [0 .. y -> 0 .. 1]\ninit f == [i in 0 .. 1 -> 0]\ninvariant i: TRUE"
bad "a function on other keys"          "f is not a function on 0 .. 1"    "var f : [0 .. 1 -> 0 .. 1]\ninit f == [i in 1 .. 2 -> 0]\ninvariant i: TRUE"
printf 'module sd\nvar x : {0, 2}\ninit x == 0\naction a:\n  when x == 0\n  x%s = 2\ninvariant i: x != 1\n' "'" > "$OUT/sd.cml"
expect "a set domain holds its members"  0 "2 distinct states found" --spec "$OUT/sd.cml" --no-deadlock
bad "a parameter that redeclares a var" "parameter x redeclares"           "$V\naction a(x in 0 .. 1):\n  x' = 1\n$I"
bad "a parameter domain that is not a set" "param domain: expected a set" "$V\naction a(i in x):\n  x' = 1\n$I"
bad "a parameter domain that fails"     "param domain"                     "$V\naction a(i in 0 .. y):\n  x' = 1\n$I"
bad "too many parameters"               "too many params"                  "$V\naction a(i in 0 .. 1, j in 0 .. 1, k in 0 .. 1, l in 0 .. 1, m in 0 .. 1):\n  x' = 1\n$I"
bad "a guard that fails to evaluate"    "action a: unknown identifier"     "$V\naction a:\n  when y\n  x' = 1\n$I"
bad "an invariant that fails to evaluate" "invariant evaluation error"     "$V\ninvariant i: y"
bad "too many successors"               "too many successors"              "var x : 0 .. 4095\ninit x == 0\naction a(i in 0 .. 23, j in 0 .. 23, k in 0 .. 23):\n  when x == 0\n  x' = i\n$I"
L=$(printf '%01100d' 0)
bad "a line longer than the loader holds" "line is too long"               "$V\ninvariant i: x <= $L"
bad "an init longer than its buffer"    "init is too long"                 "var x : 0 .. 1\ninit x == $(printf '%0400d' 0)\ninit x == $(printf '%0400d' 0)\n$I"
# a name or text past its buffer is refused, naming which, never cut
N33=$(printf 'n%032d' 0)
E600=$(printf '0 + %0596d' 0)
printf 'module %s\n%b\n%b\n' "$N33" "$V" "$I" > "$OUT/bad.cml"
expect "a module name too long" 3 "module name is too long" --spec "$OUT/bad.cml"
bad "a const name too long"         "const name is too long"      "const $N33 = 1\n$V\n$I"
bad "a var name too long"           "var name is too long"        "var $N33 : 0 .. 1\ninit $N33 == 0\n$I"
bad "a var domain too long"         "var domain is too long"      "var x : $E600 .. 1\ninit x == 0\n$I"
bad "a function domain too long"    "function domain is too long" "var f : [$E600 .. 1 -> 0 .. 1]\ninit f == 0\n$I"
bad "a function range too long"     "function range is too long"  "var f : [0 .. 1 -> $E600 .. 1]\ninit f == 0\n$I"
bad "a property name too long"      "property name is too long"   "$V\nproperty $N33: eventually TRUE"
bad "an eventually too long"        "property is too long"        "$V\nproperty p: eventually $E600 == 0"
bad "a leads-to side too long"      "property is too long"        "$V\nproperty p: $E600 == 0 ~> TRUE"
bad "a symmetry set too long"       "symmetry set is too long"    "$V\nsymmetry {$E600}\n$I"
bad "an init too long"              "init is too long"            "var x : 0 .. 1\ninit x == $E600\n$I"
bad "an action name too long"       "action name is too long"     "$V\naction $N33:\n  x' = 1\n$I"
bad "a parameterised action name too long" "action name is too long" "$V\naction $N33(i in 0 .. 1):\n  x' = i\n$I"
bad "an action tag too long"        "action tag is too long"      "$V\naction a [$N33]:\n  x' = 1\n$I"
bad "a parameter name too long"     "parameter name is too long"  "$V\naction a($N33 in 0 .. 1):\n  x' = 1\n$I"
bad "a parameter domain too long"   "parameter domain is too long" "$V\naction a(i in $E600 .. 1):\n  x' = 1\n$I"
bad "a guard too long"              "guard is too long"           "$V\naction a:\n  when $E600 == 0\n  x' = 1\n$I"
bad "an invariant name too long"    "invariant name is too long"  "$V\ninvariant $N33: TRUE"
bad "an assignment target too long" "assignment target is too long" "$V\naction a:\n  $N33' = 1\n$I"
bad "an assignment too long"        "assignment is too long"      "$V\naction a:\n  x' = $E600\n$I"
# what the loader takes: a module named twice keeps its first name, an
# action with its colon left off, an assignment written without spaces,
# and a line that is a comment
printf 'module first\nmodule second\n# a comment\nvar x : 0 .. 1\ninit x == 0\naction a\n  x%s=1\ninvariant i: x <= 1\n' "'" > "$OUT/loose.cml"
expect "a second module line keeps the first name"   0 "spec first, 1 variable, 1 action" --spec "$OUT/loose.cml"
# the limits on declarations: one past each is refused, not dropped
many() {	# many DESC NEEDLE FMT COUNT: COUNT lines of FMT (%d = index) after $V
	# shellcheck disable=SC2059
	{ printf 'module m\n%b\n' "$V"; i=0; while [ $i -lt "$4" ]; do printf "$3\n" "$i"; i=$((i + 1)); done; printf '%b\n' "$I"; } > "$OUT/bad.cml"
	expect "$1" 3 "$2" --spec "$OUT/bad.cml"
}
many "too many consts"     "too many consts"     "const C%d = 1" 17
many "too many vars"       "too many vars"       "var v%d : 0 .. 1" 16
many "too many invariants" "too many invariants" "invariant j%d: TRUE" 16
many "too many properties" "too many properties" "property p%d: eventually TRUE" 17
many "too many actions"    "too many actions"    "action a%d:" 33
{ printf 'module m\n%b\naction a:\n' "$V"; i=0; while [ $i -lt 17 ]; do printf "  v%d' = 1\n" $i; i=$((i + 1)); done; printf '%b\n' "$I"; } > "$OUT/bad.cml"
expect "too many assignments" 3 "too many assignments" --spec "$OUT/bad.cml"
# a liveness goal or premise that fails to evaluate, or is not boolean
bad "an eventually goal that fails"     "liveness: unknown identifier"    "$V\naction a:\n  x' = 1\nproperty p: eventually y == 0"
bad "a leads-to premise that fails"     "liveness: unknown identifier"    "$V\naction a:\n  x' = 1\nproperty p: y == 0 ~> x == 1"
bad "a liveness goal that is an integer" "expected a boolean in a property" "$V\naction a:\n  x' = 1\nproperty p: eventually x"
# --define: more tags, or a longer tag, than the checker holds
printf 'module m\n%b\naction a [T]:\n  x%s = 1\n%b\n' "$V" "'" "$I" > "$OUT/tag.cml"
expect "a --define tag past its buffer" 3 "at most 16 tags of under 32 bytes" --spec "$OUT/tag.cml" --define "$(printf 'T%032d' 0)"
set -- --spec "$OUT/tag.cml"; i=0; while [ $i -lt 17 ]; do set -- "$@" --define T; i=$((i + 1)); done
expectp "more --define tags than it holds" 3 "usage:" "$CMC" "$@"
set --
# extends: resolved beside the extending file, with or without .cml,
# and an absolute path
printf 'module base\nvar x : 0 .. 1\ninit x == 0\n' > "$OUT/base.cml"
printf 'module top\nextends base\ninvariant i: x == 0\n' > "$OUT/top.cml"
expect "extends finds the base beside the file" 0 "1 distinct states found" --spec "$OUT/top.cml" --no-deadlock
printf 'module top\nimport %s\ninvariant i: x == 0\n' "$OUT/base.cml" > "$OUT/top.cml"
expect "import takes an absolute path"         0 "1 distinct states found" --spec "$OUT/top.cml" --no-deadlock
printf 'module top\nextends base.cml\ninvariant i: x == 0\n' > "$OUT/top.cml"
if (cd "$OUT" && "$CMC" --spec top.cml --no-deadlock > "$OUT/rel.txt" 2>&1) && grep -q "1 distinct" "$OUT/rel.txt"; then
	echo "ok   a spec named without a directory extends from ."
else
	echo "FAIL a spec named without a directory extends from ."; fail=1
fi
printf 'module top\nextends %s\ninvariant i: x == 0\n' "$(printf '%01000d' 0)" > "$OUT/top.cml"
expect "an extends path past its buffer"       3 "extends path is too long" --spec "$OUT/top.cml"

# soundness rows from the 1.3.0 audit (docs/plans/soundness-remediation.md,
# Phase 1).  Each asserts the verdict the semantics require; each was a
# false pass before its fix.
echo "-- abstract checker soundness (remediation Phase 1)"
S=$HERE/test/spec/sound
RD=$OUT/refine_driver
# shellcheck disable=SC2086
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L ${CFLAGS:--O1} -I"$HERE/src" -I"$HERE/include" \
	-o "$RD" "$HERE/test/spec/refine_driver.c" "$HERE/src/cmc_spec.c" \
	|| { echo "FAIL refine_driver does not build"; fail=1; }
# 1.1: the first observation must satisfy init
expectp "C2: an observation outside init is rejected" 1 "rejected at step 0" "$RD" "$S/c2init.cml" "$S/scalar.bind" 2
expectp "C2: a behaviour from init is accepted"       0 "accepted 3"         "$RD" "$S/c2init.cml" "$S/scalar.bind" 0 1 2
# 1.2: a [TAG] action that is not --define'd is not a step
expectp "C2: a disabled [BUG] action is not a step"    1 "rejected at step 1" "$RD" "$S/c2gate.cml" "$S/scalar.bind" 0 2
# a constraint narrows TLC's search, not the spec, so refinement against
# a spec that has one is refused rather than checked against a narrower
# model
{ cat "$S/c2init.cml"; printf 'constraint c: x < 2\n'; } > "$OUT/c2con.cml"
expectp "refinement under a constraint is refused" 3 "not supported" "$RD" "$OUT/c2con.cml" "$S/scalar.bind" 0
# a binding is read whole: a line it cannot use is an error, never
# skipped, since a second binding of x that hid the first accepted the
# forbidden step 0 -> 2
binding() {	# binding DESC NEEDLE TEXT: refine_driver on c2init.cml exits 3
	printf '%b' "$3" > "$OUT/b.bind"
	expectp "$1" 3 "$2" "$RD" "$S/c2init.cml" "$OUT/b.bind" 0 2
}
binding "a variable bound twice is an error"   "x is bound twice"     '[map]\nx = "node[0][0]"\nx = "0"\n'
binding "an unknown section is an error"       "unknown section"      '[mapp]\nx = "node[0][0]"\n'
binding "a line without = is an error"         "cannot read"          '[map]\nx = "node[0][0]"\njunk\n'
binding "an index on a scalar is an error"     "x is not a function"  '[map]\nx[i] = "node[0][0]"\n'
binding "an unknown variable is an error"      "no variable y"        '[map]\nx = "node[0][0]"\ny = "0"\n'
binding "a field offset that is no number"     "offset of v"          '[fields]\nv = seven\n[map]\nx = "node[0].v"\n'
binding "a key before any section is an error" "cannot read"          'x = "node[0][0]"\n[map]\nx = "node[0][0]"\n'
binding "a header key other than spec or binary" "cannot read model" 'model = x\n[map]\nx = "node[0][0]"\n'
binding "a field offset past 254"              "from 0 to 254, not '300'" '[fields]\nv = 300\n[map]\nx = "node[0].v"\n'
binding "a field offset with no value"         "from 0 to 254, not ''" '[fields]\nv =\n[map]\nx = "node[0].v"\n'
binding "a line past the reader's buffer"      "a line is too long" "[map]\nx = \"$(printf '%01100d' 0)\"\n"
binding "a binding expression too long"        "binding expression is too long" "[map]\nx = \"$(printf '0 + %0596d' 0)\"\n"
binding "a field name too long"                "field name is too long" "[fields]\n$(printf 'f%032d' 0) = 0\n[map]\nx = \"0\"\n"
binding "a binding variable too long"          "binding variable is too long" "[map]\n$(printf 'x%032d' 0) = \"0\"\n"
{ printf 'module many\n'; i=0; while [ $i -lt 16 ]; do printf 'var v%d : 0 .. 1\n' $i; i=$((i + 1)); done
  printf 'init v0 == 0'; i=1; while [ $i -lt 16 ]; do printf ' and v%d == 0' $i; i=$((i + 1)); done; printf '\ninvariant i: TRUE\n'; } > "$OUT/many.cml"
{ printf '[map]\n'; i=0; while [ $i -lt 16 ]; do printf 'v%d = "0"\n' $i; i=$((i + 1)); done; printf 'x = "0"\n'; } > "$OUT/b.bind"
expectp "too many bindings" 3 "too many bindings" "$RD" "$OUT/many.cml" "$OUT/b.bind" 0
binding "a binding index too long"             "binding index is too long" "[map]\nx[$(printf 'i%032d' 0)] = \"0\"\n"
{ printf '[fields]\n'; i=0; while [ $i -lt 17 ]; do printf 'f%d = %d\n' $i $i; i=$((i + 1)); done; printf '[map]\nx = "0"\n'; } > "$OUT/b.bind"
expectp "too many fields"   3 "too many fields"   "$RD" "$S/c2init.cml" "$OUT/b.bind" 0
# a header, comments, and an unquoted expression are read
printf 'spec = c2init.cml  # the spec\nbinary = x\n\n[map]\nx = node[0][0]\n' > "$OUT/b.bind"
expectp "a header, comments and an unquoted expression" 0 "accepted 3" "$RD" "$S/c2init.cml" "$OUT/b.bind" 0 1 2
# a function variable: bound entry by entry, and refused bound whole
printf 'module f\nvar c : [0 .. 1 -> 0 .. 1]\ninit c == [i in 0 .. 1 -> 0]\naction set(i in 0 .. 1):\n  when c[i] == 0\n  c%s = [c except [i] = 1]\ninvariant i: TRUE\n' "'" > "$OUT/fun.cml"
printf '[map]\nc = "0"\n' > "$OUT/fun.bind"
expectp "a function variable bound without an index"  3 "c is a function" "$RD" "$OUT/fun.cml" "$OUT/fun.bind" 0
printf '[map]\nc[i] = "node[0][0] - i >= 1"\n' > "$OUT/fun.bind"
expectp "C2: a function bound entry by entry steps"   0 "accepted 3" "$RD" "$OUT/fun.cml" "$OUT/fun.bind" 0 1 2
expectp "C2: two entries set at once match no action" 1 "no matching abstract action" "$RD" "$OUT/fun.cml" "$OUT/fun.bind" 0 2
printf '[map]\nc[i] = "{i}"\n' > "$OUT/fun.bind"
expectp "an entry bound to a set is an error"         3 "binding c\[i\]: expected an integer, not a set" "$RD" "$OUT/fun.cml" "$OUT/fun.bind" 0
printf '[map]\nc[i] = "y"\n' > "$OUT/fun.bind"
expectp "an entry that fails to evaluate"             3 "unknown identifier" "$RD" "$OUT/fun.cml" "$OUT/fun.bind" 0
printf 'module fd\nvar c : [0 .. y -> 0 .. 1]\ninit c == 0\ninvariant i: TRUE\n' > "$OUT/fd.cml"
printf '[map]\nc[i] = "0"\n' > "$OUT/fun.bind"
expectp "a function domain that fails to evaluate"    3 "" "$RD" "$OUT/fd.cml" "$OUT/fun.bind" 0
# C2 at the step level: an invariant broken by an observed step, and a
# guard that fails to evaluate while matching the step
printf 'module g\nvar x : 0 .. 2\ninit x == 0\naction up:\n  when x < 2 and y\n  x%s = x + 1\ninvariant i: x <= 1\n' "'" > "$OUT/g.cml"
printf '[map]\nx = "node[0][0]"\n' > "$OUT/b.bind"
printf 'module h\nvar x : 0 .. 2\ninit x == 0\naction up:\n  when x < 2\n  x%s = x + 1\ninvariant low: x <= 1\n' "'" > "$OUT/h.cml"
expectp "C2: an observed step breaks the invariant"   1 "rejected at step 2 (value 2): low" "$RD" "$OUT/h.cml" "$OUT/b.bind" 0 1 2
expectp "C2: a guard that fails while matching a step" 3 "unknown identifier" "$RD" "$OUT/g.cml" "$OUT/b.bind" 0 1
expectp "C1: an invariant that fails to evaluate"     3 "" "$RD" -1 "$OUT/g.cml" "$OUT/b.bind" 3
# C2: the first state must be init, an init that fails is an error, and a
# spec variable left unbound is refused before the first step
printf 'module ib\nvar x : 0 .. 2\ninit x == y\ninvariant i: TRUE\n' > "$OUT/ib.cml"
printf 'module ub\nvar x : 0 .. 2\nvar z : 0 .. 1\ninit x == 0 and z == 0\ninvariant i: TRUE\n' > "$OUT/ub.cml"
expectp "C2: an init that fails to evaluate"   3 "" "$RD" "$OUT/ib.cml" "$OUT/b.bind" 0
expectp "C2: an unbound spec variable"         3 "'z' has no \[map\] entry" "$RD" "$OUT/ub.cml" "$OUT/b.bind" 0
expectp "C2: a stutter is accepted"            0 "accepted 3" "$RD" "$S/c2init.cml" "$OUT/b.bind" 0 0 1
# the observables a binding reads: node[i][k], node[i].field, nodes,
# replies and a reply's .src and .val (refine_driver.c says what each
# reports), each mapped onto a spec whose invariant checks the value
obs() {	# obs DESC WANT NEEDLE EXPR [VALUES...]: x = EXPR, C1 check
	desc=$1 want=$2 needle=$3 expr=$4; shift 4
	printf '[fields]\nlo = 0\n[map]\nx = "%s"\n' "$expr" > "$OUT/o.bind"
	printf 'module o\nvar x : 0 .. 9\ninit x == 0\ninvariant i: x <= 3\n' > "$OUT/o.cml"
	expectp "$desc" "$want" "$needle" "$RD" -1 "$OUT/o.cml" "$OUT/o.bind" "$@"
}
obs "node[0][0] is the observed value"        0 "accepted 3" "node[0][0]" 0 1 3
obs "node[1].lo is one more"                  1 "rejected at step 1" "node[1].lo" 0 3
obs "count over nodes sees both"              1 "rejected at step 0" "4 * (count i in nodes : node[i][0] >= 0)" 0
obs "count over replies sees the one reply"   0 "accepted 2" "count r in replies : r.src == 1 and r.val == 2" 0 2
obs "a reply's value is observed"             1 "rejected at step 1" "4 * (count r in replies : r.val == 2)" 0 2
obs "an unknown field is an error"            3 "unknown field" "node[0].hi" 0
obs "node with no [ is an error"              3 "expected \[ after node" "node 0" 0
obs "node[i] with no index or field"          3 "needs \[k\] or .field" "node[0] + 1" 0
obs "node[] of a set is an error"              3 "expected an integer in node\[\]" "node[{0}][0]" 0
obs "node[0 with no ] is an error"              3 "expected \]" "node[0 [0]" 0
obs "node[0][] of a set is an error"            3 "expected an integer in node\[\]\[\]" "node[0][{0}]" 0
obs "node[0][0 with no ] is an error"           3 "expected \]" "node[0][0 + 1" 0
obs "a skipped node read is not made"           0 "accepted 2" "node[0][0] + (count i in {} : node[i][0] == 0)" 0 1
obs "r outside replies is not a reply"          3 "unknown identifier" "r + 1" 0
obs "a reply field other than src or val"     3 "reply has .src and .val" "count r in replies : r.dst == 0" 1
# an observed state outside the declared domains is refused, even where
# the invariant holds (x : 0 .. 9, invariant x <= 3 fails first, so TRUE)
printf 'module d\nvar x : 0 .. 2\ninit x == 0\ninvariant t: TRUE\n' > "$OUT/dom.cml"
printf '[map]\nx = "node[0][0]"\n' > "$OUT/dom.bind"
expectp "C1: an observed value outside its domain is refused" 3 "outside the spec: x = 7 is outside its domain" "$RD" -1 "$OUT/dom.cml" "$OUT/dom.bind" 0 7
expectp "C2: an observed value outside its domain is refused" 3 "outside the spec" "$RD" "$OUT/dom.cml" "$OUT/dom.bind" 0 7
obs "a set where an integer belongs"          3 "binding x: expected an integer, not a set" "{node[0][0]}" 0
# 1.4: a truncated expression is an error, never a shorter invariant
expect "an over-long expression is a model error"  3 "too long" --spec "$S/truncate.cml"
# 1.5: types, declared domains and integer overflow
expect "a non-set parameter domain is an error"     3 "expected a set" --spec "$S/typepass.cml"
expect "init outside the declared domain is an error" 3 "outside" --spec "$S/boundpass.cml"
expect "a successor outside the domain is an error"  3 "outside" --spec "$S/succbound.cml"
expect "an overflowing literal is an error"         3 "overflow" --spec "$S/overflow.cml"
# 1.6: a primed variable on a right-hand side is refused, not read unprimed
expect "a primed right-hand side is refused"        3 "primed" --spec "$S/primed.cml"
# constructs TLC rejects or reads as FALSE: an init that reads a variable
# before setting it, two assignments to one primed variable, and a bound
# name that redeclares one in scope
expect "init reading an unset variable is an error" 3 "before assigning" --spec "$S/init_order.cml"
expect "a variable assigned twice is an error"      3 "assigned twice" --spec "$S/duplicate.cml"
expect "a redeclared bound name is an error"        3 "redeclares" --spec "$S/quantifier.cml"
# 1.8: a graph with more edges than states (16x16 stuttering bindings per
# state) was a heap overflow in the liveness graph; it now just holds
expect "liveness: an edge-dense graph is checked"   0 "1 property held" --spec "$S/edges.cml"
# 1.7: liveness agrees with TLC on the emitted module (TLC verdicts in
# the comments were measured on the audit host)
expect "liveness: a dead state never reaches the goal" 1 "progress is violated" --spec "$S/dead.cml"     # TLC: violated
expect "liveness: unfair stuttering avoids the goal"   1 "progress is violated" --spec "$S/unfair.cml"   # TLC: violated
expect "liveness: a goal already reached holds"        0 "No error has been found" --spec "$S/once.cml" # TLC: holds
expect "liveness: every bad component is checked"      1 "response is violated" --spec "$S/leads.cml"   # TLC: violated
expect "liveness: reaching B discharges A ~> B"        0 "No error has been found" --spec "$S/leadscross.cml" # TLC: holds
expect "liveness under symmetry is refused"          3 "under symmetry" --spec "$S/symlive.cml"

echo "-- machine-readable counterexamples (--trace-out, --trace-format)"
# trace_row KIND SPEC NACT [--define TAG]: the file parses, holds the
# shortest trace's NACT actions plus the initial state, and its actions
# and final state are the ones cmc printed (test/trace_check.py).  With
# no python3 the fallback counts the states only.
trace_row() {
	kind=$1 spec=$2 nact=$3; shift 3
	f="$OUT/trace.$kind"; : > "$f"
	"$CMC" --spec "$spec" "$@" --trace-out "$f" --trace-format "$kind" > "$OUT/trace.txt" 2>&1
	rc=$?
	if [ $rc != 1 ] || ! grep -q "is ($nact actions)" "$OUT/trace.txt"; then
		echo "FAIL trace $kind $(basename "$spec"): exit $rc"; sed 's/^/     /' "$OUT/trace.txt" | head; fail=1; return
	fi
	if command -v python3 > /dev/null 2>&1; then
		python3 "$HERE/test/trace_check.py" "$kind" "$OUT/trace.txt" "$f" > "$OUT/tc.txt" 2>&1
	else
		[ "$(grep -o '"index":' "$f" | wc -l)" = $((nact + 1)) ] > "$OUT/tc.txt" 2>&1
	fi
	if [ $? = 0 ]; then echo "ok   trace $kind $(basename "$spec"): $((nact + 1)) states, as printed"
	else echo "FAIL trace $kind $(basename "$spec")"; sed 's/^/     /' "$OUT/tc.txt"; fail=1; fi
}
trace_row itf  "$HERE/test/spec/mutex.cml" 4 --define BUG
trace_row json "$HERE/test/spec/mutex.cml" 4 --define BUG
trace_row itf  "$HERE/examples/spec/paxos.cml" 12 --define BUG
trace_row json "$HERE/examples/spec/paxos.cml" 12 --define BUG
expect "trace: ITF is the default format" 1 '(itf, 5 states)' --spec "$HERE/test/spec/mutex.cml" --define BUG --trace-out "$OUT/t.itf"
expect "trace: a format other than itf or json is an error" 3 "the formats are itf and json" --spec "$HERE/test/spec/mutex.cml" --trace-out "$OUT/t.x" --trace-format tla
expect "trace: --trace-format without --trace-out is an error" 3 "needs --trace-out" --spec "$HERE/test/spec/mutex.cml" --trace-format json
expect "trace: an unwritable file is an error" 3 "could not write" --spec "$HERE/test/spec/mutex.cml" --define BUG --trace-out "$OUT/no/such/dir/t.itf"
expect "trace: a failed write is an error"     3 "could not write" --spec "$HERE/test/spec/mutex.cml" --define BUG --trace-out /dev/full
expect "trace: a liveness violation writes no file" 1 "No trace written" --spec "$HERE/test/spec/sound/dead.cml" --trace-out "$OUT/live.itf"
expect "trace: --emit-tla takes no --trace-out" 3 "not supported with --emit-tla" --emit-tla "$HERE/test/spec/mutex.cml" --trace-out "$OUT/t.itf"
rm -f "$OUT/pass.itf"
expect "trace: a pass writes no file" 0 "No error has been found" --spec "$HERE/test/spec/mutex.cml" --trace-out "$OUT/pass.itf"
[ -e "$OUT/pass.itf" ] && { echo "FAIL trace: a pass wrote $OUT/pass.itf"; fail=1; }
# the checker has teeth: a trace that lost its last state fails it
if command -v python3 > /dev/null 2>&1; then
	"$CMC" --spec "$HERE/test/spec/mutex.cml" --define BUG --trace-out "$OUT/t.json" --trace-format json > "$OUT/trace.txt" 2>&1
	python3 -c 'import json,sys; t=json.load(open(sys.argv[1])); json.dump(t[:-1],open(sys.argv[1],"w"))' "$OUT/t.json"
	if python3 "$HERE/test/trace_check.py" json "$OUT/trace.txt" "$OUT/t.json" > /dev/null 2>&1; then
		echo "FAIL trace_check passed a trace with a state missing"; fail=1
	else echo "ok   trace_check fails a trace with a state missing"; fi
fi

# the Apalache gate's verdict classifier: a tool error (parse, typing,
# input) must never count as an expected violation (audit 0.2)
if bash "$HERE/test/apalache_gate.sh" --self-test > "$OUT/apa-selftest.txt" 2>&1; then
	echo "ok   apalache gate classifies tool errors as errors"
else
	echo "FAIL apalache gate classifier"; sed 's/^/     /' "$OUT/apa-selftest.txt"; fail=1
fi

[ $fail = 0 ] && echo "spec_gate: ok (abstract-model checker: verdicts, counts and depths match TLC)" || echo "spec_gate: FAILED"
exit $fail
