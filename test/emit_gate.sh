#!/usr/bin/env bash
# emit_gate.sh CMC: the TLA+ bridge (Milestone D).  For each .cml spec,
# `cmc --emit-tla` must produce a module that TLC accepts and that gives
# the same verdict as `cmc --spec` -- so the abstract model reaches TLC,
# Apalache and TLAPS unchanged.  Needs nix (tlc); run under `nix develop`.
set -u
CMC=${1:?usage: emit_gate.sh CMC}
HERE=$(cd "$(dirname "$0")/.." && pwd)
TLC="$HERE/calib/hc/tlc.sh"
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/emit-gate
mkdir -p "$OUT"
fail=0

# row: DIR | CML | MODULE | CFGLINE (TLC constants) | INV | want | define
rows="
test/spec|counter|counter|CONSTANT N = 3|in_range|PASS|
examples/spec|paxos|paxos|CONSTANTS V = 2  A = 3  B = 2  Q = 2|agreement|PASS|
examples/spec|paxos|paxos|CONSTANTS V = 2  A = 3  B = 2  Q = 2|agreement|VIOLATION|--define BUG
examples/spec|raft|raft|CONSTANTS S = 3  Q = 2|election_safety|PASS|
examples/spec|raft|raft|CONSTANTS S = 3  Q = 2|election_safety|VIOLATION|--define BUG
examples/spec|hc|hc|CONSTANTS V = 2  P = 3  Q = 2|agreement|PASS|
examples/spec|hc|hc|CONSTANTS V = 2  P = 3  Q = 2|agreement|VIOLATION|--define BUG
examples/spec|zab|zab|CONSTANTS P = 3  Q = 2|durable|PASS|
examples/spec|zab|zab|CONSTANTS P = 3  Q = 2|durable|VIOLATION|--define BUG
examples/spec|fpaxos|fpaxos|CONSTANTS V = 2  A = 5  Q1 = 4  Q2 = 2|agreement|PASS|
examples/spec|fpaxos|fpaxos|CONSTANTS V = 2  A = 5  Q1 = 4  Q2 = 2|agreement|VIOLATION|--define BUG
examples/spec|caspaxos|caspaxos|CONSTANTS V = 2  A = 3  Q = 2|agreement|PASS|
examples/spec|caspaxos|caspaxos|CONSTANTS V = 2  A = 3  Q = 2|agreement|VIOLATION|--define BUG
examples/spec|twophase|twophase|CONSTANT N = 3|consistency|PASS|
examples/spec|twophase|twophase|CONSTANT N = 3|consistency|VIOLATION|--define BUG
examples/spec|fastpaxos|fastpaxos|CONSTANTS V = 2  A = 5  QC = 3  F = 4|agreement|PASS|
examples/spec|fastpaxos|fastpaxos|CONSTANTS V = 2  A = 5  QC = 3  F = 4|agreement|VIOLATION|--define BUG
"
check() {
	dir=$1 cml=$2 mod=$3 cfgline=$4 inv=$5 want=$6 def=$7
	# the emitted module is named after the .cml `module` line; write it to
	# <module>.tla so TLC's filename==module rule is satisfied
	d="$OUT/$mod-$want"; mkdir -p "$d"
	# shellcheck disable=SC2086
	"$CMC" --emit-tla "$HERE/$dir/$cml.cml" $def > "$d/$mod.tla" 2>&1
	printf 'SPECIFICATION Spec\n%s\nINVARIANT %s\nCHECK_DEADLOCK FALSE\n' "$cfgline" "$inv" > "$d/$mod.cfg"
	bash "$TLC" "$d/$mod.tla" "$d/$mod.cfg" -workers 1 > "$d/out.txt" 2>&1
	if grep -qiE 'Parse Error|semantic analysis failed' "$d/out.txt"; then
		echo "FAIL $cml [$want]: TLC rejected the generated TLA+"; sed 's/^/     /' "$d/out.txt" | grep -iE 'error|line' | head -4; echo x>"$OUT/fail"; return
	fi
	v=$(grep -c 'is violated' "$d/out.txt")
	if [ "$want" = VIOLATION ]; then
		[ "$v" -ge 1 ] && echo "ok   $cml [$want]: generated TLA+ violates in TLC" || { echo "FAIL $cml [$want]: expected a violation"; echo x>"$OUT/fail"; }
	else
		[ "$v" = 0 ] && grep -q 'No error' "$d/out.txt" && echo "ok   $cml [$want]: generated TLA+ holds in TLC" || { echo "FAIL $cml [$want]: expected no error"; echo x>"$OUT/fail"; }
	fi
}

echo "-- the TLA+ bridge: cmc --emit-tla produces TLC-checkable modules"
rm -f "$OUT/fail"
printf '%s\n' "$rows" | while IFS='|' read -r dir cml mod cfgline inv want def; do
	[ -z "$cml" ] && continue
	check "$dir" "$cml" "$mod" "$cfgline" "$inv" "$want" "$def"
done

# liveness: the generated TLA+ carries WF fairness and a temporal PROPERTY;
# TLC must agree with cmc --spec (ticker holds; ticker+RESET violates)
checkprop() {
	dir=$1 cml=$2 mod=$3 cfgline=$4 prop=$5 want=$6 def=$7
	d="$OUT/$mod-prop-$want"; mkdir -p "$d"
	# shellcheck disable=SC2086
	"$CMC" --emit-tla "$HERE/$dir/$cml.cml" $def > "$d/$mod.tla" 2>&1
	# the module name must match the filename for TLC
	sed -i "s/MODULE $cml /MODULE $mod /" "$d/$mod.tla"
	printf 'SPECIFICATION Spec\n%s\nPROPERTY %s\nCHECK_DEADLOCK FALSE\n' "$cfgline" "$prop" > "$d/$mod.cfg"
	bash "$TLC" "$d/$mod.tla" "$d/$mod.cfg" -workers 1 > "$d/out.txt" 2>&1
	if grep -qiE 'Parse Error|semantic analysis failed' "$d/out.txt"; then echo "FAIL $cml prop [$want]: TLC rejected the TLA+"; echo x>"$OUT/fail"; return; fi
	v=$(grep -c 'Temporal properties were violated' "$d/out.txt")
	if [ "$want" = VIOLATION ]; then
		[ "$v" -ge 1 ] && echo "ok   $cml prop [$want]: TLC violates the temporal property" || { echo "FAIL $cml prop [$want]: expected a temporal violation"; echo x>"$OUT/fail"; }
	else
		[ "$v" = 0 ] && echo "ok   $cml prop [$want]: TLC holds the temporal property" || { echo "FAIL $cml prop [$want]: expected it to hold"; echo x>"$OUT/fail"; }
	fi
}
checkprop examples/spec ticker ticker "CONSTANT N = 3" reaches_max PASS ""
checkprop examples/spec ticker tickerb "CONSTANT N = 3" reaches_max VIOLATION "--define RESET"

[ -f "$OUT/fail" ] && fail=1
[ $fail = 0 ] && echo "emit_gate: ok (generated TLA+ parses and agrees with cmc --spec)" || echo "emit_gate: FAILED"
exit $fail
