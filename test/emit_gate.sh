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

# verdict FILE: what a cmc --spec or TLC run concluded
verdict() {
	if grep -q 'Deadlock reached' "$1"; then echo DEADLOCK
	elif grep -q 'Invariant .* is violated' "$1"; then echo VIOLATION
	elif grep -q 'Temporal properties were violated\|Property .* is violated' "$1"; then echo TEMPORAL
	elif grep -q 'No error has been found' "$1"; then echo PASS
	else echo ERROR; fi
}
num() { grep -oE "$2" "$1" | grep -oE '[0-9]+' | head -1; }

# check DIR CML MODULE WANT [ARG...] (DIR absolute or under the repo): `cmc --emit-tla` and
# `cmc --emit-cfg` give a module and a .cfg that TLC accepts and that
# give the verdict WANT, as `cmc --spec` does; on a pass, TLC also finds
# the same distinct states and depth (ND, when set, is the distinct
# count TLC must find instead of cmc's).  The .cfg names every invariant,
# property and constraint, and CHECK_DEADLOCK FALSE when the model turns
# the deadlock check off.
check() {
	dir=$1 cml=$2 mod=$3 want=$4; shift 4
	d="$OUT/$mod-$want"; mkdir -p "$d"
	case $dir in /*) src=$dir/$cml.cml ;; *) src=$HERE/$dir/$cml.cml ;; esac
	"$CMC" --emit-tla "$src" "$@" > "$d/$mod.tla" 2>&1
	"$CMC" --emit-cfg "$src" "$@" > "$d/$mod.cfg" 2>&1
	"$CMC" --spec "$src" "$@" > "$d/cmc.txt" 2>&1
	# the module name must match the filename for TLC
	sed -i "s/MODULE $cml /MODULE $mod /" "$d/$mod.tla"
	bash "$TLC" "$d/$mod.tla" "$d/$mod.cfg" -workers 1 > "$d/out.txt" 2>&1
	if grep -qiE 'Parse Error|semantic analysis failed' "$d/out.txt"; then
		echo "FAIL $mod [$want]: TLC rejected the generated TLA+"; sed 's/^/     /' "$d/out.txt" | grep -iE 'error|line' | head -4; echo x>"$OUT/fail"; return
	fi
	cv=$(verdict "$d/cmc.txt") tv=$(verdict "$d/out.txt")
	if [ "$cv" != "$want" ] || [ "$tv" != "$want" ]; then
		echo "FAIL $mod [$want]: cmc --spec says $cv, TLC says $tv"; echo x>"$OUT/fail"; return
	fi
	if [ "$want" = PASS ]; then
		cd_=${ND:-$(num "$d/cmc.txt" 'generated, [0-9]+ distinct')} td=$(num "$d/out.txt" 'generated, [0-9]+ distinct')
		cp_=$(num "$d/cmc.txt" 'search is [0-9]+') tp=$(num "$d/out.txt" 'search is [0-9]+')
		if [ -z "$cd_" ] || [ "$cd_" != "$td" ] || [ "$cp_" != "$tp" ]; then
			echo "FAIL $mod [$want]: cmc d=$cd_ depth=$cp_, TLC d=$td depth=$tp"; echo x>"$OUT/fail"; return
		fi
		echo "ok   $mod [$want]: TLC agrees (d=$td depth=$tp)"
	else
		echo "ok   $mod [$want]: TLC agrees"
	fi
}

echo "-- the TLA+ bridge: cmc --emit-tla produces TLC-checkable modules"
rm -f "$OUT/fail"
check test/spec counter counter PASS
check examples/spec paxos paxos PASS
check examples/spec paxos paxosb VIOLATION --define BUG
check examples/spec raft raft PASS
check examples/spec raft raftb VIOLATION --define BUG
check examples/spec hc hc PASS
check examples/spec hc hcb VIOLATION --define BUG
check examples/spec zab zab PASS
check examples/spec zab zabb VIOLATION --define BUG
check examples/spec fpaxos fpaxos PASS
check examples/spec fpaxos fpaxosb VIOLATION --define BUG
check examples/spec caspaxos caspaxos PASS
check examples/spec caspaxos caspaxosb VIOLATION --define BUG
check examples/spec twophase twophase PASS
check examples/spec twophase twophaseb VIOLATION --define BUG
check examples/spec fastpaxos fastpaxos PASS
check examples/spec fastpaxos fastpaxosb VIOLATION --define BUG
check examples/spec logrep logrep PASS
check examples/spec logrep logrepb VIOLATION --define BUG
check test/spec distinct distinct PASS
# under symmetry cmc --spec finds 278 states; the emitted module has no
# SYMMETRY, so TLC must find the 357 states cmc finds without it
ND=357 check test/spec symrec symrec PASS
check test/spec countops countops PASS
check test/spec guard guard PASS
check test/spec guard guardb VIOLATION --define BUG
# the deadlock check and TLC's CONSTRAINT and ACTION_CONSTRAINT, with
# and without each (test/spec/deadlock*.cml, constrain*.cml)
check test/spec deadlock deadlock DEADLOCK
check test/spec deadlock deadlocknd PASS --no-deadlock
check test/spec deadlock_off deadlock_off PASS
check test/spec dlorder dlorder VIOLATION
# a model with no action deadlocks at once, in TLC as in cmc --spec (the
# emitted Next is FALSE, not a stutter)
printf 'module nostep\nvar x : 0 .. 1\ninit x == 0\ninvariant ok: x == 0\n' > "$OUT/nostep.cml"
check "$OUT" nostep nostep DEADLOCK
check test/spec deadlock_c deadlock_c PASS
check test/spec deadlock_ac deadlock_ac PASS
check test/spec constrain constrain PASS
check test/spec constrain_c constrain_c PASS
check test/spec constrain_ac constrain_ac PASS
check test/spec constrain_inv constrain_inv VIOLATION
check test/spec constrain_acinv constrain_acinv VIOLATION

# liveness: the generated TLA+ carries WF fairness and a temporal
# PROPERTY; TLC must agree with cmc --spec (ticker holds; ticker+RESET
# violates)
check examples/spec ticker ticker PASS
check examples/spec ticker tickerb TEMPORAL --define RESET
# the soundness probes (remediation 1.7): TLC fixes the verdict that
# test/spec_gate.sh asserts for cmc --spec on the same file
check test/spec/sound dead dead TEMPORAL
check test/spec/sound unfair unfair TEMPORAL
check test/spec/sound once once PASS
check test/spec/sound leads leads TEMPORAL
check test/spec/sound leadscross leadscross PASS
# one of each form the bridge emits (test/spec/shape.cml), an invariant
# and both properties
check test/spec shape shape PASS

[ -f "$OUT/fail" ] && fail=1
[ $fail = 0 ] && echo "emit_gate: ok (generated TLA+ parses and agrees with cmc --spec)" || echo "emit_gate: FAILED"
exit $fail
