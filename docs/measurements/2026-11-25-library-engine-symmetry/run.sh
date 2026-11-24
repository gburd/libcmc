#!/usr/bin/env bash
# sym-meas.sh B OUT: PX rows of pg_chain's gate, symmetry off / on / on+check
set -u
B=$1 OUT=$2
mkdir -p "$OUT"
PX_INV=OneValuePerSlot,Agreement,OneValuePerBallot,LearnedChosen,RefusedNeverChosen,AnsweredOnce
rows="
A|--persist-cap 6 --net-cap 12 --cfg slots=1,rounds=3,candidates=1 --clients 3 --crashes 2|ReachChosen,ReachSecondLeader,ReachRefused
B|--persist-cap 4 --net-cap 10 --cfg slots=1,rounds=1 --clients 1 --crashes 1|ReachChosen,ReachAmbiguous,ReachRefused,ReachRecovered
C|--persist-cap 6 --net-cap 12 --cfg slots=2,candidates=1 --clients 2 --drops 1|ReachChosen,ReachRefused
D|--persist-cap 6 --net-cap 12 --cfg slots=2,rounds=2,candidates=1 --clients 3 --crashes 1|ReachChosen,ReachSecondLeader,ReachRefused
R|--persist-cap 6 --net-cap 14 --cfg nodes=4,slots=2,rounds=2,candidates=1,change=c --clients 3 --crashes 1|ReachChosen,ReachSecondLeader,ReachRefused,ReachReconfigured
V|--persist-cap 6 --net-cap 14 --cfg nodes=5,voters=5,q1=3,q2=3,slots=1,rounds=2,candidates=1 --clients 2 --crashes 1|ReachChosen,ReachSecondLeader,ReachRefused
"
while IFS='|' read -r row bounds wits; do
	[ -z "$row" ] && continue
	for mode in off on check; do
		extra="--symmetry off"
		[ $mode = on ] && extra="--symmetry on"
		[ $mode = check ] && extra="--symmetry on --sym-check"
		# shellcheck disable=SC2086
		/usr/bin/time -f "wall %e s, max RSS %M kB" "$B"/pgchain_mc --model paxos --inv $PX_INV \
			--prop "ChosenStable,$wits" --progress 0 $extra $bounds > "$OUT/px$row-$mode.txt" 2>&1
		echo "$row $mode: $(grep -E 'No error|is violated|distinct states|depth of|wall' "$OUT/px$row-$mode.txt" | tr '\n' ' ')"
	done
done <<< "$rows"
