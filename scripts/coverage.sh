#!/bin/sh
# coverage.sh MIN BUILDDIR [DIR...]: the coverage report of make coverage.
# Runs gcovr over the gcov data found under BUILDDIR and each DIR, prints
# function and branch coverage of every src/*.c, and exits 1 unless both
# are at least MIN percent in every file and overall.
#
# A file named in $COV_PENDING (space separated, e.g. "src/cmc_proc.c")
# that misses MIN is reported PENDING instead of failing, and so is the
# overall total while any file is pending.  It is for development; a
# release runs with it empty.
#
# Exclusions: gcovr's --exclude-unreachable-branches (branches on lines
# with no code, which the compiler made) and --exclude-throw-branches
# (exception edges, none in C), and GCOVR_EXCL markers in the source,
# which are allowed only on a defensive check of a libc call that cannot
# fail in a test, each with its reason.  The report lists every marker.
#
# Writes BUILDDIR/coverage.csv, BUILDDIR/coverage-branches.txt (the
# uncovered branches by line) and BUILDDIR/coverage/index.html.
set -eu
MIN=${1:?usage: coverage.sh MIN BUILDDIR [DIR...]}
B=${2:?usage: coverage.sh MIN BUILDDIR [DIR...]}
shift 2
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"
PENDING=${COV_PENDING:-}

command -v gcovr > /dev/null 2>&1 ||
	{ echo "coverage: gcovr is not installed (pip install gcovr)"; exit 1; }
for p in $PENDING; do
	case $p in
		src/*.c) [ -f "$p" ] && continue ;;
	esac
	echo "coverage: COV_PENDING names $p, which is not a src/*.c file"
	exit 1
done

# Hit counts in the hash loops run past gcovr's "suspicious" threshold,
# which exists for a gcov bug that gives absurd counts; ours are real.
mkdir -p "$B/coverage"
gcovr -r . --filter 'src/' --exclude-unreachable-branches --exclude-throw-branches \
	--gcov-suspicious-hits-threshold 0 \
	--csv "$B/coverage.csv" --txt-metric branch --txt "$B/coverage-branches.txt" \
	--html-details "$B/coverage/index.html" "$B" "$@" > "$B/coverage.log" 2>&1 ||
	{ echo "coverage: gcovr failed"; cat "$B/coverage.log"; exit 1; }

echo "coverage: src/*.c, at least $MIN% of functions and of branches in each file and overall"
echo "coverage: gcovr --exclude-unreachable-branches --exclude-throw-branches"
nmark=$(grep -c 'GCOVR_EXCL' src/*.c | awk -F: '{ n += $2 } END { print n + 0 }')
echo "coverage: $nmark GCOVR_EXCL marker(s) in src/*.c"
grep -n 'GCOVR_EXCL' src/*.c | sed 's/^/  /' || true

# One row per src/*.c file, from the CSV (filename, line total, covered,
# percent, branch total, covered, percent, function total, covered,
# percent).  A file with no data at all fails: it was never run.
fail=0
for f in src/*.c; do
	row=$(awk -F, -v f="$f" '$1 == f' "$B/coverage.csv")
	pend=no
	case " $PENDING " in *" $f "*) pend=yes ;; esac
	echo "$f|${row:-none}|$pend"
done | awk -F'|' -v min="$MIN" '
	function pct(c, t) { return t ? 100 * c / t : 0 }
	function ok(c, t) { return t > 0 && c * 100 >= min * t }
	BEGIN { printf "%-20s %22s %22s  %s\n", "file", "functions", "branches", "status" }
	{
		if ($2 == "none") { fc = ft = bc = bt = 0 }
		else { split($2, c, ","); bt = c[5]; bc = c[6]; ft = c[8]; fc = c[9] }
		FC += fc; FT += ft; BC += bc; BT += bt
		st = ok(fc, ft) && ok(bc, bt) ? "ok" : $3 == "yes" ? "PENDING" : "FAIL"
		if (st == "FAIL") bad = 1
		if (st == "PENDING") pend = 1
		printf "%-20s %6.1f%% (%5d/%5d)  %6.1f%% (%5d/%5d)  %s%s\n", $1, pct(fc, ft), fc, ft,
			pct(bc, bt), bc, bt, st, $2 == "none" ? " (no coverage data)" : ""
	}
	END {
		st = ok(FC, FT) && ok(BC, BT) ? "ok" : pend ? "PENDING" : "FAIL"
		if (st == "FAIL") bad = 1
		printf "%-20s %6.1f%% (%5d/%5d)  %6.1f%% (%5d/%5d)  %s\n", "TOTAL", pct(FC, FT), FC, FT,
			pct(BC, BT), BC, BT, st
		exit bad
	}' || fail=1
echo "coverage: uncovered branches by line in $B/coverage-branches.txt; HTML in $B/coverage/index.html"
if [ $fail != 0 ]; then
	echo "coverage: FAILED"
elif [ -n "$PENDING" ]; then
	echo "coverage: ok, with PENDING files: $PENDING (a release needs COV_PENDING empty)"
else
	echo "coverage: ok"
fi
exit $fail
