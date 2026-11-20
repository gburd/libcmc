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
for f in repl elect lease durlog idle fifo chain bigmsg clocks accept_timeout quorum pollloop guarded unmodelled crossrpc twoconn leasedrift; do
	$CC -O1 -no-pie -w -o "$B/proc/$f" "$T/$f.c" || { echo "proc_gate: $f.c does not build"; fail=1; }
done
for f in repl elect lease durlog quorum pollloop leasedrift; do
	$CC -O1 -no-pie -w -DBUG -o "$B/proc/$f-bug" "$T/$f.c" || { echo "proc_gate: $f.c -DBUG does not build"; fail=1; }
done
# epoll backend (davros eval ask 9): an event loop built on epoll, with a
# correct and a -DBUG build, the epoll twin of pollloop
$CC -O1 -no-pie -w -o "$B/proc/epollloop" "$T/epollloop.c" || { echo "proc_gate: epollloop.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/epollloop-bug" "$T/epollloop.c" || { echo "proc_gate: epollloop.c -DBUG does not build"; fail=1; }
# restart (Tier 3 ask 6): crash-and-recover, so it needs both builds
$CC -O1 -no-pie -w -o "$B/proc/restart" "$T/restart.c" || { echo "proc_gate: restart.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/restart-bug" "$T/restart.c" || { echo "proc_gate: restart.c -DBUG does not build"; fail=1; }
# the consensus examples are real, tested fixtures too
$CC -O1 -no-pie -w -o "$B/proc/paxos" "$HERE/examples/paxos/paxos.c" || { echo "proc_gate: paxos.c does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/paxos2" "$HERE/examples/paxos/paxos2.c" || { echo "proc_gate: paxos2.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/paxos2-bug" "$HERE/examples/paxos/paxos2.c" || { echo "proc_gate: paxos2.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/paxos-bug" "$HERE/examples/paxos/paxos.c" || { echo "proc_gate: paxos.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/mono" "$HERE/test/proc/mono.c" || { echo "proc_gate: mono.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/mono-bug" "$HERE/test/proc/mono.c" || { echo "proc_gate: mono.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/noprogress" "$HERE/test/proc/noprogress.c" || { echo "proc_gate: noprogress.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DNEGFD -o "$B/proc/noprogress-stuck" "$HERE/test/proc/noprogress.c" || { echo "proc_gate: noprogress.c -DNEGFD does not build"; fail=1; }
# the two-threaded fixture needs pthreads (clone/clone3 + futex)
$CC -O1 -no-pie -w -o "$B/proc/twothread" "$HERE/test/proc/twothread.c" -lpthread || { echo "proc_gate: twothread.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/twothread-bug" "$HERE/test/proc/twothread.c" -lpthread || { echo "proc_gate: twothread.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/sleeps" "$T/sleeps.c" -lpthread || { echo "proc_gate: sleeps.c does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/twothread_join" "$HERE/test/proc/twothread_join.c" -lpthread || { echo "proc_gate: twothread_join.c does not build"; fail=1; }
for f in racewake raceemit raceborn; do
	$CC -O1 -no-pie -w -o "$B/proc/$f" "$T/$f.c" -lpthread || { echo "proc_gate: $f.c does not build"; fail=1; }
	$CC -O1 -no-pie -w -DBUG -o "$B/proc/$f-bug" "$T/$f.c" -lpthread || { echo "proc_gate: $f.c -DBUG does not build"; fail=1; }
done
$CC -O1 -no-pie -w -o "$B/proc/racepipe" "$T/racepipe.c" -lpthread || { echo "proc_gate: racepipe.c does not build"; fail=1; }
for f in fault kernelfd durack threadpos memrace syncpoint sendmsg files syscorner edges; do
	$CC -O1 -no-pie -w -o "$B/proc/$f" "$T/$f.c" -lpthread || { echo "proc_gate: $f.c does not build"; fail=1; }
done
$CC -O1 -no-pie -w -DSINGLE -o "$B/proc/durack-single" "$T/durack.c" || { echo "proc_gate: durack.c -DSINGLE does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/midmerge" "$T/midmerge.c" || { echo "proc_gate: midmerge.c does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/ackfirst" "$T/ackfirst.c" || { echo "proc_gate: ackfirst.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/ackfirst-bug" "$T/ackfirst.c" || { echo "proc_gate: ackfirst.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -DWRITEV -o "$B/proc/ackfirst-writev" "$T/ackfirst.c" || { echo "proc_gate: ackfirst.c -DWRITEV does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/fslog" "$T/fslog.c" || { echo "proc_gate: fslog.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/fslog-bug" "$T/fslog.c" || { echo "proc_gate: fslog.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG2 -o "$B/proc/fslog-bug2" "$T/fslog.c" || { echo "proc_gate: fslog.c -DBUG2 does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/fsfail" "$T/fsfail.c" || { echo "proc_gate: fsfail.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/syncpoint-bug" "$T/syncpoint.c" -lpthread || { echo "proc_gate: syncpoint.c -DBUG does not build"; fail=1; }
$CC -O1 -no-pie -w -DC11 -DBUG -o "$B/proc/syncpoint-c11-bug" "$T/syncpoint.c" -lpthread || { echo "proc_gate: syncpoint.c -DC11 -DBUG does not build"; fail=1; }
# A multi-threaded node passes only when built with the race runtime:
# compiled with -fsanitize=thread, linked with libcmc-race.a in place of
# the sanitizer's own runtime (cmc(7), THREADS).  tsan NAME SRC [FLAGS]
# builds $B/proc/NAME that way.
tsan() {
	name=$1 src=$2; shift 2
	if ! $CC -O1 -fno-pie -w -fsanitize=thread "$@" -c -o "$B/proc/$name.o" "$src" ||
		! $CC -no-pie -o "$B/proc/$name" "$B/proc/$name.o" "$B/libcmc-race.a" -lpthread; then
		echo "proc_gate: $src does not build with the race runtime"; fail=1
	fi
}
for f in twothread twothread_join racewake raceemit raceborn durack memrace syncpoint kernelfd timedwait; do
	tsan "$f-tsan" "$T/$f.c"
done
tsan raceemit-bug-tsan "$T/raceemit.c" -DBUG
tsan atomic128-tsan "$T/atomic128.c" -mcx16
tsan threadexit-tsan "$T/threadexit.c"
tsan memcopy-tsan "$T/memcopy.c"
tsan memcopy-chk-tsan "$T/memcopy.c" -DCHK
tsan syncedge-tsan "$T/syncedge.c"
tsan syncedge-chk-tsan "$T/syncedge.c" -DCHK
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
	$PFX "$CMC" "$@" > "$OUT/out.txt" 2>&1
	rc=$?
	if [ "$rc" = "$want" ] && grep -q -- "$needle" "$OUT/out.txt"; then
		echo "ok   $desc -> exit $rc"
	else
		echo "FAIL $desc -> exit $rc (wanted $want and \"$needle\")"
		sed 's/^/     /' "$OUT/out.txt" | head -30
		fail=1
	fi
}
# expect_by SECS DESC EXIT NEEDLE cmc-args...: expect, for a run that
# must end by itself within SECS seconds (a hang is exit 124, a FAIL)
PFX=
expect_by() {
	PFX="timeout $1"; shift
	expect "$@"
	PFX=
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
# refinement (Milestone C): the shipping Paxos binary is checked against
# the REUSABLE abstract model examples/spec/paxos.cml via a binding --
# the same abstract model `cmc --spec` checks standalone and TLC
# calibrates.  Correct refines; the planted bug does not.
expect "paxos: the binary refines the abstract spec" 0 "No error has been found" $(runx "$HERE/examples/spec/paxos-refine.model" paxos) --refine "$HERE/examples/spec/paxos.cml" --bind "$HERE/examples/spec/paxos.bind"
expect "paxos: the bug does NOT refine (agreement)"  1 "does not refine" $(runx "$HERE/examples/spec/paxos-refine.model" paxos-bug) --refine "$HERE/examples/spec/paxos.cml" --bind "$HERE/examples/spec/paxos.bind" --simulate --seed 1 --walks 3000
# one model, many implementations: a SECOND, structurally different Paxos
# (3 symmetric nodes, different hook layout) refines the SAME paxos.cml,
# with only its binding (paxos2.bind) changed.  Its model requires that
# some node decides, so a pass is not vacuous, and its planted bug makes
# two nodes decide differently, which the binding maps onto the spec's
# agreement.
expect "paxos2: a second impl refines the same spec" 0 "No error has been found" $(runx "$HERE/examples/spec/paxos2-refine.model" paxos2) --refine "$HERE/examples/spec/paxos.cml" --bind "$HERE/examples/spec/paxos2.bind"
expect "paxos2: its bug does NOT refine (agreement)" 1 "abstract invariant agreement is violated" $(runx "$HERE/examples/spec/paxos2-refine.model" paxos2-bug) --refine "$HERE/examples/spec/paxos.cml" --bind "$HERE/examples/spec/paxos2.bind"
# action-level refinement with stuttering (Milestone C2): a step the
# abstract model forbids is caught even when every state it reaches is
# in range -- which a state-level (C1) check cannot see.  mono.cml says
# the counter may step up only by one; the by-one build refines at the
# step level, the by-two build does not, yet C1 passes BOTH (the gap C2
# closes).  --refine-steps refuses a spec whose variables are not all
# bound rather than guess.
expect "mono: C1 passes the by-two bug (the gap)" 0 "No error has been found" $(runx "$HERE/test/proc/mono.model" mono-bug) --refine "$HERE/test/proc/mono.cml" --bind "$HERE/test/proc/mono.bind"
expect "mono: C2 passes the by-one impl"          0 "No error has been found" $(runx "$HERE/test/proc/mono.model" mono) --refine "$HERE/test/proc/mono.cml" --bind "$HERE/test/proc/mono.bind" --refine-steps
expect "mono: C2 catches the by-two step"         1 "no matching abstract action" $(runx "$HERE/test/proc/mono.model" mono-bug) --refine "$HERE/test/proc/mono.cml" --bind "$HERE/test/proc/mono.bind" --refine-steps
# a binding that fails to evaluate ends the check (exit 3), in each
# search and at each level; and C2 under --simulate still catches the
# by-two step
printf '[fields]\ncval = 0\n[map]\nc = "node[0].cval / 0"\n' > "$OUT/mono-div0.bind"
for o in "" "--refine-steps" "--simulate --seed 1 --walks 20" "--simulate --seed 1 --walks 20 --refine-steps"; do
	# shellcheck disable=SC2086
	expect "a binding that fails to evaluate${o:+ ($o)}" 3 "refinement: divide by zero" $(runx "$HERE/test/proc/mono.model" mono) --refine "$HERE/test/proc/mono.cml" --bind "$OUT/mono-div0.bind" $o
done
# a binding over the reply log: repl acks 5 from node 0 once, which a
# spec with one ack step refines and a spec without any does not
printf 'module rr\nvar n : 0 .. 1\ninit n == 0\naction ack():\n  when n == 0\n  n%s = 1\ninvariant small: n <= 1\n' "'" > "$OUT/rr.cml"
sed 's/^module rr$/module rr0/; s/^invariant small: n <= 1$/invariant none: n == 0/' "$OUT/rr.cml" > "$OUT/rr0.cml"
printf '[map]\nn = "count r in replies : r.src == 0 and r.val == 5"\n' > "$OUT/rr.bind"
expect "a binding over replies refines one ack" 0 "No error has been found" $(run repl repl) --refine "$OUT/rr.cml" --bind "$OUT/rr.bind" --refine-steps
expect "a binding over replies sees the ack"    1 "abstract invariant none is violated" $(run repl repl) --refine "$OUT/rr0.cml" --bind "$OUT/rr.bind"
# a binding reads a byte past the nodes or past a node's record as -1
printf '[fields]\ncval = 0\n[map]\nc = "node[0].cval + node[5][0] + node[0][40] + 2"\n' > "$OUT/mono-past.bind"
expect "a binding reads past the record as -1" 0 "No error has been found" $(runx "$HERE/test/proc/mono.model" mono) --refine "$HERE/test/proc/mono.cml" --bind "$OUT/mono-past.bind" --refine-steps
printf '[fields]\ncval = 0\n[map]\nc = "node[0].cval + node[5][0] + node[0][40] + 1"\n' > "$OUT/mono-out.bind"
expect "a bound value outside the spec's domain" 3 "c = -1 is outside its domain" $(runx "$HERE/test/proc/mono.model" mono) --refine "$HERE/test/proc/mono.cml" --bind "$OUT/mono-out.bind" --refine-steps
expect "mono: C2 under --simulate catches the by-two step" 1 "no matching abstract action" $(runx "$HERE/test/proc/mono.model" mono-bug) --refine "$HERE/test/proc/mono.cml" --bind "$HERE/test/proc/mono.bind" --refine-steps --simulate --seed 1 --walks 20
expect "C2 refuses a partly-bound spec"           3 "needs every spec variable bound" $(runx "$HERE/examples/spec/paxos-refine.model" paxos) --refine "$HERE/examples/spec/paxos.cml" --bind "$HERE/examples/spec/paxos.bind" --refine-steps
expect "raft: election safety holds"        0 "No error has been found" $(runx "$HERE/examples/raft/raft.model" raft)
expect "raft: double vote -> two leaders"   1 "election_safety is violated" $(runx "$HERE/examples/raft/raft.model" raft-bug)
expect "raft: --simulate finds it"          1 "election_safety is violated" $(runx "$HERE/examples/raft/raft.model" raft-bug) --simulate --seed 1 --walks 2000
expect "raftlog: no committed entry is lost" 0 "No error has been found" $(runx "$HERE/examples/raft/raftlog.model" raftlog)
expect "raftlog: early-ack loses a committed entry" 1 "no_lost_committed is violated" $(runx "$HERE/examples/raft/raftlog.model" raftlog-bug)
expect "raftlog: --simulate finds it"       1 "no_lost_committed is violated" $(runx "$HERE/examples/raft/raftlog.model" raftlog-bug) --simulate --seed 1 --walks 2000
# hc's model requires that some participant adopts, so agreement is not
# a vacuous pass.  The full check and the AP-mode bug take about seven
# minutes together, so they run only with CMC_SLOW=1 (make
# check-examples-slow, on the calibration's triggers); the quick row
# checks a shorter run is INCOMPLETE rather than a pass.
expect "hc: a bounded run is not a pass" 2 "INCOMPLETE" $(runx "$HERE/examples/hc/hc.model" hc) --depth 12
if [ "${CMC_SLOW:-0}" = 1 ]; then
	expect "hc: agreement holds, and a participant adopts" 0 "No error has been found" $(runx "$HERE/examples/hc/hc.model" hc)
	expect "hc: AP-mode adoption breaks agreement" 1 "agreement is violated" $(runx "$HERE/examples/hc/hc.model" hc-bug) --timeouts 12 --simulate --seed 4 --walks 30000 --depth 600
fi
expect "zab: no committed txn is lost"       0 "No error has been found" $(runx "$HERE/examples/zab/zab.model" zab)
expect "zab: early-ack loses a commit on crash" 1 "no_lost_commit is violated" $(runx "$HERE/examples/zab/zab.model" zab-bug)
expect "zab: --simulate finds it"           1 "no_lost_commit is violated" $(runx "$HERE/examples/zab/zab.model" zab-bug) --simulate --seed 1 --walks 2000
expect "idle: time passing is not merged"  1 "never_late is violated" $(run idle idle)
expect "fifo: a connection is in order"    0 "No error has been found" $(run fifo fifo)
# two connections between one pair of nodes (soundness plan 3.7): each
# carries its own bytes, so a reply is never the peer's request
expect "crossrpc: connections never share bytes" 0 "No error has been found" $(run crossrpc crossrpc)
# two connections from one peer are two queues: the second's byte can be
# read first, which a checker naming a connection by its peer misses
expect "twoconn: the second connection's byte can arrive first" 1 "never_b_first is violated" $(run twoconn twoconn)
# drop-as-reset (ask 7): a drop resets the connection, it does not vanish
# one byte from a live stream.  With drops = 1 node 1 still never sees the
# second value ahead of the first -- a reset gives EOF, not reordering.
# Were a drop a silent head loss, this would report never_b_first
# violated (a counterexample TCP cannot produce).
expect "reset: a drop is a reset, not a lost byte" 0 "No error has been found" $(run reset fifo)
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
# ask 11/12: a binary that keeps its state hook but guards it on
# CMC_STATE_FD, and allocates with time()/posix_memalign through the
# preload, checks and holds -- the state fd is set per node and the
# preload keeps time() and the aligned allocators deterministic.
expect "guarded hook + time()/posix_memalign are deterministic" 0 "No error has been found" $(run guarded guarded)
expect "a depth bound is INCOMPLETE"       2 "depth bound" $(run durlog durlog-bug) --depth 2
expect "a state bound is INCOMPLETE"       2 "max states" $(runx "$HERE/examples/paxos/paxos.model" paxos) --max-states 10
expect "an over-long write is an error"    1 "limit is 64" $(run bigmsg bigmsg)
expect "the counterexample is named"       1 "crash n0" $(run durlog durlog-bug)
expect "a server idle in accept() can time out" 1 "never_fires is violated" $(run accept_timeout accept_timeout)
# finite timeouts on per-node clocks with bounded drift (2.2): a lease
# held for L(1 - rho) on the holder's clock ends before the granter's
# wait of L(1 + rho) for drift 50; L(1 + rho) is caught only through
# drift (it holds on equal clocks), and the correct margin gives way once
# the clocks may drift further than it allows for
expect "leasedrift: L(1 - rho) holds under drift 50"   0 "No error has been found" $(run leasedrift leasedrift)
expect "leasedrift: L(1 + rho) caught, node 0 slow"    1 "n0 clock slow" $(run leasedrift leasedrift-bug)
sed 's/^drift = 50 .*/drift = 0/' "$T/leasedrift.model" > "$OUT/ld0.model"
expect "leasedrift: L(1 + rho) holds on equal clocks"  0 "No error has been found" --model-file "$OUT/ld0.model" --binary "$B/proc/leasedrift-bug"
sed 's/^drift = 50 .*/drift = 200/' "$T/leasedrift.model" > "$OUT/ld200.model"
expect "leasedrift: drift 200 beats the margin"        1 "one_holder is violated" --model-file "$OUT/ld200.model" --binary "$B/proc/leasedrift"
# each call with a finite timeout returns as Linux returns it on expiry
# (0, nothing ready, select's timeval counted down, ETIMEDOUT from a
# futex wait or pthread_cond_timedwait) and the node's clock
# reads exactly the 7 ms timeout later; natively the record is the same
# but for the clock, which moves at least that far
for m in poll ppoll epoll select pselect nanosleep clock_nanosleep abstime futex futexabs condtimed; do
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[invariants]\nlinux = "node[0][0] < 1 or (node[0][1] == 1 and node[0][2] == 0 and node[0][3] == 7)"\nreached = "eventually node[0][0] == 1"\n' \
		"$B/proc/sleeps" "$m" > "$OUT/sl-$m.model"
	expect_by 60 "sleeps $m: a finite timeout fires on the node clock" 0 "No error has been found" --model-file "$OUT/sl-$m.model"
done
# a timed condition wait with a second thread signalling: the signal and
# the timeout can each come first, and the run is complete (race runtime)
for g in 1 2; do
	printf '[target]\nbinary = %s\nnodes = 1\n[invariants]\nnever%d = "node[0][1] != %d"\n' "$B/proc/timedwait-tsan" $g $g > "$OUT/tw$g.model"
done
expect_by 120 "timedwait: the signal can win"  1 "never1 is violated" --model-file "$OUT/tw1.model"
expect_by 120 "timedwait: the timeout can win" 1 "never2 is violated" --model-file "$OUT/tw2.model"
printf '[target]\nbinary = %s\nnodes = 1\n[invariants]\nsome = "node[0][0] != 1 or node[0][1] == 1 or node[0][1] == 2"\n' "$B/proc/timedwait-tsan" > "$OUT/tw.model"
expect_by 120 "timedwait: both outcomes, nothing else"  0 "No error has been found" --model-file "$OUT/tw.model"
printf '[target]\nbinary = %s\nnodes = 1\nargs = selectfd\n[invariants]\nany = "node[0][0] < 9"\n' "$B/proc/sleeps" > "$OUT/sl-selectfd.model"
expect_by 60 "sleeps: select on descriptors is refused" 3 "select or pselect6 on descriptors is not modelled" --model-file "$OUT/sl-selectfd.model"
sed 's/selectfd/selectforever/' "$OUT/sl-selectfd.model" > "$OUT/sl-selectforever.model"
expect_by 60 "sleeps: select with no timeout is refused" 3 "no descriptor and no timeout" --model-file "$OUT/sl-selectforever.model"
sed 's/selectfd/clockcpu/' "$OUT/sl-selectfd.model" > "$OUT/sl-clockcpu.model"
expect_by 60 "sleeps: a sleep on a CPU-time clock is refused" 3 "is not modelled (only REALTIME, MONOTONIC and BOOTTIME)" --model-file "$OUT/sl-clockcpu.model"
for m in setitimer alarm timer_create timerfd; do
	sed "s/selectfd/$m/" "$OUT/sl-selectfd.model" > "$OUT/sl-$m.model"
	expect_by 60 "sleeps: $m is refused" 3 "arms a real-time timer" --model-file "$OUT/sl-$m.model"
done

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

echo "-- async I/O: poll/fcntl/eventfd event loop"
# the poll-driven event loop (poll/ppoll, fcntl O_NONBLOCK, non-blocking
# accept/read).  The search drives the loop as it drives a deferred recv:
# it wakes the poll, services the accept, wakes the poll again, then
# delivers the request -- poll readiness never auto-delivers (M5).  The
# correct build stores the byte it read; the -DBUG build stores a fixed
# wrong byte, so once a request is received the stored byte is not what
# the client sent, which the invariant catches.
expect "pollloop: correct event loop holds"            0 "No error has been found" $(run pollloop pollloop)
expect "pollloop: a wrong stored byte is caught"       1 "stored_ok is violated" $(run pollloop pollloop-bug)
expect "pollloop: the counterexample names the wake"   1 "wake n1 listener" $(run pollloop pollloop-bug)
# epoll backend (davros eval ask 9): the epoll twin of pollloop.  libxtc's
# default Linux backend is epoll, so the shipped binary parks in
# epoll_wait over a persistent interest list, not poll().  The search
# drives it the same way (wake the listener, accept, wake the connection,
# deliver), and epoll_wait returns the epoll_event.data the loop
# registered so the server routes the ready fd.  Readiness is a search
# decision (M5), never automatic.  The correct build stores the byte it
# read; the -DBUG build stores a fixed wrong byte, which the invariant
# catches, naming the epoll wake in the trace.
expect "epollloop: correct epoll loop holds"           0 "No error has been found" $(run epollloop epollloop)
expect "epollloop: a wrong stored byte is caught"      1 "stored_ok is violated" $(run epollloop epollloop-bug)
expect "epollloop: the counterexample names the wake"  1 "wake n1 listener" $(run epollloop epollloop-bug)
# no-progress detection (davros eval ask 4): a safety-only model must not
# pass a program that never advances past its initial state.  The correct
# build progresses and holds; the -DNEGFD build only ever polls an
# untracked fd, never serves, and must be reported INCOMPLETE (exit 2).
expect "noprogress: a progressing loop holds"           0 "No error has been found" $(run noprogress noprogress)
expect "noprogress: a stuck loop is not a vacuous pass"  2 "made no progress" $(run noprogress noprogress-stuck)

echo "-- threads: per-thread scheduling, futex handoff"
# per-thread scheduling (Tier 3 ask 8, soundness plan 3.8): a server node
# whose main thread owns the socket and whose worker thread stores the
# result, handing the job over a mutex + condition variable (clone/clone3
# + futex underneath).  In a node with more than one thread each visible
# operation (a futex call, a call on a checker-owned fd, a state-hook
# write, a clone) ends the thread's step with its answer held, and the
# search chooses which thread runs next ("run n1 t1"), one at a time.  A
# new thread and its parent are both held at the clone's return.  The
# correct build stores the byte it was handed; the -DBUG build drops it
# across the handoff, which the invariant catches.
expect "twothread: the handoff holds in every schedule"  0 "No error has been found" $(run twothread twothread-tsan)
expect "twothread: without the race runtime it is INCOMPLETE" 2 "without the race runtime" $(run twothread twothread)
expect "twothread: a dropped handoff is caught"         1 "handoff is violated" $(run twothread twothread-bug)
expect "twothread: the counterexample names the delivery" 1 "deliver n0->n1" $(run twothread twothread-bug)
expect "twothread: the counterexample names the worker's run" 1 "run n1 t1" $(run twothread twothread-bug)
# the data crosses the handoff: reaching the stored value is search-driven
printf '[target]\nnodes = 2\n[fields]\nrecvd = 0\nstored = 1\n[invariants]\nprocessed = "eventually node[1].stored == 120"\n' > "$OUT/tt-reach.model"
expect "twothread: the worker reaches the stored value"  0 "No error has been found" --model-file "$OUT/tt-reach.model" --binary "$B/proc/twothread-tsan"
# a worker that exits after one job, joined by the main thread: exercises
# the thread-exit slot-free and the pthread_join futex on the worker's
# clear-tid word (woken by the checker's SYS_exit CLONE_CHILD_CLEARTID).
expect "twothread_join: exit + join holds"              0 "No error has been found" $(run twothread_join twothread_join-tsan)
# Each race below is missed by an engine that runs one order of a node's
# threads.  racewake: a worker woken before the data it reads is stored
# runs before the waker's next call in some schedule.  raceemit: two
# workers that read a shared counter, log it, and store it plus one,
# with no lock, lose an update when one runs between the other's read
# and store.  raceborn: a new thread's first send can come before its
# parent's next one.  Each correct build holds in every schedule.
expect "racewake: a wake before the store is caught"      1 "stored_first is violated" $(run racewake racewake-bug)
expect "racewake: the woken worker runs first"            1 "run n0 t1" $(run racewake racewake-bug)
expect "racewake: storing first holds"                    0 "No error has been found" $(run racewake racewake-tsan)
expect "raceemit: an unlocked increment loses an update"  1 "no_lost_update is violated" $(run raceemit raceemit-bug)
expect "raceemit: the locked increment holds"             0 "No error has been found" $(run raceemit raceemit-tsan)
expect "raceemit: the unlocked increment is a data race"   1 "data race on node 0" $(run raceemit raceemit-bug-tsan)
expect "raceborn: a new thread can send before its parent" 1 "a_first is violated" $(run raceborn raceborn-bug)
expect "raceborn: sending before the clone holds"         0 "No error has been found" $(run raceborn raceborn-tsan)
# durack: a worker's state-hook write takes effect when the worker
# reaches it, like a call on a checker-owned fd, so a crash can come
# between it and the worker's next call (the value durable, its ack not
# sent).  A hook write held before it ran made the two one step.
expect "durack: a crash between a durable write and its ack" 0 "No error has been found" $(run durack durack-tsan)
# A crash can fall between any two calls with an outside effect, in a
# single-threaded node too: durack-single makes both calls in one step,
# and ackfirst-bug acks before its durable write with no blocking call
# between them.
expect "durack-single: a crash between two calls of one step" 0 "No error has been found" $(run durack durack-single)
expect "ackfirst: durable before the ack holds"            0 "No error has been found" $(run ackfirst ackfirst)
expect "ackfirst: an ack before the durable write is lost" 1 "acked_durable is violated" $(run ackfirst ackfirst-bug)
# a writev() of the durable record is a crash point like write(); a
# pwrite() on it is the kernel's ESPIPE (a pipe), with no effect
expect "ackfirst: a crash between a durable writev and the ack" 1 "durable_then_ack is violated" $(run ackwritev ackfirst-writev)
# a world held mid-step is never merged on its (stale) state hook, so a
# crash budget cannot hide a violation reachable without one
expect "midmerge: the violation is found without a crash"   1 "notfirst is violated" $(run midmerge midmerge) --crashes 0
expect "midmerge: and with a crash in the budget"           1 "notfirst is violated" $(run midmerge midmerge)
# connect() is a crash point too: the connection is in the peer's
# backlog, so a crash before the first write leaves it empty
$CC -O1 -no-pie -w -o "$B/proc/conncrash" "$T/conncrash.c" || { echo "proc_gate: conncrash.c does not build"; fail=1; }
expect "conncrash: a crash between connect() and the first write" 1 "no_empty is violated" $(run conncrash conncrash)
# a crash may lose what the node sent and its peer has not read: the
# primary acks once REPL(v) is in its send queue, too early
$CC -O1 -no-pie -w -o "$B/proc/sendlost" "$T/sendlost.c" || { echo "proc_gate: sendlost.c does not build"; fail=1; }
expect "sendlost: a crash loses bytes still in the send queue" 1 "no_loss is violated" $(run sendlost sendlost)

echo "-- a modelled file system (plan 2.2, item 3)"
# fslog keeps its log in files under the model's [files] dir: pwrite and
# fdatasync for the log, a control file renamed into place and the
# directory fsynced, then the ack.  A crash keeps each file's synced
# bytes and a chosen prefix of the rest, and a rename only once its
# directory is synced.  -DBUG skips the directory fsync, -DBUG2 the
# fdatasync; each loses an acked value at a crash and a restart.
expect "fslog: synced log and control file hold"   0 "No error has been found" $(run fslog fslog)
expect "fslog: no directory fsync is caught"       1 "acked_durable is violated" $(run fslog fslog-bug)
expect "fslog: no fdatasync is caught"             1 "acked_durable is violated" $(run fslog fslog-bug2)
expect "fslog: the bugs need the crash"            0 "No error has been found" $(run fslog fslog-bug2) --crashes 0
# A seccomp filter the gate itself runs under outranks the checker's
# when it answers with an errno: the nix build sandbox answers the xattr
# calls ENOTSUP, so they never reach cmc (and change nothing).  Those
# rows are skipped, by name, only when this process is under a seccomp
# filter and a native xattr write gets ENOTSUP (95).
xattr_blocked=0
: > "$OUT/xa"
if grep -q '^Seccomp:[[:space:]]*2' /proc/self/status; then
	(cd "$OUT" && timeout 30 "$B/proc/files" 0 xattrprobe "$OUT/xa" 3>"$OUT/xa.state" > /dev/null 2>&1)
	[ "$(od -An -tu1 "$OUT/xa.state" | tr -s ' \n' ' ' | cut -d' ' -f7)" = 95 ] && xattr_blocked=1
fi
echo "files: xattr probe: seccomp $(grep '^Seccomp:' /proc/self/status | tr -s ' \t' ' '), state '$(od -An -tu1 "$OUT/xa.state" 2>/dev/null | tr -s ' \n' ' ')', blocked $xattr_blocked"
xattr_skip() {	# xattr_skip MODE: true if this host's sandbox answers MODE itself
	case $1 in *xattr*) [ "$xattr_blocked" = 1 ] ;; *) false ;; esac
}
# fsfail: a write or fsync may fail (eio, enospc).  A failed fsync
# forgets the unwritten data, so retrying the fsync alone loses an acked
# value at a crash (retry); writing again first does not (rewrite).
# Past the bytes budget, and on the calls the model does not have, the
# check ends (exit 3).
ffm() {	# ffm MODE [sed script]: fsfail.model with args = MODE
	sed "s/^args = .*/args = $1/;${2:-}" "$T/fsfail.model" > "$OUT/ff-$1.model"
	echo "--model-file $OUT/ff-$1.model --binary $B/proc/fsfail"
}
expect "fsfail: rewrite after EIO holds"            0 "No error has been found" $(ffm rewrite)
expect "fsfail: an fsync retried after EIO loses"   1 "acked_durable is violated" $(ffm retry)
expect "fsfail: the counterexample names the failed call" 1 "fail n0 t0 (EIO)" $(ffm retry)
expect "fsfail: the loss needs the EIO"             0 "No error has been found" $(ffm retry 's/^eio = 1/eio = 0/')
expect "fsfail: rewrite after ENOSPC holds"         0 "No error has been found" $(ffm rewrite 's/^eio = 1/enospc = 1/')
expect "fsfail: past the bytes budget is refused"   3 "past the model's \[files\] bytes" $(ffm big)
expect "fsfail: O_DIRECT is refused"                3 "which are not modelled (O_DIRECT" $(ffm odirect)
expect "fsfail: mmap of a modelled file is refused" 3 "maps modelled file fd" $(ffm mmap)
expect "fsfail: link is refused"                    3 "is not modelled (links" $(ffm link)
if xattr_skip xattr; then echo "skip fsfail: xattr (the sandbox answers it before the checker)"
else expect "fsfail: an xattr read is refused"      3 "on modelled file fd 501 is not modelled" $(ffm xattr)
fi
expect "fsfail: a rename out of the model is refused" 3 "between a modelled and an unmodelled path" $(ffm cross)
expect "fsfail: a rename with flags is refused"     3 "is not modelled (links" $(ffm noreplace)
expect "fsfail: unlinkat of a directory is refused" 3 "is not modelled (links" $(ffm rmdir)
expect "fsfail: a rename of a directory is refused" 3 "is not modelled (a rename between directories, or of a directory)" $(ffm dirmv)
expect "fsfail: too many unsynced writes are refused" 3 "passes a limit of the file model" $(ffm ops)
expect "fsfail: a .. in a modelled path is refused" 3 "has a .. part, which is not modelled" $(ffm dotdot)
expect "fsfail: a name past the model's limit is refused" 3 "has a name longer than 31 bytes" $(ffm longname)
expect "fsfail: a path relative to a modelled dirfd is refused" 3 "relative to modelled directory fd" $(ffm dirrel)
expect "fsfail: poll of a modelled file is refused" 3 "polls modelled file fd" $(ffm pollfile)
expect "fsfail: too many ways to crash are refused" 3 "could leave its modelled files in more than 256 ways" $(ffm ways)
# fsfail calls: every file call the model answers, natively in an empty
# directory and then under cmc on a modelled one, must give the same
# answers (one record, reached in every run; the properties split it
# into tens, since each holds at most 255 bytes)
CD=$OUT/ffcalls
[ -d "$CD" ] && find "$CD" -mindepth 1 -user "$(id -u)" -delete
mkdir -p "$CD"
(timeout 30 "$B/proc/fsfail" 0 calls "$CD" 3>"$OUT/ffcalls.state" > /dev/null 2>&1)
native=$(od -An -tu1 "$OUT/ffcalls.state" | tr -s ' \n' ' ' | sed 's/^ //; s/ $//')
# shellcheck disable=SC2086
set -- $native
if [ $# -gt 40 ] && [ "$1" = $(($# - 1)) ]; then
	shift
	inv="" k=0
	for c in "$@"; do
		inv="$inv and node[0][$k] == $c"; k=$((k + 1))
		if [ $((k % 10)) = 0 ] || [ $k = $# ]; then echo "c$k = \"eventually node[0][0] >= 0$inv\""; inv=""; fi
	done > "$OUT/ffcalls.inv"
	printf '[target]\nbinary = %s\nnodes = 1\nargs = calls %s\n[files]\ndir = %s\n[invariants]\n' \
		"$B/proc/fsfail" "$CD" "$CD" > "$OUT/ffcalls.model"
	cat "$OUT/ffcalls.inv" >> "$OUT/ffcalls.model"
	echo 'any = "node[0][0] <= 255"' >> "$OUT/ffcalls.model"
	expect_by 60 "fsfail: the modelled file calls give Linux's answers" 0 "No error has been found" --model-file "$OUT/ffcalls.model"
else echo "FAIL fsfail calls: the native run gave '$native'"; fail=1; fi
# threadpos: worlds with a multi-threaded node are never merged, since a
# thread's record cannot tell apart two iterations of one loop (loop), a
# thread born on the stack of one already joined (seq), or memory that
# two threads update between their calls (order).  Each reachable state
# must be found.  A thread that loops forever makes the search a tree
# with no end, which a bound turns into INCOMPLETE (spin).
for m in loop seq order spin; do
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[invariants]\n' "$B/proc/threadpos" "$m" > "$OUT/tp-$m.model"
done
printf 'never = "node[0][0] == 0"\n' >> "$OUT/tp-loop.model"
printf 'never = "node[0][0] == 0"\n' >> "$OUT/tp-seq.model"
printf 'any = "node[0][0] == 0"\n' >> "$OUT/tp-spin.model"
cp "$OUT/tp-order.model" "$OUT/tp-order12.model"
printf 'not12 = "node[0][0] == 0 or node[0][1] != 12"\n' >> "$OUT/tp-order12.model"
printf 'not21 = "node[0][0] == 0 or node[0][1] != 21"\n' >> "$OUT/tp-order.model"
expect "threadpos: a loop's second iteration is reached"    1 "never is violated" --model-file "$OUT/tp-loop.model"
expect "threadpos: a thread on a joined thread's stack runs" 1 "never is violated" --model-file "$OUT/tp-seq.model"
expect "threadpos: one order of a memory update is reached" 1 "not12 is violated" --model-file "$OUT/tp-order12.model"
expect "threadpos: the other order is reached too"          1 "not21 is violated" --model-file "$OUT/tp-order.model"
expect_by 300 "threadpos: an endless thread is INCOMPLETE"  2 "max states reached" --model-file "$OUT/tp-spin.model" --max-states 300
# The default bound on that tree (CMC_TREE_CAP, 1000000) takes hours to
# reach, so a copy of cmc built with a bound of 200 checks its branch:
# without --max-states the search stops there, INCOMPLETE, and with it
# --max-states decides, even past the built-in bound.
mkdir -p "$B/cap200"
printf '#include "cmc.h"\nint main(int argc, char **argv) { return cmc_main(argc, argv, NULL, 0); }\n' > "$B/cap200/main.c"
# it compiles the engine, so it takes the build's CFLAGS (a word list)
# shellcheck disable=SC2086
if $CC -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -w ${CFLAGS:-} -DCMC_TREE_CAP=200 -I"$HERE/include" -o "$B/cap200/cmc" \
	"$B/cap200/main.c" "$HERE/src/cmc_cli.c" "$HERE/src/cmc_proc.c" "$HERE/src/cmc_spec.c" "$HERE/src/cmc_fs.c" "$HERE/src/cmc.c"; then
	CMC=$B/cap200/cmc
	export CMC_PRELOAD="$B/cmc-preload.so"
	expect_by 120 "tree bound: an endless thread stops at the built-in bound" 2 "200 worlds with a multi-threaded node were searched without merging" --model-file "$OUT/tp-spin.model"
	expect_by 120 "tree bound: --max-states takes precedence" 2 "max states reached; 300 distinct" --model-file "$OUT/tp-spin.model" --max-states 300
	unset CMC_PRELOAD
	CMC=$B/cmc
else echo "proc_gate: cmc with CMC_TREE_CAP=200 does not build"; fail=1; fi
# racepipe: two threads share two kernel pipes.  Their contents are not
# in the world, so the node is refused rather than searched.
expect "racepipe: threads sharing kernel pipes are refused" 3 "uses kernel fd" $(run racepipe racepipe)

# A node the checker cannot follow must end the check, never hang it.
# A fault signal (abort, a failed assert) parks a ptrace-seized thread in
# a signal-delivery stop, which makes no call: the checker reports the
# node killed.  A thread blocked in a kernel call it does not see makes
# none either: the check ends with the lull error after 10 s.  In a node
# with more than one thread, a call on a kernel fd (a pipe the threads
# share) is refused before it runs: the pipe's contents are not in the
# world, so schedules that leave different bytes in it would merge.  A
# worker's writes to stdout and stderr are allowed.
for m in main worker; do
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[invariants]\nany = "node[0][0] <= 1"\n' "$B/proc/fault" "$m" > "$OUT/fault-$m.model"
done
for m in alone block order mixed stdio; do
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[invariants]\nany = "node[0][0] <= 1"\n' "$B/proc/kernelfd" "$m" > "$OUT/kfd-$m.model"
done
expect_by 60 "fault: an abort() is reported, not waited on"     1 "killed by signal 6" --model-file "$OUT/fault-main.model"
expect_by 60 "fault: an abort() in a worker thread is reported" 1 "killed by signal 6" --model-file "$OUT/fault-worker.model"
expect_by 60 "kernelfd: a read blocked in the kernel ends the check" 1 "made no checked syscall" --model-file "$OUT/kfd-alone.model"
expect_by 60 "kernelfd: a worker's blocking pipe read is refused" 3 "uses kernel fd" --model-file "$OUT/kfd-block.model"
expect_by 60 "kernelfd: two workers writing one pipe are refused" 3 "uses kernel fd" --model-file "$OUT/kfd-order.model"
expect_by 60 "kernelfd: a pipe beside an eventfd is refused"     3 "uses kernel fd" --model-file "$OUT/kfd-mixed.model"
printf '[target]\nbinary = %s\nnodes = 1\nargs = stdio\n[invariants]\nany = "node[0][0] <= 1"\n' "$B/proc/kernelfd-tsan" > "$OUT/kfd-stdio-tsan.model"
expect_by 60 "kernelfd: a worker may print to stdout and stderr" 0 "No error has been found" --model-file "$OUT/kfd-stdio-tsan.model"

# Memory interleavings (plan 2.1, item 3).  syncpoint: a worker that runs
# between two uncontended critical sections of main sees a half-published
# value; no system call is made there, so only the preload's marker
# before each lock (pthread and C11) makes it a choice point.  memrace:
# instrumented with the race runtime, an unlocked counter and a relaxed
# handoff are data races; a locked counter and a seq_cst handoff pass; a
# relaxed atomic makes a pass INCOMPLETE (weak memory is not explored).
# Uninstrumented, a multi-threaded node cannot pass.
expect "syncpoint: a switch at an uncontended lock is explored" 1 "whole is violated" $(run syncpoint syncpoint-bug)
expect "syncpoint: the same through C11 mtx_lock"             1 "whole is violated" $(run syncpoint syncpoint-c11-bug)
expect "syncpoint: publishing under the lock holds"           0 "No error has been found" $(run syncpoint syncpoint-tsan)
for m in unlocked locked seqcst relaxed relaxedadd; do
	for v in "" -tsan; do
		printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[fields]\nphase = 0\nvalue = 1\n[invariants]\nwhole = "node[0].phase < 2 or node[0].value == 2 or node[0].value == 7"\ndone = "eventually node[0].phase == 2"\n' \
			"$B/proc/memrace$v" "$m" > "$OUT/mr-$m$v.model"
	done
done
expect "memrace: an unlocked counter is not a pass uninstrumented" 2 "without the race runtime" --model-file "$OUT/mr-unlocked.model"
expect "memrace: an unlocked counter is a data race"          1 "data race on node 0" --model-file "$OUT/mr-unlocked-tsan.model"
expect "memrace: a locked counter holds"                      0 "No error has been found" --model-file "$OUT/mr-locked-tsan.model"
expect "memrace: a seq_cst handoff holds"                     0 "No error has been found" --model-file "$OUT/mr-seqcst-tsan.model"
expect "memrace: a relaxed handoff is a data race"            1 "data race on node 0" --model-file "$OUT/mr-relaxed-tsan.model"
expect "memrace: a relaxed atomic makes a pass INCOMPLETE"    2 "weaker than seq_cst" --model-file "$OUT/mr-relaxedadd-tsan.model"
# atomic128: 16-byte atomics (-mcx16) link against the runtime, order a
# payload when seq_cst, race when relaxed, and compute their operations.
for m in seqcst relaxed rmw; do
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[fields]\nphase = 0\nvalue = 1\n[invariants]\nwhole = "node[0].phase < 2 or node[0].value == 7 or node[0].value == 25"\ndone = "eventually node[0].phase == 2"\n' \
		"$B/proc/atomic128-tsan" "$m" > "$OUT/a128-$m.model"
done
expect "atomic128: a seq_cst 16-byte handoff holds"           0 "No error has been found" --model-file "$OUT/a128-seqcst.model"
expect "atomic128: a relaxed 16-byte handoff is a data race"  1 "data race on node 0" --model-file "$OUT/a128-relaxed.model"
expect "atomic128: 16-byte fetch_add and compare-exchange"    0 "No error has been found" --model-file "$OUT/a128-rmw.model"
# threadexit: a thread that ends by pthread_exit or thrd_exit, from its
# start function or deeper, gives its join the same edge as a return
# (on 2.1.0 each was reported as a race, and pthread_exit's first-use
# load of libgcc_s was refused as a kernel fd); a read before the join
# is still a race.
for m in exit thrd nested early; do
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[fields]\nphase = 0\nvalue = 1\n[invariants]\nwhole = "node[0].phase < 2 or node[0].value == 7"\ndone = "eventually node[0].phase == 2"\n' \
		"$B/proc/threadexit-tsan" "$m" > "$OUT/te-$m.model"
done
expect "threadexit: pthread_exit publishes to the join"        0 "No error has been found" --model-file "$OUT/te-exit.model"
expect "threadexit: thrd_exit publishes to thrd_join"          0 "No error has been found" --model-file "$OUT/te-thrd.model"
expect "threadexit: pthread_exit from a nested call"           0 "No error has been found" --model-file "$OUT/te-nested.model"
expect "threadexit: a read before the join is a data race"     1 "data race on node 0" --model-file "$OUT/te-early.model"
# memcopy: a memcpy, memset or memmove into a buffer another thread reads
# unlocked is a data race, whether the compiler left a plain call (gcc),
# a __tsan_ call (clang) or a fortified __*_chk call (both, under
# _FORTIFY_SOURCE); under a mutex it holds.  On 2.1.0 every plain and
# fortified form passed.
for v in "" -chk; do
	for m in cpy set move cpylock; do
		printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[fields]\nphase = 0\nvalue = 1\n[invariants]\nsane = "node[0].phase < 2 or node[0].value == 1"\ndone = "eventually node[0].phase == 2"\n' \
			"$B/proc/memcopy$v-tsan" "$m" > "$OUT/mc-$m$v.model"
	done
	expect "memcopy$v: an unlocked memcpy is a data race"  1 "data race on node 0" --model-file "$OUT/mc-cpy$v.model"
	expect "memcopy$v: an unlocked memset is a data race"  1 "data race on node 0" --model-file "$OUT/mc-set$v.model"
	expect "memcopy$v: an unlocked memmove is a data race" 1 "data race on node 0" --model-file "$OUT/mc-move$v.model"
	expect "memcopy$v: a memcpy under the mutex holds"     0 "No error has been found" --model-file "$OUT/mc-cpylock$v.model"
	# the C library's string and format writers (passed on 2.2's first cut)
	for m in scpy sncpy scat spcpy spr snpr vsnpr; do
		printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[fields]\nphase = 0\nvalue = 1\n[invariants]\nsane = "node[0].phase < 2 or node[0].value == 1"\n' \
			"$B/proc/memcopy$v-tsan" "$m" > "$OUT/mc-$m$v.model"
		expect "memcopy$v: an unlocked $m is a data race" 1 "data race on node 0" --model-file "$OUT/mc-$m$v.model"
	done
done
# syncedge: the review of the race runtime (docs/research/04-review-
# race-runtime.md).  Each of these passed on 2.1.0.  rwread: two readers
# of a rwlock are not ordered.  casfail: a failed compare-exchange with a
# relaxed failure order is no acquire (casok, seq_cst, holds).  bargen:
# one barrier round does not order the next.  sem2: a semaphore of count
# two lets both threads in at once, and try*: a trylock fails only while
# the other thread holds the lock; both lie inside a section the search
# runs without a switch, so a pass is INCOMPLETE.  readbuf: the bytes
# a read stores are an access like any other.
for m in rwread casfail casok bargen sem2 trymutex trysem tryrw readbuf; do
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s\n[fields]\nphase = 0\nvalue = 1\n[invariants]\nok = "node[0].phase < 2 or node[0].value == 1"\ndone = "eventually node[0].phase == 2"\n' \
		"$B/proc/syncedge-tsan" "$m" > "$OUT/se-$m-tsan.model"
done
expect "syncedge: two readers of a rwlock race"                 1 "data race on node 0" --model-file "$OUT/se-rwread-tsan.model"
expect "syncedge: a relaxed failed compare-exchange is no acquire" 1 "data race on node 0" --model-file "$OUT/se-casfail-tsan.model"
expect "syncedge: a seq_cst failed compare-exchange acquires"   0 "No error has been found" --model-file "$OUT/se-casok-tsan.model"
expect "syncedge: a barrier round does not order the next"      1 "data race on node 0" --model-file "$OUT/se-bargen-tsan.model"
expect "syncedge: a read into a buffer races a plain load of it" 1 "data race on node 0" --model-file "$OUT/se-readbuf-tsan.model"
expect "syncedge: the same through a fortified __read_chk"     1 "data race on node 0" --model-file "$OUT/se-readbuf-tsan.model" --binary "$B/proc/syncedge-chk-tsan"
expect "syncedge: a semaphore of count two admits two"          1 "data race on node 0" --model-file "$OUT/se-sem2-tsan.model"
expect "syncedge: a trylock is INCOMPLETE"                      2 "admits more than one thread" --model-file "$OUT/se-trymutex-tsan.model"
expect "syncedge: a sem_trywait is INCOMPLETE"                  2 "admits more than one thread" --model-file "$OUT/se-trysem-tsan.model"
expect "syncedge: a tryrdlock is INCOMPLETE"                    2 "admits more than one thread" --model-file "$OUT/se-tryrw-tsan.model"
# two nodes running the racy program: the first report wins
sed 's/^nodes = 1$/nodes = 2/' "$OUT/mr-unlocked-tsan.model" > "$OUT/mr-unlocked2-tsan.model"
expect "memrace: two racing nodes, one report"                1 "data race on node" --model-file "$OUT/mr-unlocked2-tsan.model"
# sendmsg on a kernel socket (below fd 500) is seen too: a second filter
# stops it for the checker, which sends it on in a node with one thread
# and refuses it in one with more.  In worker mode main's send can land
# between the worker's two, which the world cannot tell apart.
printf '[target]\nbinary = %s\nnodes = 1\nargs = worker\n[invariants]\nnever_b = "node[0][0] == 0 or node[0][1] != 98"\n' "$B/proc/sendmsg" > "$OUT/sm-worker.model"
printf '[target]\nbinary = %s\nnodes = 1\nargs = alone\n[invariants]\nback = "node[0][0] == 0 or node[0][1] == 120"\nreached = "eventually node[0][0] == 1"\n' "$B/proc/sendmsg" > "$OUT/sm-alone.model"
expect_by 60 "sendmsg: a worker's send on a kernel socket is refused" 3 "uses kernel fd" --model-file "$OUT/sm-worker.model"
expect_by 60 "sendmsg: one thread's send on a kernel socket runs" 0 "No error has been found" --model-file "$OUT/sm-alone.model"

echo "-- the file system is read-only to a node (plan 2.1, item 2)"
# Durable state goes through fd 193, which the world holds.  A file a
# node writes would survive a crash and every later replay outside the
# world: persist keeps its state in a file, crashes, restarts and reads
# it back, which 2.0.0 passed; its write open is refused now.  A
# read-only open, and lseek and fsync on it in a node with one thread,
# still go to the kernel (config); a worker's lseek on an fd main also
# uses is refused (lseek), as is every call that changes the namespace
# or metadata.  On an owned socket lseek, fsync and the rest get Linux's
# answer, which sockerr compares with a native run.
FD=$OUT/files
mkdir -p "$FD"
printf 'k' > "$FD/config"
filesm() {	# filesm NAME MODE PATH [extra model lines]: a model for files.c
	printf '[target]\nbinary = %s\nnodes = 1\nargs = %s %s\n[environment]\n%s\n[invariants]\n%s\n' \
		"$B/proc/files" "$2" "$3" "$4" "$5" > "$OUT/fs-$1.model"
}
filesm persist persist "$FD/state" "crashes = 1
restarts = 1" 'fresh = "node[0][0] != 2"'
: > "$FD/state"
expect_by 60 "files: a file written for state is refused"        3 "opens $FD/state for writing" --model-file "$OUT/fs-persist.model"
filesm config config "$FD/config" "" 'read = "node[0][0] != 9 and (node[0][0] == 0 or node[0][1] == 107)"
reached = "eventually node[0][0] == 1"'
expect_by 60 "files: a configuration file is read, sought, fsynced" 0 "No error has been found" --model-file "$OUT/fs-config.model"
filesm lseek lseek "$FD/config" "" 'any = "node[0][0] <= 1"'
expect_by 60 "files: a worker's lseek on a shared fd is refused"  3 "uses kernel fd" --model-file "$OUT/fs-lseek.model"
filesm devnull devnull "" "" 'wrote = "node[0][0] != 9"
reached = "eventually node[0][0] == 1"'
expect_by 60 "files: /dev/null may be opened for writing"        0 "No error has been found" --model-file "$OUT/fs-devnull.model"
filesm badpath badpath "" "" 'any = "node[0][0] <= 1"'
expect_by 60 "files: a path the checker cannot read is refused"  3 "cannot read" --model-file "$OUT/fs-badpath.model"
filesm durfsync durfsync "" "" 'synced = "node[0][0] != 9"
reached = "eventually node[0][0] == 1"'
expect_by 60 "files: fsync of fd 193 succeeds"                   0 "No error has been found" --model-file "$OUT/fs-durfsync.model"
# each call that changes the file system ends the check, and none of
# them touches $FD/none (a refused call never runs)
for m in open_wronly openat_rdwr openat_creat openat_trunc openat_append openat_tmpfile openat2 creat \
	unlink unlinkat rename renameat renameat2 mkdir mkdirat rmdir link linkat symlink symlinkat \
	truncate mknod mknodat chmod fchmodat fchmodat2 chown lchown fchownat utime utimes futimesat \
	utimensat setxattr lsetxattr setxattrat removexattr lremovexattr removexattrat file_setattr; do
	if xattr_skip "$m"; then echo "skip files: $m (the sandbox answers it before the checker)"; continue; fi
	filesm "$m" "$m" "$FD/none" "" 'any = "node[0][0] <= 1"'
	expect_by 60 "files: $m is refused" 3 "read-only to a node" --model-file "$OUT/fs-$m.model"
done
for m in fchmod fchown futimens fsetxattr fremovexattr ftruncate fallocate flock io_uring_setup; do
	if xattr_skip "$m"; then echo "skip files: $m (the sandbox answers it before the checker)"; continue; fi
	filesm "$m" "$m" "$FD/config" "" 'any = "node[0][0] <= 1"'
	expect_by 60 "files: $m is refused" 3 "read-only to a node\|not modelled" --model-file "$OUT/fs-$m.model"
done
if [ -e "$FD/none" ]; then echo "FAIL files: a refused call made $FD/none"; fail=1; fi
# Linux's errnos for lseek, fsync, fdatasync, ftruncate and
# sync_file_range on a TCP socket, natively, then under cmc on an owned one
native=$(cd "$FD" && timeout 30 "$B/proc/files" 0 sockerr 3>"$FD/sockerr.state" > /dev/null 2>&1; od -An -tu1 "$FD/sockerr.state" | tr -s ' \n' ' ' | sed 's/^ //')
# the records are [2][0][0] then [8][1][seven codes]
codes=$(echo "$native" | cut -d' ' -f6-12)
if [ "$codes" = "1 2 2 2 2 1 2" ]; then echo "ok   files: Linux gives a socket ESPIPE, EINVAL x4, ESPIPE, EINVAL"
else echo "FAIL files: Linux gave '$codes' for a socket, the fixture expects '1 2 2 2 2 1 2'"; fail=1; fi
inv=""
k=1
for c in $codes; do inv="$inv and node[0][$k] == $c"; k=$((k + 1)); done
filesm sockerr sockerr "" "" "linux = \"node[0][0] == 0 or (node[0][0] == 1$inv)\"
reached = \"eventually node[0][0] == 1\""
expect_by 60 "files: an owned socket gets Linux's errnos" 0 "No error has been found" --model-file "$OUT/fs-sockerr.model"

echo "-- the model file and the invariant language"
# Every malformed model file is refused before a node starts, with the
# line at fault; every invariant the language defines evaluates as its
# meaning says, on repl's states (node 1 stores what the client wrote,
# 5, and node 0 acks it; a reply is r.src 0, r.val 5).
MF=$OUT/modelfile
mkdir -p "$MF"
mf() {	# mf NAME EXIT NEEDLE CONTENT: a model file, checked against repl
	printf '%b' "$4" > "$MF/$1.model"
	expect_by 60 "model file: $1" "$2" "$3" --model-file "$MF/$1.model" --binary "$B/proc/repl"
}
H='[target]\nnodes = 2\n[environment]\nclient_write = 5\n'
mf long 3 "line too long" "${H}[invariants]\nx = \"$(printf '%600s' '' | tr ' ' 'x')\"\n"
mf header 3 "malformed section header" "[target\nnodes = 2\n"
mf trailing 3 "malformed section header" "[target] x\nnodes = 2\n"
mf section 3 "unknown section \\[bogus\\]" "[bogus]\n"
mf noeq 3 "expected key = value" "[target]\nnodes\n"
mf quote 3 "unterminated or trailing text" "${H}[invariants]\nx = \"node[0][0] == 0\n"
mf quoted 3 "unterminated or trailing text" "${H}[invariants]\nx = \"node[0][0] == 0\" y\n"
mf outside 3 "nodes outside a section" "nodes = 2\n"
mf nobinary 3 "binary is empty" "[target]\nbinary =\nnodes = 2\n"
mf nodes 3 "nodes = \"9\": expected an integer from 1 to 8" "[target]\nnodes = 9\n"
mf maxwrite 3 "max_write = \"0\"" "[target]\nnodes = 2\nmax_write = 0\n"
mf state 3 "state = disk is not supported" "[target]\nnodes = 2\nstate = disk\n"
mf statecanon 0 "No error has been found" "[target]\nnodes = 2\nstate = canon\nmax_write = 64\n[environment]\nclient_write = 5\n[invariants]\nok = \"node[0][0] < 9\" # a comment\n"
for k in client_write timeouts crashes drops restarts depth drift short_writes partitions; do
	mf "env-$k" 3 "$k = \"x\": expected an integer" "[target]\nnodes = 2\n[environment]\n$k = x\n"
done
for k in eio enospc bytes; do
	mf "files-$k" 3 "$k = \"x\": expected an integer" "[target]\nnodes = 2\n[files]\ndir = /data\n$k = x\n"
done
mf filesnodir 3 "eio and enospc need a dir" "${H}[files]\nenospc = 1\n[invariants]\nok = \"node[0][0] < 9\"\n"
mf filesslash 0 "No error has been found" "${H}[files]\ndir = /data//\n[invariants]\nok = \"node[0][0] < 9\"\n"
for d in data / /a/./b /a//b "/$(printf '%300s' '' | tr ' ' 'd')"; do
	mf "filesdir-$(echo "$d" | tr -c 'a-z\n' '_' | cut -c1-8)" 3 "dir must be an absolute directory" "[target]\nnodes = 2\n[files]\ndir = $d\n"
done
mf depthok 2 "depth bound reached" "$H""depth = 2\n[invariants]\nok = \"node[0][0] < 9\"\n"
mf fieldlong 3 "field name too long" "[target]\nnodes = 2\n[fields]\n$(printf '%40s' '' | tr ' ' 'f') = 1\n"
mf fieldtwice 3 "field a defined twice" "[target]\nnodes = 2\n[fields]\na = 1\na = 2\n"
mf fieldoff 3 "a = \"255\": expected an integer from 0 to 254" "[target]\nnodes = 2\n[fields]\na = 255\n"
mf fieldmany 3 "more than 16 fields" "[target]\nnodes = 2\n[fields]\n$(for i in $(seq 0 16); do printf 'f%d = %d\\n' "$i" "$i"; done)"
mf invmany 3 "more than 8 invariants" "${H}[invariants]\n$(for i in $(seq 0 8); do printf 'i%d = \"node[0][0] < 9\"\\n' "$i"; done)"
mf invlong 3 "invariant name or expression too long" "${H}[invariants]\n$(printf '%40s' '' | tr ' ' 'n') = \"node[0][0] < 9\"\n"
mf invtwice 3 "invariant a defined twice" "${H}[invariants]\na = \"node[0][0] < 9\"\na = \"node[0][0] < 9\"\n"
mf unknownkey 3 "unknown key bogus in \\[target\\]" "[target]\nnodes = 2\nbogus = 1\n"
mf nonodes 3 "\\[target\\] nodes is required" "[target]\nargs = x\n[invariants]\nx = \"node[0][0] < 9\"\n"
mf noinv 3 "no invariant to check" "[target]\nnodes = 2\n"
expect_by 60 "model file: a missing file" 3 "cannot open model file" --model-file "$MF/no-such.model" --binary "$B/proc/repl"
# the invariant language, each with a parse error or its meaning
inv() {	# inv NAME EXIT NEEDLE EXPR...: invariants over repl's states
	n=$1 want=$2 needle=$3; shift 3
	body=""
	k=0
	for e in "$@"; do body="$body""i$k = \"$e\"\n"; k=$((k + 1)); done
	mf "inv-$n" "$want" "$needle" "${H}[fields]\nval = 0\n[invariants]\n$body"
}
inv holds 0 "No error has been found" \
	"node[1][0] == 0 or node[1][0] == 5" \
	"node[1].val != 7 and node[1].val <= 5 and node[1].val >= 0 and not (node[1].val > 5) and node[1].val < 6" \
	"(node[1][0] + 1) * 2 - 2 >= 0 and node[1][0] mod 5 == 0 and node[1][9] < 1" \
	"all r in replies : r.src == 0 and r.val == 5 and r == r" \
	"some i in nodes : i == 1" "count i in nodes : i >= 0 == 2" \
	"all b in node[1] : b in node[1]" "(count r in replies : r.val == 5) <= 1"
inv reached 0 "No error has been found" "eventually (some r in replies : r.val == 5) and (count b in node[1] : b == 5) == 1"
# each comparison's false side, a node index past the nodes, a field past
# the record, a reply variable used bare and a bound byte variable, a
# quantifier over node[i] by an index variable, and the fields of a reply
inv sides 0 "No error has been found" \
	"not (node[1][0] != node[1][0]) and not (node[1][0] < 0) and not (node[1][0] > 200)" \
	"not (node[1][0] <= 0 - 1) and not (node[1][0] >= 200) and not (node[1][0] == 200)" \
	"node[7][0] == 0 - 1 and node[1][40] == 0 - 1" \
	"all r in replies : r >= 0 and r.val == 5" "all b in node[1] : b == 5 or b == 0" \
	"all i in nodes : all b in node[i] : b < 200" "count i in nodes : not (i == 0) == 1" \
	"some i in nodes : node[i].val == node[i][0]"
inv reached2 0 "No error has been found" "eventually some r in replies : r.src == 0 and r.val > 4 and r.val < 6"
for bad in "all r in replies : r.x == 0" "node[0].val2 == 0" "count r in replies r.val == 1" "some r in replies r.val == 1" \
	"all i in nodes : node[j][0] == 0" "node[0][0] in nodes" "1 in" "not" "node[0][0] == (1" "node[0][0] == 1 or" \
	"node[0][0] == 1 and" "count i in nodes : i == 0 and" "(node[0][0] == 0) junk" "node [ 0 ] . val == 0 ==" \
	"all i in node[ : 1 == 1" "count i in node[0 : 1 == 1" "count i in node[x] : 1 == 1" "count i in node[0) : 1 == 1" \
	"some in nodes : 1 == 1" "all  : 1 == 1" "node[0][0] == $(printf '%40s' '' | tr ' ' 'z')"; do
	mf "bad$k" 3 "cannot parse" "${H}[fields]\nval = 0\n[invariants]\nb = \"$bad\"\n"
	k=$((k + 1))
done
# `and` and `or` mixed at one level are refused: left to right, the
# invariant below meant (x == 7 or x == 5) and x == 9, which 2.0.0 found
# violated in a state that `x == 7 or (x == 5 and x == 9)` would let pass
for bad in "node[0][0] != " "node[0][0] <= " "node[0][0] >= " "node[0][0] < " "node[0][0] > " \
	"eventuallyx node[0][0] == 0" "all r in replies : r.abcdefghijk == 0"; do
	mf "bad$k" 3 "cannot parse" "${H}[invariants]\nb = \"$bad\"\n"
	k=$((k + 1))
done
inv sides2 0 "No error has been found" "node[0][-1] == 0 - 1" "node[1][99] == 0 - 1"
for bad in "count  in nodes : 1 == 1" "node[0]. == 0" "r == 0" "all b in node[1] : b.src == 0"; do
	mf "bad$k" 3 "cannot parse" "${H}[invariants]\nb = \"$bad\"\n"
	k=$((k + 1))
done
mf badword 3 "cannot parse" "${H}[invariants]\nb = \"node_x == 0\"\n"
L32=$(printf '%32s' '' | tr ' ' 'v')
for bad in "count $L32 in nodes : 1 == 1" "node == 0" "node[0].$L32 == 0" "node[0 - 1][0] == 0" \
	"all r in replies : q == 0"; do
	mf "bad$k" 3 "cannot parse" "${H}[fields]\nval = 0\n[invariants]\nb = \"$bad\"\n"
	k=$((k + 1))
done
mf invexprlong 3 "invariant name or expression too long" "${H}[invariants]\nx = \"$(printf '%300s' '' | tr ' ' 'x')\"\n"
mf evtab 0 "No error has been found" "${H}[invariants]\nok = \"eventually	node[1][0] == 5\"\n"
printf '[target]\nnodes = 2\n[environment]\nclient_write = 5\n[invariants]\nok = "node[0][0] < 9"' > "$MF/nonewline.model"
expect_by 60 "model file: a last line without a newline" 0 "No error has been found" --model-file "$MF/nonewline.model" --binary "$B/proc/repl"
printf '[target]\nbinary = repl\nnodes = 2\n[environment]\nclient_write = 5\n[invariants]\nok = "node[0][0] < 9"\n' > "$B/proc/bare.model"
(cd "$B/proc" && fail=0 && expect_by 60 "model file: a binary named beside a bare model path" 0 "No error has been found" --model-file bare.model; exit $fail) || fail=1
inv mixed 3 "cannot parse" "node[1][0] == 7 or node[1][0] == 5 and node[1][0] == 9"
inv grouped 0 "No error has been found" "node[1][0] != 7 or (node[1][0] == 5 and node[1][0] == 9)" "(node[1][0] == 0 or node[1][0] == 5) and node[1][0] < 9"
for bad in "node[0" "node[x][0] == 0" "node[0]" "node[0][0" "node[0].nope == 0" "nodez == 1" "(node[0][0] == 0" \
	"node[0][0] ==" "node[0][0] == 0 junk" "node[0][0] mod 0 == 0" "all i in : i == 0" "all i nodes : i == 0" \
	"all i in nodes i == 0" "count in nodes : 1 == 1" "count i nodes : 1 == 1" "count i in bogus : 1 == 1" \
	"count i in nodes 1 == 1" "1 in replies" "1 in nodes" "some r in replies : r.bogus == 0" "1 + == 2" "1 * == 2" \
	"node[0][0] ? 1" "(1 + 1" "some i in node[9] : i == 0" "some i in node[0 : i == 0" "some i in node[q] : 1 == 1" \
	"some i in node[0) : 1 == 1" "count i in nodes : (1 == 1"; do
	mf "bad$k" 3 "cannot parse" "${H}[invariants]\nb = \"$bad\"\n"
	k=$((k + 1))
done

echo "-- the corners of the calls the checker answers itself"
# syscorner.c records each call's outcome.  Run natively first: its record
# is then the model's invariant (cmc reaches it, and only it).  On 2.0.0
# a fcntl on an epoll fd or an unconnected socket changed the listener's
# flags, an eventfd's FD_CLOEXEC read back 0, poll called a listening
# socket writable, and socket calls on an eventfd, an epoll instance or a
# socket in the wrong state succeeded where Linux refuses them (wrongfd);
# each mode below that covers one of these failed.
SC=$OUT/syscorner
mkdir -p "$SC"
for m in badbuf eventfd epoll poll clock futex nbepoll wrongfd notconn pollclosed robuf selfdial fileops \
	sleeperr pollmore kernelsock; do
	(cd "$SC" && timeout 30 "$B/proc/syscorner" 0 "$m" 3>"$SC/$m.state" > /dev/null 2>&1)
	# shellcheck disable=SC2046
	set -- $(od -An -tu1 "$SC/$m.state" | tr -s ' \n' ' ')
	if [ $# -lt 3 ]; then echo "FAIL syscorner $m: the native run left no record"; fail=1; continue; fi
	shift 2; shift
	# the record as invariants of eight bytes each (an invariant holds at
	# most 255 characters), all of which must hold once the node is done,
	# and the whole record must be reached
	invs="" cond="" k=0
	for v in "$@"; do
		cond="$cond and node[0][$k] == $v"; k=$((k + 1))
		if [ $((k % 8)) = 0 ] || [ $k = $# ]; then
			invs="${invs}linux$k = \"node[0][0] < 1 or (${cond# and })\"\n"; cond=""
		fi
	done
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n[invariants]\n%breached = "eventually node[0][0] == 1 and node[0][%d] >= 0"\n' \
		"$B/proc/syscorner" "$m" "$invs" "$((k - 1))" > "$SC/$m.model"
	expect_by 60 "syscorner $m: cmc answers as Linux does" 0 "No error has been found" --model-file "$SC/$m.model"
done
# a limit or an unmodelled argument ends the check, never a guess
scm() {	# scm MODE EXIT NEEDLE
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n[invariants]\nany = "node[0][0] < 9"\n' "$B/proc/syscorner" "$1" > "$SC/r-$1.model"
	expect_by 60 "syscorner $1: $3" "$2" "$3" --model-file "$SC/r-$1.model"
}
scm robufefd 3 "an eventfd read at an address it does not own"
printf '[target]\nbinary = %s\nnodes = 2\nargs = durread\n[invariants]\nfresh = "node[0][0] < 1 or (node[0][1] == 1 and node[0][2] == 1 and node[0][3] == 1)"\nreached = "eventually node[0][0] == 1"\n' "$B/proc/syscorner" > "$SC/durread.model"
expect_by 60 "syscorner durread: a fresh durable record reads EOF, fsyncs, cannot seek" 0 "No error has been found" --model-file "$SC/durread.model"
scm ipv6 3 "is not modelled (only AF_INET, SOCK_STREAM)"
scm connectlow 3 "connects to port 19999, which is no node's"
scm sendtoaddr 3 "sendto flags 0 or a destination address are not modelled"
scm fcntldup 1 "fcntl cmd 0 on virtual fd 500 has no model"
scm epollop 1 "epoll_ctl op 99 has no model"
scm futexrequeue 1 "futex op 3 has no model"
scm efdwrite4 1 "writes 4 bytes to an eventfd"
scm efdread4 1 "reads 4 bytes from an eventfd"
scm efdflags 3 "eventfd flags 0x2 are not modelled"
scm manyefd 1 "opens more than 8 eventfds"
scm manyepoll 1 "opens more than 4 epoll instances"
scm epollmax 1 "maxevents is 0"
scm epollnotep 1 "epoll_ctl on fd 500, which is no epoll instance"
scm epollwaitnotep 1 "epoll_wait on fd 500, which is no epoll instance"
scm manypoll 1 "polls 80 fds; the limit is 16"
scm bigwrite 1 "writes 4096 bytes in one call"
printf '[target]\nbinary = %s\nnodes = 2\nargs = notconnpipe\n[invariants]\nnever = "node[0][0] < 1"\nkilled = "eventually node[0][0] < 1"\n' "$B/proc/syscorner" > "$SC/r-notconnpipe.model"
expect_by 60 "syscorner notconnpipe: SIGPIPE ends the writer" 0 "No error has been found" --model-file "$SC/r-notconnpipe.model"
scm connect 3 "connects to port 20099, which is no node's"
scm shutlisten 3 "shutdown of a listening socket is not modelled"
scm duplisten 3 "which is not a connection, is not modelled"
scm dupopen 3 "which is open or the kernel's, is not modelled"
scm namefresh 3 "getsockname on a socket not connected is not modelled"
scm setopt 3 "setsockopt level 1 option 12 is not modelled"
scm getopt 3 "getsockopt level 1 option 12 is not modelled"
scm efdwritev 3 "a vector write to an eventfd is not modelled"
scm efdreadv 3 "a vector read of an eventfd is not modelled"
scm sendname 3 "sendmsg with a destination address or ancillary data is not modelled"
scm recvflags 3 "recvmsg flags 0x2 or ancillary data are not modelled"
scm manyriov 3 "reads into 40 iovec entries"
scm flocksock 3 "system call 73 on checker-owned fd 500 is not modelled"
scm byhandle 3 "opens a file by handle for writing"
scm openhow 3 "openat2 passes an open_how the checker cannot read"
scm manyfds 1 "opens more than 256 checker-owned fds"
scm manyconn 1 "more than 16 connections wait at node 0"
scm manywatch 1 "epoll instance watches more than 16 fds"
scm earlyshut 3 "shutdown of a socket whose non-blocking connect has not completed is not modelled"
scm earlyread 3 "a blocking read of a socket whose non-blocking connect has not completed"
scm earlywrite 3 "a blocking write to a socket whose non-blocking connect has not completed"
scm earlydial 3 "a blocking connect on a socket whose non-blocking connect has not completed"

echo "-- a libpq-style client: non-blocking connect, POLLOUT, SO_ERROR"
# nbconnect.c makes the socket calls libpq's connectDBStart() and
# PQconnectPoll() make.  Each mode runs natively on loopback (the server
# first, except in refused, where nothing listens), and its record is
# then the model's invariant: cmc must reach it and no other.  On 2.1.0
# the first non-blocking connect ended the check (exit 3).
$CC -O1 -no-pie -w -o "$B/proc/nbconnect" "$T/nbconnect.c" || { echo "proc_gate: nbconnect.c does not build"; fail=1; }
NB=$OUT/nbconnect
mkdir -p "$NB"
# record_model BINARY MODE STATEFILE ENV MODEL: a model whose invariants
# say node 0's finished record is the one in STATEFILE, eight bytes to
# an invariant, and that it is reached
record_model() {
	# shellcheck disable=SC2046
	set -- "$1" "$2" "$4" "$5" $(od -An -tu1 "$3" | tr -s ' \n' ' ')
	bin=$1 mode=$2 env=$3 model=$4; shift 4
	if [ $# -lt 3 ]; then return 1; fi
	shift 2; shift
	invs="" cond="" k=0
	for v in "$@"; do
		cond="$cond and node[0][$k] == $v"; k=$((k + 1))
		if [ $((k % 8)) = 0 ] || [ $k = $# ]; then
			invs="${invs}linux$k = \"node[0][0] < 1 or (${cond# and })\"\n"; cond=""
		fi
	done
	# a finished record must be reached, named by its last two bytes
	# (nbconnect ends its record in two check bytes over it); a mode
	# whose other records are Linux's too (early) asks only that
	[ "$mode" = early ] && invs=""
	eval "h1=\${$(($# - 1))} h2=\${$#}"
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n[environment]\n%s\n[invariants]\n%breached = "eventually node[0][0] == 1 and node[0][%d] == %s and node[0][%d] == %s"\n' \
		"$bin" "$mode" "$env" "$invs" "$((k - 2))" "$h1" "$((k - 1))" "$h2" > "$model"
}
for m in ok early refused; do
	(
		cd "$NB" || exit 1
		if [ "$m" != refused ]; then timeout 30 "$B/proc/nbconnect" 1 "$m" 3>/dev/null > /dev/null 2>&1 & sleep 0.1; fi
		timeout 30 "$B/proc/nbconnect" 0 "$m" 3>"$NB/$m.state" > /dev/null 2>&1
		wait
	)
	if ! record_model "$B/proc/nbconnect" "$m" "$NB/$m.state" "timeouts = 0" "$NB/$m.model"; then
		echo "FAIL nbconnect $m: the native run left no record"; fail=1; continue
	fi
	expect_by 120 "nbconnect $m: cmc answers as Linux does" 0 "No error has been found" --model-file "$NB/$m.model"
done

echo "-- the socket calls past read and write"
# sockapi.c dials its own listener and records each call's outcome, so
# it runs the same natively and under cmc: cmc must reach Linux's
# record and no other.  On 2.1.0 each of these calls on a checker-owned
# socket ended the check (exit 3).
$CC -O1 -no-pie -w -o "$B/proc/sockapi" "$T/sockapi.c" || { echo "proc_gate: sockapi.c does not build"; fail=1; }
for m in iov shut dup opts errs revents; do
	(cd "$NB" && timeout 30 "$B/proc/sockapi" 0 "$m" 3>"$NB/sa-$m.state" > /dev/null 2>&1)
	if ! record_model "$B/proc/sockapi" "$m" "$NB/sa-$m.state" "timeouts = 0" "$NB/sa-$m.model"; then
		echo "FAIL sockapi $m: the native run left no record"; fail=1; continue
	fi
	expect_by 60 "sockapi $m: cmc answers as Linux does" 0 "No error has been found" --model-file "$NB/sa-$m.model"
done

echo "-- short writes: a full send buffer as a budgeted choice"
# shortw.c sends on a non-blocking socket; the correct writer resends
# after a short write or EAGAIN, the -DBUG one ignores the count, which
# loses bytes only when the buffer fills.  On 2.1.0 a send always
# succeeded whole, so the bug passed.
$CC -O1 -no-pie -w -o "$B/proc/shortw" "$T/shortw.c" || { echo "proc_gate: shortw.c does not build"; fail=1; }
$CC -O1 -no-pie -w -DBUG -o "$B/proc/shortw-bug" "$T/shortw.c" || { echo "proc_gate: shortw.c -DBUG does not build"; fail=1; }
sed 's/short_writes = 2/short_writes = 0/' "$T/shortw.model" > "$OUT/shortw0.model"
expect_by 120 "shortw: a writer that resends holds"            0 "No error has been found" $(run shortw shortw)
expect_by 120 "shortw: a writer that ignores the count is caught" 1 "whole is violated" $(run shortw shortw-bug)
expect_by 120 "shortw: the counterexample names the full buffer" 1 "full n0 t0 (EAGAIN)" $(run shortw shortw-bug)
expect_by 120 "shortw: the bug needs a full buffer"           0 "No error has been found" $(runx "$OUT/shortw0.model" shortw-bug)
# the same with the body sent by writev and by sendmsg, in two pieces
for h in writev sendmsg; do
	sed "s/^nodes = 2/nodes = 2\nargs = $h/" "$T/shortw.model" > "$OUT/shortw-$h.model"
	expect_by 120 "shortw $h: a writer that resends holds"       0 "No error has been found" $(runx "$OUT/shortw-$h.model" shortw)
	expect_by 120 "shortw $h: one that ignores the count is caught" 1 "whole is violated" $(runx "$OUT/shortw-$h.model" shortw-bug)
done

echo "-- other nodes act between two calls of one step"
# nbread: a non-blocking read, a zero-timeout poll or epoll_wait right
# after a send may find the peer's echo there (all three passed: the
# read was EAGAIN and the poll empty unless a poll had just woken it).
# rstmid: the peer's RST may arrive between two writes of one step.
$CC -O1 -no-pie -w -o "$B/proc/nbread" "$T/nbread.c" || { echo "proc_gate: nbread.c does not build"; fail=1; }
$CC -O1 -no-pie -w -o "$B/proc/rstmid" "$T/rstmid.c" || { echo "proc_gate: rstmid.c does not build"; fail=1; }
for m in read poll epoll; do
	sed "s/^nodes = 2/nodes = 2\nargs = $m/" "$T/nbread.model" > "$OUT/nbread-$m.model"
	expect_by 120 "nbread $m: the peer's answer can arrive first" 1 "nofast is violated" $(runx "$OUT/nbread-$m.model" nbread)
done
expect_by 120 "rstmid: an RST can arrive between two writes"  1 "not1 is violated" $(run rstmid rstmid)
# midstep.c: node 0 sends, then a call whose answer Linux gives from
# what node 1 did meanwhile; ms MODE DESC EXIT NEEDLE INVARIANTS writes
# and runs its model
$CC -O1 -no-pie -w -o "$B/proc/midstep" "$T/midstep.c" || { echo "proc_gate: midstep.c does not build"; fail=1; }
ms() {
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n%b\n' "$B/proc/midstep" "$1" "$5" > "$OUT/ms-$1.model"
	expect_by 120 "midstep $1: $2" "$3" "$4" --model-file "$OUT/ms-$1.model"
}
# a poll or epoll_wait woken by a reset reports POLLIN|POLLERR|POLLHUP
# (25), as Linux does, never POLLIN alone (it did)
for m in rstwake rstewake; do
	ms $m "a reset wakes POLLIN|POLLERR|POLLHUP" 0 "No error has been found" \
		'[invariants]\nlinux = "node[0][0] == 0 or node[0][0] == 25"\nreached = "eventually node[0][0] == 25"'
done
# poll(POLLOUT) or epoll_wait(EPOLLOUT) with a zero timeout right after
# the write may find node 1's RST there (it never could: only a poll
# for POLLIN stopped for the others)
for m in rstout rsteout; do
	ms $m "the peer's RST can come before a zero-timeout POLLOUT poll" 1 "not1 is violated" \
		'[invariants]\nnot1 = "node[0][0] != 1"'
done

echo "-- partitions: a cut link holds segments back until it heals"
# partitions = N lets the search cut a directed link between two nodes
# and heal it later; bytes, FINs and handshakes on it wait.  The counts
# pin that cut and healed worlds are searched (repl without partitions:
# 7 distinct states), including a dial that waits for the link to heal.
# An RST that would cross a cut link is refused.
sed 's/^\[environment\]/[environment]\npartitions = 1/' "$T/repl.model" > "$OUT/repl-cut.model"
expect_by 300 "partitions: repl holds with a cut link"     0 "34 distinct states" $(runx "$OUT/repl-cut.model" repl)
expect_by 300 "partitions: repl's bug is still caught"     1 "agree_on_ack is violated" $(runx "$OUT/repl-cut.model" repl-bug)
# a poll loop's wake waits for a cut link to heal: more worlds (5
# without partitions), the same verdicts
sed '/^\[invariants\]/i [environment]\npartitions = 1\n' "$T/pollloop.model" > "$OUT/pollloop-cut.model"
expect_by 300 "partitions: pollloop holds with a cut link"  0 "20 distinct states" $(runx "$OUT/pollloop-cut.model" pollloop)
expect_by 300 "partitions: pollloop's bug is still caught"  1 "stored_ok is violated" $(runx "$OUT/pollloop-cut.model" pollloop-bug)
$CC -O1 -no-pie -w -o "$B/proc/tcpsem" "$T/tcpsem.c" || { echo "proc_gate: tcpsem.c does not build"; fail=1; }
printf '[target]\nbinary = %s\nnodes = 2\nargs = rstdata\n[environment]\npartitions = 1\n[invariants]\nx = "1 == 1"\n' "$B/proc/tcpsem" > "$OUT/rstcut.model"
expect_by 120 "partitions: an RST across a cut link is refused" 3 "an RST held back by a partition is not modelled" --model-file "$OUT/rstcut.model"
# a blocking connect to a live node across a cut link waits for a heal
# or times out (ETIMEDOUT, after Linux's 127 s); it used to go straight
# into the backlog, so a program that mishandles the failure passed
$CC -O1 -no-pie -w -o "$B/proc/cutconn" "$T/cutconn.c" || { echo "proc_gate: cutconn.c does not build"; fail=1; }
expect_by 120 "partitions: a blocking connect across a cut link times out" 1 "nofail is violated" $(run cutconn cutconn)

echo "-- edges: limits, partial transfers, refusals and signals"
# edges.c: each mode passes a limit or a malformed argument and must end
# the check as named, or records a corner whose answer is Linux's
ED=$OUT/edges
mkdir -p "$ED"
edm() {	# edm MODE EXIT NEEDLE [INVARIANT]
	inv=${4:-any = "node[0][0] < 9"}
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n[invariants]\n%b\n' "$B/proc/edges" "$1" "$inv" > "$ED/$1.model"
	expect_by 60 "edges $1: $3" "$2" "$3" --model-file "$ED/$1.model"
}
edm partwrite 3 "write sends from a buffer that ends"
edm parthook 3 "a state-hook write sends from a buffer that ends"
edm partrandom 3 "getrandom fills a buffer at an address it does not own"
edm partefd 3 "an eventfd read at an address it does not own"
edm partpoll 0 "No error has been found" 'efault = "node[0][0] < 1 or node[0][1] == 24"\nreached = "eventually node[0][0] == 1"'
edm bighook 3 "writes 5000 bytes to its state hook in one call"
edm floodhook 1 "more state records between two quiescent points than the checker's pipe holds"
edm manyconn 1 "more than 16 connections wait at node 1"
edm epollitems 1 "epoll instance watches more than 16 fds"
edm negpoll 1 "polls -1 fds"
for m in dup2 dup3 splice sendfile; do edm "$m" 3 "on checker-owned fd 500 is not modelled"; done
for m in recvfrom sendto close fcntl readv writev pread pwrite dup ioctl; do
	edm "worker-$m" 3 "a thread of a multi-threaded node uses kernel fd"
done
# the C library's own calls a worker may make: no refusal (INCOMPLETE,
# since the fixture is not built with the race runtime)
for m in stdin_tty stdout durable tty2; do edm "worker-$m" 2 "without the race runtime"; done
# a worker's eventfd write wakes main parked in poll or epoll_wait on it
edm efdpoll 2 "without the race runtime" 'woke = "node[0][0] < 1 or (node[0][1] == 1 and node[0][2] == 1)"\nreached = "eventually node[0][0] == 1"'
edm efdepoll 2 "without the race runtime" 'woke = "node[0][0] < 1 or (node[0][1] == 1 and node[0][2] == 1)"\nreached = "eventually node[0][0] == 1"'
edm fscorners 0 "No error has been found" 'linux = "node[0][0] < 1 or (node[0][1] == 32 and node[0][2] == 1)"\nreached = "eventually node[0][0] == 1"'
edm openat2small 3 "openat2 passes an open_how the checker cannot read"
# a channel to another process: another program run in the node, SysV
# shared memory or a message queue (which outlive the process, so their
# state would survive a crash outside the world), another process's
# memory, a signal to another process or a process group, a pidfd.  On
# 2.0.0 each went to the kernel unseen.  A signal to the node itself
# (signal 0 here) still goes to the kernel.
edm exec 3 "runs another program (execve)"
for m in shm vmread mq; do edm "$m" 3 "shares state between processes outside the model"; done
edm killparent 3 "a signal to another process is not modelled"
edm killgroup 3 "signals process 0; a signal to another process is not modelled"
edm pidfd 3 "reaches another process through a pidfd"
edm killself 0 "No error has been found" 'sent = "node[0][0] < 1 or (node[0][1] == 1 and node[0][2] == 1 and node[0][3] == 1)"\nreached = "eventually node[0][0] == 1"'
edm segvhandler 3 "raised signal 11 with a handler installed or the signal ignored"
edm usr1 3 "stopped with signal 10"
edm segv 1 "killed by signal 11"
edm fpe 1 "killed by signal 8"
edm bus 1 "killed by signal 7"
edm ill 1 "killed by signal 4"
edm bothfault 1 "killed by signal 6"
edm corners 0 "No error has been found" 'linux = "node[0][0] < 1 or (node[0][1] == 1 and node[0][2] == 1 and node[0][3] == 21 and node[0][4] == 1 and node[0][5] == 1 and node[0][6] == 1 and node[0][7] == 1 and node[0][8] == 1)"\nreached = "eventually node[0][0] == 1"'
edm threads 1 "creates more than 8 threads"
edm badsocket 3 "is not modelled (only AF_INET, SOCK_STREAM)"
edm badsendto 3 "sendto flags 0x40 or a destination address are not modelled"
edm badrecvfrom 3 "recvfrom flags 0 or a source address are not modelled"
edm badaccept4 3 "accept4 flags 0x800 are not modelled"
printf '[target]\nbinary = %s\nnodes = 1\nargs = replies\n[environment]\nclient_write = 1\n[invariants]\nany = "node[0][0] < 9"\n' "$B/proc/edges" > "$ED/replies.model"
expect_by 60 "edges replies: more than 64 client replies" 1 "more than 64 client replies" --model-file "$ED/replies.model"
edm corners2 0 "No error has been found" 'first = "node[0][0] < 1 or (node[0][1] == 1 and node[0][2] == 1 and node[0][3] == 21 and node[0][4] == 1 and node[0][5] == 1 and node[0][6] == 1)"\nrest = "node[0][0] < 1 or (node[0][7] == 1 and node[0][8] == 1 and node[0][9] == 1 and node[0][10] == 116 and node[0][11] == 1 and node[0][12] == 1 and node[0][13] == 1)"\nreached = "eventually node[0][0] == 1"'

echo "-- the search's other modes: simulation, bounds and refinement"
# simulation reports a fault and a data race like the search does, and a
# simulation or a search that runs out of time is INCOMPLETE; a step that
# does not refine, an abstract invariant broken on the way, and an
# evaluation error in a binding are reported in simulation too
expect_by 60 "simulate: a fault is reported with its walk" 1 "killed by signal 6" --model-file "$OUT/fault-main.model" --simulate --seed 1 --walks 5
expect_by 60 "simulate: a data race is reported with its walk" 1 "data race on node 0" --model-file "$OUT/mr-unlocked-tsan.model" --simulate --seed 1 --walks 50
expect_by 60 "simulate: an exhausted walk ends early, INCOMPLETE" 2 "not exhaustive" $(run elect elect) --simulate --seed 3 --walks 5 --depth 2
expect_by 60 "simulate: the time bound stops it" 2 "not exhaustive" $(run elect elect) --simulate --seed 3 --walks 100000000 --time 1
expect_by 60 "search: the time bound stops it" 2 "time limit reached" $(runx "$HERE/examples/raft/raft.model" raft) --time 1
expect_by 60 "simulate: an invariant broken on a walk is reported" 1 "agree_on_ack is violated" $(run repl repl-bug) --simulate --seed 1 --walks 200
expect_by 120 "simulate: C2 catches the by-two step on a walk" 1 "no matching abstract action" $(runx "$HERE/test/proc/mono.model" mono-bug) --refine "$HERE/test/proc/mono.cml" --bind "$HERE/test/proc/mono.bind" --refine-steps --simulate --seed 1 --walks 50
expect_by 120 "simulate: C2 passes the by-one impl, not exhaustively" 2 "not exhaustive" $(runx "$HERE/test/proc/mono.model" mono) --refine "$HERE/test/proc/mono.cml" --bind "$HERE/test/proc/mono.bind" --refine-steps --simulate --seed 1 --walks 20
expect_by 120 "simulate: C1 catches the paxos bug" 1 "does not refine" $(runx "$HERE/examples/spec/paxos-refine.model" paxos-bug) --refine "$HERE/examples/spec/paxos.cml" --bind "$HERE/examples/spec/paxos.bind" --simulate --seed 1 --walks 3000

# faults the fixtures' own models leave out: their invariants still hold
# with a drop, a crash and a timeout added from the command line
for f in crossrpc lease fifo quorum; do
	expect_by 120 "$f: holds with a drop, a crash and a timeout added" 0 "No error has been found" $(run "$f" "$f") --drops 1 --crashes 1 --timeouts 1 --max-states 3000
done
# guarded's backup keeps the value only in memory, so a crash right after
# it acks (between two of its calls, not between steps) loses an acked value
expect_by 120 "guarded: a crash between the backup's ack and its exit loses the value" 1 "agree_on_ack is violated" $(run guarded guarded) --drops 1 --crashes 1 --timeouts 1 --max-states 3000
# what is not a node binary is refused before anything starts
cp /etc/hostname "$OUT/notelf" 2> /dev/null || printf 'x' > "$OUT/notelf"
expect "a file that is no ELF is refused" 3 "is not an ELF executable" $(run repl repl) --binary "$OUT/notelf"
cp "$B/proc/repl" "$OUT/noexec" && chmod -x "$OUT/noexec"
expect_by 60 "a binary that cannot be run is refused" 3 "did not load the preload" $(run repl repl) --binary "$OUT/noexec"
# an invariant that cannot be evaluated in a reached state (a mod by a
# zero byte) ends the search and the simulation
printf '[target]\nnodes = 2\n[environment]\nclient_write = 5\n[invariants]\nz = "node[1][0] mod node[0][0] == 0"\n' > "$OUT/modzero.model"
expect_by 60 "an invariant that divides by zero in a state is an error" 3 "invariant z parse error" --model-file "$OUT/modzero.model" --binary "$B/proc/repl"
expect_by 60 "the same in a simulation" 3 "invariant z parse error" --model-file "$OUT/modzero.model" --binary "$B/proc/repl" --simulate --seed 1 --walks 5

echo "-- restart: relaunch a crashed node and recover (Tier 3 ask 6)"
# A server (node 1) stores a client's value durably (fd 193) and loops
# serving; the client (node 0) acks the synthetic client only once the
# server has made it durable.  The checker crashes the server at a
# quiescent point where it holds the committed value, then RESTARTS it:
# a restarted process is fresh and reads its surviving fd-193 record back
# on startup, so a server that recovers from its own log keeps the
# committed value.  no_lost_ack: an acked value is present on some node
# after the restart.
#
# Correct: the server reads fd 193 and re-emits the recovered value, so it
# is back after the restart.  -DBUG: the server does not recover, so after
# the restart the committed value is on neither node -- caught.
expect "restart: crash-and-recover holds"               0 "No error has been found" $(run restart restart)
expect "restart: a lost recovery is caught"             1 "no_lost_ack is violated" $(run restart restart-bug)
expect "restart: the counterexample names the restart" 1 "restart n1" $(run restart restart-bug)
# Without a restart budget the crash is final (the node never comes back),
# so the recovery bug is not explorable: the -DBUG build passes vacuously.
# This is the teeth -- it pins that RESTART is what makes recovery
# checkable, and that a model with restarts = 0 behaves exactly as before.
printf '[target]\nnodes = 2\n[environment]\nclient_write = 7\ncrashes = 1\nrestarts = 0\n[invariants]\nno_lost_ack = "all r in replies : r.val in node[0] or r.val in node[1]"\n' > "$OUT/restart0.model"
expect "restart: restarts = 0 keeps the crash final"     0 "No error has been found" --model-file "$OUT/restart0.model" --binary "$B/proc/restart-bug"
# arithmetic evaluates with the usual precedence
printf '[target]\nnodes = 2\n[environment]\nclient_write = 5\n[invariants]\nt = "2 + 3 * 4 == 14 and 10 mod 3 == 1 and (1 + 1) * 2 == 4"\n' > "$OUT/q.model"
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
# with no CMC_PRELOAD, a cmc with no preload beside it or in ../lib/libcmc
mkdir -p "$OUT/lone/bin" && cp "$CMC" "$OUT/lone/bin/cmc"
env -u CMC_PRELOAD "$OUT/lone/bin/cmc" --model-file "$T/lease.model" --binary "$B/proc/lease-bug" > "$OUT/lone.txt" 2>&1
rc=$?
if [ "$rc" = 3 ] && grep -q "cannot find cmc-preload.so beside this program" "$OUT/lone.txt"; then
	echo "ok   no preload beside cmc is an error -> exit 3"
else echo "FAIL no preload beside cmc is an error -> exit $rc"; sed 's/^/     /' "$OUT/lone.txt" | head -5; fail=1; fi

# constructs the engine answered wrongly (soundness plan 3.4, 3.6): a
# write from an unmapped buffer gets EFAULT, not a zero-filled success,
# and EPOLLET and EFD_SEMAPHORE are refused; a finite poll timeout, once
# refused, now fires (2.2)
for m in none badptr pollto epollet semaphore longrec partrec refused rstwrite rstignore rsthandler rstblock fork writev sendmsg peek nbaccept udp; do
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n[invariants]\nuntouched = "node[0][0] != 1"\n' "$B/proc/unmodelled" "$m" > "$OUT/u-$m.model"
done
printf 'faulted = "eventually node[0][0] == 2"\n' >> "$OUT/u-badptr.model"
printf 'refused = "eventually node[0][0] == 2"\n[environment]\ntimeouts = 1\n' >> "$OUT/u-refused.model"
printf 'killed = "node[0][0] != 2"\n' >> "$OUT/u-rstwrite.model"
# rstignore: node 1 may read "x" and close (a FIN) before node 0 sends
# "y", as on Linux, so node 0 may record 1; the row pins that EPIPE is
# reachable, not that the other order is not
printf '[target]\nbinary = %s\nnodes = 2\nargs = rstignore\n[invariants]\nepiped = "eventually node[0][0] == 2"\n' "$B/proc/unmodelled" > "$OUT/u-rstignore.model"
expect "unmodelled: a bad write buffer gets EFAULT"      0 "No error has been found" --model-file "$OUT/u-badptr.model"
# Which node steps first between two quiescent points is a search
# choice (STEP), never the kernel's scheduling.  In dialexit node 1 exits
# at once while node 0 makes 16 calls and dials it: on Linux the dial
# waits or is refused by which process runs first, and the search must
# reach both.  On 2.0.0, which drove both nodes at once, one model gave
# one outcome or the other at random, so each of 300 runs here must
# give the same verdict and count too, as must badptr (node 0 dials
# node 1 while it exits; 2.0.0 failed it about once in 100 runs).
printf '[target]\nbinary = %s\nnodes = 2\nargs = dialexit\n[invariants]\nwaited = "eventually node[0][0] == 1"\nrefused = "eventually node[0][0] == 2"\n' "$B/proc/unmodelled" > "$OUT/u-dialexit.model"
expect "dialexit: a dial both waits and is refused, by node order" 0 "No error has been found" --model-file "$OUT/u-dialexit.model"
for m in badptr dialexit; do
	for _ in $(seq 300); do
		"$CMC" --model-file "$OUT/u-$m.model" > "$OUT/det.txt" 2>&1
		echo "exit $? $(grep -o '[0-9]* distinct states found' "$OUT/det.txt")"
	done | sort | uniq -c > "$OUT/det-$m.txt"
	if [ "$(wc -l < "$OUT/det-$m.txt")" = 1 ]; then echo "ok   $m: 300 runs give one verdict and one count ($(sed 's/^ *//' "$OUT/det-$m.txt"))"
	else echo "FAIL $m: 300 runs differ:"; sed 's/^/     /' "$OUT/det-$m.txt"; fail=1; fi
done
# 3.9: a root that enables no action explored nothing, so a safety-only
# model of it is not a pass
expect "noprogress: a root with no action is not a pass" 2 "no action was ever enabled" --model-file "$OUT/u-none.model"
expect "unmodelled: a finite poll timeout fires"        1 "untouched is violated" --model-file "$OUT/u-pollto.model"
expect "unmodelled: EPOLLET is refused"                  3 "are not modelled" --model-file "$OUT/u-epollet.model"
expect "unmodelled: EFD_SEMAPHORE is refused"            3 "EFD_SEMAPHORE" --model-file "$OUT/u-semaphore.model"
# a node that forks is two processes the model cannot tell apart
expect "unmodelled: a fork is refused"                   3 "forks a second process" --model-file "$OUT/u-fork.model"
# calls and flags the model does not answer are refused, not passed to a
# kernel that has no such socket, or ignored
expect "unmodelled: a writev past max_write is refused"   1 "in one vector call; the limit is 64" --model-file "$OUT/u-writev.model"
expect "unmodelled: sendmsg with ancillary data is refused" 3 "ancillary data is not modelled" --model-file "$OUT/u-sendmsg.model"
expect "unmodelled: MSG_PEEK is refused"                  3 "recvfrom flags" --model-file "$OUT/u-peek.model"
expect "unmodelled: accept4 SOCK_NONBLOCK is refused"     3 "accept4 flags" --model-file "$OUT/u-nbaccept.model"
expect "unmodelled: a UDP socket is refused"              3 "is not modelled" --model-file "$OUT/u-udp.model"
# 3.3: a state record the checker cannot store whole is an error, never
# a truncated or dropped state
# 3.7: a connect() to a node that is gone is refused, as on Linux
expect "connect: a dead node refuses the connection"      0 "No error has been found" --model-file "$OUT/u-refused.model"
# a close with bytes unread is an RST: the peer's read gets ECONNRESET and
# its write EPIPE, and SIGPIPE does what the node's disposition says
expect "rst: the default SIGPIPE ends the writer"          0 "No error has been found" --model-file "$OUT/u-rstwrite.model"
# a node starts with every signal at its default, whatever the checker
# inherited: run cmc itself with SIGPIPE ignored (as CI runners do)
(trap '' PIPE; fail=0; expect "rst: a node does not inherit an ignored SIGPIPE" 0 "No error has been found" --model-file "$OUT/u-rstwrite.model"; exit $fail) || fail=1
expect "rst: an ignored SIGPIPE leaves EPIPE"              0 "No error has been found" --model-file "$OUT/u-rstignore.model"
expect "rst: a SIGPIPE handler is refused"                 3 "SIGPIPE handler" --model-file "$OUT/u-rsthandler.model"
expect "rst: a blocked SIGPIPE is refused"                 3 "with SIGPIPE blocked" --model-file "$OUT/u-rstblock.model"
expect "unmodelled: an over-long state record is refused" 3 "longer than the 64" --model-file "$OUT/u-longrec.model"
expect "unmodelled: a partial state record is refused"    3 "partial volatile state record" --model-file "$OUT/u-partrec.model"

# TCP close and reset semantics against Linux (soundness plan 3.7): each
# scenario of test/proc/tcpsem.c runs natively on loopback, then under
# cmc, which must reach exactly the code Linux gave.  Node 0's code is
# two bytes; a digit is one call's result (see tcpsem.c).
$CC -O1 -no-pie -w -o "$B/proc/tcpsem" "$T/tcpsem.c" || { echo "proc_gate: tcpsem.c does not build"; fail=1; }
tcpsem_native() {	# MODE FIRST PAUSE: run natively, print node 0's two code bytes
	d="$OUT/tcpsem-$1"; mkdir -p "$d"
	other=$((1 - $2))
	# a run that hangs (under load the second process can start before
	# the first listens) fails the row instead of hanging the gate
	( cd "$d" && exec timeout 30 "$B/proc/tcpsem" "$2" "$1" 3>"$d/n$2.state" ) & pa=$!
	sleep "$3"
	( cd "$d" && exec timeout 30 "$B/proc/tcpsem" "$other" "$1" 3>"$d/n$other.state" ) & pb=$!
	wait $pa; wait $pb
	od -An -tu1 "$d/n0.state" | tr -s ' ' '\n' | grep -v '^$' | tail -2 | tr '\n' ' '
}
tcpsem_row() {	# MODE FIRST PAUSE HI LO ENV
	got=$(tcpsem_native "$1" "$2" "$3")
	if [ "$got" = "$4 $5 " ]; then echo "ok   tcpsem $1: Linux gives $4,$5"
	else echo "FAIL tcpsem $1: Linux gave '$got', the fixture expects '$4 $5'"; fail=1; fi
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n[environment]\n%s\n[fields]\nhi = 0\nlo = 1\n[invariants]\nlinux = "node[0].hi < 1 or (node[0].hi == %s and node[0].lo == %s) or node[0].lo == 9"\nreached = "eventually node[0].hi == %s and node[0].lo == %s"\n' \
		"$B/proc/tcpsem" "$1" "$6" "$4" "$5" "$4" "$5" > "$OUT/tcpsem-$1.model"
	expect "tcpsem $1: cmc reaches Linux's result and no other" 0 "No error has been found" --model-file "$OUT/tcpsem-$1.model"
}
tcpsem_row finwrite 1 0.1 13 44 "timeouts = 0"
tcpsem_row rstdata  1 0.1 7 24  "timeouts = 0"
tcpsem_row backlog  1 0.1 0 24  "timeouts = 1"
tcpsem_row redial   0 0.3 6 51  "crashes = 1
restarts = 1"

# the world encoding (soundness plan 3.1, 3.2): worlds whose futures
# differ encode differently, and the seen set compares bytes, not hashes
# the whitebox drivers include the engine source, so they take its CFLAGS
# (a --coverage build measures it through them); node binaries do not
# shellcheck disable=SC2086
if $CC -std=c11 -O1 -w ${CFLAGS:-} -I"$HERE/src" -I"$HERE/include" -o "$B/proc/fingerprint" "$T/fingerprint.c" "$HERE/src/cmc_spec.c" "$HERE/src/cmc_fs.c"; then
	if "$B/proc/fingerprint" > "$OUT/fp.txt" 2>&1; then echo "ok   the world encoding separates differing futures"
	else echo "FAIL the world encoding"; sed 's/^/     /' "$OUT/fp.txt"; fail=1; fi
else echo "proc_gate: fingerprint.c does not build"; fail=1; fi

# the modelled file system on its own (plan 2.2, item 3): what a crash
# keeps, a failed fsync, a failed write, an unlinked open file, errors
# shellcheck disable=SC2086
if $CC -std=c11 -O1 -w ${CFLAGS:-} -I"$HERE/src" -o "$B/proc/fsmodel" "$T/fsmodel.c" "$HERE/src/cmc_fs.c"; then
	if "$B/proc/fsmodel" > "$OUT/fsmodel.txt" 2>&1; then echo "ok   the file model keeps what a crash keeps"
	else echo "FAIL the file model"; sed 's/^/     /' "$OUT/fsmodel.txt"; fail=1; fi
else echo "proc_gate: fsmodel.c does not build"; fail=1; fi

# the same, over random worlds (soundness plan, Phase 5): a mutation of
# any future-relevant field changes the encoding, and slot order, tids
# and allocation positions do not
# shellcheck disable=SC2086
if $CC -std=c11 -O1 -w ${CFLAGS:-} -I"$HERE/src" -I"$HERE/include" -o "$B/proc/fpfuzz" "$T/fpfuzz.c" "$HERE/src/cmc_spec.c" "$HERE/src/cmc_fs.c"; then
	if "$B/proc/fpfuzz" > "$OUT/fpfuzz.txt" 2>&1; then echo "ok   the world encoding holds over random worlds ($(tail -1 "$OUT/fpfuzz.txt" | cut -d' ' -f4-))"
	else echo "FAIL the world encoding over random worlds"; sed 's/^/     /' "$OUT/fpfuzz.txt"; fail=1; fi
else echo "proc_gate: fpfuzz.c does not build"; fail=1; fi

# the preload's allocator (soundness plan 3.5): an impossible size fails
# instead of wrapping, and realloc copies only the old block
# shellcheck disable=SC2086
if $CC -O1 -w ${CFLAGS:-} -o "$B/proc/preload_alloc" "$T/preload_alloc.c"; then
	if "$B/proc/preload_alloc" > "$OUT/alloc.txt" 2>&1; then echo "ok   the preload allocator checks sizes and copies exactly"
	else echo "FAIL the preload allocator"; sed 's/^/     /' "$OUT/alloc.txt"; fail=1; fi
else echo "proc_gate: preload_alloc.c does not build"; fail=1; fi

# the race runtime's happens-before logic, in process (race_rt.c): every
# edge it models orders, a call that fails adds none, unordered
# conflicting accesses are one report, and its limits are reported
# shellcheck disable=SC2086
if $CC -O1 -w ${CFLAGS:-} -o "$B/proc/race_rt" "$T/race_rt.c" -lpthread; then
	if timeout 120 "$B/proc/race_rt" > "$OUT/race_rt.txt" 2>&1; then
		echo "ok   the race runtime orders, reports and limits as modelled ($(grep -c '^ok' "$OUT/race_rt.txt") checks)"
	else echo "FAIL the race runtime"; grep -v '^ok' "$OUT/race_rt.txt" | sed 's/^/     /'; fail=1; fi
else echo "proc_gate: race_rt.c does not build"; fail=1; fi

# the binary in the model file is found relative to the model file
printf '[target]\nbinary = repl-bug\nnodes = 2\n[environment]\nclient_write = 5\n[invariants]\nagree_on_ack = "all r in replies : r.val in node[1]"\n' > "$B/proc/rel.model"
# (from another directory; not a subshell, so a failure counts)
cd /
expect "binary = is relative to the model file" 1 "agree_on_ack is violated" --model-file "$B/proc/rel.model"
cd "$HERE"

[ $fail = 0 ] && echo "proc_gate: ok (process front end: every verdict and every refusal)" || echo "proc_gate: FAILED"
exit $fail
