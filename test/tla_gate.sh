#!/usr/bin/env bash
# tla_gate.sh BUILDDIR: the TLA+ reader (cmc --spec FILE.tla --config
# FILE.cfg).  For every .cml model in examples/spec and test/spec, with
# and without each of its --define tags, the module `cmc --emit-tla`
# writes, read back with the .cfg `cmc --emit-cfg` writes, must give
# what `cmc --spec` gives on the .cml: the same exit status and the same
# report (verdict, distinct states, depth, trace), all but the header
# line, which counts the actions a --define leaves out, and the count of
# invariants held (the module adds TypeOK).  C-only.
set -u
B=$(cd "${1:?usage: tla_gate.sh BUILDDIR}" && pwd)
HERE=$(cd "$(dirname "$0")/.." && pwd)
CMC=$B/cmc
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/tla-gate
mkdir -p "$OUT"
fail=0 n=0

# trip CML [--define TAG]: the round trip for one model
trip() {
	cml=$1; shift
	base=$(basename "$cml" .cml)
	# TLC's SYMMETRY permutes model values, not an integer range, so
	# --emit-cfg leaves a .cml `symmetry` out: compare with the model
	# without it (its own gate checks the verdict is the same)
	if grep -q '^symmetry' "$cml"; then
		grep -v '^symmetry' "$cml" > "$OUT/nosym-$base.cml"; cml=$OUT/nosym-$base.cml
	fi
	timeout 600 "$CMC" --spec "$cml" "$@" > "$OUT/a.txt" 2>&1; ra=$?
	# --no-deadlock is not a .cml option to translate: --emit-cfg writes
	# CHECK_DEADLOCK FALSE only for `deadlock = off`
	"$CMC" --emit-tla "$cml" "$@" > "$OUT/$base.tla" 2>/dev/null
	"$CMC" --emit-cfg "$cml" "$@" > "$OUT/$base.cfg" 2>/dev/null
	timeout 600 "$CMC" --spec "$OUT/$base.tla" --config "$OUT/$base.cfg" > "$OUT/b.txt" 2>&1; rb=$?
	n=$((n + 1))
	if [ "$ra" = 3 ] && [ "$rb" = 3 ]; then
		echo "ok   $base $*: both refuse (exit 3)"; return
	fi
	# The module --emit-tla writes adds TypeOK, which --emit-cfg lists as
	# an invariant: it holds wherever the .cml's types do, so the
	# "N invariants held" line differs by one and is left out
	if [ "$ra" = "$rb" ] && diff <(tail -n +2 "$OUT/a.txt" | grep -v ' held\.$') <(tail -n +2 "$OUT/b.txt" | grep -v ' held\.$') > "$OUT/d.txt"; then
		echo "ok   $base $*: exit $ra, $(grep -oE '[0-9]+ distinct states found|is \([0-9]+ actions\)|is violated|Deadlock reached' "$OUT/a.txt" | head -2 | tr '\n' ' ')"
	else
		echo "FAIL $base $*: .cml exit $ra, read back from TLA+ exit $rb"
		sed 's/^/     /' "$OUT/d.txt" | head -10; sed 's/^/     /' "$OUT/b.txt" | head -5
		fail=1
	fi
}

echo "-- the TLA+ reader: cmc --emit-tla reads back to the same check"
# test/spec/fair: per-action and strong fairness, action properties;
# test/spec/ind: [inductive] invariants (ASSUME, IndInv and THEOREMs in
# the module, all skipped or checked by the reader)
for cml in "$HERE"/examples/spec/*.cml "$HERE"/test/spec/*.cml "$HERE"/test/spec/fair/*.cml "$HERE"/test/spec/ind/*.cml; do
	trip "$cml"
	for tag in $(sed -n 's/^action .*\[\([A-Za-z0-9_]*\)\].*:.*$/\1/p' "$cml" | sort -u); do
		trip "$cml" --define "$tag"
	done
done
# the form a TLA+ user types, with the files never written
paxos=$HERE/examples/spec/paxos.cml
timeout 600 "$CMC" --spec <("$CMC" --emit-tla "$paxos" --define BUG) --config <("$CMC" --emit-cfg "$paxos" --define BUG) > "$OUT/p.txt" 2>&1
if [ $? = 1 ] && grep -q 'is (12 actions)' "$OUT/p.txt"; then echo "ok   paxos BUG through process substitution"; else echo "FAIL paxos BUG through process substitution"; fail=1; fi

[ $fail = 0 ] || { echo "tla_gate: FAILED"; exit 1; }
echo "tla_gate: ok ($n round trips)"
