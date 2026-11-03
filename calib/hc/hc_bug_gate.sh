#!/usr/bin/env bash
# hc_bug_gate.sh: the calibration's own teeth.  Each planted bug in
# calib/hc/hc_core.c (-DCMC_BUG=n) must make check-calibration fail,
# in the way the bug predicts; a bug that still calibrates is a HOLE.
#   101  A21 without "every member answered": code-cp S1 must FAIL
#   102  MajOK ignores the repairing list:   exact PASS counts must move
#   103  sets kept unsorted (non-canonical): distinct counts must grow
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/../.."
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/hc-bug-gate
mkdir -p "$OUT"
CFG=calib/hc/cfg
fail=0
for n in 101 102 103; do
	b=build-bug$n
	make -s B=$b CFLAGS="-O2 -g -DCMC_BUG=$n" $b/cmc > /dev/null || { echo "bug $n: build failed"; fail=1; continue; }
	$b/cmc --model hc --tlc-cfg $CFG/hc_code_cp_n3_S1_AgreementNonNone.cfg --cfg tlc_mult=TRUE \
		--progress 0 --quiet > "$OUT/bug$n.txt" 2>&1
	v=$(grep -oE 'Invariant [A-Za-z0-9_]+ is violated|No error has been found' "$OUT/bug$n.txt" | head -1)
	d=$(grep -oE '[0-9]+ distinct states' "$OUT/bug$n.txt" | grep -oE '^[0-9]+')
	case $n in
		101) [ "$v" = "Invariant S1_AgreementNonNone is violated" ] && r=caught || r=HOLE ;;
		102|103) { [ "$v" = "No error has been found" ] && [ "$d" = 7412120 ]; } && r=HOLE || r=caught ;;
	esac
	[ $r = HOLE ] && fail=1
	echo "bug $n: code-cp S1: ${v:-?}, $d distinct (correct: PASS, 7412120): $r"
done
[ $fail = 0 ] && echo "hc_bug_gate: ok" || echo "hc_bug_gate: FAILED"
exit $fail
