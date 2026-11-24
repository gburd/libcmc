#!/usr/bin/env bash
# hist-gate.sh B: history variables (libcmc 2.4) on pg_chain's Paxos core.
# The paxos_hist model (libcmc's copy only) checks HistChosen, "a value
# chosen at a slot stays safe in every later state", over a history
# variable.  The correct core must pass with ReachChosen reached, and each
# planted bug ChosenStable catches in pg_chain's gate (107, 110) must be
# caught by HistChosen alone.  Logs go to $CMC_SCRATCH.
set -u
B=$1
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/pgchain-hist
mkdir -p "$OUT"
PX_D="--persist-cap 6 --net-cap 12 --cfg slots=2,rounds=2,candidates=1 --clients 3 --crashes 1"
PX_R="--persist-cap 6 --net-cap 14 --cfg nodes=4,slots=2,rounds=2,candidates=1,change=c --clients 3 --crashes 1"
fail=0
# shellcheck disable=SC2086
"$B"/pgchain_mc --model paxos_hist --all-props --progress 0 $PX_D > "$OUT/ok.txt" 2>&1
if grep -q 'No error has been found' "$OUT/ok.txt" && grep -q 'Witness ReachChosen: reached' "$OUT/ok.txt"; then
	echo "ok    paxos_hist correct core: PASS, $(grep -oE '[0-9]+ distinct' "$OUT/ok.txt")"
else
	echo "FAIL  paxos_hist correct core (see $OUT/ok.txt)"
	fail=1
fi
for bug in 107 110; do
	# shellcheck disable=SC2086
	"$B"/pgchain_mc-bug$bug --model paxos_hist --inv HistChosen --progress 0 $PX_R > "$OUT/bug$bug.txt" 2>&1
	if grep -q 'Invariant HistChosen is violated' "$OUT/bug$bug.txt"; then
		echo "ok    paxos_hist bug$bug caught by HistChosen at depth $(grep -oE 'search is [0-9]+' "$OUT/bug$bug.txt" | grep -oE '[0-9]+')"
	else
		echo "HOLE  paxos_hist bug$bug not caught by HistChosen (see $OUT/bug$bug.txt)"
		fail=1
	fi
done
# four BFS workers print what one does, on the correct core
# shellcheck disable=SC2086
"$B"/pgchain_mc --model paxos_hist --all-props --progress 0 $PX_D --workers 4 > "$OUT/ok-w4.txt" 2>&1
if cmp -s <(grep -v '^Finished in' "$OUT/ok.txt") <(grep -v '^Finished in' "$OUT/ok-w4.txt"); then
	echo "ok    paxos_hist correct core: --workers 4 prints the same"
else
	echo "FAIL  paxos_hist correct core: --workers 4 differs (see $OUT/ok-w4.txt)"
	fail=1
fi
exit $fail
