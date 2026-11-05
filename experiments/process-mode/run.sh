#!/bin/sh
# run.sh: build and run the process-mode prototype
# (experiments/process-mode/DESIGN.md).  Four milestones:
#   M0 (checker.c, demo/server.c): catch a durability bug in a server
#      written as it ships -- interception + one crash branch.
#   M1 (search.c, demo/kv.c): canonical state + a real BFS.  A state
#      reached by two orders is counted once (the diamond merges), so the
#      search terminates; a planted bug is caught with the shortest prefix.
#   M2 (search2.c, demo/list.c, demo/detmalloc.c): a real malloc heap.  A
#      deterministic arena defeats ASLR; raw heap bytes still cannot merge
#      two commuting orders (the wall); a ~8-line canonical-emit hook does.
#   M3 (mc.c, list.model, demo/list.c): a declarative model file drives
#      the checker -- what to run, the environment to explore, and the
#      invariants as a small expression DSL over the canonical state and
#      the client-input log.  No protocol logic in the file.
#   M4 (mc2.c, repl.model, demo/repl.c): multi-node.  The checker runs N
#      node processes and owns the network between them; it explores the
#      order inter-node messages are delivered, deduping over all nodes'
#      states plus the message bag, and checks invariants over node[i] and
#      the ghost reply log.
#   M5 (mc2.c, elect.model, demo/elect.c): time as an environment action.
#      A blocked recv is satisfied by a message OR by a bounded TIMEOUT the
#      search fires, so leader election is explorable; split brain caught.
#   M5b (mc2.c, lease.model, demo/lease.c): absolute time is engine-
#      controlled.  detmalloc.so forces clock_gettime off the vDSO; the
#      checker answers with a logical clock advanced only at timeouts, so a
#      stale-lease write (write after the lease really expired) is caught.
#   M6 (mc2.c, durlog.model, demo/durlog.c): crash and partition.  The
#      checker may CRASH a node (losing its volatile fd-3 state, keeping
#      its durable fd-193 state) or DROP a message, both budgeted; a
#      protocol that acks a client before the write is durable on a
#      quorum loses a committed write, and that is caught.
#   M7 (mc2.c, demo/{idle,fifo,chain,bigmsg,paxos}.c): honest verdicts at scale.
#      Probes that the M6 engine got wrong (merged-away time, out-of-order
#      TCP, a hardcoded topology, a depth-bounded "pass", a silently
#      truncated message) now give the right verdict; a bounded run exits
#      2 (INCOMPLETE), a cap is an error; single-decree Paxos with five
#      processes is checked exhaustively, and its bug is also found by
#      seeded --simulate.
#
# Kill criterion: this script prints "process-mode: ok".  Linux only
# (seccomp user notification); skips elsewhere, loudly.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}

case "$(uname -s)" in
Linux) ;;
*) echo "process-mode: SKIP (needs Linux seccomp user notification)"; exit 0 ;;
esac

$CC -O1 -w -o server          demo/server.c
$CC -O1 -w -DBUG -o server-bug demo/server.c
$CC -O2 -w -o checker         checker.c
$CC -O1 -no-pie -w -o kv      demo/kv.c
$CC -O1 -no-pie -w -DBUG -o kv-bug demo/kv.c
$CC -O2 -w -o search          search.c
$CC -shared -fPIC -w -o detmalloc.so demo/detmalloc.c
$CC -O1 -no-pie -w -o list            demo/list.c
$CC -O1 -no-pie -w -DCANON -o list-canon demo/list.c
$CC -O1 -no-pie -w -DCANON -DBUG -o list-bug demo/list.c
$CC -O2 -w -o search2         search2.c
$CC -O2 -w -o mc              mc.c
$CC -O1 -no-pie -w -o repl     demo/repl.c
$CC -O1 -no-pie -w -DBUG -o repl-bug demo/repl.c
$CC -O1 -no-pie -w -o elect    demo/elect.c
$CC -O1 -no-pie -w -DBUG -o elect-bug demo/elect.c
$CC -O1 -no-pie -w -o lease    demo/lease.c
$CC -O1 -no-pie -w -DBUG -o lease-bug demo/lease.c
$CC -O1 -no-pie -w -o durlog   demo/durlog.c
$CC -O1 -no-pie -w -DBUG -o durlog-bug demo/durlog.c
$CC -O1 -no-pie -w -o idle     demo/idle.c
$CC -O1 -no-pie -w -o fifo     demo/fifo.c
$CC -O1 -no-pie -w -o chain    demo/chain.c
$CC -O1 -no-pie -w -o bigmsg   demo/bigmsg.c
$CC -O1 -no-pie -w -o paxos    demo/paxos.c
$CC -O1 -no-pie -w -DBUG -o paxos-bug demo/paxos.c
$CC -O2 -w -o mc2             mc2.c

fail=0
expect() {				# expect DESC EXIT  cmd...
	desc=$1 want=$2; shift 2
	out=$("$@" 2>&1) && rc=0 || rc=$?
	note=$(echo "$out" | grep -E 'trace:|distinct|prefix:|WALL|VIOLATION|INCOMPLETE|ERROR|client inputs' | tr '\n' ';' || true)
	if [ "$rc" = "$want" ]; then echo "ok   $desc -> exit $rc ($note)"
	else echo "FAIL $desc -> exit $rc, expected $want"; echo "$out" | sed 's/^/     /'; fail=1; fi
}

# assert a line is present (for the WALL claim, which is not an exit code)
expect_out() {			# expect_out DESC NEEDLE  cmd...
	desc=$1 needle=$2; shift 2
	out=$("$@" 2>&1) || true
	if echo "$out" | grep -q "$needle"; then echo "ok   $desc ($needle)"
	else echo "FAIL $desc: '$needle' not in output"; echo "$out" | sed 's/^/     /'; fail=1; fi
}

echo "-- M0: a durability bug in a server written as it ships"
expect "M0 correct, no crash"  0 ./checker ./server
expect "M0 correct, one crash" 0 ./checker ./server --crash
expect "M0 bug, no crash"      0 ./checker ./server-bug
expect "M0 bug, one crash"     1 ./checker ./server-bug --crash

echo "-- M1: canonical state and a real search"
expect "M1 correct kv (diamond merges)" 0 ./search ./kv
expect "M1 bug kv (invariant caught)"   1 ./search ./kv-bug

echo "-- M2: a real malloc heap, and the wall it hits"
expect_out "M2 raw heap hits the wall" WALL ./search2 --raw ./list
expect "M2 canon merges reorders"       0 ./search2 --canon ./list-canon
expect "M2 canon bug (lost key caught)" 1 ./search2 --canon ./list-bug

echo "-- M3: a model file drives the checker (docs/model-file.md)"
expect "M3 correct (model file + DSL invariant)" 0 ./mc list.model ./list-canon
expect "M3 bug (DSL invariant caught)"           1 ./mc list.model ./list-bug

echo "-- M4: multi-node (the checker owns the inter-node network)"
expect "M4 correct repl (agree-on-ack holds)"   0 ./mc2 repl.model ./repl
expect "M4 bug repl (acked write backup lacks)" 1 ./mc2 repl.model ./repl-bug

echo "-- M5: time is an environment action (election timeout)"
expect "M5 correct elect (one leader per term)" 0 ./mc2 elect.model ./elect
expect "M5 bug elect (split brain caught)"      1 ./mc2 elect.model ./elect-bug

echo "-- M5b: absolute time is engine-controlled (stale-lease write)"
expect "M5b correct lease (no write after expiry)" 0 ./mc2 lease.model ./lease
expect "M5b bug lease (stale-lease write caught)"  1 ./mc2 lease.model ./lease-bug

echo "-- M6: crash loses volatile, keeps durable (lost committed write)"
expect "M6 correct durlog (no lost commit)"      0 ./mc2 durlog.model ./durlog
expect "M6 bug durlog (lost committed write)"    1 ./mc2 durlog.model ./durlog-bug

echo "-- M7: honest verdicts (probes that were wrong before M7), and Paxos"
expect "M7 idle: time passing is not merged away"     1 ./mc2 idle.model ./idle
expect "M7 fifo: a connection is delivered in order"  0 ./mc2 fifo.model ./fifo
expect "M7 chain: any topology the nodes dial"        1 ./mc2 chain.model ./chain
expect "M7 depth bound is INCOMPLETE, not a pass"     2 ./mc2 durlog.model ./durlog-bug --depth 2
expect "M7 a cap is an error, not a truncation"       1 ./mc2 bigmsg.model ./bigmsg
expect "M7 correct paxos (agreement, N=5, complete)"  0 ./mc2 paxos.model ./paxos
expect "M7 bug paxos (two values decided)"            1 ./mc2 paxos.model ./paxos-bug
expect "M7 bug paxos found by --simulate"             1 ./mc2 paxos.model ./paxos-bug --simulate --seed 1 --walks 2000
expect "M7 correct paxos --simulate is not a pass"    2 ./mc2 paxos.model ./paxos --simulate --seed 1 --walks 100

if [ "$fail" = 0 ]; then
	echo "process-mode: ok (bug found in deployed-style code; search over"
	echo "               interleavings terminates because equal states merge;"
	echo "               a real heap needs a small canonical-emit hook, shown;"
	echo "               a declarative model file drives it, invariants in a DSL;"
	echo "               multi-node: the checker owns the inter-node network;"
	echo "               time is an environment action, so elections explore;"
	echo "               absolute time is engine-controlled, so a stale lease bites;"
	echo "               a crash loses volatile but keeps durable, so a lost commit shows;"
	echo "               verdicts are honest: a bounded run is INCOMPLETE, a cap an error)"
else
	echo "process-mode: FAILED"
fi
exit $fail
