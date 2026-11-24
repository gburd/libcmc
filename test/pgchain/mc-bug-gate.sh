#!/usr/bin/env bash
# mc-bug-gate.sh: pg_chain's protocol cores under libcmc (PLAN section 11).
#   1. every correct core passes its properties at the gate's bounds, and
#      reaches every witness (each outcome it claims to handle happens);
#   2. every planted bug (-DPGCHAIN_MC_BUG=n in the shipped source) is
#      reported with the property it breaks.  A bug that passes is a HOLE.
# The gate is exhaustive BFS.  Note that uniform random walks (--simulate)
# missed bug 1 in 4 x 200,000 walks: it needs the tail to flush s=1 after
# receiving s=2 and before flushing it, a narrow schedule.  Simulation is
# for depth beyond the exhaustive bounds, never a substitute for them.
# Binaries come from make check-mc (MC_B, default test/mc/build); logs go
# to $PGCHAIN_MC_OUT (default ~/tmp/pgchain-mc), never /tmp.
set -u
B=${MC_B:-test/mc/build}
OUT=${PGCHAIN_MC_OUT:-$HOME/tmp/pgchain-mc}/$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$OUT"
ENV="--persist-cap 4 --net-cap 12 --cfg reconfigs=1 --crashes 1 --clients 2"
SAFETY_INV=NoLostAck,AckedEverywhere,DefiniteAbort,PrefixOrder,LogContiguous,AnsweredOnce
SAFETY_ACT=WedgeAfterAmbiguity,SealedNoNewS
PX_INV=OneValuePerSlot,Agreement,OneValuePerBallot,LearnedChosen,RefusedNeverChosen,AnsweredOnce
PX_ACT=ChosenStable
# paxos: one candidate through three rounds and two crashes (A); two
# candidates racing for one round each with a crash (B); one candidate,
# two slots, a connection reset (C); one candidate, two slots, two rounds
# and a crash (D); a voter change from {0,1,2} to {2,3} with two rounds
# and a crash (R).  PX_E, two candidates with a crash and a reset,
# catches bug 108 in under a minute but takes minutes to pass, so its
# correct-core row runs in check-mc-slow, with two candidates through two
# rounds.
PX_A="--persist-cap 6 --net-cap 12 --cfg slots=1,rounds=3,candidates=1 --clients 3 --crashes 2"
PX_B="--persist-cap 4 --net-cap 10 --cfg slots=1,rounds=1 --clients 1 --crashes 1"
PX_C="--persist-cap 6 --net-cap 12 --cfg slots=2,candidates=1 --clients 2 --drops 1"
PX_D="--persist-cap 6 --net-cap 12 --cfg slots=2,rounds=2,candidates=1 --clients 3 --crashes 1"
PX_E="--persist-cap 5 --net-cap 10 --cfg slots=1,rounds=1 --clients 1 --crashes 1 --drops 1"
PX_R="--persist-cap 6 --net-cap 14 --cfg nodes=4,slots=2,rounds=2,candidates=1,change=c --clients 3 --crashes 1"
fail=0

# model | bounds | expected distinct states (exact: a change means the core
# or the engine changed, so look at why before updating it) | witnesses
# that must be reached ("all": every property, witnesses included)
CM_W=Reach,ReachError,ReachAmbiguous,ReachAmbiguousCommitted
CORRECT="
chain_member|--persist-cap 4 --clients 1|76|$CM_W
chain_member|--persist-cap 4 --clients 2|2171|all
chain_member|--persist-cap 4 --crashes 1|1212|$CM_W
chain_member|--persist-cap 4 --drops 1|356|$CM_W
chain_member|--persist-cap 4 --crashes 1 --drops 1 --clients 2|159439|all
chain_member|--persist-cap 4 --cfg members=4 --crashes 1 --drops 1 --clients 2|2033788|all
chain_member|--persist-cap 4 --net-cap 12 --cfg reconfigs=1 --crashes 1 --clients 2|8166198|all
chain_member|--persist-cap 4 --net-cap 12 --cfg reconfigs=1 --drops 1 --clients 2|2579050|$CM_W,ReachRefused,ReachReconfig
chain_member|--persist-cap 4 --net-cap 12 --cfg reconfigs=1 --crashes 1 --drops 1 --clients 1|3034658|$CM_W,ReachReconfig,ReachCommitAfterReconfig
chain_member|--persist-cap 5 --net-cap 16 --cfg reconfigs=3 --crashes 1 --clients 1|10086243|$CM_W,ReachReconfig,ReachCommitAfterReconfig,ReachRejoined
paxos|$PX_A|2017514|ReachChosen,ReachSecondLeader,ReachRefused
paxos|$PX_B|4501801|ReachChosen,ReachAmbiguous,ReachRefused,ReachRecovered
paxos|$PX_C|53229|ReachChosen,ReachRefused
paxos|$PX_D|600110|ReachChosen,ReachSecondLeader,ReachRefused
paxos|$PX_R|4187917|ReachChosen,ReachSecondLeader,ReachRefused,ReachReconfigured
"
[ "${PGCHAIN_MC_SLOW:-0}" = 1 ] && CORRECT="$CORRECT
paxos|$PX_E|76949120|ReachChosen,ReachAmbiguous,ReachRefused,ReachRecovered
paxos|--persist-cap 6 --net-cap 12 --cfg slots=1,rounds=2 --clients 1|28813545|ReachChosen,ReachSecondLeader,ReachAmbiguous,ReachRefused,ReachRecovered
"
while IFS='|' read -r model bounds want wits; do
	[ -z "$model" ] && continue
	log=$OUT/correct-$model-$(echo "$bounds" | tr -c 'a-z0-9' '_').txt
	if [ "$wits" = all ]; then
		props=--all-props
	elif [ "$model" = paxos ]; then
		props="--inv $PX_INV --prop $PX_ACT,$wits,EventuallyDecided"
	else
		props="--inv $SAFETY_INV --prop $SAFETY_ACT,$wits,EventuallyReplied"
	fi
	# shellcheck disable=SC2086
	"$B"/pgchain_mc --model "$model" $props --progress 0 $bounds > "$log" 2>&1
	got=$(grep -oE '[0-9]+ distinct states' "$log" | grep -oE '^[0-9]+')
	wit_ok=1
	if [ "$wits" = all ]; then
		grep -q 'NOT reached' "$log" && wit_ok=0
	else
		for wname in ${wits//,/ }; do
			grep -q "Witness $wname: reached" "$log" || wit_ok=0
		done
	fi
	if grep -q 'No error has been found' "$log" && [ $wit_ok = 1 ] && [ "$got" = "$want" ]; then
		echo "ok    $model $bounds: PASS, $got distinct"
	else
		echo "FAIL  $model $bounds: expected PASS with $want distinct, got ${got:-?} (see $log)"
		fail=1
	fi
done <<< "$CORRECT"

# binary | model | property it must break | bounds.  The binary is a
# planted bug (bugN) or the correct core under a configuration the design
# forbids (ok).  Each property is checked alone, so the gate shows that it
# catches the bug, not that some other one trips first.
BUGS="
bug1|chain_member|NoLostAck|$ENV
bug2|chain_member|AckedEverywhere|$ENV
bug3|chain_member|NoLostAck|$ENV
bug4|chain_member|LogContiguous|$ENV
bug5|chain_member|DefiniteAbort|$ENV
bug6|chain_member|WedgeAfterAmbiguity|$ENV
bug7|chain_member|DefiniteAbort|$ENV
bug8|chain_member|NoLostAck|$ENV
bug9|chain_member|NoLostAck|--persist-cap 5 --net-cap 16 --cfg reconfigs=3 --crashes 1 --clients 1
bug10|chain_member|PrefixOrder|--persist-cap 4 --net-cap 12 --cfg reconfigs=1 --crashes 1 --drops 1 --clients 1
bug101|paxos|LearnedChosen|$PX_B
bug102|paxos|LearnedChosen|$PX_A
bug103|paxos|LearnedChosen|$PX_A
bug104|paxos|OneValuePerBallot|$PX_A
bug105|paxos|LearnedChosen|$PX_A
bug106|paxos|LearnedChosen|$PX_B
bug107|paxos|ChosenStable|$PX_R
bug108|paxos|LearnedChosen|$PX_E
bug109|paxos|LearnedChosen|$PX_R
bug110|paxos|ChosenStable|$PX_R
ok|paxos|OneValuePerSlot|--persist-cap 4 --net-cap 10 --cfg slots=1,rounds=1,q2=1 --clients 1
ok|paxos|LearnedChosen|--persist-cap 4 --net-cap 10 --cfg slots=1,rounds=1,q1=1 --clients 1
"
while IFS='|' read -r bin model prop env; do
	[ -z "$bin" ] && continue
	log=$OUT/$bin-$model-$prop-$(echo "$env" | tr -c 'a-z0-9' '_').txt
	case $prop in WedgeAfterAmbiguity|SealedNoNewS|ChosenStable) sel="--prop $prop" ;; *) sel="--inv $prop" ;; esac
	exe=$B/pgchain_mc-$bin
	[ "$bin" = ok ] && exe=$B/pgchain_mc
	what="$bin"
	[ "$bin" = ok ] && what="ok ($(echo "$env" | grep -oE 'q[12]=[0-9]'))"
	# shellcheck disable=SC2086
	"$exe" --model "$model" $sel --progress 0 $env > "$log" 2>&1
	if grep -qE "(Invariant|Action property) $prop is violated" "$log"; then
		echo "ok    $model $what caught by $prop at depth $(grep -oE 'search is [0-9]+' "$log" | grep -oE '[0-9]+')"
	else
		echo "HOLE  $model $what not caught by $prop (see $log)"
		fail=1
	fi
done <<< "$BUGS"
[ $fail = 0 ] && echo "mc-bug-gate: ok (logs in $OUT)" || echo "mc-bug-gate: FAILED (logs in $OUT)"
exit $fail
