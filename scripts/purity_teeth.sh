#!/usr/bin/env bash
# purity_teeth.sh CC BUILDDIR: the purity gate's own teeth.  Each probe in
# test/probe/ hides impurity a different way (docs/research/02 B2, m2, M3); the gate
# must reject every one.  A probe that passes is a HOLE.
set -u
CC=${1:?usage: purity_teeth.sh CC BUILDDIR}
B=${2:?usage: purity_teeth.sh CC BUILDDIR}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$B/probe"
fail=0
for p in static_state asm_clock builtin_clock out_index; do
	o=$B/probe/$p.o
	"$CC" -std=c11 -O2 -c -o "$o" "$HERE/../test/probe/$p.c" -I"$HERE/../include" || { echo "teeth: $p: build failed"; fail=1; continue; }
	if "$HERE/purity_gate.sh" "$o" "$HERE/../test/probe/$p.c" > "$B/probe/$p.txt" 2>&1; then
		echo "teeth: HOLE: purity gate accepted probe/$p.c"; fail=1
	else
		echo "teeth: probe/$p.c rejected: $(grep -vE '^purity gate' "$B/probe/$p.txt" | head -1)"
	fi
done
[ $fail = 0 ] && echo "purity_teeth: ok" || echo "purity_teeth: FAILED"
exit $fail
