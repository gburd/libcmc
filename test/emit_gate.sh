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
	elif grep -q 'Action property .* is violated' "$1"; then echo ACTION
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
	elif [ "$want" = ACTION ]; then
		# the shortest trace to the bad step: cmc's actions, TLC's states
		ct=$(num "$d/cmc.txt" 'is \([0-9]+ actions') tt=$(($(grep -cE '^State [0-9]+:' "$d/out.txt") - 1))
		if [ -z "$ct" ] || [ "$ct" != "$tt" ]; then
			echo "FAIL $mod [$want]: cmc trace=$ct, TLC trace=$tt"; echo x>"$OUT/fail"; return
		fi
		echo "ok   $mod [$want]: TLC agrees (trace=$tt)"
	elif [ "$want" = TEMPORAL ]; then
		# the lasso: as many states, and the same state the cycle
		# goes back to (or a stutter), in cmc's print as in TLC's.
		# cmc's lasso is a valid fair one but not TLC's in general;
		# on these rows both searches are deterministic and agree,
		# so a change that moves either is flagged here
		shape='^(State [0-9]+: Stuttering|State [0-9]+|Back to state [0-9]+)'
		cl=$(grep -oE "$shape" "$d/cmc.txt" | tr '\n' ' ') tl=$(grep -oE "$shape" "$d/out.txt" | tr '\n' ' ')
		if [ -z "$tl" ] || [ "$cl" != "$tl" ]; then
			echo "FAIL $mod [$want]: cmc lasso [$cl], TLC lasso [$tl]"; echo x>"$OUT/fail"; return
		fi
		echo "ok   $mod [$want]: TLC agrees (lasso $(echo "$tl" | sed 's/State //g'))"
	else
		echo "ok   $mod [$want]: TLC agrees"
	fi
}

# cov DIR CML MODULE GEN [ARG...]: on a complete run, cmc --spec
# --coverage reports for each action the distinct:generated counts TLC's
# -coverage reports for the emitted module.  With GEN=distinct only the
# distinct counts are compared: TLC counts an action's successors once
# per witness of an existential quantifier in its guard, cmc once.
cov() {
	dir=$1 cml=$2 mod=$3 gen=$4; shift 4
	d="$OUT/cov-$mod"; mkdir -p "$d"
	src=$HERE/$dir/$cml.cml
	"$CMC" --emit-tla "$src" "$@" > "$d/$mod.tla" 2>&1
	"$CMC" --emit-cfg "$src" "$@" > "$d/$mod.cfg" 2>&1
	"$CMC" --spec "$src" --coverage "$@" > "$d/cmc.txt" 2>&1
	sed -i "s/MODULE $cml /MODULE $mod /" "$d/$mod.tla"
	bash "$TLC" "$d/$mod.tla" "$d/$mod.cfg" -workers 1 -coverage 1 > "$d/out.txt" 2>&1
	# "<A line ...>: d:g" in TLC, "<A>: d:g  (enabled ...)" in cmc
	sed -n 's/^<\([A-Za-z0-9_]*\) line [^>]*>: \([0-9]*\):\([0-9]*\)$/\1 \2 \3/p' "$d/out.txt" | grep -v '^Init ' > "$d/tlc.cov"
	sed -n 's/^<\([A-Za-z0-9_]*\)>: \([0-9]*\):\([0-9]*\)  (.*/\1 \2 \3/p' "$d/cmc.txt" > "$d/cmc.cov"
	if [ "$gen" = distinct ]; then
		cut -d' ' -f1,2 "$d/tlc.cov" > "$d/t" && cut -d' ' -f1,2 "$d/cmc.cov" > "$d/c"
	else cp "$d/tlc.cov" "$d/t" && cp "$d/cmc.cov" "$d/c"; fi
	if [ ! -s "$d/t" ] || ! cmp -s "$d/t" "$d/c"; then
		echo "FAIL $mod coverage: cmc [$(tr '\n' ' ' < "$d/cmc.cov")] TLC [$(tr '\n' ' ' < "$d/tlc.cov")]"; echo x>"$OUT/fail"; return
	fi
	echo "ok   $mod coverage: TLC agrees ($gen: $(tr '\n' ' ' < "$d/tlc.cov"))"
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
# under symmetry cmc --spec finds 179 states; the emitted module has no
# SYMMETRY, so TLC must find the 357 states cmc finds without it
ND=357 check test/spec symrec symrec PASS
check test/spec countops countops PASS
check test/spec prec prec PASS
check test/spec names names PASS
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
# the same over a record and a sequence (test/spec/constrain_rec*.cml)
for m in constrain_rec constrain_rec_c constrain_rec_ac; do
	check test/spec $m $m DEADLOCK
	check test/spec $m ${m}nd PASS --no-deadlock
	check test/spec $m ${m}f PASS --define FLIP
done

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
# fairness per action and strong fairness (test/spec/fair/): toggle and
# inner give different verdicts under WF and SF, and TLC fixes each; the
# action properties hold, fail, and fail on a step out of the model
check test/spec/fair toggle toggle TEMPORAL
check test/spec/fair toggle_wf toggle_wf TEMPORAL
check test/spec/fair toggle_sfonly toggle_sfonly TEMPORAL
check test/spec/fair toggle_sf toggle_sf PASS
check test/spec/fair toggle_strong toggle_strong PASS
check test/spec/fair inner_wf inner_wf TEMPORAL
check test/spec/fair inner_sf inner_sf TEMPORAL
check test/spec/fair inner_sf2 inner_sf2 PASS
check test/spec/fair param param TEMPORAL
check test/spec/fair lasso lasso TEMPORAL
check test/spec/fair lasso_sf lasso_sf TEMPORAL
check test/spec/fair mono mono PASS
check test/spec/fair mono_bad mono_bad ACTION
check test/spec/fair mono_c mono_c ACTION
# action coverage (cmc --spec --coverage) against TLC's -coverage: an
# action constraint, a stutter-only action, two protocols, and Paxos
# and ZAB, where a guard (phase2a, commit) has an existential TLC counts
# per witness
cov test/spec constrain_ac constrain_ac all
cov test/spec/fair mono mono all
cov examples/spec twophase twophase all
cov examples/spec hc hc all
cov examples/spec paxos paxos distinct
cov examples/spec zab zab distinct
# one of each form the bridge emits (test/spec/shape.cml), an invariant
# and both properties
check test/spec shape shape PASS

# SANY over every module the bridge emits: each .cml in examples/spec
# and test/spec (with each of its [TAG]s defined in turn) that cmc
# checks must give a module SANY parses, so an emitter syntax bug is
# caught on any model, not only on the rows above.  A file cmc refuses
# (a base module with no init, a refusal fixture: exit 3) is skipped.
echo "-- SANY parses every emitted module"
d="$OUT/sany"; mkdir -p "$d"; nsany=0
for src in "$HERE"/examples/spec/*.cml "$HERE"/test/spec/*.cml "$HERE"/test/spec/*/*.cml; do
	for tag in "" $(sed -n 's/^action .*) *\[\([A-Za-z0-9_]*\)\] *:.*/\1/p; s/^action [A-Za-z0-9_]* *\[\([A-Za-z0-9_]*\)\] *:.*/\1/p' "$src" | sort -u); do
		mod=S$(basename "$src" .cml | tr -c 'A-Za-z0-9\n' _)$tag
		timeout 600 "$CMC" --spec "$src" ${tag:+--define "$tag"} > "$d/$mod.cmc" 2>&1
		[ $? = 3 ] && continue
		"$CMC" --emit-tla "$src" ${tag:+--define "$tag"} > "$d/$mod.tla" 2> "$d/$mod.err" || continue
		sed -i "1s/MODULE [A-Za-z0-9_]* /MODULE $mod /" "$d/$mod.tla"
		(cd "$d" && bash "$TLC" --sany "$mod.tla") > "$d/$mod.out" 2>&1
		if [ $? != 0 ] || grep -qE 'Parse Error|Semantic errors|Fatal errors|Could not parse|\*\*\* Errors' "$d/$mod.out"; then
			echo "FAIL SANY rejects the module emitted from ${src#"$HERE"/}${tag:+ --define $tag}"
			grep -iE 'error|line' "$d/$mod.out" | head -4 | sed 's/^/     /'; echo x>"$OUT/fail"
		else nsany=$((nsany+1)); fi
	done
done
echo "ok   SANY parsed $nsany emitted modules"
# Proof hooks, against TLC: with INIT IndInv and NEXT Next, TLC starts
# from every state of the IndInv that cmc --emit-tla writes for the
# invariants marked [inductive], takes the steps, and checks IndInv.
# An inductive one reaches no state outside IndInv, so TLC finds as many
# distinct states as cmc --spec --check-inductive counts in IndInv; one
# that is not has a step out of it, which both report.  WEAK unmarks
# the strengthening, as in apalache_gate.
ind() {
	cml=$1 mod=$2 want=$3 weak=$4
	d="$OUT/ind-$mod"; mkdir -p "$d"
	if [ -n "$weak" ]; then
		sed "s/^invariant $weak \[inductive\]:/invariant $weak:/" "$HERE/test/spec/ind/$cml.cml" > "$d/$mod.cml"
	else
		cp "$HERE/test/spec/ind/$cml.cml" "$d/$mod.cml"
	fi
	"$CMC" --emit-tla "$d/$mod.cml" > "$d/$mod.tla" 2>&1
	sed -i "s/MODULE $cml /MODULE $mod /" "$d/$mod.tla"
	{ printf 'INIT IndInv\nNEXT Next\nINVARIANT IndInv\nCHECK_DEADLOCK FALSE\n'
	  "$CMC" --emit-cfg "$d/$mod.cml" | grep '^CONSTANT'; } > "$d/$mod.cfg"
	"$CMC" --spec "$d/$mod.cml" --check-inductive > "$d/cmc.txt" 2>&1; rc=$?
	bash "$TLC" "$d/$mod.tla" "$d/$mod.cfg" -workers 1 > "$d/out.txt" 2>&1
	tv=$(verdict "$d/out.txt")
	case $rc in 0) cv=PASS ;; 1) cv=VIOLATION ;; *) cv="ERROR (exit $rc)" ;; esac
	if [ "$cv" != "$want" ] || [ "$tv" != "$want" ]; then
		echo "FAIL inductive $mod [$want]: cmc --check-inductive says $cv, TLC says $tv"; echo x>"$OUT/fail"; return
	fi
	if [ "$want" = PASS ]; then
		cn=$(num "$d/cmc.txt" 'each of the [0-9]+') td=$(num "$d/out.txt" 'generated, [0-9]+ distinct')
		if [ -z "$cn" ] || [ "$cn" != "$td" ]; then
			echo "FAIL inductive $mod: cmc counts $cn states in IndInv, TLC finds $td"; echo x>"$OUT/fail"; return
		fi
		echo "ok   inductive $mod [$want]: TLC agrees (IndInv has $td states)"
	else
		echo "ok   inductive $mod [$want]: TLC agrees"
	fi
}
ind mutex_ind mutex_ind PASS ""
ind mutex_ind mutex_weak VIOLATION lock_matches
ind queue queue PASS ""
ind queue queue_weak VIOLATION bounded
ind escape escape VIOLATION ""

[ -f "$OUT/fail" ] && fail=1
[ $fail = 0 ] && echo "emit_gate: ok (generated TLA+ parses and agrees with cmc --spec)" || echo "emit_gate: FAILED"
exit $fail
