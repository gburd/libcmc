#!/bin/sh
# docs_gate.sh BUILDDIR: the documentation is a gate, like the code.
#   1. every man page passes mandoc lint (skipped, loudly, without mandoc);
#   2. every public function and macro in include/cmc.h, and every option
#      cmc_main() parses, is documented in the man pages;
#   3. no snippet holds Liquid markup, which Jekyll would expand;
#   4. the guide's model (docs/_includes/snippets/quickstart.c) builds
#      against libcmc.a and does what the guide says: the correct server
#      passes with a crash, the planted bug is caught, its replay
#      reproduces it, and the bug is invisible without a crash.
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
for o in $(grep -oE 'strcmp\(a, "--[a-z-]+"\)' "$HERE/src/cmc_cli.c" | grep -oE -- '--[a-z-]+' | sort -u); do
	grep -qE "^\.(It|Op) Fl -${o#--}( |\$)" "$HERE/man/man1/cmc.1" ||
		{ echo "docs: option $o is not in cmc(1)"; fail=1; }
done

# Jekyll reads included snippets as Liquid: a {{ or {% in one renders wrong
if grep -nE '\{\{|\{%' "$HERE"/docs/_includes/snippets/*; then
	echo "docs: a snippet contains Liquid markup ({{ or {%)"; fail=1
fi

W="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-missing-field-initializers"
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
[ $fail = 0 ] && echo "docs_gate: ok (3 man pages, quickstart model)" || echo "docs_gate: FAILED"
exit $fail
