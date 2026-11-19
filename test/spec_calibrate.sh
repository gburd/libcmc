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
# TLC is optional: without a java the checks cannot run, so say so
# (tlc.sh uses its pinned java, or finds one through nix)
java=$(sed -n 's/^JAVA=//p' "$TLC")
if [ ! -x "$java" ] && ! command -v nix >/dev/null; then
	echo "spec_calibrate: skipped (no TLC: neither the pinned java nor nix is available)"
	exit 0
fi

# row: DIR | CML [--define TAG] | TLA | CFG | want-verdict(PASS|VIOLATION)
rows="
test/spec|counter.cml||counter.tla|counter.cfg|PASS
test/spec|mutex.cml||mutex.tla|mutex_safe.cfg|PASS
test/spec|mutex.cml|--define BUG|mutex.tla|mutex_bug.cfg|VIOLATION
test/spec|guard.cml||guard.tla|guard_safe.cfg|PASS
test/spec|guard.cml|--define BUG|guard.tla|guard_bug.cfg|VIOLATION
examples/spec|paxos.cml||paxos.tla|paxos_safe.cfg|PASS
examples/spec|paxos.cml|--define BUG|paxos.tla|paxos_bug.cfg|VIOLATION
examples/spec|fpaxos.cml||fpaxos.tla|fpaxos_safe.cfg|PASS
examples/spec|fpaxos.cml|--define BUG|fpaxos.tla|fpaxos_bug.cfg|VIOLATION
examples/spec|caspaxos.cml||caspaxos.tla|caspaxos_safe.cfg|PASS
examples/spec|caspaxos.cml|--define BUG|caspaxos.tla|caspaxos_bug.cfg|VIOLATION
examples/spec|zab.cml||zab.tla|zab_safe.cfg|PASS
examples/spec|zab.cml|--define BUG|zab.tla|zab_bug.cfg|VIOLATION
examples/spec|fastpaxos.cml||fastpaxos.tla|fastpaxos_view.cfg|PASS
examples/spec|fastpaxos.cml|--define BUG|fastpaxos.tla|fastpaxos_bug.cfg|VIOLATION
"
# fastpaxos.tla keeps each round 2 promise's sender, which the .cml
# counts away; fastpaxos_view.cfg checks it under VIEW Abstract, the
# .cml's state, so the distinct-state counts are comparable (without
# the VIEW, TLC finds 2112 states at the same depth, 12).

num() { grep -oE "$2" "$1" | grep -oE '[0-9]+' | head -1; }
printf '%s\n' "$rows" | while IFS='|' read -r dir cml def tla cfg want; do
	[ -z "$cml" ] && continue
	co="$OUT/${cml%.cml}-${cfg%.cfg}.cmc.txt"
	to="$OUT/${cml%.cml}-${cfg%.cfg}.tlc.txt"
	# shellcheck disable=SC2086
	"$CMC" --spec "$HERE/$dir/$cml" $def > "$co" 2>&1

	bash "$TLC" "$HERE/$dir/$tla" "$HERE/$dir/$cfg" -workers 1 > "$to" 2>&1

	# the summary line: TLC's progress lines print "33,335 distinct
	# states found" with a thousands separator
	c_distinct=$(num "$co" 'generated, [0-9]+ distinct states found')
	t_distinct=$(num "$to" 'generated, [0-9]+ distinct states found')
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
