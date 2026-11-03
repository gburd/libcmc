#!/usr/bin/env bash
# calibrate.sh CMC [fast|all]: run cmc's HC calibration core on the
# TLC configurations of docs/research/01 4b and compare against TLC's results.
#
# Expected values are from TLC 2.19 with -workers 1 (strict BFS order, so
# depths are shortest-counterexample lengths); except hc_paper_cp_n3_S2w's depth
# (30: its 1-worker TLC run had not finished; multi-worker TLC said 32) and
# hc_paper_cp_n3_S3a (TLC never finished it; its counts are the S2w run's,
# the same constants and so the same state space); see
# docs/measurements/2026-11-02-mc-calibration.md for why multi-worker TLC
# depths are not used.  Verdict and depth must
# match for every row; distinct states must match exactly for PASS rows.
# Logs go to $CMC_SCRATCH/calib (never /tmp).
set -u
MC=${1:?usage: calibrate.sh CMC [fast|all]}
MODE=${2:-all}
HERE=$(cd "$(dirname "$0")" && pwd)
CFG=$HERE/cfg
# one directory per run, so a failing run keeps its logs
OUT=${CMC_SCRATCH:-$HOME/tmp/cmc-build}/calib/$(date +%Y%m%d-%H%M%S)-$$
# the determinism gate doubles the cost; on by default, DET= turns it off
DET=${DET---det}
mkdir -p "$OUT"

# cfg | extra cmc args | expected verdict (property or PASS) | depth |
#   distinct (PASS only) | generated (PASS only; tlc_mult=TRUE makes these comparable)
ROWS_FAST="
hc_paper_cp_n3                        ||S1_Agreement|6|
hc_paper_cp_n3_s1                     ||S1_AgreementNonNone|7|
hc_code_ap_n3_S1                      ||S1_AgreementNonNone|6|
hc_paper_ap_n3_s1                     ||S1_AgreementNonNone|6|
hc_paper_cp_n3_S2_NoSplitBrainServing ||S2_NoSplitBrainServing|5|
hc_paper_ap_n3_S2w                    ||S2w_NoConcurrentWriteChains|6|
hc_paper_cp_n3_S2r_NoStaleRead        ||S2r_NoStaleRead|9|
hc_code_cp_n3_S2r_NoStaleRead         ||S2r_NoStaleRead|9|
hc_codeCZ_cp_n3_S2r                   ||S2r_NoStaleRead|11|
hc_paper_ap_n3_S3a                    ||S3a_NoLostAckActive|9|
hc_paper_cp_n3_W                      ||W_AckAfterReconfig|9|
hc_paper_cp_n3_S3_NoLostAck           ||S3_NoLostAck|12|
hc_code_cp_n3_S3_NoLostAck            ||S3_NoLostAck|12|
hc_codeZ_cp_n3_S3                     ||S3_NoLostAck|12|
hc_codeZ_cp_n3_S3a                    ||S3a_NoLostAckActive|13|
hc_codeC_cp_n3_S3                     ||S3_NoLostAck|14|
"
ROWS_PASS="
hc_code_cp_n3_S1_AgreementNonNone     ||PASS|24|7412120|64710625
hc_code_cp_n3_S3a                     ||PASS|24|7412120|64710625
hc_codeC_cp_n3_S3a                    ||PASS|28|9428240|82484959
hc_codeCZ_cp_n3_S1                    |--inv S3_NoLostAck,S3a_NoLostAckActive|PASS|32|15523080|136045719
hc_paper_cp_n3_S2w_NoConcurrentWriteChains ||PASS|30|35895280|323513489
hc_paper_cp_n3_S3a                    ||PASS|30|35895280|323513489
"
ROWS=$ROWS_FAST
[ "$MODE" = all ] && ROWS="$ROWS_FAST$ROWS_PASS"

fail=0
printf '%-44s %-28s %-28s %5s %5s %10s %10s %11s %11s %8s\n' cfg expected got depth exp distinct expected generated expected wall_s
while IFS="|" read -r cfg extra exp edepth edist egen; do
	cfg=$(echo "$cfg" | tr -d ' ')
	[ -z "$cfg" ] && continue
	log=$OUT/$cfg.txt
	# shellcheck disable=SC2086
	"$MC" --model hc --tlc-cfg "$CFG/$cfg.cfg" --cfg tlc_mult=TRUE $extra --progress 0 $DET > "$log" 2>&1
	got=$(grep -oE '(Invariant|Action property|Witness|Quiescence property|Engine check) [A-Za-z0-9_:]+ is violated' "$log" | head -1 | awk '{print $(NF-2)}')
	grep -q 'No error has been found' "$log" && got=PASS
	[ -z "$got" ] && got="ERROR(see $log)"
	depth=$(grep -oE 'search is [0-9]+' "$log" | grep -oE '[0-9]+')
	dist=$(grep -oE '[0-9]+ distinct states' "$log" | grep -oE '^[0-9]+')
	gen=$(grep -oE "^[0-9]+ states generated" "$log" | grep -oE "^[0-9]+")
	wall=$(grep -oE 'Finished in [0-9.]+' "$log" | grep -oE '[0-9.]+$')
	ok=ok
	[ "$got" != "$exp" ] && ok=MISMATCH
	[ "$depth" != "$edepth" ] && ok=MISMATCH
	[ -n "$edist" ] && [ "$dist" != "$edist" ] && ok=MISMATCH
	[ -n "${egen:-}" ] && [ "$gen" != "$egen" ] && ok=MISMATCH
	[ $ok != ok ] && fail=1
	printf '%-44s %-28s %-28s %5s %5s %10s %10s %11s %11s %8s %s\n' "$cfg" "$exp" "$got" "$depth" "$edepth" "$dist" "${edist:--}" "$gen" "${egen:--}" "$wall" "$ok"
done <<< "$ROWS"
[ $fail = 0 ] && echo "check-calibration: every row matches TLC" || echo "check-calibration: MISMATCH (logs kept in $OUT)"
exit $fail
