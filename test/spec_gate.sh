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

echo "-- symmetry reduction: interchangeable node ids collapse"
# the token spec's node ids are fully symmetric; with symmetry declared,
# the state space shrinks (10 -> 3) with the SAME verdict -- the soundness
# check for the declaration.
expect "token: symmetry collapses the state space" 0 "3 distinct states found" --spec "$HERE/examples/spec/token.cml"

echo "-- liveness: temporal properties under weak fairness"
# the ticker must eventually reach its max under weak fairness (holds);
# with a reset action a fair cycle avoids the max (fails).
expect "ticker: eventually-max holds under fairness" 0 "1 property held" --spec "$HERE/examples/spec/ticker.cml"
expect "ticker: a fair reset cycle breaks it"        1 "reaches_max is violated" --spec "$HERE/examples/spec/ticker.cml" --define RESET

echo "-- the TLA+ bridge (cmc --emit-tla), structure only (check-emit-tla runs TLC)"
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
typed "or of a function"                  "expected a boolean" "TRUE or f"
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
# the values the evaluator computes, each pinned by the verdict it gives
value() {	# value DESC WANT EXPR: invariant EXPR holds (0) or is violated (1)
	printf 'module m\nvar x : 0 .. 1\nvar f : [0 .. 1 -> 0 .. 1]\nvar g : [0 .. 1 -> 0 .. 1]\ninit x == 0 and f == [i in 0 .. 1 -> i] and g == [i in 0 .. 1 -> 0]\ninvariant i: %s\n' "$3" > "$OUT/value.cml"
	if [ "$2" = 0 ]; then expect "$1" 0 "No error has been found" --spec "$OUT/value.cml"
	else expect "$1" 1 "Invariant i is violated" --spec "$OUT/value.cml"; fi
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
# the same operators on the right types still evaluate
printf 'module m\nvar x : 0 .. 1\nvar f : [0 .. 1 -> 0 .. 1]\ninit x == 0 and f == [i in 0 .. 1 -> 0]\naction a:\n  when not (x == 1) and (TRUE or FALSE) and x in {0, 1}\n  x%s = 1\n  f%s = [f except [x] = 1]\ninvariant i: (count i in 0 .. 1 : f[i] == 1) + x * 2 <= 3 and {x} != {2} and f != [i in 0 .. 1 -> 1] and (TRUE == TRUE)\n' "'" "'" > "$OUT/typed-ok.cml"
expect "well-typed operators still evaluate" 0 "2 distinct states found" --spec "$OUT/typed-ok.cml"

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
# extends: resolved beside the extending file, with or without .cml,
# and an absolute path
printf 'module base\nvar x : 0 .. 1\ninit x == 0\n' > "$OUT/base.cml"
printf 'module top\nextends base\ninvariant i: x == 0\n' > "$OUT/top.cml"
expect "extends finds the base beside the file" 0 "1 distinct states found" --spec "$OUT/top.cml"
printf 'module top\nimport %s\ninvariant i: x == 0\n' "$OUT/base.cml" > "$OUT/top.cml"
expect "import takes an absolute path"         0 "1 distinct states found" --spec "$OUT/top.cml"
printf 'module top\nextends base.cml\ninvariant i: x == 0\n' > "$OUT/top.cml"
if (cd "$OUT" && "$CMC" --spec top.cml > "$OUT/rel.txt" 2>&1) && grep -q "1 distinct" "$OUT/rel.txt"; then
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
printf '[map]\nx = "node[0][0]"\n' > "$OUT/b.bind"
printf 'module f\nvar c : [0 .. 1 -> 0 .. 1]\ninit c == [i in 0 .. 1 -> 0]\ninvariant i: TRUE\n' > "$OUT/fun.cml"
printf '[map]\nc = "0"\n' > "$OUT/fun.bind"
expectp "a function variable bound without an index"  3 "c is a function" "$RD" "$OUT/fun.cml" "$OUT/fun.bind" 0
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

# the Apalache gate's verdict classifier: a tool error (parse, typing,
# input) must never count as an expected violation (audit 0.2)
if bash "$HERE/test/apalache_gate.sh" --self-test > "$OUT/apa-selftest.txt" 2>&1; then
	echo "ok   apalache gate classifies tool errors as errors"
else
	echo "FAIL apalache gate classifier"; sed 's/^/     /' "$OUT/apa-selftest.txt"; fail=1
fi

[ $fail = 0 ] && echo "spec_gate: ok (abstract-model checker: verdicts, counts and depths match TLC)" || echo "spec_gate: FAILED"
exit $fail
