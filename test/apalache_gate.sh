#!/usr/bin/env bash
# apalache_gate.sh CMC: the TLA+ bridge reaches Apalache (Milestone D).
# `cmc --emit-tla` now writes @type annotations and a CInit operator, so
# Apalache's symbolic (SMT-backed) checker accepts the generated module
# directly.  For each .cml fixture this gate runs `apalache-mc check` to
# the row's length bound and requires the verdict to match `cmc --spec`:
# counter and Paxos hold, the planted Paxos bug violates.
#
# Apalache is an OPTIONAL external tool (not in nixpkgs, not a build
# dependency of cmc), resolved like TLC's jar is in calib/hc/tlc.sh:
#   1. $APALACHE_MC, if set, is the apalache-mc launcher;
#   2. else a cached install under $CMC_SCRATCH/apalache-<ver>/;
#   3. else the gate downloads that version's tarball once.
# Java 21+ is required (Apalache 0.63 is class-file 65); the gate uses
# `nix shell nixpkgs#jdk21` when the host java is older or absent.
set -u

# classify OUT: the verdict in an `apalache-mc check` log, from Apalache's
# own outcome and exit lines and nothing else.  PASS needs outcome NoError
# (or ExecutionsTooShort: no error up to the bound) with EXITCODE OK;
# VIOLATION needs outcome Error, a violated state invariant, and exit
# code 12 (Apalache's code for a found violation).  Any other log --
# a parse, typing or input error, a crash, a missing outcome -- is
# ERROR, so a tool failure can never pass as an expected violation.
classify() {
	log=$1
	if grep -qE 'The outcome is: (NoError|ExecutionsTooShort)' "$log" &&
	   grep -qE '^EXITCODE: OK' "$log"; then
		echo PASS
	elif grep -qE 'The outcome is: Error' "$log" &&
	     grep -qE 'state invariant [0-9]+ violated' "$log" &&
	     grep -qE '^EXITCODE: ERROR \(12\)' "$log"; then
		echo VIOLATION
	else
		echo ERROR
	fi
}

# --self-test: the classifier against recorded and failing logs, with no
# Apalache needed (run by the spec gate)
if [ "${1:-}" = --self-test ]; then
	t=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/apalache-selftest; mkdir -p "$t"; st=0
	printf 'The outcome is: NoError\nEXITCODE: OK\n' > "$t/pass"
	printf 'State 6: state invariant 0 violated.\nThe outcome is: Error\nChecker has found an error\nEXITCODE: ERROR (12)\n' > "$t/viol"
	printf 'Input error: artificial parser failure\nChecker has found an error\nEXITCODE: ERROR (120)\n' > "$t/parse"
	printf 'Typing input error: Expected a type annotation\nThe outcome is: Error\nEXITCODE: ERROR (120)\n' > "$t/type"
	printf 'The outcome is: NoError\n' > "$t/noexit"
	for c in pass:PASS viol:VIOLATION parse:ERROR type:ERROR noexit:ERROR; do
		got=$(classify "$t/${c%%:*}")
		[ "$got" = "${c#*:}" ] || { echo "apalache_gate self-test: ${c%%:*} gave $got, want ${c#*:}"; st=1; }
	done
	[ $st = 0 ] && echo "apalache_gate self-test: ok"
	exit $st
fi
CMC=${1:?usage: apalache_gate.sh CMC}
HERE=$(cd "$(dirname "$0")/.." && pwd)
VER=${APALACHE_VERSION:-0.63.0}
SCRATCH=${CMC_SCRATCH:-$HOME/tmp/cmc-build}
OUT="$SCRATCH/apalache-gate"
mkdir -p "$OUT"
fail=0

# A missing tool is a SKIP (exit 0) for a developer, but release evidence
# must not be a skip: with CMC_REQUIRE_TOOLS=1 a skip fails the gate.
skip() {
	echo "apalache_gate: SKIP ($1)"
	[ "${CMC_REQUIRE_TOOLS:-0}" = 1 ] && { echo "apalache_gate: FAILED (CMC_REQUIRE_TOOLS=1 forbids a skip)"; exit 1; }
	exit 0
}

# resolve the apalache-mc launcher
MC=${APALACHE_MC:-}
if [ -z "$MC" ]; then
	cand="$SCRATCH/apalache-$VER/bin/apalache-mc"
	if [ ! -x "$cand" ]; then
		echo "-- fetching Apalache $VER (optional tool; cached in $SCRATCH)"
		url="https://github.com/apalache-mc/apalache/releases/download/v$VER/apalache-$VER.tgz"
		if ! curl -fsSL -o "$SCRATCH/apalache-$VER.tgz" "$url"; then
			skip "could not fetch $url; set APALACHE_MC to run"
		fi
		tar xzf "$SCRATCH/apalache-$VER.tgz" -C "$SCRATCH" || skip "tarball did not unpack"
	fi
	MC="$cand"
fi
[ -x "$MC" ] || skip "no apalache-mc at $MC; set APALACHE_MC"

# the ITF comparison of the VIOLATION rows reads JSON with python3
command -v python3 > /dev/null 2>&1 || skip "needs python3 to compare ITF traces"

# a Java 21+ runner: use the host java if new enough, else nix jdk21
java_ok() { command -v java >/dev/null 2>&1 && java -version 2>&1 | grep -qE 'version "(2[1-9]|[3-9][0-9])'; }
if java_ok; then
	RUN() { "$@"; }
elif command -v nix >/dev/null 2>&1; then
	RUN() { nix shell nixpkgs#jdk21 -c sh -c 'export JAVA_HOME=$(dirname $(dirname $(readlink -f $(which java)))); exec "$@"' sh "$@"; }
else
	skip "needs Java 21+ or nix for jdk21"
fi

# row: DIR | CML | MODULE | INV | want | length | define
# length is the bound on steps (--length); a VIOLATION row needs at
# least the length of the shortest counterexample cmc --spec reports.
# A PASS row whose length is at least cmc's depth minus one (the depth
# counts the initial state) explores every reachable state: counter,
# raft, hc, twophase and fastpaxos.  The others are bounded passes; see
# docs/measurements/2026-11-20-apalache-length.md for longer runs.
rows="
test/spec|counter|counter|in_range|PASS|10|
examples/spec|paxos|paxos|agreement|PASS|10|
examples/spec|paxos|paxosbug|agreement|VIOLATION|12|--define BUG
examples/spec|raft|raft|election_safety|PASS|10|
examples/spec|raft|raftb|election_safety|VIOLATION|10|--define BUG
examples/spec|hc|hc|agreement|PASS|10|
examples/spec|hc|hcb|agreement|VIOLATION|10|--define BUG
examples/spec|zab|zab|committed_prefix|PASS|10|
examples/spec|zab|zabi|initial_history|PASS|10|
examples/spec|zab|zabb|initial_history|VIOLATION|13|--define BUG
examples/spec|fpaxos|fpaxos|agreement|PASS|10|
examples/spec|fpaxos|fpaxosb|agreement|VIOLATION|12|--define BUG
examples/spec|caspaxos|caspaxos|descends|PASS|10|
examples/spec|caspaxos|caspaxosb|descends|VIOLATION|12|--define BUG
examples/spec|twophase|twophase|consistency|PASS|10|
examples/spec|twophase|twophaseb|consistency|VIOLATION|10|--define BUG
examples/spec|fastpaxos|fastpaxos|agreement|PASS|11|
examples/spec|fastpaxos|fastpaxosb|agreement|VIOLATION|11|--define BUG
examples/spec|logrep|logrep|durable|PASS|8|
examples/spec|logrep|logrepb|durable|VIOLATION|5|--define BUG
"
check() {
	dir=$1 cml=$2 mod=$3 inv=$4 want=$5 len=$6 def=$7
	d="$OUT/$mod-$want"; mkdir -p "$d"
	# shellcheck disable=SC2086
	"$CMC" --emit-tla "$HERE/$dir/$cml.cml" $def > "$d/$mod.tla" 2>&1
	# module name must match the filename for the tools
	sed -i "s/MODULE $cml /MODULE $mod /" "$d/$mod.tla"
	( cd "$d" && RUN "$MC" check --no-deadlock --cinit=CInit "--inv=$inv" "--length=$len" --run-dir=run "$mod.tla" > out.txt 2>&1 )
	disp=$(classify "$d/out.txt")
	if [ "$disp" != "$want" ]; then
		echo "FAIL $cml [$want]: Apalache gave $disp"; grep -iE 'outcome|error|EXITCODE' "$d/out.txt" | head -3 | sed 's/^/     /'; echo x>"$OUT/fail"; return
	fi
	# cmc's ITF counterexample has the structure of Apalache's: header,
	# variables, parameters, types, length, and each value's form
	if [ "$want" = VIOLATION ]; then
		# shellcheck disable=SC2086
		"$CMC" --spec "$HERE/$dir/$cml.cml" $def --trace-out "$d/cmc.itf.json" > "$d/cmc.txt" 2>&1
		if ! python3 "$HERE/test/trace_check.py" apalache "$d/cmc.itf.json" "$d/run/violation.itf.json" > "$d/tc.txt" 2>&1; then
			echo "FAIL $cml: cmc's ITF trace differs from Apalache's"; sed 's/^/     /' "$d/tc.txt"; echo x>"$OUT/fail"; return
		fi
	fi
	if [ "$want" = VIOLATION ]; then echo "ok   $cml [$want]: Apalache found an invariant violation, an ITF trace like cmc's"
	else echo "ok   $cml [$want]: Apalache reports no error up to length $len"; fi
}

echo "-- the TLA+ bridge reaches Apalache: cmc --emit-tla is Apalache-checkable"
rm -f "$OUT/fail"
printf '%s\n' "$rows" | while IFS='|' read -r dir cml mod inv want len def; do
	[ -z "$cml" ] && continue
	check "$dir" "$cml" "$mod" "$inv" "$want" "$len" "$def"
done

[ -f "$OUT/fail" ] && fail=1
[ $fail = 0 ] && echo "apalache_gate: ok (generated TLA+ type-checks and agrees with cmc --spec in Apalache)" || echo "apalache_gate: FAILED"
exit $fail
