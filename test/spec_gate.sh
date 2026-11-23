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
	# a run that does not end is a failure, never a hung gate (124)
	timeout 600 "$@" > "$OUT/out.txt" 2>&1
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

# Log replication (Raft's AppendEntries core) over records, bounded
# sequences and defs.  Counts, depth and the 5-action trace match
# single-worker TLC on the emitted module and on the hand-written
# logrep.tla; the bug drops the follower's index check.
expect "logrep: durable holds"                0 "4439 distinct states found" --spec "$HERE/examples/spec/logrep.cml"
expect "logrep: depth matches TLC (16)"       0 "complete state graph search is 16" --spec "$HERE/examples/spec/logrep.cml"
expect "logrep: no index check loses a commit" 1 "durable is violated" --spec "$HERE/examples/spec/logrep.cml" --define BUG
expect "logrep: shortest trace is 5 actions"  1 "is (5 actions)" --spec "$HERE/examples/spec/logrep.cml" --define BUG
expect "logrep: the trace shows sequences in functions" 1 "log=\[0:<<0, 1>>,1:<<1>>,2:<<>>\]" --spec "$HERE/examples/spec/logrep.cml" --define BUG
# a record field, a sequence element or a length that differs makes a
# different state (TLC: the same 144 states, depth 7)
expect "states differing in one field or element are distinct" 0 "144 distinct states found" --spec "$HERE/test/spec/distinct.cml"

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
# over a record and a sequence (emit_gate checks these against TLC)
expect "deadlock: over a record and a sequence"      1 "is (6 actions)" --spec "$T/constrain_rec.cml"
expect "deadlock: off, over a record and a sequence" 0 "26 distinct states found" --spec "$T/constrain_rec.cml" --no-deadlock
expect "constraint: on Len(s) and a record field"    0 "10 distinct states found" --spec "$T/constrain_rec_c.cml" --no-deadlock
expect "constraint: on Len(s), with flip"            0 "36 distinct states found" --spec "$T/constrain_rec_c.cml" --define FLIP
expect "action constraint: on s' and r'.k"           0 "10 distinct states found" --spec "$T/constrain_rec_ac.cml" --no-deadlock
expect "action constraint: on s' and r'.k, with flip" 0 "52 distinct states found" --spec "$T/constrain_rec_ac.cml" --define FLIP
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

echo "-- fairness per action, strong fairness, action properties, coverage"
# test/spec/fair/: each verdict is TLC's on the emitted module
# (emit_gate.sh); toggle and inner tell weak from strong fairness
FD=$HERE/test/spec/fair
expect "toggle: no fairness, a run may stop"         1 "finishes is violated" --spec "$FD/toggle.cml"
expect "toggle: WF on both, flip for ever is fair"   1 "finishes is violated" --spec "$FD/toggle_wf.cml"
expect "toggle: SF on finish alone, a stutter at 0"  1 "finishes is violated" --spec "$FD/toggle_sfonly.cml"
expect "toggle: WF flip and SF finish, it finishes"  0 "1 property held" --spec "$FD/toggle_sf.cml"
expect "toggle: fairness strong, it finishes"        0 "1 property held" --spec "$FD/toggle_strong.cml"
expect "inner: WF, the cycle 0 <-> 1 is fair"        1 "finishes is violated" --spec "$FD/inner_wf.cml"
expect "inner: SF finish, 0 <-> 1 never enables it"  1 "finishes is violated" --spec "$FD/inner_sf.cml"
expect "inner: SF up and finish, it finishes"        0 "1 property held" --spec "$FD/inner_sf2.cml"
expect "param: WF on go(i) is fairness of the disjunction" 1 "finishes is violated" --spec "$FD/param.cml"
expect "mono: two action properties hold"            0 "2 properties held" --spec "$FD/mono.cml"
expect "mono: x' >= x fails on the wrap"             1 "Action property bad is violated" --spec "$FD/mono_bad.cml"
expect "mono: the shortest trace ends in the bad step" 1 "is (4 actions)" --spec "$FD/mono_bad.cml"
expect "mono: a step out of the model is checked"    1 "Action property bad is violated" --spec "$FD/mono_c.cml"
expect "emit-tla: WF and SF per action"              0 "Spec == Init /. \[\]\[Next\]_vars /. WF_vars(flip) /. SF_vars(finish)$" --emit-tla "$FD/toggle_sf.cml"
expect "emit-tla: fairness strong"                   0 "SF_vars(flip) /. SF_vars(finish)$" --emit-tla "$FD/toggle_strong.cml"
expect "emit-tla: an action property"                0 "^  \[\]\[(x' >= x)\]_vars$" --emit-tla "$FD/mono_bad.cml"
expect "coverage: distinct:generated per action (TLC's 3:6)" 0 "^<incx>: 3:6  (enabled in 6 states)" --spec "$HERE/test/spec/constrain_ac.cml" --coverage
expect "coverage: an action seen only as a stutter"  0 "^<stay>: 0:1  (enabled in 1 states)" --spec "$FD/mono.cml" --coverage
expect "coverage: a never-enabled action is a warning" 1 "Warning: action stay is never enabled" --spec "$FD/mono_bad.cml" --coverage
expect "coverage: is only for --spec"                3 "--coverage is not supported with --emit-tla" --emit-tla "$FD/mono.cml" --coverage
printf 'module fx\nvar x : 0 .. 1\ninit x == 0\naction a:\n  x%s = 1 - x\nfair weak b\nproperty p: eventually x == 1\n' "'" > "$OUT/fx.cml"
expect "a fair line naming no action is an error"    3 "fair b names no action" --spec "$OUT/fx.cml"
# a weaker fairness line after a stronger one leaves it strong
printf 'module fs\nvar x : 0 .. 1\nvar d : 0 .. 1\ninit x == 0 and d == 0\naction flip:\n  when d == 0\n  x%s = 1 - x\naction finish:\n  when x == 1 and d == 0\n  d%s = 1\ndeadlock = off\nfairness strong\nfairness weak\nproperty p: eventually d == 1\n' "'" "'" > "$OUT/fs.cml"
expect "fairness weak after fairness strong stays strong" 0 "1 property held" --spec "$OUT/fs.cml"
expect "coverage: an action --define leaves out is not listed" 0 "^<tick>: 3:3  (enabled in 3 states)" --spec "$HERE/examples/spec/ticker.cml" --coverage
expect "coverage: no report on a model error"        3 "outside" --spec "$HERE/test/spec/sound/succbound.cml" --coverage
expect "mono: an unwritable trace file is an error"  3 "could not write" --spec "$FD/mono_bad.cml" --trace-out /dev/full

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
# records, sequences and defs reach TLA+ in their own forms (emit_gate
# runs TLC on logrep, distinct and symrec)
lr() { expect "emit-tla: $1" 0 "$2" --emit-tla "$HERE/examples/spec/logrep.cml"; }
lr "a function of sequences is typed"     "@type: Int -> Seq(Int);"
lr "a sequence of records is typed"       "@type: Int -> Seq({ idx: Int, val: Int });"
lr "the module extends Sequences"         "EXTENDS Naturals, FiniteSets, Sequences"
lr "a def with parameters"                "^Prefix(p, q) ==\$"
lr "an except path gets its !"            "EXCEPT !\[f\] = Append(@, \[idx |-> i, val |-> log\[0\]\[i\]\])\]"
expect "emit-tla: a def without parameters" 0 "^Server ==\$" --emit-tla "$HERE/examples/spec/raft.cml"
expect "emit-tla: a function of records is typed" 0 "@type: Int -> { used: Int, votes: Int };" --emit-tla "$HERE/examples/spec/raft.cml"
expect "emit-tla: two paths in one except" 0 "EXCEPT !\[s\].used = 1, !\[c\].votes = @ + 1\]" --emit-tla "$HERE/examples/spec/raft.cml"
printf 'module nest\nvar f : [0 .. 1 -> [0 .. 1 -> 0 .. 1]]\ninit f == [i in 0 .. 1 -> [j in 0 .. 1 -> 0]]\ninvariant ok: f[1][0] == 0\n' > "$OUT/nest.cml"
expect "nest: a function of functions" 0 "1 distinct states found" --spec "$OUT/nest.cml" --no-deadlock
expect "emit-tla: a nested function is typed" 0 "@type: Int -> (Int -> Int);" --emit-tla "$OUT/nest.cml"
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
typed "a function of booleans"            "compares a boolean with an integer" "[i in 0 .. 1 -> TRUE] == f"
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
typed "a function value that is a set" "compares a set with an integer" "[f except [0] = {1}] == f"
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
typed "a literal body of a set"     "compares a set with an integer" "[i in 0 .. 1 -> {i}] == f"
# each malformed form is refused with its reason
typed "an unclosed (" "expected )" "(x == 0"
typed "an unclosed [i in" "expected ]" "f == [i in 0 .. 1 -> 0"
typed "a literal with no ->" "expected -> in function literal" "[i in 0 .. 1 : 0] == f"
typed "[..] with neither in nor except" "expected 'in', '|->' or 'except'" "[f with [0] = 1] == f"
typed "except on a constant" "except on a value that is not a function" "[1 except [0] = 1] == f"
typed "except on a scalar" "except on a value that is not a function" "[x except [0] = 1] == f"
typed "except with no [" "expected \\[ or . in except" "[f except 0 = 1] == f"
typed "except with no ]" "expected ]" "[f except [0 = 1] == f"
typed "except with no =" "expected = in except" "[f except [0] 1] == f"
typed "except with no closing ]" "expected ] to close except" "f == [f except [0] = 1"
typed "an unclosed {" "expected }" "{0} == {0, 1"
typed "a set past its limit" "set too large" "{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24} == {0}"
typed "a range past its limit" "range too large" "x in 0 .. 24"
typed "a literal past its limit" "function too large" "[i in 0 .. 12 -> 0] == f"
typed "an except outside the domain" "except outside the domain" "[f except [2] = 0, [3] = 0, [4] = 0, [5] = 0, [6] = 0, [7] = 0, [8] = 0, [9] = 0, [10] = 0, [11] = 0, [12] = 0] == f"
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
typed "nested bound names past the limit" "too many nested bound names" "all a in 0 .. 1 : all b in 0 .. 1 : all c in 0 .. 1 : all d in 0 .. 1 : all e in 0 .. 1 : all g in 0 .. 1 : all h in 0 .. 1 : all k in 0 .. 1 : all l in 0 .. 1 : TRUE"
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
value() {	# value DESC WANT EXPR: invariant EXPR holds (0), is violated (1) or is an error (3)
	printf 'module m\nvar x : 0 .. 1\nvar f : [0 .. 1 -> 0 .. 1]\nvar g : [0 .. 1 -> 0 .. 1]\ninit x == 0 and f == [i in 0 .. 1 -> i] and g == [i in 0 .. 1 -> 0]\ninvariant i: %s\n' "$3" > "$OUT/value.cml"
	if [ "$2" = 0 ]; then expect "$1" 0 "No error has been found" --spec "$OUT/value.cml" --no-deadlock
	elif [ "$2" = 3 ]; then expect "$1" 3 "evaluation error" --spec "$OUT/value.cml" --no-deadlock
	else expect "$1" 1 "Invariant i is violated" --spec "$OUT/value.cml" --no-deadlock; fi
}
value "division truncates"          0 "7 / 2 == 3 and 7 mod 2 == 1 and 2 * 3 - 1 == 5"
value "sets compare by members"     0 "{1, 0, 1} == {0, 1} and {0} != {1} and {} == {}"
value "a set and a longer set"      1 "{0} == {0, 1}"
value "functions compare by entries" 0 "f != g and f == [i in 0 .. 1 -> i] and [g except [1] = 1] == f"
value "a function and a shorter one" 1 "f == [i in 0 .. 0 -> 0]"
# TLC leaves the value unchanged (with a warning); cmc refuses it
value "an except outside the domain is an error" 3 "[f except [5] = 0] == f"
value "booleans compare"            0 "TRUE != FALSE and (x == 0) == TRUE"
value "quantifiers and count"       0 "(some i in 0 .. 1 : f[i] == 1) and not (all i in 0 .. 1 : f[i] == 1) and (count i in 0 .. 1 : f[i] == 0) == 1"
value "orders"                      0 "0 < 1 and 1 > 0 and 1 <= 1 and 1 >= 1 and not (1 < 1)"
value "membership"                  0 "x in {0} and not (x in {}) and 1 in 0 .. 1"
value "functions of one length differ in a value" 1 "f == [i in 0 .. 1 -> 0]"
value "functions of one length differ in a key"   1 "[i in 0 .. 1 -> i] == [i in 1 .. 2 -> i - 1]"
# the right operand of a decided and/or is not evaluated (cmc 2.1
# evaluated it and refused each of these)
value "or skips a division by zero"  0 "x == 0 or 1 / x == 1"
value "and skips an application outside the domain" 0 "(x > 0 and f[2] == 0) or x == 0"
value "or skips a non-boolean"       0 "TRUE or f"
value "and skips a quantifier" 0 "not (FALSE and (all i in 0 .. 1 : 1 / i == 0))"
value "a quantifier over no element evaluates no body" 0 "all i in 1 .. x : 1 / (i - 1) == 0"
value "and skips an overflow"        0 "(x == 1 and 9223372036854775807 + 1 == 0) or TRUE"
value "or skips a set past its limit" 0 "TRUE or x in 0 .. 99"
value "and skips a function literal" 0 "not (FALSE and [i in 0 .. 1 -> 1 / i] == f)"
value "and skips an except outside the domain" 0 "(x == 1 and [x except [0] = 1] == f) or TRUE"
value "an evaluated operand still errs" 1 "x == 0 and f[0] == 1"
# a skipped operand is parsed, not evaluated: no error from its values
value "and skips a literal over a set" 0 "not (FALSE and [i in {0, 1} -> 0] == f)"
value "and skips membership of a scalar" 0 "not (FALSE and x in 0)"
value "and skips a mixed comparison" 0 "(FALSE and {0} == 1) or TRUE"
value "and skips each arithmetic error" 0 "(FALSE and 9223372036854775807 * 2 + 1 / 0 + (0 - 1) / 1 + 1 / (0 - 2) == 0) or TRUE"
value "and skips each mod error" 0 "(FALSE and (1 mod 0) + ((0 - 1) mod 2) + (1 mod (0 - 2)) == 0) or TRUE"
value "and skips an overflow in -" 0 "(FALSE and (0 - 9223372036854775807) - 2 == 0) or TRUE"
value "and skips a two-key except" 0 "(FALSE and [f except [0] = 1, [1] = 1] == f) or TRUE"
# the same operators on the right types still evaluate
printf 'module m\nvar x : 0 .. 1\nvar f : [0 .. 1 -> 0 .. 1]\ninit x == 0 and f == [i in 0 .. 1 -> 0]\naction a:\n  when not (x == 1) and (TRUE or FALSE) and x in {0, 1}\n  x%s = 1\n  f%s = [f except [x] = 1]\ninvariant i: (count i in 0 .. 1 : f[i] == 1) + x * 2 <= 3 and {x} != {2} and f != [i in 0 .. 1 -> 1] and (TRUE == TRUE)\n' "'" "'" > "$OUT/typed-ok.cml"
expect "well-typed operators still evaluate" 0 "2 distinct states found" --spec "$OUT/typed-ok.cml" --no-deadlock

# records, sequences and defs: each value pinned by the verdict it
# gives, each misuse refused with its reason (TLC reports each as an
# error too)
echo "-- records, bounded sequences and def operators"
# rs DESC WANT EXPR: invariant EXPR over a record r, a sequence q and a
# function of records g holds (0), is violated (1) or is an error (3)
rs() {
	printf 'module m\ndef two = 2\ndef inc(a) = a + 1\ndef pick(c, a, b) = (c and a) or (not c and b)\nvar r : [a : 0 .. 3, b : 0 .. 1]\nvar q : Seq(0 .. 3, 3)\nvar g : [0 .. 1 -> [c : 0 .. 2, d : Seq(0 .. 1, 2)]]\ninit r == [b |-> 0, a |-> 1] and q == <<1, 2>> and g == [i in 0 .. 1 -> [c |-> i, d |-> <<i>>]]\ninvariant i: %s\n' "$3" > "$OUT/rs.cml"
	case $2 in
	0) expect "$1" 0 "No error has been found" --spec "$OUT/rs.cml" --no-deadlock ;;
	1) expect "$1" 1 "Invariant i is violated" --spec "$OUT/rs.cml" --no-deadlock ;;
	*) expect "$1" 3 "$2" --spec "$OUT/rs.cml" --no-deadlock ;;
	esac
}
rs "a record compares by its fields"   0 "r == [a |-> 1, b |-> 0] and r != [a |-> 1, b |-> 1] and r.a == 1"
rs "records of other fields differ"    1 "r == [a |-> 1]"
rs "a record except, with @"           0 "[r except !.a = @ + 2, .b = 1] == [a |-> 3, b |-> 1]"
rs "a nested except path"              0 "[g except ![1].d = Append(@, 0), [0].c = 2][1].d == <<1, 0>> and g[0].c == 0"
rs "a function of records"             0 "g[1] == [c |-> 1, d |-> <<1>>] and g[0].d[1] == 0"
rs "sequence operators"                0 "Len(q) == 2 and Head(q) == 1 and Tail(q) == <<2>> and Append(q, 3) == <<1, 2, 3>> and q[2] == 2"
rs "SubSeq and concatenation"          0 "SubSeq(q, 2, 2) == <<2>> and SubSeq(q, 2, 1) == <<>> and q \\o <<3>> == <<1, 2, 3>> and <<>> \\o q == q"
rs "sequences of other lengths differ" 1 "q == <<1, 2, 0>>"
rs "a tuple is a sequence"             0 "<<1, TRUE>>[2] and Len(<<r, q>>) == 2 and <<r, q>>[1].a == 1"
rs "a sequence except"                 0 "[q except ![2] = 3] == <<1, 3>>"
rs "def operators"                     0 "inc(two) == 3 and pick(r.a == 1, TRUE, 1 / 0 == 0)"
rs "a def argument is evaluated where it is used" 0 "pick(FALSE, 1 / 0 == 0, TRUE)"
rs "a field of a non-record"           "expected a record in a field access" "q.a == 0"
rs "a missing field"                   "a record has no such field" "r.c == 0"
rs "a record literal with a field twice" "a record field is given twice" "[a |-> 1, a |-> 2] == r"
rs "an except of a missing field"      "except outside the domain" "[r except !.c = 1] == r"
rs "an except past a sequence's end"   "except outside the domain" "[q except ![3] = 1] == q"
rs "an except through a scalar"        "except on a value that is not" "[r except !.a.b = 1] == r"
rs "@ outside an except"               "@ outside an except value" "@ == 1"
rs "== of a record and a function"     "compares a record with a function" "r == g"
rs "== of records with fields of other types" "compares an integer with a boolean" "[a |-> 1] == [a |-> TRUE]"
rs "Head of the empty sequence"        "Head of the empty sequence" "Head(<<>>) == 0"
rs "Tail of the empty sequence"        "Tail of the empty sequence" "Tail(<<>>) == <<>>"
rs "an index past the end"             "a sequence index outside 1 .. Len" "q[3] == 0"
rs "an index of zero"                  "a sequence index outside 1 .. Len" "q[0] == 0"
rs "SubSeq past the end"               "SubSeq outside the sequence" "SubSeq(q, 1, 3) == q"
rs "Len of a set"                      "expected a sequence in Len" "Len({1}) == 0"
rs "\\o of a record"                    "expected a sequence in .o" "q \\o r == q"
rs "Append with one argument"          "expected , between arguments" "Append(q) == q"
rs "a def with too few arguments"      "expected , between arguments" "pick(TRUE, 1) == 1"
rs "a def with no arguments given"     "expected ( after an operator" "inc == 1"
rs "a sequence literal with no >>"     "expected >> to close" "q == <<1, 2"
rs "a sequence past the limit"         "sequence too large" "Len(<<0,0,0,0,0,0,0,0,0,0,0,0,0>>) == 13"
rs "a record past its literal"         "expected |-> in record" "[a |-> 1, b] == r"
# symmetry maps the ids inside records and sequences (and only there,
# not field names or positions); without it the states are TLC's 357
expect "symmetry reaches into records and sequences" 0 "179 distinct states found" --spec "$HERE/test/spec/symrec.cml"
sed '/^symmetry/d' "$HERE/test/spec/symrec.cml" > "$OUT/symrec_off.cml"
expect "the same model without symmetry: TLC's 357 states" 0 "357 distinct states found" --spec "$OUT/symrec_off.cml"
# a model that is not symmetric under its symmetry set is refused where
# a state shows it: a value of the set used as a counter (x = 2 folded
# onto x = 1 and passed), or one held where its image leaves the domain
printf 'module sb\nsymmetry 1 .. 2\nvar x : 0 .. 2\ninit x == 0\naction a:\n  when x < 2\n  x%s = x + 1\ninvariant not2: x != 2\n' "'" > "$OUT/symbreak.cml"
expect "symmetry: an invariant that tells the values apart" 3 "not symmetric under 1 .. 2" --spec "$OUT/symbreak.cml" --no-deadlock
printf 'module sd\nsymmetry 1 .. 2\nvar x : 0 .. 1\ninit x == 0\naction a:\n  x%s = 1\ninvariant ok: TRUE\n' "'" > "$OUT/symdom.cml"
expect "symmetry: a value whose image leaves the domain" 3 "not symmetric under 1 .. 2" --spec "$OUT/symdom.cml" --no-deadlock
sed 's/n : 5 .. 6/n : 0 .. 1/; s/0 .. 6\]/0 .. 1]/; s/k in 5 .. 6/k in 0 .. 1/' "$HERE/test/spec/symrec.cml" > "$OUT/symrec_old.cml"
expect "symmetry: symrec with its numbers in the set" 3 "not symmetric under 1 .. N" --spec "$OUT/symrec_old.cml"
# the encoding over random nested values: bytes equal exactly when the
# values are, enc_eq agrees, and a permutation twice is the identity
EF=$OUT/encfuzz
# shellcheck disable=SC2086
if ${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L ${CFLAGS:--O1} -w -I"$HERE/src" -I"$HERE/include" -o "$EF" "$HERE/test/spec/encfuzz.c"; then
	expectp "the value encoding holds over random nested values" 0 "encfuzz: ok" "$EF"
else echo "FAIL encfuzz.c does not build"; fail=1; fi
rs "an except path past its depth"     "an except path is too deep" "[g except ![0].d[1].c.e = 1] == g"
rs "an except path with no field name" "expected a field name after ." "[r except !. = 1] == r"
rs "an except path with no selector"   "expected \\[ or . in except" "[r except ! = 1] == r"
rs "an except with a skipped @ body"   0 "(FALSE and [r except !.a = @ / 0] == r) or TRUE"
rs "a record literal with no field name" "expected a field name in record" "[a |-> 1, ] == r"
rs "a record literal with no ]"        "expected \] to close record" "r == [a |-> 1, b |-> 0"
rs "a sequence operator with no ("     "expected ( after a sequence" "Len == 0"
rs "a sequence operator with no )"     "expected ) after the arguments" "0 == Len(q"
rs "a def call with no )"              "expected ) after the arguments" "inc(1 == 2"
rs "a def call with a broken argument" "expected )" "inc((1) == 2"
rs "an Append past the sequence limit" "sequence too large" "Len(Append(<<0,0,0,0,0,0,0,0,0,0,0,0>>, 0)) == 13"
rs "a concatenation past the limit"    "sequence too large" "Len(<<0,0,0,0,0,0>> \\o <<0,0,0,0,0,0,0>>) == 13"
rs "SubSeq bounds of a set"            "expected an integer in SubSeq" "SubSeq(q, {1}, 1) == q"
rs "a skipped sequence operator"       0 "(FALSE and Head(<<>>) == Tail(<<>>) and SubSeq(q, 0, 9) == Append(q, 0) and Len(r) == 0) or TRUE"
rs "a skipped field access and index"  0 "(FALSE and q.a == g[5].c and r[0] == 0 and <<1>> == <<>>) or TRUE"
rs "a skipped record literal"          0 "(FALSE and [a |-> 1 / 0] == r) or TRUE"
rs "a skipped sequence literal"        0 "(FALSE and <<1 / 0, 2>> == q) or TRUE"
rs "a skipped concatenation"           0 "(FALSE and (q \\o r) == q) or TRUE"
rs "an application of a record"        "applied a non-function" "r[1] == 0"
rs "== of records of other fields"     1 "[a |-> 1] == [b |-> 1]"
rs "a range is a set operand"          0 "(0 .. 2) == {0, 1, 2} and 1 in 0 .. 2"
rs "a value past the evaluation arena"  "values too large for the evaluation arena" "[a1 in 0 .. 11 -> [a2 in 0 .. 11 -> [a3 in 0 .. 11 -> [a4 in 0 .. 11 -> <<r, r, r, r, r, r, r, r, r, r, r, r>>]]]] == g"
rs "a seq of booleans compares"        0 "<<TRUE, FALSE>> != <<FALSE, FALSE>> and <<{1}>> == <<{1}>>"
rs "a record of too many field names"  "too many record field names" "[f1 |-> 0, f2 |-> 0, f3 |-> 0, f4 |-> 0, f5 |-> 0, f6 |-> 0, f7 |-> 0, f8 |-> 0, f9 |-> 0, f10 |-> 0, f11 |-> 0, f12 |-> 0, f13 |-> 0, f14 |-> 0, f15 |-> 0, f16 |-> 0, f17 |-> 0, f18 |-> 0, f19 |-> 0, f20 |-> 0, f21 |-> 0, f22 |-> 0, f23 |-> 0, f24 |-> 0, f25 |-> 0, f26 |-> 0, f27 |-> 0, f28 |-> 0, f29 |-> 0, f30 |-> 0, f31 |-> 0] == r"
# a stored value outside its declared type is a model error, never cut
# or explored: a sequence past its bound, a record with a missing or
# extra field, a field outside its range
b2() { printf 'module m\n%b\n' "$2" > "$OUT/b2.cml"; expect "$1" 3 "$3" --spec "$OUT/b2.cml"; }
b2 "an append past the bound is an error" "var q : Seq(0 .. 1, 2)\ninit q == <<>>\naction a:\n  q' = Append(q, 0)\ninvariant i: TRUE" "q has length 3, past its bound 2"
b2 "a state past its word limit is an error" "var a : [0 .. 11 -> Seq(0 .. 1, 12)]\nvar b : [0 .. 11 -> Seq(0 .. 1, 12)]\nvar c : [0 .. 11 -> Seq(0 .. 1, 12)]\nvar d : [0 .. 11 -> Seq(0 .. 1, 12)]\nvar e : [0 .. 11 -> Seq(0 .. 1, 12)]\nvar g : [0 .. 11 -> Seq(0 .. 1, 12)]\ninit a == [i in 0 .. 11 -> <<0,0,0,0,0,0,0,0,0,0,0,0>>] and b == a and c == a and d == a and e == a and g == a\ninvariant i: TRUE" "a state needs more than 1024 words"
b2 "a Seq type without )"             "var q : Seq(0 .. 1, 2\ninit q == <<>>\ninvariant i: TRUE" "Seq type needs )"
b2 "a function var with no range"     "var f : [0 .. 1 -> ]\ninit f == 0\ninvariant i: TRUE" "function var needs a range"
b2 "a def with () and no parameter"   "def d() = 1\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "a def with () needs a parameter"
b2 "a def with too many parameters"   "def d(a, b, c, e, g) = 1\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "too many def parameters"
b2 "a def with text after the body"   "def d = 1 )\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def d: trailing text"
b2 "a def named like a const"         "const N = 1\ndef N = 2\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def N redeclares"
b2 "a def with text after its )"      "def d(a) 1 = a\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def parameters need )"
printf 'module m\ndef d == 1\nvar x : 0 .. 1\ninit x == d\ninvariant i: x == 2\n' > "$OUT/b2.cml"
expect "a def written with ==, as TLA+ does" 1 "Invariant i is violated" --spec "$OUT/b2.cml"
b2 "a sequence element outside its type" "var q : Seq(0 .. 1, 2)\ninit q == <<2>>\ninvariant i: TRUE" "q\\[1\\] = 2 is outside its range 0 .. 1"
b2 "a record missing a field"        "var r : [a : 0 .. 1, b : 0 .. 1]\ninit r == [a |-> 0]\ninvariant i: TRUE" "r does not have exactly the fields"
b2 "a record field outside its range" "var r : [a : 0 .. 1]\ninit r == [a |-> 2]\ninvariant i: TRUE" "r.a = 2 is outside its range 0 .. 1"
b2 "a sequence where a record goes"  "var r : [a : 0 .. 1]\ninit r == <<0>>\ninvariant i: TRUE" "r is a sequence, not a record"
b2 "a recursive def"                 "def loop(n) = loop(n)\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def loop: a def may only use the defs declared before it"
b2 "a def using a later def"         "def a1 = b1\ndef b1 = 1\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def a1: a def may only use"
b2 "a def with a broken body"        "def d = (1\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def d: expected )"
b2 "a def that redeclares a var"     "var x : 0 .. 1\ndef x = 1\ninit x == 0\ninvariant i: TRUE" "def x redeclares"
b2 "a def without ="                 "def d\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def needs = EXPR"
b2 "a def without )"                 "def d(a = a\nvar x : 0 .. 1\ninit x == 0\ninvariant i: TRUE" "def parameters need )"
b2 "a Seq type without a bound"      "var q : Seq(0 .. 1)\ninit q == <<>>\ninvariant i: TRUE" "needs a maximum length"
b2 "a Seq bound past the limit"      "var q : Seq(0 .. 1, 13)\ninit q == <<>>\ninvariant i: TRUE" "a Seq length bound must be"
b2 "a record type field without :"   "var r : [a : 0 .. 1, b]\ninit r == [a |-> 0]\ninvariant i: TRUE" "a record field needs name : type"
b2 "a record type field twice"       "var r : [a : 0 .. 1, a : 0 .. 1]\ninit r == [a |-> 0]\ninvariant i: TRUE" "record field a is declared twice"
b2 "a record var without ]"          "var r : [a : 0 .. 1\ninit r == [a |-> 0]\ninvariant i: TRUE" "record var needs \\]"
b2 "a bound name that shadows a def" "def d = 1\nvar x : 0 .. 1\ninit x == 0\ninvariant i: all d in 0 .. 1 : TRUE" "redeclares"
printf 'module m\ndef z = 0\nvar x : 0 .. 1\ninit x == z\ninvariant i: x == z\n' > "$OUT/b2.cml"
expect "a def call inside init" 0 "1 distinct states found" --spec "$OUT/b2.cml" --no-deadlock

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
# the mixes of operators TLA+ refuses, or reads otherwise, are refused:
# `x mod 3 * 2` was (x mod 3) * 2 here and x % 6 to TLC
bad "mod beside *"                  "needs parentheses, as in TLA+" "$V\ninvariant i: x mod 3 * 2 != 4"
bad "* beside /"                    "needs parentheses, as in TLA+" "$V\ninvariant i: x * 2 / 2 == x"
bad "/ after /"                     "needs parentheses, as in TLA+" "$V\ninvariant i: x / 2 / 1 == 0"
bad "mod after +"                   "needs parentheses, as in TLA+" "$V\ninvariant i: 1 + x mod 4 >= 1"
bad "mod before -"                  "needs parentheses, as in TLA+" "$V\ninvariant i: x mod 4 - 1 <= 1"
bad "and beside or"                 "needs parentheses, as in TLA+" "$V\ninvariant i: x == 1 or x == 0 and TRUE"
bad "or beside and"                 "needs parentheses, as in TLA+" "$V\ninvariant i: x == 0 and TRUE or x == 1"
expect "prec: parenthesised mixes, read as TLA+ reads them" 0 "3 distinct states found" --spec "$HERE/test/spec/prec.cml"
# names that end in an operator word stay whole, and a count inside an
# application closes at its bracket (emit_gate runs SANY and TLC on it)
expect "names: votedFor, small, origin and min"   0 "9 distinct states found" --spec "$HERE/test/spec/names.cml"
expect "emit-tla: a name ending in an operator word" 0 "votedFor\\[i\\] = 0 /. small" --emit-tla "$HERE/test/spec/names.cml"
expect "emit-tla: a parameter ending in in"       0 "origin(xin) ==" --emit-tla "$HERE/test/spec/names.cml"
expect "emit-tla: a const ending in or in TypeOK"  0 "min .in (0 .. Nor)" --emit-tla "$HERE/test/spec/names.cml"
expect "names: once is inductive"                 0 "inductive" --spec "$HERE/test/spec/names.cml" --check-inductive
expect "emit-tla: a count inside an application"  0 "g\\[Cardinality({i .in 1 .. N  :  votedFor\\[i\\] # 0})\\] = origin(min)" --emit-tla "$HERE/test/spec/names.cml"
bad "a const declared twice"        "const N is declared twice" "const N = 1\nconst N = 2\n$V\n$I"
# a const an extended module declared cannot be overridden: the base has
# already evaluated with its value (the emitted .cfg had both values)
printf 'module ovb\nconst N = 2\n' > "$OUT/ovb.cml"
bad "a const that redeclares an extended one" "const N is declared twice" "extends ovb\nconst N = 4\n$V\n$I"
bad "a var without :"               "var needs :"              "var x 0 .. 1\ninit x == 0\n$I"
bad "a function var without ->"     "function var needs ->"    "var f : [0 .. 1, 0 .. 1]\ninit f == 0\n$I"
bad "fairness other than weak or strong" "fairness is weak or strong" "$V\nfairness fast\n$I"
bad "a fair line with no kind"      "fair needs weak or strong" "$V\nfair a\n$I"
bad "a fair line with no action"    "fair needs weak or strong" "$V\nfair weak\n$I"
bad "a fair line of another kind"   "fair needs weak or strong" "$V\nfair weakly a\n$I"
bad "a fair line, strong run on"    "fair needs weak or strong" "$V\nfair strongest a\n$I"
bad "fairness strong run on"        "fairness is weak or strong" "$V\nfairness strongly\n$I"
bad "too many fair lines"           "too many fair lines"      "$V\n$(for i in $(seq 33); do printf 'fair weak a\\n'; done)$I"
bad "a fair action name too long"   "fair action name"         "$V\nfair weak $(printf 'a%.0s' $(seq 80))\n$I"
bad "always run on"                 "property must be"         "$V\nproperty p: alwaysx\n$I"
bad "an action property without ["  "an action property is"    "$V\nproperty p: always x_vars]_vars\n$I"
bad "an empty action property"      "an action property is"    "$V\nproperty p: always []_vars\n$I"
bad "an action property too long"   "property"                 "$V\nproperty p: always [$(printf 'x == x and %.0s' $(seq 60))TRUE]_vars\n$I"
bad "an action property without _vars" "an action property is"   "$V\nproperty p: always [x' >= x]\n$I"
bad "a property without a name"     "property needs name:"     "$V\nproperty x == 1\n$I"
bad "a property of no known form"   "property must be"         "$V\nproperty p: x == 0\n$I"
bad "an action property on a non-boolean" "expected a boolean in an action property" "$V\naction a:\n  x' = 1 - x\nproperty p: always [x']_vars"
bad "an invariant without a name"   "invariant needs name:"    "$V\ninvariant x <= 1"
bad "a when outside an action"      "when outside action"      "$V\nwhen x == 0\n$I"
bad "a when after an action ends"   "when outside action"      "$V\naction a:\n  x' = 1 - x\n$I\nwhen x == 5"
# two when lines are two conjuncts, as TLA+ /\\ lines; the second must not
# replace the first, in the check or in the emitted module
printf 'module w2\nvar x : 0 .. 3\ninit x == 0\naction inc:\n  when x != 1\n  when x < 3\n  x%s = x + 1\ninvariant i: x <= 1\n' "'" > "$OUT/w2.cml"
expect "two when lines are both required"  0 "2 distinct states found" --spec "$OUT/w2.cml" --no-deadlock
expect "emit-tla: two when lines are conjoined" 0 "(x # 1) /. (x < 3)" --emit-tla "$OUT/w2.cml"
bad "an action parameter without in" "action param needs 'in'" "$V\naction a(i):\n  x' = 1\n$I"
bad "a line that is nothing"        "cannot parse: x = 1"      "$V\nx = 1\n$I"
bad "an extends that does not load" "cannot extend nosuch"     "extends nosuch\n$V\n$I"
bad "a function var without ]"     "function var needs ]"     "var f : [0 .. 1 -> 0 .. 1\ninit f == [i in 0 .. 1 -> 0]\n$I"
bad "an action tag without ]"       "action tag needs ]"       "$V\naction a [BUG:\n  x' = 1\n$I"
bad "action parameters without )"   "action parameters need )" "$V\naction a(i in 0 .. 1:\n  x' = i\n$I"
# the [TAG] is the bracket after the parameter list, never a bracket
# inside a parameter domain (which read as tag i and turned the action off)
printf 'module tg\nvar f : [1 .. 2 -> 0 .. 2]\nvar b : 0 .. 1\ninit f == [k in 1 .. 2 -> 1] and b == 0\naction pick(i in 1 .. 2, j in 0 .. f[i]):\n  when j == 1\n  b%s = 1\ninvariant nobad: b == 0\n' "'" > "$OUT/tg.cml"
expect "a [ in a parameter domain is not a tag" 1 "nobad is violated" --spec "$OUT/tg.cml"
bad "text after the action tag"     "cannot parse action header" "$V\naction a [BUG] x:\n  x' = 1\n$I"
bad "text after the parameters"     "cannot parse action header" "$V\naction a(i in 0 .. 1) x:\n  x' = i\n$I"
bad "an action tag that is not a name" "action tag is not a name" "$V\naction a [B G]:\n  x' = 1\n$I"
bad "an action name that is not a name" "cannot parse action header" "$V\naction a b:\n  x' = 1\n$I"
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
bad "a domain that is not a set"        "expected a set"                   "const K = 1\nvar x : K\ninit x == 0\ninvariant i: TRUE"
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
obs "node[0 with no ] is an error"              3 "expected \]" "node[0 + 1" 0
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
# records, sequences and nested functions (test/spec/tracerec.cml), and
# logrep's functions of sequences of records
trace_row itf  "$HERE/test/spec/tracerec.cml" 3
trace_row json "$HERE/test/spec/tracerec.cml" 3
trace_row itf  "$HERE/examples/spec/logrep.cml" 5 --define BUG
trace_row json "$HERE/examples/spec/logrep.cml" 5 --define BUG
"$CMC" --spec "$HERE/test/spec/tracerec.cml" --trace-out "$OUT/t.itf" > /dev/null 2>&1
expectp "trace: a record's fields sorted by name, as Apalache" 0 '"r":{"a":{"#bigint":"1"},"zz":{"#bigint":"3"}}' cat "$OUT/t.itf"
expectp "trace: ITF varTypes of records, sequences, functions" 0 '"t":"(Int -> { a: Int, b: Seq(Int) })"' cat "$OUT/t.itf"
expectp "trace: ITF varTypes of a function of functions" 0 '"g":"(Int -> (Int -> Int))"' cat "$OUT/t.itf"
expect "trace: ITF is the default format" 1 '(itf, 5 states)' --spec "$HERE/test/spec/mutex.cml" --define BUG --trace-out "$OUT/t.itf"
expect "trace: a format other than itf or json is an error" 3 "the formats are itf and json" --spec "$HERE/test/spec/mutex.cml" --trace-out "$OUT/t.x" --trace-format tla
expect "trace: --trace-format without --trace-out is an error" 3 "needs --trace-out" --spec "$HERE/test/spec/mutex.cml" --trace-format json
expect "trace: an unwritable file is an error" 3 "could not write" --spec "$HERE/test/spec/mutex.cml" --define BUG --trace-out "$OUT/no/such/dir/t.itf"
expect "trace: a failed write is an error"     3 "could not write" --spec "$HERE/test/spec/mutex.cml" --define BUG --trace-out /dev/full
# a liveness violation prints its lasso, as TLC does, and writes it:
# ITF marks the cycle with ADR-015's "loop", JSON repeats its first state
expect "lasso: printed with the state the cycle goes back to" 1 "Back to state 3: <flip>" --spec "$HERE/test/spec/fair/lasso.cml"
# a type is fixed before any state: a variable in one read as 0 (G2)
VD=$HERE/test/spec/sound/vardom.cml
expect "a var domain that reads a variable is refused" 3 "the type y .. 3: a variable's type and a const must be constant" --spec "$VD"
expect "inductive: a var domain reading a variable"    3 "must be constant expressions" --spec "$VD" --check-inductive
expect "emit-tla: a var domain reading a variable"     3 "must be constant expressions" --emit-tla "$VD"
printf 'module kv\nvar y : 0 .. 1\nvar f : [0 .. y -> 0 .. 1]\ninit y == 0 and f == [i in 0 .. 0 -> 0]\ninvariant t: TRUE\n' > "$OUT/kv.cml"
expect "a function key set that reads a variable is refused" 3 "the type 0 .. y: a variable" --spec "$OUT/kv.cml"
printf 'module cv\nvar y : 0 .. 1\nconst K = y + 1\ninit y == 0\ninvariant t: TRUE\n' > "$OUT/cv.cml"
expect "a const that reads a variable is refused" 3 "const K: bad value" --spec "$OUT/cv.cml"
expect "lasso: a cycle that stutters is printed so" 1 "State 2: Stuttering" --spec "$HERE/test/spec/sound/dead.cml"
expect "lasso: written as ITF with its loop" 1 '(itf, 4 states, loop at state 3)' --spec "$HERE/test/spec/fair/lasso.cml" --trace-out "$OUT/live.itf"
expectp "lasso: ITF loop is the index of the cycle's first state" 0 '"loop":2}' cat "$OUT/live.itf"
expectp "lasso: ITF names the action that closes the cycle" 0 '"loopAction":"flip"' cat "$OUT/live.itf"
expect "lasso: a stutter cycle loops at its last state" 1 '(json, 1 states, loop at state 1)' --spec "$HERE/test/spec/sound/dead.cml" --trace-out "$OUT/live.json" --trace-format json
expectp "lasso: JSON repeats the loop state, a stutter" 0 '"action":"Stuttering","back_to":0' cat "$OUT/live.json"
expect "lasso: as JSON" 1 '(json, 4 states, loop at state 3)' --spec "$HERE/test/spec/fair/lasso.cml" --trace-out "$OUT/live.json" --trace-format json
expectp "lasso: JSON repeats the loop state with its action" 0 '"index":4,"action":"flip","back_to":2' cat "$OUT/live.json"
expect "lasso: strong fairness owes only what is enabled" 1 "Back to state 3: <flip>" --spec "$HERE/test/spec/fair/lasso_sf.cml"
expect "lasso: the cycle of A ~> B starts at an A state" 1 "State 3: Stuttering" --spec "$HERE/test/spec/sound/leads.cml"
expect "lasso: an unwritable file is an error" 3 "could not write" --spec "$HERE/test/spec/sound/dead.cml" --trace-out /dev/full
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

echo "-- the TLA+ reader (cmc --spec FILE.tla --config FILE.cfg)"
# The hand-written twins in the subset read directly, with TLC's counts
# (check-spec-calibration re-runs TLC on them); the rest are refused by
# name.  The round trip over every .cml is test/tla_gate.sh.
expect "tla: counter twin, TLC's counts"      0 "4 distinct states found" --spec "$HERE/test/spec/counter.tla" --config "$HERE/test/spec/counter.cfg"
expect "tla: the .cfg beside the module is the default" 0 "4 distinct states found" --spec "$HERE/test/spec/counter.tla"
expect "tla: mutex twin, TLC's counts"        0 "8 distinct states found" --spec "$HERE/test/spec/mutex.tla" --config "$HERE/test/spec/mutex_safe.cfg"
expect "tla: mutex twin with USE_BUG = TRUE"  1 "is (4 actions)" --spec "$HERE/test/spec/mutex.tla" --config "$HERE/test/spec/mutex_bug.cfg"
expect "tla: guard twin, CHECK_DEADLOCK FALSE" 1 "Invariant Marked is violated" --spec "$HERE/test/spec/guard.tla" --config "$HERE/test/spec/guard_bug.cfg"
expect "tla: deadlock twin"                   1 "Deadlock reached" --spec "$HERE/test/spec/deadlock.tla" --config "$HERE/test/spec/deadlock.cfg"
expect "tla: ACTION_CONSTRAINT"               0 "10 distinct states found" --spec "$HERE/test/spec/constrain.tla" --config "$HERE/test/spec/constrain_ac.cfg"
expect "tla: CONSTRAINT"                      0 "6 distinct states found" --spec "$HERE/test/spec/constrain.tla" --config "$HERE/test/spec/constrain_c.cfg"
expect "tla: logrep twin: records, sequences, defs, @" 0 "4439 distinct states found" --spec "$HERE/examples/spec/logrep.tla" --config "$HERE/examples/spec/logrep_read_safe.cfg"
expect "tla: logrep twin, depth 16"           0 "search is 16" --spec "$HERE/examples/spec/logrep.tla" --config "$HERE/examples/spec/logrep_read_safe.cfg"
expect "tla: logrep twin with BUG"            1 "is (5 actions)" --spec "$HERE/examples/spec/logrep.tla" --config "$HERE/examples/spec/logrep_read_bug.cfg"
# two type-invariant forms on one variable: TypeOK, else the first, types
# it; the other is checked as written (TLC's trace, 3 actions)
expect "tla: TypeOK types, a tighter x \\in T is checked" 1 "Tight is violated" --spec "$HERE/test/spec/typeinv.tla" --config "$HERE/test/spec/typeinv_ok.cfg"
expect "tla: the first types, a later x \\in T is checked" 1 "is (3 actions)" --spec "$HERE/test/spec/typeinv.tla" --config "$HERE/test/spec/typeinv_lt.cfg"
expect "tla: a tighter first type stops (exit 3)" 3 "x = 3 is outside its domain 0 .. 2" --spec "$HERE/test/spec/typeinv.tla" --config "$HERE/test/spec/typeinv_tl.cfg"
# a T that reads a variable is no type: checked as written (TLC: 1 action)
expect "tla: an x \\in T reading a variable is checked" 1 "is (1 actions)" --spec "$HERE/test/spec/vardom.tla" --config "$HERE/test/spec/vardom.cfg"
expect "tla: logrep's TypeOK is refused"      3 "logrep.tla:22: UNION is not supported" --spec "$HERE/examples/spec/logrep.tla" --config "$HERE/examples/spec/logrep_safe.cfg"
expect "tla: paxos twin is refused (strings)" 3 "paxos.tla:35: a string literal" --spec "$HERE/examples/spec/paxos.tla" --config "$HERE/examples/spec/paxos_safe.cfg"
expect "tla: zab twin is refused (\\X)"       3 "zab.tla:53: the set operator \\\\X" --spec "$HERE/examples/spec/zab.tla" --config "$HERE/examples/spec/zab_safe.cfg"
expect "tla: --emit-cml prints the translation" 0 "action Try(i in 0 .. N - 1):" --emit-cml "$HERE/test/spec/mutex.tla" --config "$HERE/test/spec/mutex_safe.cfg"
expect "tla: --emit-cml needs a TLA+ module"  3 "reads a TLA+ module" --emit-cml "$HERE/test/spec/mutex.cml"
expect "tla: --define is for .cml"            3 "--define is for .cml" --spec "$HERE/test/spec/mutex.tla" --config "$HERE/test/spec/mutex_safe.cfg" --define BUG
expect "tla: a missing .cfg"                  3 "cannot open" --spec "$HERE/test/spec/mutex.tla"

# tla NAME EXIT NEEDLE BODY [CFG]: a module m with constant N and
# variables x and f, Init x = 0 /\ f = [i \in 0..N |-> 0], with BODY
# after it, read with CFG (default: Spec, N = 2, invariant Inv, no
# deadlock check)
tla() {
	printf -- '---- MODULE m ----\nEXTENDS Naturals, FiniteSets, Sequences, TLC\nCONSTANTS %s\nVARIABLES x, f\nvars == <<x, f>>\nInit == x = 0 /\\ f = [i \\in 0..N |-> 0]\n%b\n====\n' "${CONSTS:-N}" "$4" > "$OUT/m.tla"
	printf '%b\n' "${5:-SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nCHECK_DEADLOCK FALSE}" > "$OUT/m.cfg"
	expect "tla: $1" "$2" "$3" --spec "$OUT/m.tla" --config "$OUT/m.cfg"
}
S='Spec == Init /\\ [][Next]_vars'
INC='Inc == x < N /\\ x'"'"' = x + 1 /\\ UNCHANGED f'
tla "a minimal module"                 0 "3 distinct states found" "$INC\nNext == Inc\n$S\nInv == x <= N"
tla "a bullet list ends at its column" 0 "4 distinct states found" "Next ==\n  /\\\\ x < N\n  /\\\\ \\\\/ x' = x + 1\n     \\\\/ x' = x + 2\n  /\\\\ UNCHANGED f\n$S\nInv == x <= N + 1"
# a token left of its item's bullet ends the item, as SANY reads it
tla "a token left of its bullet ends the item" 3 "m.tla:10: expected an expression" "Next ==\n  /\\\\ x < N\n  /\\\\ x' =\n x + 1\n  /\\\\ UNCHANGED f\n$S\nInv == TRUE"
tla "IF THEN ELSE, the untaken branch unevaluated" 0 "3 distinct states found" "$INC\nNext == Inc\n$S\nInv == IF x = 0 THEN TRUE ELSE N \\\\div x >= 1"
tla "a def is expanded, bound names apart" 0 "3 distinct states found" "Q(i) == \\\\E x2 \\\\in 0..i : x2 = x\n$INC\nNext == Inc\n$S\nInv == \\\\A x2 \\\\in 0..N : Q(x2 + N)"
tla "x' \\\\in S is a parameter"       0 "3 distinct states found" "Next == x' \\\\in 0..N /\\\\ UNCHANGED f\n$S\nInv == TRUE"
tla "an action that leaves a variable unsaid" 3 "does not say what f' is" "Next == x' = 1\n$S\nInv == TRUE"
tla "a variable both assigned and UNCHANGED" 3 "both given a value and UNCHANGED" "Next == x' = 1 /\\\\ UNCHANGED <<x, f>>\n$S\nInv == TRUE"
tla "WF on each action" 0 "1 property held" "$INC\nNext == Inc\nSpec == Init /\\\\ [][Next]_vars /\\\\ WF_vars(Inc)\nInv == TRUE\nP == <>(x = N)" "SPECIFICATION Spec\nCONSTANT N = 2\nPROPERTY P\nCHECK_DEADLOCK FALSE"
tla "no fairness: <> fails" 1 "Property P is violated" "$INC\nNext == Inc\n$S\nInv == TRUE\nP == <>(x = N)" "SPECIFICATION Spec\nCONSTANT N = 2\nPROPERTY P\nCHECK_DEADLOCK FALSE"
tla "~> is a property" 0 "1 property held" "$INC\nNext == Inc\nSpec == Init /\\\\ [][Next]_vars /\\\\ WF_vars(Next)\nInv == TRUE\nP == x = 0 ~> x = N" "SPECIFICATION Spec\nCONSTANT N = 2\nPROPERTY P\nCHECK_DEADLOCK FALSE"
tla "WF on a disjunction is refused" 3 "is a disjunction of actions" "$INC\nDec == x > 0 /\\\\ x' = x - 1 /\\\\ UNCHANGED f\nNext == Inc \\\\/ Dec\nSpec == Init /\\\\ [][Next]_vars /\\\\ WF_vars(Next)\nInv == TRUE"
tla "WF on part of Next, as fair weak"  0 "3 distinct states found" "$INC\nDec == x > 0 /\\\\ x' = x - 1 /\\\\ UNCHANGED f\nNext == Inc \\\\/ Dec\nSpec == Init /\\\\ [][Next]_vars /\\\\ WF_vars(Inc)\nInv == TRUE"
tla "WF under an \\\\E is refused" 3 "fair per parameter value" "A(i) == x < i /\\\\ x' = x + 1 /\\\\ UNCHANGED f\nB == \\\\E i \\\\in 0..N : A(i)\nNext == B\nSpec == Init /\\\\ [][Next]_vars /\\\\ WF_vars(A)\nInv == TRUE"
tla "SF on an action"                  0 "1 property held" "$INC\nNext == Inc\nSpec == Init /\\\\ [][Next]_vars /\\\\ SF_vars(Inc)\nInv == TRUE\nP == <>(x = N)" "SPECIFICATION Spec\nCONSTANT N = 2\nPROPERTY P\nCHECK_DEADLOCK FALSE"
tla "[][A]_vars as a property"         1 "Action property P is violated" "$INC\nNext == Inc\n$S\nInv == TRUE\nP == [][x' < 2]_vars" "SPECIFICATION Spec\nCONSTANT N = 2\nPROPERTY P\nCHECK_DEADLOCK FALSE"
tla "a TypeOK is the variables' types" 0 "3 distinct states found" "$INC\nNext == Inc\n$S\nInv == /\\\\ x \\\\in 0..N\n       /\\\\ f \\\\in [0..N -> {0, 1}]" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nCHECK_DEADLOCK FALSE"
tla "a TypeOK violation is a model error" 3 "outside its domain" "$INC\nNext == Inc\n$S\nInv == x \\\\in 0..1 /\\\\ f \\\\in [0..N -> 0..0]" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nCHECK_DEADLOCK FALSE"
tla "a THEOREM and its proof are skipped" 0 "3 distinct states found" "$INC\nNext == Inc\n$S\nInv == x <= N\nASSUME ConstOK == N = 2\nTHEOREM T == Spec => []Inv\n<1>1. Init => Inv\n  BY DEF Init, Inv\n<1>2. QED\n  BY <1>1\nLater == TRUE"
tla "a partial subscript is refused"   3 "the tuple of all the variables" "$INC\nNext == Inc\nSpec == Init /\\\\ [][Next]_x\nInv == TRUE"
tla "INIT and NEXT"                    0 "3 distinct states found" "$INC\nNext == Inc\nInv == TRUE" "INIT Init\nNEXT Next\nCONSTANT N = 2\nINVARIANT Inv\nCHECK_DEADLOCK FALSE"
tla "an ASSUME that holds"             0 "3 distinct states found" "ASSUME N \\\\in Nat\n$INC\nNext == Inc\n$S\nInv == TRUE"
tla "a false ASSUME is refused"        3 "is false for the configuration" "ASSUME N \\\\in BOOLEAN\n$INC\nNext == Inc\n$S\nInv == TRUE"
tla "a nondeterministic Init"          3 "nondeterministic Init" "Init2 == x \\\\in 0..1 /\\\\ f = <<>>\n$INC\nNext == Inc\nSpec == Init2 /\\\\ [][Next]_vars\nInv == TRUE"
tla "recursion is refused"             3 "used before it is defined" "R(i) == IF i = 0 THEN 0 ELSE R(i - 1)\n$INC\nNext == Inc\n$S\nInv == R(x) = 0"
tla "RECURSIVE is refused"             3 "RECURSIVE is not supported" "RECURSIVE R(_)\n$INC\nNext == Inc\n$S\nInv == TRUE"
tla "LET is refused"                   3 "LET is not supported" "$INC\nNext == Inc\n$S\nInv == LET y == x IN y <= N"
tla "CHOOSE is refused"                3 "CHOOSE is not supported" "$INC\nNext == Inc\n$S\nInv == (CHOOSE y \\\\in 0..N : TRUE) = 0"
tla "CASE is refused"                  3 "CASE is not supported" "$INC\nNext == Inc\n$S\nInv == CASE x = 0 -> TRUE [] OTHER -> TRUE"
tla "SUBSET is refused"                3 "SUBSET is not supported" "$INC\nNext == Inc\n$S\nInv == {} \\\\in SUBSET {1}"
tla "DOMAIN is refused"                3 "DOMAIN is not supported" "$INC\nNext == Inc\n$S\nInv == DOMAIN f = 0..N"
tla "a string is refused"              3 "a string literal" "$INC\nNext == Inc\n$S\nInv == \"a\" = \"a\""
tla "\\\\cup is refused"               3 "the set operator .cup" "$INC\nNext == Inc\n$S\nInv == 1 \\\\in {1} \\\\cup {2}"
tla "an unbounded quantifier is refused" 3 "unbounded quantifier" "$INC\nNext == Inc\n$S\nInv == \\\\A y : TRUE"
tla "a temporal quantifier is refused" 3 "temporal quantifier" "$INC\nNext == Inc\n$S\nInv == \\\\EE y : TRUE"
tla "ENABLED is refused"               3 "ENABLED is not supported" "$INC\nNext == Inc\n$S\nInv == ENABLED Inc"
tla "INSTANCE is refused"              3 "INSTANCE is not supported" "INSTANCE Naturals\n$INC\nNext == Inc\n$S\nInv == TRUE"
tla "an infix definition is refused"   3 "an infix operator definition" "a ++ b == a + b\n$INC\nNext == Inc\n$S\nInv == TRUE"
tla "a function definition is refused" 3 "the function definition" "g[i \\\\in 0..N] == i\n$INC\nNext == Inc\n$S\nInv == TRUE"
tla "a nested module is refused"       3 "a nested MODULE" "---- MODULE n ----\n====\n$INC\nNext == Inc\n$S\nInv == TRUE"
tla "a primed read in a guard is refused" 3 "reads a primed variable but is not" "Next == x' = 1 /\\\\ f[x'] > 0 /\\\\ UNCHANGED f\n$S\nInv == TRUE"
tla "a set variable is refused"        3 "holds a set" "Init2 == x = {} /\\\\ f = 0\nNext == UNCHANGED vars\nSpec == Init2 /\\\\ [][Next]_vars\nInv == TRUE"
tla "a type conflict between assignments" 3 "an integer in one place and a boolean" "Next == x' = TRUE /\\\\ UNCHANGED f\n$S\nInv == TRUE"
tla "a type conflict is refused"       3 "is an integer in one place and a sequence" "Next == x' = <<1>> /\\\\ UNCHANGED f\n$S\nInv == TRUE"
tla "records, sequences, EXCEPT paths" 0 "3 distinct states found" "Next == x < N /\\\\ x' = x + 1 /\\\\ f' = [f EXCEPT ![x] = 1]\n$S\nInv == [[a |-> <<x>>] EXCEPT !.a[1] = @ + 1].a = <<x + 1>> /\\\\ Len(Append(<<>>, x) \\\\o <<1>>) = 2"
tla "a tuple of a function is refused" 3 "a function of a tuple" "$INC\nNext == Inc\n$S\nInv == f[0, 1] = 0"
# what SANY refuses is never checked: these were read and checked
tla "/\\\\ beside \\\\/ needs parentheses" 3 "m.tla:10: /. ... ./ needs parentheses" "$INC\nNext == Inc\n$S\nInv == x = 0 /\\\\ TRUE \\\\/ x > 0"
tla "a parameter named like a variable" 3 "m.tla:7: x is declared again" "Set(x) == f' = [f EXCEPT ![0] = x] /\\\\ UNCHANGED x\nNext == Set(1)\n$S\nInv == TRUE"
tla "a bound name bound again inside" 3 "m.tla:10: i is declared again" "$INC\nNext == Inc\n$S\nInv == \\\\A i \\\\in 0..1 : \\\\E i \\\\in 0..1 : i = 0"
tla "a bound name named like a definition" 3 "m.tla:10: Inc is declared again" "$INC\nNext == Inc\n$S\nInv == \\\\A Inc \\\\in 0..1 : Inc >= 0"
printf -- '---- MODULE m ----\nEXTENDS Bags\nVARIABLE x\n====\n' > "$OUT/m.tla"; printf 'INIT Init\nNEXT Next\n' > "$OUT/m.cfg"
expect "tla: EXTENDS of another module is refused" 3 "EXTENDS Bags" --spec "$OUT/m.tla" --config "$OUT/m.cfg"
tla ".cfg: a constant substitution is refused" 3 "substitution is not supported" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nCONSTANT N <- Two\nINVARIANT Inv"
tla ".cfg: an unbound constant"        3 "constant N has no value" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nINVARIANT Inv"
tla ".cfg: an undeclared constant"     3 "does not declare as a constant" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nCONSTANT N = 2 M = 1\nINVARIANT Inv"
tla ".cfg: SPECIFICATION and INIT"     3 "both SPECIFICATION and INIT" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nINIT Init\nCONSTANT N = 2"
tla ".cfg: an unknown keyword"         3 "unexpected FOO" "$INC\nNext == Inc\n$S\nInv == TRUE" "FOO Inv\nSPECIFICATION Spec\nCONSTANT N = 2"
tla ".cfg: ALIAS is refused"           3 "ALIAS is not supported" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nCONSTANT N = 2\nALIAS Inv"
tla ".cfg: an undefined invariant"     3 "INVARIANT Nope is not defined" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Nope"
tla ".cfg: VIEW of all the variables"  0 "3 distinct states found" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nVIEW vars\nCHECK_DEADLOCK FALSE"
tla ".cfg: any other VIEW is refused"  3 "VIEW V: only the tuple" "$INC\nNext == Inc\n$S\nInv == TRUE\nV == x" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nVIEW V"
tla ".cfg: a PROPERTY other than <> or ~>" 3 "only <>P, P ~> Q and" "$INC\nNext == Inc\n$S\nInv == TRUE\nP == []<>(x = 0)" "SPECIFICATION Spec\nCONSTANT N = 2\nPROPERTY P\nCHECK_DEADLOCK FALSE"
tla ".cfg: CHECK_DEADLOCK FALSE"       0 "3 distinct states found" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nCHECK_DEADLOCK FALSE"
tla ".cfg: the deadlock check is on"   1 "Deadlock reached" "$INC\nNext == Inc\n$S\nInv == TRUE" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nCHECK_DEADLOCK TRUE"
# model values: a set of them is a dedicated range of integers, and
# SYMMETRY Permutations(it) is .cml's symmetry over that range
CONSTS="N, P"
tla "SYMMETRY of model values reduces" 0 "2 distinct states found" "Take == \\\\E p \\\\in P : x' = p /\\\\ UNCHANGED f\nNext == Take\n$S\nInv == TRUE\nSym == Permutations(P)" "SPECIFICATION Spec\nCONSTANT N = 2\nCONSTANT P = {p1, p2, p3}\nINVARIANT Inv\nSYMMETRY Sym\nCHECK_DEADLOCK FALSE"
sed -i '/^SYMMETRY/d' "$OUT/m.cfg"; expect "tla: model values without SYMMETRY" 0 "4 distinct states found" --spec "$OUT/m.tla" --config "$OUT/m.cfg"
# Model values are integers past TLC's, so what TLC stops on is refused
# (it once computed p + 1 and passed); a function keyed by them, with
# SYMMETRY, gives TLC's 10 distinct states and depth 7
MVC="SPECIFICATION Spec\nCONSTANT N = 2\nCONSTANT P = {p1, p2}\nINVARIANT Inv\nCHECK_DEADLOCK FALSE"
tla "arithmetic on a model value is refused" 3 "m.tla:7: + on a model value" "Next == \\\\E p \\\\in P : x' = p + 1 /\\\\ UNCHANGED f\n$S\nInv == TRUE" "$MVC"
tla "order on a model value is refused" 3 "m.tla:9: < on an integer or a model value" "Next == \\\\E p \\\\in P : x' = p /\\\\ UNCHANGED f\n$S\nInv == x < 5" "$MVC"
tla "\\\\in Nat of a model value is refused" 3 "in Nat of a model value" "Next == \\\\E p \\\\in P : x' = p /\\\\ UNCHANGED f\n$S\nInv == x \\\\in Nat" "$MVC"
tla "@ of a model value is refused" 3 "m.tla:7: + on an integer or a model value" "Next == \\\\E i \\\\in 0..N, p \\\\in P : f' = [f EXCEPT ![i] = p] /\\\\ x' = [f EXCEPT ![i] = @ + 1][i]\n$S\nInv == TRUE" "$MVC"
tla "an integer past TLC's is refused" 3 "past TLC's integers" "$INC\nNext == Inc\n$S\nInv == x # 1099511627776" "$MVC"
tla "a function keyed by model values, SYMMETRY" 0 "10 distinct states found" "Init2 == x = 0 /\\\\ f = [p \\\\in P |-> 0]\nNext == \\\\E p \\\\in P : f[p] < 2 /\\\\ f' = [f EXCEPT ![p] = @ + 1] /\\\\ UNCHANGED x\nSpec == Init2 /\\\\ [][Next]_vars\nSym == Permutations(P)\nInv == \\\\A p \\\\in P : f[p] <= 2" "SPECIFICATION Spec\nCONSTANT N = 2\nCONSTANT P = {p1, p2, p3}\nINVARIANT Inv\nSYMMETRY Sym\nCHECK_DEADLOCK FALSE"
expect "tla: ... and TLC's depth 7" 0 "search is 7" --spec "$OUT/m.tla" --config "$OUT/m.cfg"
CONSTS=
tla "SYMMETRY of integers is refused"  3 "holds an integer, not only model values" "Next == UNCHANGED vars\n$S\nInv == TRUE\nSym == Permutations(N)" "SPECIFICATION Spec\nCONSTANT N = {1, 2}\nINVARIANT Inv\nSYMMETRY Sym"
tla "SYMMETRY other than Permutations is refused" 3 "only Permutations(C)" "Next == UNCHANGED vars\n$S\nInv == TRUE\nSym == {}" "SPECIFICATION Spec\nCONSTANT N = 2\nINVARIANT Inv\nSYMMETRY Sym"

# raw NAME EXIT NEEDLE MODULE CFG: the module and configuration as given
raw() {
	printf -- '%b\n' "$4" > "$OUT/r.tla"; printf '%b\n' "$5" > "$OUT/r.cfg"
	expect "tla: $1" "$2" "$3" --spec "$OUT/r.tla" --config "$OUT/r.cfg"
}
H='---- MODULE r ----\nEXTENDS Naturals\nVARIABLE x'
E='===='
C='INIT I\nNEXT N\nINVARIANT V\nCHECK_DEADLOCK FALSE'
OK="I == x = 0\nN == x < 2 /\\\\ x' = x + 1\nV == TRUE"
raw "no MODULE header"            3 "no ---- MODULE" "VARIABLE x" "$C"
raw "no ---- after the name"      3 "expected ---- after MODULE" "---- MODULE r\nVARIABLE x\n$E" "$C"
raw "no ==== end"                 3 "has no ==== end line" "$H\n$OK" "$C"
raw "an unterminated comment"     3 "unterminated (\\* comment" "$H\n(* a (* nested *)\n$OK\n$E" "$C"
raw "comments, nested and line"   0 "3 distinct states" "$H\n(* a (* nested *) comment *)\n\\\\* a line comment\n$OK\n$E" "$C"
raw "a name from a digit"         3 "starts with a digit" "$H\nI == x = 0\nN == x' = 1a\nV == TRUE\n$E" "$C"
raw "a number that overflows"     3 "overflows" "$H\nI == x = 99999999999999999999\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a stray character"           3 "unexpected character" "$H\nI == x = 0 ; 1\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a too-long name"             3 "too long" "$H\nI == x = 0\nN == x' = x\nV == aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n$E" "$C"
raw "\\\\land, \\\\lor, \\\\lnot, \\\\leq, \\\\geq, \\\\neg, \\\\equiv, =<" 0 "3 distinct states" "$H\nI == x = 0\nN == x \\\\leq 1 \\\\land x' = x + 1\nV == (\\\\lnot x \\\\geq 3 \\\\lor \\\\neg TRUE) \\\\equiv x =< 2\n$E" "$C"
raw "/= and \\\\notin"            0 "3 distinct states" "$H\nI == x = 0\nN == x /= 2 /\\\\ x' = x + 1\nV == x \\\\notin {5, 6}\n$E" "$C"
raw "-, unary - and %"            0 "3 distinct states" "$H\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1\nV == -x <= 0 /\\\\ x % 3 = x /\\\\ 2 - x >= 0 /\\\\ x - -1 > 0\n$E" "$C"
raw "=> in an invariant"          0 "3 distinct states" "$H\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1\nV == x > 5 => FALSE\n$E" "$C"
raw "\\\\in Int and BOOLEAN"      0 "3 distinct states" "$H\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1\nV == x \\\\in Int /\\\\ (x = 0) \\\\in BOOLEAN /\\\\ x \\\\in Nat\n$E" "$C"
raw "\\\\notin Nat is refused"    3 "notin Nat is not supported" "$H\nI == x = 0\nN == x' = x\nV == x \\\\notin Nat\n$E" "$C"
raw "Nat as a value is refused"   3 "the infinite set Nat" "$H\nI == x = 0\nN == x' = x\nV == Nat = Nat\n$E" "$C"
raw "\\\\E over Nat is refused"   3 "cannot be enumerated" "$H\nI == x = 0\nN == x' = x\nV == \\\\E y \\\\in Nat : TRUE\n$E" "$C"
raw "a filter outside Cardinality" 3 "only inside Cardinality" "$H\nI == x = 0\nN == x' = x\nV == \\\\E y \\\\in {z \\\\in 0..1 : TRUE} : TRUE\n$E" "$C"
raw "a non-set is refused"        3 "expected a set" "$H\nI == x = 0\nN == x' = x\nV == \\\\E y \\\\in x : TRUE\n$E" "$C"
raw "Cardinality of a set"        0 "3 distinct states" "$H\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1\nV == Cardinality(0..x) = x + 1 /\\\\ Cardinality({1, 2}) = 2\n$E" "$C"
raw "Cardinality of two arguments" 3 "takes one argument" "$H\nI == x = 0\nN == x' = x\nV == Cardinality(0..1, 2) = 1\n$E" "$C"
raw "an unknown operator"         3 "the operator Foo is not supported" "$H\nI == x = 0\nN == x' = x\nV == Foo(1)\n$E" "$C"
raw "an unknown name"             3 "unknown name y" "$H\nI == x = 0\nN == x' = x\nV == y\n$E" "$C"
raw "a sequence operator's arity" 3 "Len takes 1 argument" "$H\nI == x = 0\nN == x' = x\nV == Len(<<>>, 1) = 0\n$E" "$C"
raw "a def's arity"               3 "takes 1 argument, given 2" "$H\nD(a) == a\nI == x = 0\nN == x' = x\nV == D(1, 2) = 1\n$E" "$C"
raw "a parameter called"          3 "higher-order parameters" "$H\nD(a) == a(1)\nI == x = 0\nN == x' = x\nV == D(1) = 1\n$E" "$C"
raw "a higher-order parameter"    3 "a higher-order parameter" "$H\nD(a(_)) == 1\nI == x = 0\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a def parameter not a name"  3 "a parameter must be a name" "$H\nD(1) == 1\nI == x = 0\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a def with no body"          3 "has no body" "$H\nI == x = 0\nN == x' = x\nV ==\n$E" "$C"
raw "a def with trailing text"    3 "unexpected ) in the definition of V" "$H\nI == x = 0\nN == x' = x\nV == TRUE )\n$E" "$C"
raw "a declared-twice name"       3 "x is declared twice" "$H\nVARIABLE x\nI == x = 0\nN == x' = x\nV == TRUE\n$E" "$C"
raw "VARIABLES with no names"     3 "with no names" "$H\nVARIABLES\nI == x = 0\nN == x' = x\nV == TRUE\n$E" "$C"
raw "VARIABLES not a name"        3 "expected a name" "---- MODULE r ----\nVARIABLE 1\n$E" "$C"
raw "VARIABLES without a comma"   3 "expected , found" "---- MODULE r ----\nVARIABLE x y\n$E" "$C"
raw "an operator constant"        3 "an operator constant" "---- MODULE r ----\nCONSTANT F(_)\nVARIABLE x\n$E" "$C"
raw "a prefix definition"         3 "operator definition" "$H\n-. a == a\n$OK\n$E" "$C"
raw "a bad top-level token"       3 "expected , found )" "$H\n) \n$OK\n$E" "$C"
raw "an ASSUME with trailing text" 3 "in ASSUME" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Nat )\n$OK\n$E" "CONSTANT K = 1\n$C"
raw "an ASSUME of another form"   3 "this ASSUME is not supported" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K > 0\n$OK\n$E" "CONSTANT K = 1\n$C"
raw "ASSUME over another set"     3 "only Nat, Int and BOOLEAN" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Real\n$OK\n$E" "CONSTANT K = 1\n$C"
raw "ASSUME Int of a model value" 3 "is false" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Int\n$OK\n$E" "CONSTANT K = k\n$C"
raw "constants of each kind"      0 "3 distinct states" "---- MODULE r ----\nEXTENDS Integers\nCONSTANTS A, B, S, T, U, M\nVARIABLE x\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1 /\\\\ B\nV == A = -3 /\\\\ S = {-1, 0} /\\\\ \\\\E y \\\\in T : y = 4 /\\\\ \\\\A u \\\\in U : u # M /\\\\ M = M\n$E" "CONSTANTS A = -3 B = TRUE S = {-1, 0} T = {4, 6} U = {u1, u2} M = m\n$C"
raw "a constant bound twice"      3 "is bound twice" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K = 1 K = 2\n$C"
raw "a module constant override"  3 "a constant override" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K[M] = 1\n$C"
raw "a constant with no ="        3 "expected = after the name" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K 1\n$C"
raw "a constant set of strings"   3 "a set element must be" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K = {1, (}\n$C"
raw "a constant set with no comma" 3 "expected , or } in the set" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K = {1 2}\n$C"
raw "a constant value - name"     3 "expected a number after -" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K = - k\n$C"
raw "a constant value ("          3 "is not supported (an integer" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K = (\n$C"
raw "a keyword with no name"      3 "INVARIANT needs a name" "$H\n$OK\n$E" "INIT I\nNEXT N\nINVARIANT\n"
raw "INIT given twice"            3 "INIT is given twice" "$H\n$OK\n$E" "INIT I\nINIT I\nNEXT N"
raw "INIT of two names"           3 "INIT takes one name" "$H\n$OK\n$E" "INIT I N\nNEXT N"
raw "no SPECIFICATION or NEXT"    3 "needs SPECIFICATION, or INIT and NEXT" "$H\n$OK\n$E" "INIT I"
raw "CHECK_DEADLOCK of a number"  3 "CHECK_DEADLOCK is TRUE or FALSE" "$H\n$OK\n$E" "$C\nCHECK_DEADLOCK 1"
raw "a definition with parameters as INIT" 3 "INIT I takes parameters" "$H\nI(a) == x = a\nN == x' = x\nV == TRUE\n$E" "$C"
raw "Init that is not x = e"      3 "Init must be a conjunction" "$H\nI == x > 0\nN == x' = x\nV == TRUE\n$E" "$C"
raw "Init giving x twice"         3 "a value twice" "$H\nI == x = 0 /\\\\ x = 1\nN == x' = x\nV == TRUE\n$E" "$C"
raw "Init leaving a variable out" 3 "Init does not give y a value" "$H\nVARIABLE y\n$OK\n$E" "$C"
raw "Init with a conjunction in a value" 3 "holds a conjunction" "$H\nVARIABLE b\nI == x = 0 /\\\\ b = [i \\\\in 0..1 |-> i = 0 /\\\\ TRUE]\nN == UNCHANGED <<x, b>>\nV == TRUE\n$E" "$C"
raw "a reserved variable name"    3 "is reserved in .cml" "---- MODULE r ----\nVARIABLE when\nI == when = 0\nN == when' = 0\nV == TRUE\n$E" "$C"
raw "a reserved constant name"    0 "1 distinct state" "---- MODULE r ----\nCONSTANT and\nVARIABLE x\nI == x = and\nN == x' = x\nV == x = 2\n$E" "CONSTANT and = 2\n$C"
raw "a bound name is renamed apart" 0 "3 distinct states" "---- MODULE r ----\nCONSTANT y\nVARIABLE x\nP(z) == \\\\E y_1 \\\\in 0..z : \\\\E w \\\\in 0..y_1 : w = z\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1\nV == \\\\A x_ \\\\in 0..1 : \\\\A y_ \\\\in 0..1 : P(x_ + y_) /\\\\ \\\\A in \\\\in 0..0 : in = 0\n$E" "CONSTANT y = 5\n$C"
raw "a nondeterministic UNCHANGED of a non-variable" 3 "UNCHANGED of something other than variables" "$H\nI == x = 0\nN == UNCHANGED <<x + 1>>\nV == TRUE\n$E" "$C"
raw "a prime on an expression"    3 "a prime on something other than a variable" "$H\nI == x = 0\nN == (x + 1)' = 2\nV == TRUE\n$E" "$C"
raw "a primed value read"         3 "reads a primed variable" "$H\nVARIABLE y\nI == x = 0 /\\\\ y = 0\nN == x' = 1 /\\\\ y' = x'\nV == TRUE\n$E" "$C"
raw "x' assigned twice"           3 "a value twice in one action" "$H\nI == x = 0\nN == x' = 1 /\\\\ x' = 1\nV == TRUE\n$E" "$C"
raw "UNCHANGED outside an action" 3 "UNCHANGED is supported only" "$H\nI == x = 0\nN == x' = x\nV == UNCHANGED x\n$E" "$C"
raw "a temporal formula as an invariant" 3 "a temporal formula is supported only" "$H\nI == x = 0\nN == x' = x\nV == <>(x = 0)\n$E" "$C"
raw "an \\\\E with many parameters" 3 "parameters in one action" "$H\nI == x = 0\nN == \\\\E a, b, c, d, e, f, g, h, i \\\\in 0..0 : x' = a\nV == TRUE\n$E" "$C"
raw "IF splits an action"         0 "3 distinct states" "$H\nI == x = 0\nN == IF x < 2 THEN x' = x + 1 ELSE x' = x\nV == TRUE\n$E" "$C"
raw "an action named by Next"     0 "3 distinct states" "$H\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1\nV == TRUE\n$E" "$C"
raw "Next == FALSE has no action" 1 "Deadlock reached" "$H\nI == x = 0\nN == FALSE\nV == TRUE\n$E" "INIT I\nNEXT N\nINVARIANT V"
raw "a function of two arguments" 3 "more than one argument" "$H\nI == x = [a, b \\\\in 0..1 |-> a]\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a record type conflict"      3 "different fields in two places" "$H\nI == x = [a |-> 0]\nN == x' = [b |-> 0]\nV == TRUE\n$E" "$C"
raw "a record field twice"        3 "is given twice" "$H\nI == x = [a |-> 0, a |-> 1]\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a record of many fields"     3 "too many fields" "$H\nI == x = [a |-> 0, b |-> 0, c |-> 0, d |-> 0, e |-> 0, f |-> 0, g |-> 0, h |-> 0, i |-> 0, j |-> 0, k |-> 0, l |-> 0, m |-> 0, n |-> 0, o |-> 0, p |-> 0, q |-> 0]\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a set of records as a value" 3 "expected a set" "$H\nI == x = 0\nN == x' = x\nV == x \\\\in [a : 0..1] \\\\/ TRUE\n$E" "$C"
raw "an EXCEPT with no path"      3 "or .f after" "$H\nI == x = [i \\\\in 0..1 |-> 0]\nN == x' = [x EXCEPT ! = 1]\nV == TRUE\n$E" "$C"
raw "an EXCEPT of a tuple key"    3 "a function of a tuple" "$H\nI == x = [i \\\\in 0..1 |-> 0]\nN == x' = [x EXCEPT ![0, 1] = 1]\nV == TRUE\n$E" "$C"
raw "[ ... ] of nothing known"    3 "expected EXCEPT" "$H\nI == x = [x]\nN == x' = x\nV == TRUE\n$E" "$C"
raw "[A]_v alone"                 3 "outside .*_v: not supported" "$H\nI == x = 0\nN == x' = x\nV == [N]_x\n$E" "$C"
TY="---- MODULE r ----\nEXTENDS Naturals, Sequences\nVARIABLE x\nT == [a : 0..1, s : {<<>>} \\\\cup {<<e>> : e \\\\in 0..1} \\\\cup {<<e, d>> : e \\\\in 0..1, d \\\\in 0..1}]\nTypeOK == x \\\\in T\nI == x = [a |-> 0, s |-> <<>>]\nN == Len(x.s) < 3 /\\\\ x' = [x EXCEPT !.s = Append(@, x.a), !.a = 1 - @]\nV == TRUE\n===="
raw "a TypeOK of records and Seq bounds the search" 3 "past its bound 2" "$TY" "INIT I\nNEXT N\nINVARIANT TypeOK\nCHECK_DEADLOCK FALSE"
printf -- '%b\n' "$TY" > "$OUT/r.tla"
expect "tla: the TypeOK types are the .cml types" 0 "var x : \[a : 0 .. 1, s : Seq(0 .. 1, 2)\]" --emit-cml "$OUT/r.tla" --config "$OUT/r.cfg"
# the type x = <<x>> needs is infinite: refused at load, never searched
raw "a recursive type"            3 "would hold itself" "$H\nI == x = <<>>\nN == x' = <<x>>\nV == TRUE\n$E" "$C"
raw "sequences and records, typed" 0 "3 distinct states" "$H\nVARIABLE s\nI == x = [a |-> <<>>, b |-> 0] /\\\\ s = <<>>\nN == x.b < 2 /\\\\ x' = [x EXCEPT !.a = Append(@, Len(s)), !.b = @ + 1] /\\\\ s' = SubSeq(Tail(<<1>> \\\\o s), 1, 0)\nV == Len(x.a) = x.b /\\\\ (x.b > 0 => Head(x.a) = 0) /\\\\ s = <<>>\n$E" "$C"
raw "a parameter set with a bracket" 3 "holds \\[ or :" "$H\nI == x = 0\nN == \\\\E y \\\\in {[a |-> 1].a} : x' = y\nV == TRUE\n$E" "$C"
raw "a chain needing parentheses" 3 "needs parentheses" "$H\nI == x = 0\nN == x' = x\nV == x = 0 = TRUE\n$E" "$C"
raw "* and \\\\div mixed"         3 "needs parentheses" "$H\nI == x = 0\nN == x' = x\nV == x * 2 \\\\div 3 = 0\n$E" "$C"
raw "a bad infix operator"        3 "subseteq is not supported" "$H\nI == x = 0\nN == x' = x\nV == {} \\\\subseteq {}\n$E" "$C"
raw "a backslash operator"        3 "prec is not supported" "$H\nI == x = 0\nN == x' = x\nV == x \\\\prec 1\n$E" "$C"
raw "a [][A] without subscript"   3 "needs a subscript" "$H\nI == x = 0\nN == x' = x\nS == I /\\\\ [][N]\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "a <<>> subscript"            0 "3 distinct states" "$H\nI == x = 0\nN == x < 2 /\\\\ x' = x + 1\nS == I /\\\\ [][N]_<<x>> /\\\\ WF_<<x>>(N)\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V\nCHECK_DEADLOCK FALSE"
raw "a second [][...]"            3 "a second" "$H\nI == x = 0\nN == x' = x\nS == I /\\\\ [][N]_x /\\\\ [][N]_x\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "[][e]_v of an expression"    3 "Next must be the name" "$H\nI == x = 0\nN == x' = x\nS == I /\\\\ [][x' = x]_x\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "WF of an expression"         3 "A must be the name" "$H\nI == x = 0\nN == x' = x\nS == I /\\\\ [][N]_x /\\\\ WF_x(x' = x)\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "WF with a partial subscript" 3 "WF_v(A): v must be" "$H\nVARIABLE y\nI == x = 0 /\\\\ y = 0\nN == UNCHANGED <<x, y>>\nS == I /\\\\ [][N]_<<x, y>> /\\\\ WF_x(N)\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "WF of an action twice"       0 "1 distinct state" "$H\nI == x = 0\nN == x' = x\nS == I /\\\\ [][N]_x /\\\\ WF_x(N) /\\\\ WF_x(N)\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "WF of a non-action"          3 "not an action of Next" "$H\nI == x = 0\nN == x' = x\nM == x' = 1\nS == I /\\\\ [][N]_x /\\\\ WF_x(N) /\\\\ WF_x(M)\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "Spec without Init"           3 "must be Init" "$H\nI == x = 0\nN == x' = x\nS == [][N]_x\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "Spec of another form"        3 "Spec: only Init" "$H\nI == x = 0\nN == x' = x\nS == I /\\\\ x > 0\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "SYMMETRY of an expression"   3 "Permutations of something other" "$H\nI == x = 0\nN == x' = x\nY == Permutations({1})\nV == TRUE\n$E" "$C\nSYMMETRY Y"
# the reader's refusals and corners, one row each (the coverage gate)
raw "a comment with a star"       0 "3 distinct states" "$H\n(* a * b *)\n$OK\n$E" "$C"
raw "a number then _"             3 "starts with a digit" "$H\nI == x = 0\nN == x' = 1_a\nV == TRUE\n$E" "$C"
raw "IF without ELSE"             3 "expected ELSE, found" "$H\nI == x = 0\nN == x' = x\nV == IF TRUE THEN TRUE\n$E" "$C"
raw "a field that is a number"    3 "expected a name, found 2" "$H\nI == x = 0\nN == x' = x\nV == [a |-> 1].2 = 1\n$E" "$C"
raw "too many bound names"        3 "too many bound names" "$H\nI == x = 0\nN == x' = x\nV == \\\\A a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q \\\\in 0..0 : TRUE\n$E" "$C"
raw "a set of records of many fields" 3 "set of records with too many fields" "$H\nI == x = 0\nN == x' = x\nV == x \\\\in [a : 0..0, b : 0..0, c : 0..0, d : 0..0, e : 0..0, f : 0..0, g : 0..0, h : 0..0, i : 0..0, j : 0..0, k : 0..0, l : 0..0, m : 0..0, n : 0..0, o : 0..0, p : 0..0, q : 0..0]\n$E" "$C"
raw "[e]_v after a bracket"       3 "outside .*_v: not supported" "$H\nI == x = 0\nN == x' = x\nV == [a |-> 1]_x\n$E" "$C"
raw "an EXCEPT path too long"     3 "EXCEPT path is too long" "$H\nI == x = 0\nN == x' = [x EXCEPT ![0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0][0] = 1]\nV == TRUE\n$E" "$C"
raw "[][A] at the end"            3 "needs a subscript" "$H\nI == x = 0\nN == x' = x\nV == TRUE\nS == I /\\\\ [][N]\n$E" "SPECIFICATION S\nINVARIANT V"
raw "an expression cut short"     3 "expected an expression" "$H\nI == x = 0\nN == x' = x\nV == x +\n$E" "$C"
raw "a call with a space"         3 "unexpected ( in the definition of V" "$H\nD(a) == a\nI == x = 0\nN == x' = x\nV == D (1) = 1\n$E" "$C"
raw "an expression of )"          3 "unexpected )" "$H\nI == x = 0\nN == x' = x\nV == x = )\n$E" "$C"
raw "\\\\AA is refused"           3 "temporal quantifier .AA" "$H\nI == x = 0\nN == x' = x\nV == \\\\AA y : TRUE\n$E" "$C"
raw "\\\\A after an operand"      3 "unexpected .A in the definition" "$H\nI == x = 0\nN == x' = x\nV == TRUE \\\\A\n$E" "$C"
raw "a definition of ( )"         3 "a definition of this form" "$H\nI == x = 0\nN == x' = x\nV == 1 + 1\n2 (a) == 1\n$E" "$C"
raw "a definition of ( ]"         3 "a definition of this form" "$H\nI == x = 0\nN == x' = x\nV == TRUE\nD(a] == 1\n$E" "$C"
raw "a definition of a number"    3 "operator definition (1 ==)" "$H\nI == x = 0\nN == x' = x\nV == TRUE\n1 == 2\n$E" "$C"
raw "a proof with BY and QED lines" 0 "3 distinct states" "$H\n$OK\nTHEOREM T == TRUE\nBY DEF V\nQED\nLEMMA L == TRUE\nOBVIOUS\n$E" "$C"
raw "a constant declared twice"   3 "K is declared twice" "---- MODULE r ----\nCONSTANT K, K\nVARIABLE x\n$OK\n$E" "CONSTANT K = 1\n$C"
raw "a variable named like a def" 3 "D is declared twice" "---- MODULE r ----\nD == 1\nVARIABLE D\n$OK\n$E" "$C"
raw "parameters with no comma"    3 "expected , or ) in the parameters" "$H\nD(a b) == 1\n$OK\n$E" "$C"
raw "a parameter named twice"     3 "a is declared again" "$H\nD(a, a) == a\n$OK\nW == D(1, 2) = 1\n$E" "INIT I\nNEXT N\nINVARIANT W"
raw "MODULE with no name"         3 "no ---- MODULE Name" "---- MODULE ----\nVARIABLE x\n$E" "$C"
raw "a ---- rule between units"   0 "3 distinct states" "$H\n----\n$OK\n$E" "$C"
raw "text after a ---- rule"      3 "unexpected 1 after a ---- rule" "$H\n---- 1\n$OK\n$E" "$C"
raw "a stray token at the top"    3 "unexpected 1 at the top level" "---- MODULE r ----\n1\nVARIABLE x\n$OK\n$E" "$C"
raw "an ASSUME of TRUE"           3 "this ASSUME is not supported" "$H\nASSUME TRUE\n$OK\n$E" "$C"
raw "USE at the top"              3 "USE is not supported" "$H\nUSE DEF I\n$OK\n$E" "$C"
raw "a model value named twice"   0 "3 distinct states" "---- MODULE r ----\nCONSTANT P, Q\nVARIABLE x\n$OK\nW == Q \\\\in P\n$E" "CONSTANT P = {p1, p2} Q = p1\nINIT I\nNEXT N\nINVARIANT W\nCHECK_DEADLOCK FALSE"
raw "a .cfg keyword as a value"   3 "the value INIT is not supported" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K = INIT\n$C"
raw "a negated name in a set"     3 "a set element must be" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\n$OK\n$E" "CONSTANT K = {-k}\n$C"
raw "CHECK_DEADLOCK of a name"    3 "CHECK_DEADLOCK is TRUE or FALSE" "$H\n$OK\n$E" "$C\nCHECK_DEADLOCK yes"
raw "SPECIFICATION and NEXT"      3 "both SPECIFICATION and INIT/NEXT" "$H\n$OK\nS == I /\\\\ [][N]_x\n$E" "SPECIFICATION S\nNEXT N"
raw "NEXT without INIT"           3 "needs SPECIFICATION, or INIT and NEXT" "$H\n$OK\n$E" "NEXT N"
raw "constant sets: gaps and repeats" 0 "3 distinct states" "---- MODULE r ----\nCONSTANT S, T\nVARIABLE x\n$OK\nW == \\\\E s \\\\in S : \\\\E t \\\\in T : s + t = 4\n$E" "CONSTANT S = {1, 3} T = {1, 1}\nINIT I\nNEXT N\nINVARIANT W\nCHECK_DEADLOCK FALSE"
raw "Append of one argument"      3 "Append takes 2 arguments" "$H\nI == x = 0\nN == x' = x\nV == Append(<<>>) = <<>>\n$E" "$C"
raw "a reserved constant below 0" 0 "1 distinct state" "---- MODULE r ----\nCONSTANT and\nVARIABLE x\nI == x = and\nN == x' = x\nV == x = and * 1\n$E" "CONSTANT and = -2\n$C"
raw "BOOLEAN as a value"          3 "the infinite set BOOLEAN" "$H\nI == x = 0\nN == x' = x\nV == BOOLEAN = BOOLEAN\n$E" "$C"
raw "a primed invariant"          3 "x' is read where only the current state" "$H\nI == x = 0\nN == x' = x\nV == x' = x\n$E" "$C"
raw "a prime on an expression in a constraint" 3 "a prime on something other" "$H\nI == x = 0\nN == x' = x\nV == TRUE\nA == (x + 1)' = 1\n$E" "$C\nACTION_CONSTRAINT A"
raw "\\\\E over Int"              3 "the infinite set Int cannot be enumerated" "$H\nI == x = 0\nN == x' = x\nV == \\\\E y \\\\in Int : TRUE\n$E" "$C"
raw "\\\\E over a number"         3 "expected a set" "$H\nI == x = 0\nN == x' = x\nV == \\\\E y \\\\in 1 + 2 : TRUE\n$E" "$C"
raw "x' \\\\in S past the parameters" 3 "parameters in one action" "$H\nI == x = 0\nN == \\\\E a, b, c, d, e, f, g, h \\\\in 0..0 : x' \\\\in 0..1\nV == TRUE\n$E" "$C"
raw "a prime under \\\\A"         3 "reads a primed variable but is not" "$H\nI == x = 0\nN == \\\\A i \\\\in 0..1 : x' = i\nV == TRUE\n$E" "$C"
raw "a record with fewer fields"  3 "different fields in two places" "$H\nI == x = [a |-> 0]\nN == x' = [a |-> 0, b |-> 0]\nV == TRUE\n$E" "$C"
raw "a recursive record type"     3 "would hold itself" "$H\nI == x = [a |-> 0]\nN == x' = [a |-> x]\nV == TRUE\n$E" "$C"
raw "Next of no prime"            3 "action Next does not say what x' is" "$H\nI == x = 0\nN == TRUE\nV == TRUE\n$E" "$C"
raw "ASSUME of a model value in Int" 3 "ASSUME K .in Int is false" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Int\n$OK\n$E" "CONSTANT K = k\n$C"
raw "ASSUME of a negative in Nat" 3 "ASSUME K .in Nat is false" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Nat\n$OK\n$E" "CONSTANT K = -1\n$C"
raw "ASSUME K = -1"               0 "3 distinct states" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K = -1\n$OK\n$E" "CONSTANT K = -1\n$C"
raw "ASSUME K = an expression"    3 "only an integer literal" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K = 1 + 1\n$OK\n$E" "CONSTANT K = 2\n$C"
raw "ASSUME K = n, K a boolean"   3 "ASSUME K = 1 is false" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K = 1\n$OK\n$E" "CONSTANT K = TRUE\n$C"
raw "ASSUME K in a set"           3 "this ASSUME is not supported" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in 0..2\n$OK\n$E" "CONSTANT K = 1\n$C"
raw "a boolean variable"          3 "holds a boolean" "$H\nI == x = TRUE\nN == x' = x\nV == TRUE\n$E" "$C"
raw "Init of e = x"               3 "Init must be a conjunction" "$H\nI == 0 = x\nN == x' = x\nV == TRUE\n$E" "$C"
raw "a PROPERTY of a state predicate" 3 "PROPERTY P: only" "$H\n$OK\nP == x = 0\n$E" "INIT I\nNEXT N\nPROPERTY P"
raw "a PROPERTY [][A] of part"    3 "PROPERTY P: only" "$H\nVARIABLE y\nI == x = 0 /\\\\ y = 0\nN == UNCHANGED <<x, y>>\nV == TRUE\nP == [][x' = x]_x\n$E" "INIT I\nNEXT N\nPROPERTY P"
raw "SYMMETRY of two arguments"   3 "only Permutations(C)" "$H\n$OK\nY == Permutations(1, 2)\n$E" "$C\nSYMMETRY Y"
raw "SYMMETRY of a range"         3 "Permutations of something other" "$H\n$OK\nY == Permutations(0..1)\n$E" "$C\nSYMMETRY Y"
# a type-invariant look-alike is an ordinary invariant; typing corners
TC='INIT I\nNEXT N\nINVARIANT T\nCHECK_DEADLOCK FALSE'
SQ="---- MODULE r ----\nEXTENDS Naturals, Sequences\nVARIABLE x\nI == x = <<>>\nN == Len(x) < 1 /\\\\ x' = Append(x, 0)"
raw "a TypeOK of one variable twice" 0 "3 distinct states" "$H\n$OK\nT == x \\\\in 0..2 /\\\\ x \\\\in 0..2\n$E" "$TC"
raw "a TypeOK of an expression"   0 "3 distinct states" "$H\n$OK\nT == x + 0 \\\\in 0..2\n$E" "$TC"
raw "a TypeOK of a constant set"  0 "3 distinct states" "---- MODULE r ----\nCONSTANT S\nVARIABLE x\n$OK\nT == x \\\\in S\n$E" "CONSTANT S = {0, 1, 2}\n$TC"
raw "a TypeOK of a set of tuples"  3 "expected an integer in a set" "$H\n$OK\nT == x \\\\in {<<>>}\n$E" "$TC"
raw "a TypeOK with a boolean range" 3 "expected a set" "$H\nVARIABLE f\nI == x = 0 /\\\\ f = [i \\\\in 0..1 |-> 0]\nN == x < 2 /\\\\ x' = x + 1 /\\\\ UNCHANGED f\nT == f \\\\in [0..1 -> BOOLEAN]\n$E" "$TC"
raw "a record TypeOK with a bad field" 3 "expected a set" "$H\nVARIABLE r\nI == x = 0 /\\\\ r = [a |-> 0]\nN == x < 2 /\\\\ x' = x + 1 /\\\\ UNCHANGED r\nT == r \\\\in [a : BOOLEAN]\n$E" "$TC"
raw "a Seq union of two element types" 3 "set operator .cup" "$SQ\nT == x \\\\in {<<>>} \\\\cup {<<e>> : e \\\\in 0..1} \\\\cup {<<e, d>> : e \\\\in 0..1, d \\\\in 0..2}\n$E" "$TC"
raw "a Seq union with a wrong name" 3 "set operator .cup" "$SQ\nT == x \\\\in {<<>>} \\\\cup {<<d>> : e \\\\in 0..1}\n$E" "$TC"
raw "a Seq union with a bad element type" 3 "set operator .cup" "$SQ\nT == x \\\\in {<<>>} \\\\cup {<<e>> : e \\\\in BOOLEAN}\n$E" "$TC"
raw "a Seq union of the wrong length" 3 "set operator .cup" "$SQ\nT == x \\\\in {<<>>} \\\\cup {<<e, e>> : e \\\\in 0..1}\n$E" "$TC"
raw "a Seq union that starts elsewhere" 3 "set operator .cup" "$SQ\nT == x \\\\in {<<0>>} \\\\cup {<<e>> : e \\\\in 0..1}\n$E" "$TC"
raw "a Seq union of {<<>>} alone" 3 "set operator .cup" "$SQ\nT == x \\\\in {<<>>} \\\\cup {}\n$E" "$TC"
raw "a Seq union of a range"      3 "set operator .cup" "$SQ\nT == x \\\\in 0..1 \\\\cup {}\n$E" "$TC"
raw "unary minus is an integer"   0 "2 distinct states" "$H\nI == x = 1\nN == x' = -x\nV == TRUE\n$E" "$C"
raw "\\\\subseteq is a boolean"   3 "subseteq is not supported" "$H\nVARIABLE b\nI == x = 0 /\\\\ b = 0\nN == x' = x /\\\\ b' = IF {} \\\\subseteq {} THEN 1 ELSE 0\nV == TRUE\n$E" "$C"
raw "a set in a variable"         3 "the operator .. is not supported" "$H\nI == x = 0..1\nN == x' = x\nV == TRUE\n$E" "$C"
raw "Len and Cardinality are integers" 0 "1 distinct state" "$H\nI == x = Len(<<>>) + Cardinality({})\nN == x' = Head(<<x>>)\nV == x = 0\n$E" "$C"
raw "a function in a field"       0 "2 distinct states" "$H\nI == x = [a |-> [i \\\\in 0..1 |-> 0]]\nN == x' = [x EXCEPT !.a[1] = 1]\nV == x.a[0] = 0\n$E" "$C"
raw "an EXCEPT path past a scalar" 3 "" "$H\nI == x = [a |-> 0]\nN == x' = [x EXCEPT !.a.b = 1]\nV == TRUE\n$E" "$C"
raw "@ outside EXCEPT"            3 "" "$H\nI == x = 0\nN == x' = @\nV == TRUE\n$E" "$C"
raw "Init x \\\\in S"             3 "nondeterministic Init" "$H\nI == x \\\\in 0..1\nN == x' = x\nV == TRUE\n$E" "$C"
raw "an Init conjunct of no variable" 3 "Init must be a conjunction" "$H\nI == x = 0 /\\\\ 1 = 1\nN == x' = x\nV == TRUE\n$E" "$C"
raw "an Init of a constant"       3 "Init must be a conjunction" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nI == x = 0 /\\\\ K \\\\in 0..1\nN == x' = x\nV == TRUE\n$E" "CONSTANT K = 1\n$C"
raw "ASSUME a set in Nat"         3 "is false for the configuration" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Nat\n$OK\n$E" "CONSTANT K = {1}\n$C"
raw "ASSUME a model value in Nat" 3 "is false for the configuration" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Nat\n$OK\n$E" "CONSTANT K = k\n$C"
raw "ASSUME a boolean in Int"     3 "is false for the configuration" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K \\\\in Int\n$OK\n$E" "CONSTANT K = TRUE\n$C"
raw "ASSUME of an expression in Nat" 3 "this ASSUME is not supported" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K + 1 \\\\in Nat\n$OK\n$E" "CONSTANT K = 1\n$C"
raw "ASSUME 1 = K"                3 "this ASSUME is not supported" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME 1 = K\n$OK\n$E" "CONSTANT K = 1\n$C"
raw "ASSUME K = -n, false"        3 "ASSUME K = -2 is false" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K = -2\n$OK\n$E" "CONSTANT K = -1\n$C"
raw "ASSUME K = -e"               3 "only an integer literal" "---- MODULE r ----\nCONSTANT K\nVARIABLE x\nASSUME K = -K\n$OK\n$E" "CONSTANT K = 0\n$C"
raw "a [][N]_v of a repeated variable" 3 "v must be the tuple" "$H\n$OK\nS == I /\\\\ [][N]_<<x, x>>\n$E" "SPECIFICATION S\nINVARIANT V"
raw "a [][N]_v of an expression"  3 "v must be the tuple" "$H\n$OK\nS == I /\\\\ [][N]_<<x + 1>>\n$E" "SPECIFICATION S\nINVARIANT V"
raw "Spec with an Init of parameters" 3 "" "$H\n$OK\nJ(a) == x = a\nS == J(0) /\\\\ [][N]_x\n$E" "SPECIFICATION S\nINVARIANT V"
raw "Spec with a primed first conjunct" 3 "" "$H\n$OK\nS == N /\\\\ [][N]_x\n$E" "SPECIFICATION S\nINVARIANT V"
raw "[][N]_x, N undefined"        3 "Next must be the name of a definition" "$H\n$OK\nS == I /\\\\ [][Q]_x\n$E" "SPECIFICATION S\nINVARIANT V"
raw "WF of an undefined name"     3 "A must be the name" "$H\n$OK\nS == I /\\\\ [][N]_x /\\\\ WF_x(Q)\n$E" "SPECIFICATION S\nINVARIANT V"
raw "SF of an action twice in Next" 3 "SF_vars(A): A is a disjunction" "$H\nI == x = 0\nA == x < 2 /\\\\ x' = x + 1\nN == A \\\\/ A\nS == I /\\\\ [][N]_x /\\\\ SF_x(A)\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "SF under an \\\\E"           3 "SF_vars(A): A sits under an" "$H\nI == x = 0\nA == x < 2 /\\\\ x' = x + 1\nN == \\\\E i \\\\in 0..1 : A\nS == I /\\\\ [][N]_x /\\\\ SF_x(A)\nV == TRUE\n$E" "SPECIFICATION S\nINVARIANT V"
raw "SYMMETRY of Permutations of a name" 3 "Permutations of something other" "$H\n$OK\nY == Permutations(x)\n$E" "$C\nSYMMETRY Y"
raw "SYMMETRY of another operator" 3 "only Permutations(C)" "$H\n$OK\nY == Len(<<>>)\n$E" "$C\nSYMMETRY Y"
rm -f "$OUT/r.cfg"; expect "tla: a missing configuration" 3 "cannot open" --spec "$OUT/r.tla" --config "$OUT/r.cfg"
printf 'a\0b' > "$OUT/nul.tla"; expect "tla: a NUL byte" 3 "a NUL byte" --spec "$OUT/nul.tla" --config "$OUT/nul.tla"

# the Apalache gate's verdict classifier: a tool error (parse, typing,
# input) must never count as an expected violation (audit 0.2)
if bash "$HERE/test/apalache_gate.sh" --self-test > "$OUT/apa-selftest.txt" 2>&1; then
	echo "ok   apalache gate classifies tool errors as errors"
else
	echo "FAIL apalache gate classifier"; sed 's/^/     /' "$OUT/apa-selftest.txt"; fail=1
fi

# proof hooks: --check-inductive enumerates every state of TypeOK, not
# only the reachable ones, so it finds the counterexamples to induction
# a reachability search cannot (each weak model passes cmc --spec), and
# it refuses what it cannot enumerate rather than check part of it
I="$HERE/test/spec/ind"
echo "-- proof hooks: cmc --spec --check-inductive"
expect "inductive: mutex with lock_matches"       0 "each of the 8 states" --spec "$I/mutex_ind.cml" --check-inductive
sed 's/^invariant lock_matches \[inductive\]:/invariant lock_matches:/' "$I/mutex_ind.cml" > "$OUT/mutex_weak.cml"
expect "inductive: mutex alone holds everywhere"  0 "No error has been found" --spec "$OUT/mutex_weak.cml"
expect "inductive: but mutex alone is not"        1 "a step of enter leaves mutual_exclusion" --spec "$OUT/mutex_weak.cml" --check-inductive
expect "inductive: the CTI names its parameters"  1 "1. enter(i=0)" --spec "$OUT/mutex_weak.cml" --check-inductive
expect "inductive: a queue of records"            0 "each of the 13 states" --spec "$I/queue.cml" --check-inductive
sed 's/^invariant bounded \[inductive\]:/invariant bounded:/' "$I/queue.cml" > "$OUT/queue_weak.cml"
expect "inductive: increasing alone is not"       1 "leaves increasing" --spec "$OUT/queue_weak.cml" --check-inductive
expect "inductive: escape holds everywhere"       0 "No error has been found" --spec "$I/escape.cml"
expect "inductive: a step out of TypeOK is a CTI" 1 "a step of jump leaves TypeOK" --spec "$I/escape.cml" --check-inductive
printf 'module badinit\nvar x : 0 .. 3\ninit x == 2\naction a:\n  when x < 3\n  x%s = x + 1\ninvariant low [inductive]: x < 2\n' "'" > "$OUT/badinit.cml"
expect "inductive: an init outside the invariant" 1 "initial state does not satisfy low" --spec "$OUT/badinit.cml" --check-inductive
expect "inductive: nothing marked is an error"    3 "no invariant is marked" --spec "$HERE/test/spec/counter.cml" --check-inductive
printf 'module big\nvar a : 0 .. 999\nvar b : 0 .. 999\nvar c : 0 .. 1\ninit a == 0 and b == 0 and c == 0\ninvariant t [inductive]: c <= 1\n' > "$OUT/big.cml"
expect "inductive: a TypeOK too large is refused" 3 "more than 1000000 states" --spec "$OUT/big.cml" --check-inductive
printf 'module wide\nvar a : 0 .. 2000000\ninit a == 0\ninvariant t [inductive]: TRUE\n' > "$OUT/wide.cml"
expect "inductive: a domain too wide is refused"  3 "has more than 1000000 values" --spec "$OUT/wide.cml" --check-inductive
sed 's/\[inductive\]/[inductve]/' "$I/escape.cml" > "$OUT/mark.cml"
expect "inductive: an unknown mark is an error"   3 "only mark is \[inductive\]" --spec "$OUT/mark.cml"
{ cat "$I/escape.cml"; printf 'constraint c: x < 3\n'; } > "$OUT/indcon.cml"
expect "inductive: under a constraint, refused"   3 "not supported" --spec "$OUT/indcon.cml" --check-inductive
expect "inductive: no --trace-out"                3 "neither --trace-out" --spec "$I/escape.cml" --check-inductive --trace-out "$OUT/x.json"
# the enumeration over each kind of type: a set-literal domain, a
# function on the empty set, a record; and the products it refuses
printf 'module kinds\nvar s : {0, 2}\nvar f : [{} -> 0 .. 1]\nvar r : [a : 0 .. 1, b : {5}]\ninit s == 0 and f == [i in {} -> 0] and r == [a |-> 0, b |-> 5]\naction go:\n  when s == 0\n  s%s = 2\ninvariant t [inductive]: r.b == 5\n' "'" > "$OUT/kinds.cml"
expect "inductive: set, empty function, record"   0 "each of the 4 states" --spec "$OUT/kinds.cml" --check-inductive
printf 'module bigs\nvar s : Seq(0 .. 999, 3)\ninit s == <<>>\ninvariant t [inductive]: Len(s) <= 3\n' > "$OUT/bigs.cml"
expect "inductive: a sequence product too large"  3 "more than 1000000 states" --spec "$OUT/bigs.cml" --check-inductive
printf 'module bigfn\nvar f : [0 .. 9 -> 0 .. 9]\ninit f == [i in 0 .. 9 -> 0]\ninvariant t [inductive]: TRUE\n' > "$OUT/bigfn.cml"
expect "inductive: a function product too large"  3 "more than 1000000 states" --spec "$OUT/bigfn.cml" --check-inductive
# a [TAG] action counts only under --define; an expression that fails
# in an unreachable state of TypeOK is a model error, not a verdict
printf 'module gated\nvar x : 0 .. 2\ninit x == 0\naction up [BUG]:\n  when x < 2\n  x%s = x + 1\ninvariant zero [inductive]: x == 0\n' "'" > "$OUT/gated.cml"
expect "inductive: without --define, no BUG step" 0 "is inductive" --spec "$OUT/gated.cml" --check-inductive
expect "inductive: with --define, the BUG step"   1 "a step of up leaves zero" --spec "$OUT/gated.cml" --check-inductive --define BUG
printf 'module everr\nvar x : 0 .. 2\ninit x == 1\naction a:\n  when x == 1\n  x%s = 1\ninvariant d [inductive]: 2 / x >= 1\n' "'" > "$OUT/everr.cml"
expect "inductive: an invariant that fails to evaluate" 3 "invariant evaluation error" --spec "$OUT/everr.cml" --check-inductive
printf 'module acterr\nvar x : 0 .. 2\ninit x == 1\naction a:\n  when 2 / x >= 1\n  x%s = 1\ninvariant t [inductive]: x <= 2\n' "'" > "$OUT/acterr.cml"
expect "inductive: an action that fails to evaluate" 3 "from a state of TypeOK" --spec "$OUT/acterr.cml" --check-inductive
printf 'module inierr\nvar x : 0 .. 2\ninit x == 5\ninvariant t [inductive]: TRUE\n' > "$OUT/inierr.cml"
expect "inductive: an init outside the types"     3 "init:" --spec "$OUT/inierr.cml" --check-inductive
printf 'module inieval\nvar x : 0 .. 2\ninit x == 0\ninvariant t [inductive]: 1 / x == 1\n' > "$OUT/inieval.cml"
expect "inductive: an init invariant error"       3 "invariant evaluation error" --spec "$OUT/inieval.cml" --check-inductive
# the proof outline (TLC and Apalache read it in emit_gate and
# apalache_gate): each marked invariant, the constants, defs, actions
"$CMC" --emit-tla "$I/mutex_ind.cml" > "$OUT/mi.tla" 2>&1
if grep -q '^THEOREM Inductive == Spec => \[\]IndInv$' "$OUT/mi.tla" && grep -q '^ASSUME ConstOK ==$' "$OUT/mi.tla" &&
   grep -q 'BY ConstOK DEF Next, vars, try, enter, leave, IndInv, TypeOK, mutual_exclusion, lock_matches$' "$OUT/mi.tla" &&
   grep -q '^THEOREM Spec => \[\]lock_matches$' "$OUT/mi.tla"; then echo "ok   emit: the TLAPS outline for mutex_ind"
else echo "FAIL emit: the TLAPS outline for mutex_ind"; sed 's/^/     /' "$OUT/mi.tla" | tail -20; fail=1; fi
printf 'module dq\ndef Lo = 0\nvar x : 0 .. 1\ninit x == Lo\ninvariant t [inductive]: x >= Lo\n' > "$OUT/dq.cml"
"$CMC" --emit-tla "$OUT/dq.cml" > "$OUT/dq.tla" 2>&1
if grep -q 'BY DEF Init, IndInv, TypeOK, t, Lo$' "$OUT/dq.tla" && ! grep -q ConstOK "$OUT/dq.tla"; then echo "ok   emit: the outline opens defs, and has no ASSUME without constants"
else echo "FAIL emit: the outline with a def"; sed 's/^/     /' "$OUT/dq.tla" | tail -12; fail=1; fi
printf 'module clash\nvar TypeOK : 0 .. 1\ninit TypeOK == 0\ninvariant t: TRUE\n' > "$OUT/clash.cml"
expect "emit: a model name TypeOK is refused"     3 "TypeOK is a name the emitted TLA+ defines" --emit-tla "$OUT/clash.cml"
printf 'module clash2\nvar x : 0 .. 1\ninit x == 0\naction PTL:\n  when x == 0\n  x%s = 1\ninvariant t: TRUE\n' "'" > "$OUT/clash2.cml"
expect "emit: an action named PTL is refused"     3 "PTL is a name the emitted TLA+ defines" --emit-tla "$OUT/clash2.cml"
printf 'module clash3\nconst Inductive = 1\nvar x : 0 .. 1\ninit x == 0\ninvariant t: TRUE\n' > "$OUT/clash3.cml"
expect "emit: a constant named Inductive is refused" 3 "Inductive is a name" --emit-tla "$OUT/clash3.cml"

[ $fail = 0 ] && echo "spec_gate: ok (abstract-model checker: verdicts, counts and depths match TLC)" || echo "spec_gate: FAILED"
exit $fail
