#!/bin/sh
# proc_gate.sh BUILDDIR: the process front end (cmc --model-file) gives
# the right verdict on every fixture in test/proc/, and refuses every
# model file or setting it does not implement instead of ignoring it.
#
# Each fixture is an ordinary program written as it ships (blocking TCP,
# no libcmc header) plus a model file.  The verdicts pinned here are the
# ones the prototype got wrong before M7 (idle, fifo, chain, depth bound,
# bigmsg) and the bugs the front end exists to catch (repl, elect, lease,
# durlog, paxos).  The loader rows are holes the prototype had: a
# mistyped or unimplemented key was ignored (a false pass), and a
# missing preload let the target read the real clock.
#
# Linux x86-64 only (seccomp user notification); elsewhere it says so
# and exits 0.
set -u
B=$(cd "${1:?usage: proc_gate.sh BUILDDIR}" && pwd)
HERE=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
CMC=$B/cmc
T=$HERE/test/proc
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/proc-gate
mkdir -p "$OUT" "$B/proc"

if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
	echo "proc_gate: SKIP: the process front end needs Linux x86-64 ($(uname -s) $(uname -m))"
	exit 0
fi

fail=0
for f in repl elect lease durlog idle fifo chain bigmsg clocks accept_timeout quorum; do
	$CC -O1 -no-pie -w -o "$B/proc/$f" "$T/$f.c" || { echo "proc_gate: $f.c does not build"; fail=1; }
done
for f in repl elect lease durlog quorum; do
	$CC -O1 -no-pie -w -DBUG -o "$B/proc/$f-bug" "$T/$f.c" || { echo "proc_gate: $f.c -DBUG does not build"; fail=1; }
done
# the consensus examples are real, tested fixtures too
$CC -O1 -no-pie -w -o "$B/proc/paxos" "$HERE/examples/paxos/paxos.c" || { echo "proc_gate: paxos.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/paxos-bug" "$HERE/examples/paxos/paxos.c" || { echo "proc_gate: paxos.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/raft" "$HERE/examples/raft/raft.c" || { echo "proc_gate: raft.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/raft-bug" "$HERE/examples/raft/raft.c" || { echo "proc_gate: raft.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/raftlog" "$HERE/examples/raft/raftlog.c" || { echo "proc_gate: raftlog.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/raftlog-bug" "$HERE/examples/raft/raftlog.c" || { echo "proc_gate: raftlog.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/hc" "$HERE/examples/hc/hc.c" || { echo "proc_gate: hc.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/hc-bug" "$HERE/examples/hc/hc.c" || { echo "proc_gate: hc.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/zab" "$HERE/examples/zab/zab.c" || { echo "proc_gate: zab.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/zab-bug" "$HERE/examples/zab/zab.c" || { echo "proc_gate: zab.c -DBUG does not build"; fail=1; }

# expect DESC EXIT NEEDLE cmc-args...: the exit status, and a line of output
expect() {
	desc=$1 want=$2 needle=$3; shift 3
	"$CMC" "$@" > "$OUT/out.txt" 2>&1
	rc=$?
	if [ "$rc" = "$want" ] && grep -q -- "$needle" "$OUT/out.txt"; then
		echo "ok   $desc -> exit $rc"
	else
		echo "FAIL $desc -> exit $rc (wanted $want and \"$needle\")"
		sed 's/^/     /' "$OUT/out.txt" | head -30
		fail=1
	fi
}
run() {		# run MODEL BINARY: the --model-file arguments for a fixture
	echo "--model-file $T/$1.model --binary $B/proc/$2"
}
runx() {	# runx MODELPATH BINARY: for a model file outside test/proc/
	echo "--model-file $1 --binary $B/proc/$2"
}

echo "-- verdicts"
expect "repl: correct holds"               0 "No error has been found" $(run repl repl)
expect "repl: ack before backup caught"    1 "agree_on_ack is violated" $(run repl repl-bug)
expect "elect: correct holds"              0 "No error has been found" $(run elect elect)
expect "elect: split brain caught"         1 "one_leader_per_term is violated" $(run elect elect-bug)
expect "lease: correct holds"              0 "No error has been found" $(run lease lease)
expect "lease: stale lease caught"         1 "no_write_after_expiry is violated" $(run lease lease-bug)
expect "durlog: correct holds"             0 "No error has been found" $(run durlog durlog)
expect "durlog: lost commit caught"        1 "no_lost_commit is violated" $(run durlog durlog-bug)
expect "durlog: the bug needs the crash"   0 "No error has been found" $(run durlog durlog-bug) --crashes 0
expect "paxos: correct holds (N=5)"        0 "No error has been found" $(runx "$HERE/examples/paxos/paxos.model" paxos)
expect "paxos: two values decided"         1 "agreement is violated" $(runx "$HERE/examples/paxos/paxos.model" paxos-bug)
expect "paxos: --simulate finds it"        1 "agreement is violated" $(runx "$HERE/examples/paxos/paxos.model" paxos-bug) --simulate --seed 1 --walks 2000
expect "paxos: --simulate is not a pass"   2 "not exhaustive" $(runx "$HERE/examples/paxos/paxos.model" paxos) --simulate --seed 1 --walks 50
expect "raft: election safety holds"        0 "No error has been found" $(runx "$HERE/examples/raft/raft.model" raft)
expect "raft: double vote -> two leaders"   1 "election_safety is violated" $(runx "$HERE/examples/raft/raft.model" raft-bug)
expect "raft: --simulate finds it"          1 "election_safety is violated" $(runx "$HERE/examples/raft/raft.model" raft-bug) --simulate --seed 1 --walks 2000
expect "raftlog: no committed entry is lost" 0 "No error has been found" $(runx "$HERE/examples/raft/raftlog.model" raftlog)
expect "raftlog: early-ack loses a committed entry" 1 "no_lost_committed is violated" $(runx "$HERE/examples/raft/raftlog.model" raftlog-bug)
expect "raftlog: --simulate finds it"       1 "no_lost_committed is violated" $(runx "$HERE/examples/raft/raftlog.model" raftlog-bug) --simulate --seed 1 --walks 2000
expect "hc: agreement holds, even under a partition"   0 "No error has been found" $(runx "$HERE/examples/hc/hc.model" hc)
expect "hc: AP-mode adoption breaks agreement" 1 "agreement is violated" $(runx "$HERE/examples/hc/hc.model" hc-bug) --timeouts 6 --drops 1 --simulate --seed 3 --walks 5000
expect "zab: no committed txn is lost"       0 "No error has been found" $(runx "$HERE/examples/zab/zab.model" zab)
expect "zab: early-ack loses a commit on crash" 1 "no_lost_commit is violated" $(runx "$HERE/examples/zab/zab.model" zab-bug)
expect "zab: --simulate finds it"           1 "no_lost_commit is violated" $(runx "$HERE/examples/zab/zab.model" zab-bug) --simulate --seed 1 --walks 2000
expect "idle: time passing is not merged"  1 "never_late is violated" $(run idle idle)
expect "fifo: a connection is in order"    0 "No error has been found" $(run fifo fifo)
expect "chain: any topology the nodes dial" 1 "not_reached is violated" $(run chain chain)

# every intercepted syscall is pinned by a fixture.  clocks reads
# gettimeofday and getrandom and uses sendto/recvfrom; its invariants
# are the checker's contract.  The teeth: a model that expects the wrong
# answer must be caught, so a regression in the handlers cannot pass.
expect "clocks: gettimeofday, getrandom, sendto/recvfrom" 0 "3 invariants held" $(run clocks clocks)
printf '[target]\nnodes = 2\n[environment]\ntimeouts = 1\n[invariants]\nwrong_clock = "node[0][0] != 1"\n' > "$OUT/clk-wrong.model"
expect "clocks teeth: gettimeofday is the logical clock" 1 "wrong_clock is violated" --model-file "$OUT/clk-wrong.model" --binary "$B/proc/clocks"
printf '[target]\nnodes = 2\n[environment]\ntimeouts = 1\n[invariants]\nwrong_random = "node[0][1] != 0"\n' > "$OUT/rnd-wrong.model"
expect "clocks teeth: getrandom is engine-zeroed"      1 "wrong_random is violated" --model-file "$OUT/rnd-wrong.model" --binary "$B/proc/clocks"
expect "a depth bound is INCOMPLETE"       2 "depth bound" $(run durlog durlog-bug) --depth 2
expect "a state bound is INCOMPLETE"       2 "max states" $(runx "$HERE/examples/paxos/paxos.model" paxos) --max-states 10
expect "an over-long write is an error"    1 "limit is 64" $(run bigmsg bigmsg)
expect "the counterexample is named"       1 "crash n0" $(run durlog durlog-bug)
expect "a server idle in accept() can time out" 1 "never_fires is violated" $(run accept_timeout accept_timeout)

echo "-- the loader refuses what it does not implement"
M=$OUT/m.model
model() { printf "$1" > "$M"; }
model '[target]\nnodes = 2\n[environment]\nclient_write = 7\ncrashs = 1\n[invariants]\nx = "1 == 1"\n'
expect "a mistyped key is an error"        3 "unknown key crashs" --model-file "$M" --binary "$B/proc/durlog-bug"
model '[target]\nnodes = 2\n[environment]\ndups = 1\n[invariants]\nx = "1 == 1"\n'
expect "an unimplemented key is an error"  3 "unknown key dups" --model-file "$M" --binary "$B/proc/repl"
model '[target]\nnodes = 2\nstate = region:cmc_state\n[invariants]\nx = "1 == 1"\n'
expect "state = region is not supported"   3 "state" --model-file "$M" --binary "$B/proc/repl"
model '[target]\nnodes = 2\n[invariants]\nx = "node[0][0] =="\n'
expect "a bad invariant is an error"       3 "invariant x" --model-file "$M" --binary "$B/proc/repl"
model '[target]\nnodes = 2\n[invariants]\nx = "node[1][0] == 0 garbage here"\n'
expect "junk after an invariant is an error" 3 "invariant x" --model-file "$M" --binary "$B/proc/repl"
model '[target]\nnodes = 2\n[invariants]\nx = "all r in replies : r.vall == 5"\n'
expect "a bad quantifier body is an error, even unevaluated" 3 "invariant x" --model-file "$M" --binary "$B/proc/repl"
model '[target]\nnodes = 2\n[invariants]\n'
expect "a model with no invariant is an error" 3 "no invariant" --model-file "$M" --binary "$B/proc/repl"
model '[target]\nnodes = 2\n[invariants]\nx = "1 == 1"\n'
expect "a model with no binary is an error" 3 "binary" --model-file "$M"
model '[target]\nnodes = 9\n[invariants]\nx = "1 == 1"\n'
expect "too many nodes is an error"        3 "nodes" --model-file "$M" --binary "$B/proc/repl"
model '[tagret]\nnodes = 2\n[invariants]\nx = "1 == 1"\n'
expect "an unknown section is an error"    3 "unknown section" --model-file "$M" --binary "$B/proc/repl"

echo "-- the invariant language: quantify over nodes, and named fields"
# the agreement property written as it reads in TLA+ (for all pairs of
# proposers), with a [fields] name instead of a raw byte offset, catches
# the same bug the hand-expanded two-node form did.  This is the red
# test for the quantifier: a broken binding would make it pass.
printf '[target]\nnodes = 5\n[fields]\ndecided = 7\n[invariants]\nagreement = "all i in nodes : all j in nodes : node[i].decided <= 0 or node[j].decided <= 0 or node[i].decided == node[j].decided"\n' > "$OUT/q.model"
expect "nodes-quantifier + field: correct Paxos holds"  0 "No error has been found" --model-file "$OUT/q.model" --binary "$B/proc/paxos"
expect "nodes-quantifier + field: the bug is caught"    1 "agreement is violated" --model-file "$OUT/q.model" --binary "$B/proc/paxos-bug"
printf '[target]\nnodes = 2\n[fields]\nx = 0\n[invariants]\nt = "node[0].nosuch == 0"\n' > "$OUT/q.model"
expect "an unknown field is a parse error"             3 "invariant t" --model-file "$OUT/q.model" --binary "$B/proc/repl"
printf '[target]\nnodes = 2\n[invariants]\nt = "3 in nodes"\n' > "$OUT/q.model"
expect "membership in nodes is not defined"            3 "invariant t" --model-file "$OUT/q.model" --binary "$B/proc/repl"
printf '[target]\nnodes = 2\n[fields]\nx = 0\nx = 1\n[invariants]\nt = "1 == 1"\n' > "$OUT/q.model"
expect "a field defined twice is an error"             3 "field x defined twice" --model-file "$OUT/q.model" --binary "$B/proc/repl"
# `eventually` is a reachability check: some reached state must satisfy
# it, reported at the end of an exhaustive search.  Met => holds;
# unreachable => a violation.  (Not fairness-based liveness.)
printf '[target]\nnodes = 2\n[environment]\ntimeouts = 2\n[invariants]\nreached = "eventually node[1][0] == 1"\n' > "$OUT/q.model"
expect "eventually: a reachable state satisfies it"     0 "No error has been found" --model-file "$OUT/q.model" --binary "$B/proc/elect"
printf '[target]\nnodes = 2\n[environment]\ntimeouts = 2\n[invariants]\nreached = "eventually node[0][0] == 200"\n' > "$OUT/q.model"
expect "eventually: never reached is a violation"       1 "reached is violated" --model-file "$OUT/q.model" --binary "$B/proc/elect"

echo "-- the invariant language: arithmetic and counting (quorums)"
# `count V in SET : PRED` and arithmetic let an invariant state a
# cardinality property -- here, quorum safety: a committed value is held
# by a majority.  Inexpressible without count; it is the red test for
# Milestone A (the value model).
expect "quorum: a committed value has a majority"      0 "No error has been found" $(run quorum quorum)
expect "quorum: a sub-quorum commit is caught"         1 "quorum_commit is violated" $(run quorum quorum-bug)
expect "quorum: --simulate finds it"                   1 "quorum_commit is violated" $(run quorum quorum-bug) --simulate --seed 1 --walks 2000
# arithmetic evaluates with the usual precedence
printf '[target]\nnodes = 2\n[invariants]\nt = "2 + 3 * 4 == 14 and 10 mod 3 == 1 and (1 + 1) * 2 == 4"\n' > "$OUT/q.model"
expect "arithmetic: precedence and mod"                 0 "No error has been found" --model-file "$OUT/q.model" --binary "$B/proc/repl"
# count's body must parse even when the set is empty, so a broken body
# is never hidden; mod by zero and a missing count body are errors
printf '[target]\nnodes = 2\n[invariants]\nt = "count w in nodes : node[w].bad == 0"\n[fields]\nv = 0\n' > "$OUT/q.model"
expect "count: an unknown field in the body is an error" 3 "invariant t" --model-file "$OUT/q.model" --binary "$B/proc/repl"
printf '[target]\nnodes = 2\n[invariants]\nt = "5 mod 0 == 0"\n' > "$OUT/q.model"
expect "arithmetic: mod by zero is an error"            3 "invariant t" --model-file "$OUT/q.model" --binary "$B/proc/repl"
expect "an option it cannot honour is an error" 3 "not supported with --model-file" $(run repl repl) --inv agree_on_ack
$CC -O1 -w -fPIE -pie -o "$B/proc/repl-pie" "$T/repl.c" 2>/dev/null &&
	expect "a PIE binary is an error"      3 "non-PIE" --model-file "$T/repl.model" --binary "$B/proc/repl-pie"
CMC_PRELOAD=$OUT/no-such-preload.so expect "a missing preload is an error" 3 "preload" $(run lease lease-bug)

# the binary in the model file is found relative to the model file
printf '[target]\nbinary = repl-bug\nnodes = 2\n[environment]\nclient_write = 5\n[invariants]\nagree_on_ack = "all r in replies : r.val in node[1]"\n' > "$B/proc/rel.model"
# (from another directory; not a subshell, so a failure counts)
cd /
expect "binary = is relative to the model file" 1 "agree_on_ack is violated" --model-file "$B/proc/rel.model"
cd "$HERE"

[ $fail = 0 ] && echo "proc_gate: ok (process front end: every verdict and every refusal)" || echo "proc_gate: FAILED"
exit $fail
