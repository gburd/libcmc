#!/bin/sh
# spec_gate.sh BUILDDIR: the abstract-model front end (cmc --spec) gives
# the right verdict, distinct-state count and shortest-counterexample
# depth on the .cml fixtures in test/spec/.  The pinned numbers are the
# ones single-worker TLC gives on the hand-written TLA+ twins (see
# test/spec/*.tla); check-spec-calibration re-verifies them against live
# TLC.  This gate is C-only and fast.
set -u
B=$(cd "${1:?usage: spec_gate.sh BUILDDIR}" && pwd)
HERE=$(cd "$(dirname "$0")/.." && pwd)
CMC=$B/cmc
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/spec-gate
mkdir -p "$OUT"
fail=0

# expect DESC EXIT NEEDLE cmc-args...
expect() {
	desc=$1 want=$2 needle=$3; shift 3
	"$CMC" "$@" > "$OUT/out.txt" 2>&1
	rc=$?
	if [ "$rc" = "$want" ] && grep -q -- "$needle" "$OUT/out.txt"; then
		echo "ok   $desc -> exit $rc"
	else
		echo "FAIL $desc -> exit $rc (wanted $want and \"$needle\")"
		sed 's/^/     /' "$OUT/out.txt" | head -20
		fail=1
	fi
}

echo "-- the abstract-model checker (cmc --spec), matched to TLC"
expect "counter: in range holds"            0 "4 distinct states found" --spec "$HERE/test/spec/counter.cml"
expect "mutex: mutual exclusion holds"       0 "8 distinct states found" --spec "$HERE/test/spec/mutex.cml"
expect "mutex: depth matches TLC (4)"        0 "complete state graph search is 4" --spec "$HERE/test/spec/mutex.cml"
expect "mutex: the bug is caught"            1 "mutual_exclusion is violated" --spec "$HERE/test/spec/mutex.cml" --define BUG
expect "mutex: shortest trace is 4 actions"  1 "is (4 actions)" --spec "$HERE/test/spec/mutex.cml" --define BUG

echo "-- modules: a protocol spec extends a shared base"
expect "paxos: extends consensus, agreement holds" 0 "117 distinct states found" --spec "$HERE/examples/spec/paxos.cml"
expect "paxos: depth matches TLC (5)"         0 "complete state graph search is 5" --spec "$HERE/examples/spec/paxos.cml"
expect "paxos: the double-vote bug breaks agreement" 1 "agreement is violated" --spec "$HERE/examples/spec/paxos.cml" --define BUG

# Raft leader election (Ongaro's raft.tla): Election Safety, at most one
# leader per term.  Counts and depth match single-worker TLC (checked by
# make check-emit-tla); the revote bug lets two candidates reach a quorum.
expect "raft: election safety holds"          0 "38 distinct states found" --spec "$HERE/examples/spec/raft.cml"
expect "raft: depth matches TLC (4)"          0 "complete state graph search is 4" --spec "$HERE/examples/spec/raft.cml"
expect "raft: the revote bug elects two leaders" 1 "election_safety is violated" --spec "$HERE/examples/spec/raft.cml" --define BUG

# Humming Consensus (Machi; generic agreement over write-once epoch
# register stores): Agreement, at most one value chosen per epoch.
# Counts and depth match single-worker TLC; the overwrite bug breaks
# write-once so two values each reach a quorum.
expect "hc: agreement holds under write-once" 0 "41 distinct states found" --spec "$HERE/examples/spec/hc.cml"
expect "hc: depth matches TLC (5)"            0 "complete state graph search is 5" --spec "$HERE/examples/spec/hc.cml"
expect "hc: the overwrite bug breaks agreement" 1 "agreement is violated" --spec "$HERE/examples/spec/hc.cml" --define BUG

# ZAB (Apache ZooKeeper's Zab.tla): commit-durability, a committed
# transaction is held by a quorum so it survives a leader change.
# Counts and depth match single-worker TLC; the early-ack bug commits
# before a quorum stored, so a committed transaction can be lost.
expect "zab: commit-durability holds"         0 "20 distinct states found" --spec "$HERE/examples/spec/zab.cml"
expect "zab: depth matches TLC (6)"           0 "complete state graph search is 6" --spec "$HERE/examples/spec/zab.cml"
expect "zab: the early-ack bug loses a commit" 1 "durable is violated" --spec "$HERE/examples/spec/zab.cml" --define BUG

# Flexible Paxos (Howard, Malkhi, Spiegelman 2016): Agreement holds iff
# every Phase 1 quorum intersects every Phase 2 quorum, for which
# Q1 + Q2 > A suffices.  With Q1=4, Q2=2, A=5 (sum 6 > 5) it holds; the
# --define BUG uses the Phase 2 size for the Phase 1 guard, breaking the
# cross-phase intersection so two values are chosen.
expect "fpaxos: safe when Q1+Q2>A"           0 "No error has been found" --spec "$HERE/examples/spec/fpaxos.cml"
expect "fpaxos: the shrunk-Q1 bug breaks agreement" 1 "agreement is violated" --spec "$HERE/examples/spec/fpaxos.cml" --define BUG

# CASPaxos (Rystsov 2018): single-decree Paxos on a register via a
# client change function; Agreement (one value per register version)
# holds because a client reads from a quorum before committing.  The
# --define BUG stale read overwrites an acceptor, so two values commit.
expect "caspaxos: register agreement holds"   0 "No error has been found" --spec "$HERE/examples/spec/caspaxos.cml"
expect "caspaxos: the stale-read bug loses an update" 1 "agreement is violated" --spec "$HERE/examples/spec/caspaxos.cml" --define BUG

# Fast Paxos (Lamport 2006): a fast round may collide, so its quorum F
# must exceed 3A/4 for a classic recovery round to resolve the collision
# unambiguously.  With A=5, F=4 (> 3.75) it holds; the --define BUG lets
# recovery use a classic majority threshold where a fast quorum is
# needed, so a collided round chooses two values.
expect "fastpaxos: safe when F>3A/4"         0 "No error has been found" --spec "$HERE/examples/spec/fastpaxos.cml"
expect "fastpaxos: the small-fast-quorum bug collides" 1 "agreement is violated" --spec "$HERE/examples/spec/fastpaxos.cml" --define BUG

# Two-phase commit (Lamport's TwoPhase.tla): the canonical non-consensus
# example.  Consistency (atomicity) -- a committed and an aborted
# resource manager never coexist -- holds because a resource manager
# commits only on a unanimous prepared vote; the --define BUG commits
# without that check.
expect "twophase: atomicity holds"            0 "No error has been found" --spec "$HERE/examples/spec/twophase.cml"
expect "twophase: the no-unanimity bug breaks atomicity" 1 "consistency is violated" --spec "$HERE/examples/spec/twophase.cml" --define BUG

echo "-- symmetry reduction: interchangeable node ids collapse"
# the token spec's node ids are fully symmetric; with symmetry declared,
# the state space shrinks (10 -> 3) with the SAME verdict -- the soundness
# check for the declaration.
expect "token: symmetry collapses the state space" 0 "3 distinct states found" --spec "$HERE/examples/spec/token.cml"

echo "-- liveness: temporal properties under weak fairness"
# the ticker must eventually reach its max under weak fairness (holds);
# with a reset action a fair cycle avoids the max (fails).
expect "ticker: eventually-max holds under fairness" 0 "1 property held" --spec "$HERE/examples/spec/ticker.cml"
expect "ticker: a fair reset cycle breaks it"        1 "reaches_max is violated" --spec "$HERE/examples/spec/ticker.cml" --define RESET

echo "-- the TLA+ bridge (cmc --emit-tla), structure only (check-emit-tla runs TLC)"
expect "emit-tla: a well-formed module header" 0 "MODULE paxos" --emit-tla "$HERE/examples/spec/paxos.cml"
expect "emit-tla: translates operators to TLA+" 0 "Cardinality" --emit-tla "$HERE/examples/spec/paxos.cml"

echo "-- the loader refuses malformed specs"
printf 'module m\nvar x : 0 .. 2\ninit x == 0\naction a:\n  when x < 2\n  x%s = x + 1\n' "'" > "$OUT/noinv.cml"
expect "a spec with no invariant is an error" 3 "no invariant" --spec "$OUT/noinv.cml"
printf 'var x : 0 .. 2\ninit x == 0\ninvariant t: x <= 2\n' > "$OUT/nomod.cml"
expect "a spec with no module is an error"    3 "no module" --spec "$OUT/nomod.cml"

[ $fail = 0 ] && echo "spec_gate: ok (abstract-model checker: verdicts, counts and depths match TLC)" || echo "spec_gate: FAILED"
exit $fail
