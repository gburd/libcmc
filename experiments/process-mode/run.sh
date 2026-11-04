#!/bin/sh
# run.sh: build and run the process-mode prototype
# (experiments/process-mode/DESIGN.md).  Two milestones:
#   M0 (checker.c, demo/server.c): catch a durability bug in a server
#      written as it ships -- interception + one crash branch.
#   M1 (search.c, demo/kv.c): canonical state + a real BFS.  A state
#      reached by two orders is counted once (the diamond merges), so the
#      search terminates; a planted bug is caught with the shortest prefix.
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

fail=0
expect() {				# expect DESC EXIT  cmd...
	desc=$1 want=$2; shift 2
	out=$("$@" 2>&1) && rc=0 || rc=$?
	note=$(echo "$out" | grep -E 'trace:|distinct states|prefix:' | tr '\n' ';' || true)
	if [ "$rc" = "$want" ]; then echo "ok   $desc -> exit $rc ($note)"
	else echo "FAIL $desc -> exit $rc, expected $want"; echo "$out" | sed 's/^/     /'; fail=1; fi
}

echo "-- M0: a durability bug in a server written as it ships"
expect "M0 correct, no crash"  0 ./checker ./server
expect "M0 correct, one crash" 0 ./checker ./server --crash
expect "M0 bug, no crash"      0 ./checker ./server-bug
expect "M0 bug, one crash"     1 ./checker ./server-bug --crash

echo "-- M1: canonical state and a real search"
expect "M1 correct kv (diamond merges)" 0 ./search ./kv
expect "M1 bug kv (invariant caught)"   1 ./search ./kv-bug

if [ "$fail" = 0 ]; then
	echo "process-mode: ok (bug found in deployed-style code; search over"
	echo "               interleavings terminates because equal states merge)"
else
	echo "process-mode: FAILED"
fi
exit $fail
