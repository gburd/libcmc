#!/usr/bin/env bash
# purity_gate.sh "CORE_OBJECTS" ["CORE_SOURCES"]: the purity and isolation
# gates (docs/research/01 4a, docs/research/02 B2/m2/M3).
#   1. calls: nm -u of each core object is within the mem* allowlist
#      (no clock, randomness, I/O or allocation);
#   2. data: no writable data symbol (nm types b B d D g G s S, TLS, or
#      any symbol in a writable section); only const data (r R) is allowed,
#      so no state can hide outside the world bytes;
#   3. instructions: objdump finds no rdtsc/rdtscp/rdrand/rdseed/syscall/
#      sysenter/cpuid/int, and no absolute-address memory operand;
#   4. sources: no "asm" and no out->o[ indexing (cores emit through
#      cmc_out_push);
#   5. isolation: the engine includes no core's private header.
set -u
OBJS=${1:?usage: purity_gate.sh "OBJS" ["SRCS"]}
SRCS=${2:-}
ALLOW=" memcpy memset memmove memcmp __stack_chk_fail __memcpy_chk __memset_chk __memmove_chk "
HERE=$(cd "$(dirname "$0")" && pwd)
fail=0
for o in $OBJS; do
	for s in $(nm -u "$o" | awk '{print $NF}'); do
		case "$ALLOW" in *" $s "*) ;;
			*) echo "purity: $o calls $s"; fail=1;; esac
	done
	# writable or TLS data, by symbol type and by section flags
	nm --defined-only "$o" | awk -v F="$o" '$2 ~ /^[bBdDgGsS]$/ {print "purity: " F ": writable data symbol " $3 " (type " $2 ")"}' | grep . && fail=1
	readelf -SW "$o" | awk '/\] /{ for (i = 1; i <= NF; i++) if ($i ~ /^\.(t?bss|t?data)/ || $i ~ /^\.data\./) { sec = $i; sz = $(i + 4) } ;
		if (sec != "" && sz !~ /^0+$/) print "purity: " F ": non-empty writable section " sec; sec = "" }' F="$o" | grep . && fail=1
	readelf -sW "$o" | awk '$4 == "TLS" {print "purity: " F ": thread-local symbol " $8}' F="$o" | grep . && fail=1
	# instructions that read time, randomness or the kernel; absolute addresses
	objdump -d --no-show-raw-insn "$o" | awk -F'\t' 'NF >= 2 {print $2}' |
		grep -nE '^(rdtscp?|rdrand|rdseed|syscall|sysenter|cpuid|int |rdpid|rdpmc)\b|(^|[ ,])-?0x[0-9a-f]+(,|$)' |
		grep -vE '\$-?0x|%fs:0x28' | sed "s|^|purity: $o: instruction: |" | grep . && fail=1
done
for c in $SRCS; do
	grep -nE '(^|[^A-Za-z_])(__)?asm(__)?([^A-Za-z_]|$)' "$c" | sed "s|^|purity: $c: asm: |" | grep . && fail=1
	grep -nE 'out->o\[' "$c" | sed "s|^|purity: $c: indexes out->o (use cmc_out_push): |" | grep . && fail=1
done
if grep -nE '#include *"(hc|toy)/' "$HERE/../src/cmc.c" "$HERE/../src/cmc_cli.c"; then
	echo "isolation: engine includes a core's private header"; fail=1
fi
if grep -nE 'HcState|ToyState' "$HERE/../src/cmc.c"; then
	echo "isolation: engine names a core state type"; fail=1
fi
[ $fail = 0 ] && echo "purity gate: ok ($(echo $OBJS | wc -w) core objects, $(echo $SRCS | wc -w) sources)"
exit $fail
