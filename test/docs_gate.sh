#!/bin/sh
# docs_gate.sh BUILDDIR: the documentation is a gate, like the code.
#   1. every man page passes mandoc lint (skipped, loudly, without mandoc);
#   2. every public function, macro, constant and enum value in
#      include/cmc.h, and every option cmc_main() parses, is documented
#      in the man pages;
#   3. no snippet holds Liquid markup, which Jekyll would expand;
#   4. the guide's server (docs/_includes/snippets/kvserver.c and
#      kvserver.model) does what the guide says under cmc --model-file:
#      the correct server passes, an acknowledgement is reachable, the
#      planted bug is caught by search and by simulation, and the bug is
#      invisible without a crash (Linux x86-64 only);
#   5. the in-process example (quickstart.c, libcmc(3)) builds against
#      libcmc.a and behaves as its comments say.
set -u
B=${1:?usage: docs_gate.sh BUILDDIR}
HERE=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/docs-gate
mkdir -p "$OUT"
fail=0
MAN="$HERE/man/man1/cmc.1 $HERE/man/man3/libcmc.3 $HERE/man/man7/cmc.7"

if command -v mandoc > /dev/null 2>&1; then
	for m in $MAN; do
		mandoc -T lint -W warning "$m" > "$OUT/lint.txt" 2>&1 ||
			{ echo "docs: lint: $m"; cat "$OUT/lint.txt"; fail=1; }
	done
else
	echo "docs: SKIP lint: mandoc not installed"
fi

# names from the header: functions, then the property macros
for f in $(grep -oE '\bcmc_[a-z_]+ *\(' "$HERE/include/cmc.h" | tr -d ' (' | sort -u) \
		$(grep -oE '^#define CMC_[A-Z_]+_PROP(ERTY)?\(|^#define CMC_INVARIANT\(' "$HERE/include/cmc.h" |
		sed 's/^#define //; s/($//'); do
	grep -qE "^\.Nm $f( ,)?\$|^\.Fn $f( |\$)" $HERE/man/man3/libcmc.3 ||
		{ echo "docs: $f is not in libcmc(3)"; fail=1; }
done
# every other CMC_ name the header defines: constants and enum values
for n in $(grep -oE '\bCMC_[A-Z_]+\b' "$HERE/include/cmc.h" | grep -vx 'CMC_H' | sort -u); do
	grep -qE "(^\.(Dv|Fn) |[ (])$n( |,|\$|\))" $HERE/man/man3/libcmc.3 ||
		{ echo "docs: $n is not in libcmc(3)"; fail=1; }
done
for o in $(grep -oE 'strcmp\(a, "--[a-z-]+"\)' "$HERE/src/cmc_cli.c" | grep -oE -- '--[a-z-]+' | sort -u); do
	grep -qE "^\.(It|Op) Fl -${o#--}( |\$)" "$HERE/man/man1/cmc.1" ||
		{ echo "docs: option $o is not in cmc(1)"; fail=1; }
done

# Jekyll reads included snippets as Liquid: a {{ or {% in one renders wrong
if grep -nE '\{\{|\{%' "$HERE"/docs/_includes/snippets/*; then
	echo "docs: a snippet contains Liquid markup ({{ or {%)"; fail=1
fi

W="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-missing-field-initializers"

# the guide's server, checked as it ships
if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ]; then
	K=$HERE/docs/_includes/snippets
	KB=$(cd "$B" && pwd)/guide
	mkdir -p "$KB"
	cp "$K/kvserver.model" "$KB/"
	if $CC $W -no-pie -o "$KB/kvserver" "$K/kvserver.c" &&
		$CC $W -no-pie -DKV_BUG -o "$KB/kvserver-bug" "$K/kvserver.c"; then
		kv() {		# kv DESC EXIT NEEDLE args...
			d=$1 want=$2 needle=$3; shift 3
			"$B/cmc" --model-file "$KB/kvserver.model" "$@" > "$OUT/kv.txt" 2>&1
			rc=$?
			[ "$rc" = "$want" ] && grep -q -- "$needle" "$OUT/kv.txt" ||
				{ echo "docs: guide: $d (exit $rc, wanted $want and \"$needle\"; see $OUT/kv.txt)"; cat "$OUT/kv.txt"; fail=1; }
		}
		kv "the correct server does not pass" 0 "16 distinct states found"
		kv "the bug is not caught as the guide shows" 1 "3. crash n0" --binary "$KB/kvserver-bug"
		kv "the bug shows without a crash, which the guide says it does not" 0 "No error has been found" --binary "$KB/kvserver-bug" --crashes 0
		kv "simulation does not find the bug" 1 "no_lost_ack is violated" --binary "$KB/kvserver-bug" --simulate --seed 1 --walks 200
		printf '[target]\nbinary = kvserver\nnodes = 2\n[environment]\nclient_write = 7\ncrashes = 1\n[invariants]\nnever_acked = "all r in replies : 0 == 1"\n' > "$KB/never.model"
		"$B/cmc" --model-file "$KB/never.model" > "$OUT/kv-never.txt" 2>&1
		[ $? = 1 ] && grep -q "5. deliver n1->n0" "$OUT/kv-never.txt" ||
			{ echo "docs: guide: no acknowledgement is reachable, so no_lost_ack would pass vacuously"; fail=1; }
	else
		echo "docs: kvserver.c does not build"; fail=1
	fi
else
	echo "docs: SKIP the guide's server: cmc --model-file needs Linux x86-64"
fi

S=$HERE/docs/_includes/snippets/quickstart.c
if $CC $W -I"$HERE/include" -o "$B/quickstart" "$S" "$B/libcmc.a" &&
	$CC $W -DQUICKSTART_BUG -I"$HERE/include" -o "$B/quickstart-bug" "$S" "$B/libcmc.a"; then
	Q="--model quickstart --all-props --det"
	"$B/quickstart" $Q --crashes 1 > "$OUT/qs.txt" 2>&1 &&
		grep -q 'No error has been found' "$OUT/qs.txt" ||
		{ echo "docs: quickstart: the correct server does not pass (see $OUT/qs.txt)"; fail=1; }
	"$B/quickstart-bug" $Q > "$OUT/qs-bug0.txt" 2>&1 ||
		{ echo "docs: quickstart: the bug build fails without a crash, which the guide says it does not (see $OUT/qs-bug0.txt)"; fail=1; }
	"$B/quickstart-bug" $Q --crashes 1 --trace-out "$OUT/qs.replay" > "$OUT/qs-bug.txt" 2>&1
	if [ $? = 1 ] && grep -q 'Invariant NoLostAck is violated' "$OUT/qs-bug.txt" &&
		grep -q 'search is 5\.' "$OUT/qs-bug.txt"; then
		"$B/quickstart-bug" --replay "$OUT/qs.replay" --all-props > "$OUT/qs-replay.txt" 2>&1
		grep -q 'NoLostAck is violated' "$OUT/qs-replay.txt" ||
			{ echo "docs: quickstart: the replay does not reproduce the bug"; fail=1; }
	else
		echo "docs: quickstart: HOLE: the planted bug is not caught at depth 5 (see $OUT/qs-bug.txt)"; fail=1
	fi
else
	echo "docs: quickstart.c does not build"; fail=1
fi
[ $fail = 0 ] && echo "docs_gate: ok (3 man pages, the guide's server, quickstart model)" || echo "docs_gate: FAILED"
exit $fail
