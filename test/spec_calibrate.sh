#!/usr/bin/env bash
# spec_calibrate.sh CMC: run cmc --spec and single-worker TLC on each
# .cml fixture with a hand-written .tla twin, and check they agree on the
# verdict, the distinct-state count and the shortest-counterexample
# depth.  This is the Milestone B calibration (docs/plans/tlaplus-scope.md):
# the abstract-model checker is tied to TLC the way calib/hc ties the
# in-process engine to TLC.  Run under `nix develop` (needs tlc).
set -u
CMC=${1:?usage: spec_calibrate.sh CMC}
HERE=$(cd "$(dirname "$0")/.." && pwd)
TLC="$HERE/calib/hc/tlc.sh"
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/spec-calib
mkdir -p "$OUT"
fail=0

# row: CML [--define TAG] | TLA | CFG | want-verdict(PASS|VIOLATION)
rows="
counter.cml||counter.tla|counter.cfg|PASS
mutex.cml||mutex.tla|mutex_safe.cfg|PASS
mutex.cml|--define BUG|mutex.tla|mutex_bug.cfg|VIOLATION
"

num() { grep -oE "$2" "$1" | grep -oE '[0-9]+' | head -1; }
printf '%s\n' "$rows" | while IFS='|' read -r cml def tla cfg want; do
	[ -z "$cml" ] && continue
	co="$OUT/${cml%.cml}-${cfg%.cfg}.cmc.txt"
	to="$OUT/${cml%.cml}-${cfg%.cfg}.tlc.txt"
	# shellcheck disable=SC2086
	"$CMC" --spec "$HERE/test/spec/$cml" $def > "$co" 2>&1

	bash "$TLC" "$HERE/test/spec/$tla" "$HERE/test/spec/$cfg" -workers 1 > "$to" 2>&1

	c_distinct=$(num "$co" '[0-9]+ distinct states found')
	t_distinct=$(num "$to" '[0-9]+ distinct states found')
	c_depth=$(num "$co" 'complete state graph search is [0-9]+')
	t_depth=$(num "$to" 'complete state graph search is [0-9]+')
	c_viol=$(grep -c 'is violated' "$co")
	t_viol=$(grep -c 'is violated' "$to")

	ok=1
	if [ "$want" = VIOLATION ]; then
		[ "$c_viol" -ge 1 ] && [ "$t_viol" -ge 1 ] || ok=0
		# shortest-trace depth: cmc "(K actions)" vs TLC counterexample states
		c_trace=$(grep -oE 'is \([0-9]+ actions\)' "$co" | grep -oE '[0-9]+')
		t_trace=$(($(grep -cE '^State [0-9]+:' "$to") - 1))
		[ -n "$c_trace" ] && [ "$c_trace" = "$t_trace" ] || ok=0
		echo "$cml [$want]: cmc trace=$c_trace tlc trace=$t_trace -> $([ $ok = 1 ] && echo MATCH || echo MISMATCH)"
	else
		[ "$c_viol" = 0 ] && [ "$t_viol" = 0 ] || ok=0
		[ -n "$c_distinct" ] && [ "$c_distinct" = "$t_distinct" ] || ok=0
		[ -n "$c_depth" ] && [ "$c_depth" = "$t_depth" ] || ok=0
		echo "$cml [$want]: cmc d=$c_distinct depth=$c_depth | tlc d=$t_distinct depth=$t_depth -> $([ $ok = 1 ] && echo MATCH || echo MISMATCH)"
	fi
	[ $ok = 1 ] || { echo "spec_calibrate: MISMATCH on $cml/$cfg"; echo "FAILMARKER" > "$OUT/fail"; }
done

if [ -f "$OUT/fail" ]; then rm -f "$OUT/fail"; echo "spec_calibrate: FAILED"; exit 1; fi
echo "spec_calibrate: ok (cmc --spec agrees with single-worker TLC)"
