#!/usr/bin/env bash
# apalache_gate.sh CMC: the TLA+ bridge reaches Apalache (Milestone D).
# `cmc --emit-tla` now writes @type annotations and a CInit operator, so
# Apalache's symbolic (SMT-backed) checker accepts the generated module
# directly.  For each .cml fixture this gate runs `apalache-mc check` and
# requires the verdict to match `cmc --spec`: counter and Paxos hold,
# the planted Paxos bug violates.
#
# Apalache is an OPTIONAL external tool (not in nixpkgs, not a build
# dependency of cmc), resolved like TLC's jar is in calib/hc/tlc.sh:
#   1. $APALACHE_MC, if set, is the apalache-mc launcher;
#   2. else a cached install under $CMC_SCRATCH/apalache-<ver>/;
#   3. else the gate downloads that version's tarball once.
# Java 21+ is required (Apalache 0.63 is class-file 65); the gate uses
# `nix shell nixpkgs#jdk21` when the host java is older or absent.
set -u
CMC=${1:?usage: apalache_gate.sh CMC}
HERE=$(cd "$(dirname "$0")/.." && pwd)
VER=${APALACHE_VERSION:-0.63.0}
SCRATCH=${CMC_SCRATCH:-$HOME/tmp/cmc-build}
OUT="$SCRATCH/apalache-gate"
mkdir -p "$OUT"
fail=0

# resolve the apalache-mc launcher
MC=${APALACHE_MC:-}
if [ -z "$MC" ]; then
	cand="$SCRATCH/apalache-$VER/bin/apalache-mc"
	if [ ! -x "$cand" ]; then
		echo "-- fetching Apalache $VER (optional tool; cached in $SCRATCH)"
		url="https://github.com/apalache-mc/apalache/releases/download/v$VER/apalache-$VER.tgz"
		if ! curl -fsSL -o "$SCRATCH/apalache-$VER.tgz" "$url"; then
			echo "apalache_gate: SKIP (could not fetch $url; set APALACHE_MC to run)"; exit 0
		fi
		tar xzf "$SCRATCH/apalache-$VER.tgz" -C "$SCRATCH" || { echo "apalache_gate: SKIP (tarball did not unpack)"; exit 0; }
	fi
	MC="$cand"
fi
[ -x "$MC" ] || { echo "apalache_gate: SKIP (no apalache-mc at $MC; set APALACHE_MC)"; exit 0; }

# a Java 21+ runner: use the host java if new enough, else nix jdk21
java_ok() { command -v java >/dev/null 2>&1 && java -version 2>&1 | grep -qE 'version "(2[1-9]|[3-9][0-9])'; }
if java_ok; then
	RUN() { "$@"; }
elif command -v nix >/dev/null 2>&1; then
	RUN() { nix shell nixpkgs#jdk21 -c sh -c 'export JAVA_HOME=$(dirname $(dirname $(readlink -f $(which java)))); exec "$@"' sh "$@"; }
else
	echo "apalache_gate: SKIP (needs Java 21+ or nix for jdk21)"; exit 0
fi

# row: DIR | CML | MODULE | INV | want | define
rows="
test/spec|counter|counter|in_range|PASS|
examples/spec|paxos|paxos|agreement|PASS|
examples/spec|paxos|paxosbug|agreement|VIOLATION|--define BUG
examples/spec|raft|raft|election_safety|PASS|
examples/spec|raft|raftb|election_safety|VIOLATION|--define BUG
examples/spec|hc|hc|agreement|PASS|
examples/spec|hc|hcb|agreement|VIOLATION|--define BUG
examples/spec|zab|zab|durable|PASS|
examples/spec|zab|zabb|durable|VIOLATION|--define BUG
examples/spec|fpaxos|fpaxos|agreement|PASS|
examples/spec|fpaxos|fpaxosb|agreement|VIOLATION|--define BUG
examples/spec|caspaxos|caspaxos|agreement|PASS|
examples/spec|caspaxos|caspaxosb|agreement|VIOLATION|--define BUG
examples/spec|twophase|twophase|consistency|PASS|
examples/spec|twophase|twophaseb|consistency|VIOLATION|--define BUG
examples/spec|fastpaxos|fastpaxos|agreement|PASS|
examples/spec|fastpaxos|fastpaxosb|agreement|VIOLATION|--define BUG
"
check() {
	dir=$1 cml=$2 mod=$3 inv=$4 want=$5 def=$6
	d="$OUT/$mod-$want"; mkdir -p "$d"
	# shellcheck disable=SC2086
	"$CMC" --emit-tla "$HERE/$dir/$cml.cml" $def > "$d/$mod.tla" 2>&1
	# module name must match the filename for the tools
	sed -i "s/MODULE $cml /MODULE $mod /" "$d/$mod.tla"
	( cd "$d" && RUN "$MC" check --no-deadlock --cinit=CInit "--inv=$inv" --length=10 "$mod.tla" > out.txt 2>&1 )
	if grep -qiE 'Annotation required|type input error|Typing input error' "$d/out.txt"; then
		echo "FAIL $cml [$want]: Apalache rejected the generated types"; grep -iE 'error|annotation' "$d/out.txt" | head -3 | sed 's/^/     /'; echo x>"$OUT/fail"; return
	fi
	if [ "$want" = VIOLATION ]; then
		grep -q 'Checker has found an error' "$d/out.txt" && echo "ok   $cml [$want]: Apalache found the violation" || { echo "FAIL $cml [$want]: expected a violation"; grep -iE 'outcome|error' "$d/out.txt" | head -2 | sed 's/^/     /'; echo x>"$OUT/fail"; }
	else
		grep -qE 'Checker reports no error|The outcome is: NoError' "$d/out.txt" && echo "ok   $cml [$want]: Apalache reports no error" || { echo "FAIL $cml [$want]: expected no error"; grep -iE 'outcome|error|violat' "$d/out.txt" | head -2 | sed 's/^/     /'; echo x>"$OUT/fail"; }
	fi
}

echo "-- the TLA+ bridge reaches Apalache: cmc --emit-tla is Apalache-checkable"
rm -f "$OUT/fail"
printf '%s\n' "$rows" | while IFS='|' read -r dir cml mod inv want def; do
	[ -z "$cml" ] && continue
	check "$dir" "$cml" "$mod" "$inv" "$want" "$def"
done

[ -f "$OUT/fail" ] && fail=1
[ $fail = 0 ] && echo "apalache_gate: ok (generated TLA+ type-checks and agrees with cmc --spec in Apalache)" || echo "apalache_gate: FAILED"
exit $fail
