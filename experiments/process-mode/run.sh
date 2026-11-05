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
$CC -O2 -w -o mc2             mc2.c

fail=0
expect() {				# expect DESC EXIT  cmd...
	desc=$1 want=$2; shift 2
	out=$("$@" 2>&1) && rc=0 || rc=$?
	note=$(echo "$out" | grep -E 'trace:|distinct|prefix:|WALL|VIOLATION|client inputs' | tr '\n' ';' || true)
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

if [ "$fail" = 0 ]; then
	echo "process-mode: ok (bug found in deployed-style code; search over"
	echo "               interleavings terminates because equal states merge;"
	echo "               a real heap needs a small canonical-emit hook, shown;"
	echo "               a declarative model file drives it, invariants in a DSL;"
	echo "               multi-node: the checker owns the inter-node network)"
else
	echo "process-mode: FAILED"
fi
exit $fail
