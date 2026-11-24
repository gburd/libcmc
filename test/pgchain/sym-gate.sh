#!/usr/bin/env bash
# sym-gate.sh B: symmetry reduction (libcmc 2.4) on pg_chain's Paxos core.
# The model's symmetry() makes the non-candidate acceptors that cfg does
# not tell apart interchangeable.  Every row of pg_chain's gate (PX_A to
# PX_R, mc-bug-gate.sh) runs with --symmetry on: each correct row must
# pass with the same witnesses as without, in fewer distinct states where
# the group is not trivial (those counts are pinned here), and pass
# --sym-check; each planted bug must be caught by the same property as
# without.  Logs go to $CMC_SCRATCH.
set -u
B=$1
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/pgchain-sym
mkdir -p "$OUT"
PX_A="--persist-cap 6 --net-cap 12 --cfg slots=1,rounds=3,candidates=1 --clients 3 --crashes 2"
PX_B="--persist-cap 4 --net-cap 10 --cfg slots=1,rounds=1 --clients 1 --crashes 1"
PX_C="--persist-cap 6 --net-cap 12 --cfg slots=2,candidates=1 --clients 2 --drops 1"
PX_D="--persist-cap 6 --net-cap 12 --cfg slots=2,rounds=2,candidates=1 --clients 3 --crashes 1"
PX_E="--persist-cap 5 --net-cap 10 --cfg slots=1,rounds=1 --clients 1 --crashes 1 --drops 1"
PX_R="--persist-cap 6 --net-cap 14 --cfg nodes=4,slots=2,rounds=2,candidates=1,change=c --clients 3 --crashes 1"
PX_INV=OneValuePerSlot,Agreement,OneValuePerBallot,LearnedChosen,RefusedNeverChosen,AnsweredOnce
fail=0

# bounds | distinct without symmetry (mc-bug-gate.sh) | with | witnesses.
# Rows B (two candidates of three nodes) and R (every non-candidate in a
# class of its own: node 1 a voter outside change=c, node 2 a voter in it,
# node 3 a new voter) have a trivial group, so their counts do not move.
CORRECT="
$PX_A|2017514|1017203|ReachChosen,ReachSecondLeader,ReachRefused
$PX_B|4501801|4501801|ReachChosen,ReachAmbiguous,ReachRefused,ReachRecovered
$PX_C|53229|27928|ReachChosen,ReachRefused
$PX_D|600110|305148|ReachChosen,ReachSecondLeader,ReachRefused
$PX_R|4187917|4187917|ReachChosen,ReachSecondLeader,ReachRefused,ReachReconfigured
"
while IFS='|' read -r bounds full want wits; do
	[ -z "$bounds" ] && continue
	log=$OUT/correct-$(echo "$bounds" | tr -c 'a-z0-9' '_').txt
	# shellcheck disable=SC2086
	"$B"/pgchain_mc --model paxos --inv $PX_INV --prop "ChosenStable,$wits" --progress 0 \
		--symmetry on --sym-check $bounds > "$log" 2>&1
	got=$(grep -oE '[0-9]+ distinct states' "$log" | grep -oE '^[0-9]+')
	wit_ok=1
	for wname in ${wits//,/ }; do
		grep -q "Witness $wname: reached" "$log" || wit_ok=0
	done
	if grep -q 'No error has been found' "$log" && [ $wit_ok = 1 ] && [ "$got" = "$want" ]; then
		echo "ok    paxos $bounds: PASS, $got distinct with symmetry ($full without)"
	else
		echo "FAIL  paxos $bounds: expected PASS with $want distinct, got ${got:-?} (see $log)"
		fail=1
	fi
done <<< "$CORRECT"

# bug | property it breaks | bounds (pg_chain's gate)
BUGS="
101|LearnedChosen|$PX_B
102|LearnedChosen|$PX_A
103|LearnedChosen|$PX_A
104|OneValuePerBallot|$PX_A
105|LearnedChosen|$PX_A
106|LearnedChosen|$PX_B
107|ChosenStable|$PX_R
108|LearnedChosen|$PX_E
109|LearnedChosen|$PX_R
110|ChosenStable|$PX_R
"
while IFS='|' read -r bug prop bounds; do
	[ -z "$bug" ] && continue
	case $prop in ChosenStable) sel="--prop $prop" ;; *) sel="--inv $prop" ;; esac
	for s in off on; do
		log=$OUT/bug$bug-$s.txt
		# shellcheck disable=SC2086
		"$B"/pgchain_mc-bug$bug --model paxos $sel --progress 0 --symmetry $s $bounds > "$log" 2>&1
	done
	d_off=$(grep -oE 'search is [0-9]+' "$OUT/bug$bug-off.txt" | grep -oE '[0-9]+')
	d_on=$(grep -oE 'search is [0-9]+' "$OUT/bug$bug-on.txt" | grep -oE '[0-9]+')
	if grep -qE "(Invariant|Action property) $prop is violated" "$OUT/bug$bug-on.txt" &&
		grep -qE "(Invariant|Action property) $prop is violated" "$OUT/bug$bug-off.txt" &&
		[ "$d_on" = "$d_off" ]; then
		echo "ok    paxos bug$bug caught by $prop at depth $d_on with symmetry and without"
	else
		echo "HOLE  paxos bug$bug: $prop with symmetry at depth ${d_on:-?}, without ${d_off:-?} (see $OUT)"
		fail=1
	fi
done <<< "$BUGS"
exit $fail
