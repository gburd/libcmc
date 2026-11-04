#!/bin/sh
# run.sh: build and run the M0 process-mode prototype
# (experiments/process-mode/DESIGN.md).  It checks the SAME server binary,
# written as it ships (blocking sockets, real write()+fsync(), no step
# function), in four configurations and asserts the one that must fail does
# and the three that must hold do.
#
# The kill criterion for M0: this script prints "process-mode M0: ok".
# Linux only (seccomp user notification); skips elsewhere, loudly.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}

case "$(uname -s)" in
Linux) ;;
*) echo "process-mode M0: SKIP (needs Linux seccomp user notification)"; exit 0 ;;
esac

$CC -O1 -w -o server     demo/server.c
$CC -O1 -w -DBUG -o server-bug demo/server.c
$CC -O2 -w -o checker    checker.c

fail=0
run() {					# run DESC EXPECT_EXIT  ./server [--crash]
	desc=$1 expect=$2; shift 2
	out=$(./checker "$@" 2>&1) && rc=0 || rc=$?
	line=$(echo "$out" | grep -E 'trace:' || true)
	if [ "$rc" = "$expect" ]; then
		echo "ok   $desc -> exit $rc ($line)"
	else
		echo "FAIL $desc -> exit $rc, expected $expect"; echo "$out" | sed 's/^/     /'
		fail=1
	fi
}

run "correct server, no crash"   0 ./server
run "correct server, one crash"  0 ./server --crash
run "buggy server, no crash"     0 ./server-bug
run "buggy server, one crash"    1 ./server-bug --crash

if [ "$fail" = 0 ]; then
	echo "process-mode M0: ok (a durability bug in a server written as it ships, caught at the syscall boundary)"
else
	echo "process-mode M0: FAILED"
fi
exit $fail
