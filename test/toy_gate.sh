#!/usr/bin/env bash
# toy_gate.sh CMC: the bug gate on the toy core, and the pattern a
# project's own bug gate follows (docs/research/01 4a):
#   1. the control build must pass,
#   2. each planted bug (#if CMC_TOY_BUG == n) must be reported with the
#      expected property, within a state budget, by BFS and by simulation,
#   3. the replay file of the counterexample must reproduce it.
# A bug that is not caught is a HOLE and fails the gate.
set -u
MC=${1:?usage: toy_gate.sh CMC}
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/toy-gate
mkdir -p "$OUT"
PROPS="--inv Mutex,PhaseMonotone,AllReplied,ReachCS"
fail=0

"$MC" --model toy $PROPS --det > "$OUT/control.txt" 2>&1
if grep -q 'No error has been found' "$OUT/control.txt" &&
   grep -q '^34 distinct\|, 34 distinct states found' "$OUT/control.txt"; then
	echo "control: PASS, 34 distinct states"
else
	echo "control: FAILED (see $OUT/control.txt)"; fail=1
fi

# bug | expected property | max distinct states
for row in "toy-bug1|Mutex|100" "toy-bug2|AllReplied|100" "toy-bug3|PhaseMonotone|100"; do
	IFS='|' read -r model prop budget <<< "$row"
	log=$OUT/$model.txt
	"$MC" --model "$model" $PROPS --det --max-states "$budget" \
		--trace-out "$OUT/$model.replay" > "$log" 2>&1
	if grep -qE "(Invariant|Action property|Quiescence property) $prop is violated" "$log"; then
		depth=$(grep -oE 'search is [0-9]+' "$log" | grep -oE '[0-9]+')
		"$MC" --model "$model" $PROPS --replay "$OUT/$model.replay" > "$log.replay" 2>&1
		if grep -qE "$prop is violated" "$log.replay"; then
			r=caught
		else
			r="caught, but replay did NOT reproduce it"; fail=1
		fi
		"$MC" --model "$model" $PROPS --simulate --seed 1 --walks 5000 --depth 40 \
			> "$log.sim" 2>&1
		grep -qE "$prop is violated" "$log.sim" && r="$r; simulate caught it" ||
			{ r="$r; simulate MISSED it"; fail=1; }
		echo "$model: $prop at depth $depth: $r"
	else
		echo "$model: HOLE: $prop not reported (see $log)"; fail=1
	fi
done
[ $fail = 0 ] && echo "toy_gate: ok" || echo "toy_gate: FAILED"
exit $fail
