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
for f in repl elect lease durlog idle fifo chain bigmsg clocks accept_timeout quorum pollloop guarded unmodelled crossrpc twoconn; do
	$CC -O1 -no-pie -w -o "$B/proc/$f" "$T/$f.c" || { echo "proc_gate: $f.c does not build"; fail=1; }
done
for f in repl elect lease durlog quorum pollloop; do
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
$CC -O1 -no-pie -w -o "$B/proc/twothread_join" "$HERE/test/proc/twothread_join.c" -lpthread || { echo "proc_gate: twothread_join.c does not build"; fail=1; }
for f in racewake raceemit raceborn; do
	$CC -O1 -no-pie -w -o "$B/proc/$f" "$T/$f.c" -lpthread || { echo "proc_gate: $f.c does not build"; fail=1; }
	$CC -O1 -no-pie -w -DBUG -o "$B/proc/$f-bug" "$T/$f.c" -lpthread || { echo "proc_gate: $f.c -DBUG does not build"; fail=1; }
done
$CC -O1 -no-pie -w -o "$B/proc/racepipe" "$T/racepipe.c" -lpthread || { echo "proc_gate: racepipe.c does not build"; fail=1; }
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
	expect "hc: AP-mode adoption breaks agreement" 1 "agreement is violated" $(runx "$HERE/examples/hc/hc.model" hc-bug) --timeouts 12 --simulate --seed 2 --walks 30000 --depth 600
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
expect "twothread: the handoff holds in every schedule"  0 "No error has been found" $(run twothread twothread)
expect "twothread: a dropped handoff is caught"         1 "handoff is violated" $(run twothread twothread-bug)
expect "twothread: the counterexample names the delivery" 1 "deliver n0->n1" $(run twothread twothread-bug)
expect "twothread: the counterexample names the worker's run" 1 "run n1 t1" $(run twothread twothread-bug)
# the data crosses the handoff: reaching the stored value is search-driven
printf '[target]\nnodes = 2\n[fields]\nrecvd = 0\nstored = 1\n[invariants]\nprocessed = "eventually node[1].stored == 120"\n' > "$OUT/tt-reach.model"
expect "twothread: the worker reaches the stored value"  0 "No error has been found" --model-file "$OUT/tt-reach.model" --binary "$B/proc/twothread"
# a worker that exits after one job, joined by the main thread: exercises
# the thread-exit slot-free and the pthread_join futex on the worker's
# clear-tid word (woken by the checker's SYS_exit CLONE_CHILD_CLEARTID).
expect "twothread_join: exit + join holds"              0 "No error has been found" $(run twothread_join twothread_join)
# Each race below is missed by an engine that runs one order of a node's
# threads.  racewake: a worker woken before the data it reads is stored
# runs before the waker's next call in some schedule.  raceemit: two
# workers that read a shared counter, log it, and store it plus one,
# with no lock, lose an update when one runs between the other's read
# and store.  raceborn: a new thread's first send can come before its
# parent's next one.  Each correct build holds in every schedule.
expect "racewake: a wake before the store is caught"      1 "stored_first is violated" $(run racewake racewake-bug)
expect "racewake: the woken worker runs first"            1 "run n0 t1" $(run racewake racewake-bug)
expect "racewake: storing first holds"                    0 "No error has been found" $(run racewake racewake)
expect "raceemit: an unlocked increment loses an update"  1 "no_lost_update is violated" $(run raceemit raceemit-bug)
expect "raceemit: the locked increment holds"             0 "No error has been found" $(run raceemit raceemit)
expect "raceborn: a new thread can send before its parent" 1 "a_first is violated" $(run raceborn raceborn-bug)
expect "raceborn: sending before the clone holds"         0 "No error has been found" $(run raceborn raceborn)
# racepipe: two threads' calls on kernel pipes interleave, so main can
# read the first pipe's byte after missing the second's
expect "racepipe: calls on kernel fds interleave"          0 "No error has been found" $(run racepipe racepipe)

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

# constructs the engine answered wrongly (soundness plan 3.4, 3.6): a
# write from an unmapped buffer gets EFAULT, not a zero-filled success,
# and a finite poll timeout, EPOLLET and EFD_SEMAPHORE are refused
for m in none badptr pollto epollet semaphore longrec partrec refused rstwrite rstignore rsthandler fork writev sendmsg peek nbaccept udp; do
	printf '[target]\nbinary = %s\nnodes = 2\nargs = %s\n[invariants]\nuntouched = "node[0][0] != 1"\n' "$B/proc/unmodelled" "$m" > "$OUT/u-$m.model"
done
printf 'faulted = "eventually node[0][0] == 2"\n' >> "$OUT/u-badptr.model"
printf 'refused = "eventually node[0][0] == 2"\n[environment]\ntimeouts = 1\n' >> "$OUT/u-refused.model"
printf 'killed = "node[0][0] != 2"\n' >> "$OUT/u-rstwrite.model"
printf 'epiped = "eventually node[0][0] == 2"\n' >> "$OUT/u-rstignore.model"
expect "unmodelled: a bad write buffer gets EFAULT"      0 "No error has been found" --model-file "$OUT/u-badptr.model"
# 3.9: a root that enables no action explored nothing, so a safety-only
# model of it is not a pass
expect "noprogress: a root with no action is not a pass" 2 "no action was ever enabled" --model-file "$OUT/u-none.model"
expect "unmodelled: a finite poll timeout is refused"    3 "timeout is not modelled" --model-file "$OUT/u-pollto.model"
expect "unmodelled: EPOLLET is refused"                  3 "are not modelled" --model-file "$OUT/u-epollet.model"
expect "unmodelled: EFD_SEMAPHORE is refused"            3 "EFD_SEMAPHORE" --model-file "$OUT/u-semaphore.model"
# a node that forks is two processes the model cannot tell apart
expect "unmodelled: a fork is refused"                   3 "forks a second process" --model-file "$OUT/u-fork.model"
# calls and flags the model does not answer are refused, not passed to a
# kernel that has no such socket, or ignored
expect "unmodelled: writev on an owned fd is refused"     3 "checker-owned fd" --model-file "$OUT/u-writev.model"
expect "unmodelled: sendmsg on an owned fd is refused"    3 "checker-owned fd" --model-file "$OUT/u-sendmsg.model"
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
	( cd "$d" && exec "$B/proc/tcpsem" "$2" "$1" 3>"$d/n$2.state" ) & pa=$!
	sleep "$3"
	( cd "$d" && exec "$B/proc/tcpsem" "$other" "$1" 3>"$d/n$other.state" ) & pb=$!
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
if $CC -std=c11 -O1 -w -I"$HERE/src" -I"$HERE/include" -o "$B/proc/fingerprint" "$T/fingerprint.c" "$HERE/src/cmc_spec.c"; then
	if "$B/proc/fingerprint" > "$OUT/fp.txt" 2>&1; then echo "ok   the world encoding separates differing futures"
	else echo "FAIL the world encoding"; sed 's/^/     /' "$OUT/fp.txt"; fail=1; fi
else echo "proc_gate: fingerprint.c does not build"; fail=1; fi

# the preload's allocator (soundness plan 3.5): an impossible size fails
# instead of wrapping, and realloc copies only the old block
if $CC -O1 -w -o "$B/proc/preload_alloc" "$T/preload_alloc.c"; then
	if "$B/proc/preload_alloc" > "$OUT/alloc.txt" 2>&1; then echo "ok   the preload allocator checks sizes and copies exactly"
	else echo "FAIL the preload allocator"; sed 's/^/     /' "$OUT/alloc.txt"; fail=1; fi
else echo "proc_gate: preload_alloc.c does not build"; fail=1; fi

# the binary in the model file is found relative to the model file
printf '[target]\nbinary = repl-bug\nnodes = 2\n[environment]\nclient_write = 5\n[invariants]\nagree_on_ack = "all r in replies : r.val in node[1]"\n' > "$B/proc/rel.model"
# (from another directory; not a subshell, so a failure counts)
cd /
expect "binary = is relative to the model file" 1 "agree_on_ack is violated" --model-file "$B/proc/rel.model"
cd "$HERE"

[ $fail = 0 ] && echo "proc_gate: ok (process front end: every verdict and every refusal)" || echo "proc_gate: FAILED"
exit $fail
