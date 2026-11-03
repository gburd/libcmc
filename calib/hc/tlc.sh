#!/usr/bin/env bash
# Usage: ./tlc.sh <Spec>.tla <cfg> [extra tlc args]   (sany: ./tlc.sh --sany Spec.tla)
set -euo pipefail
JAVA=/nix/store/slw6gs8pxkmc52incl79wlja31rrx6w8-openjdk-8u504-b01-jre/bin/java
JAR=/nix/store/0y7c487n8dhqii1za55vplcy1f2sh4ci-tlaplus-1.7.4/share/java/tla2tools.jar
if [ ! -x "$JAVA" ]; then  # resolve via nix if the store paths moved
  TLC=$(nix shell nixpkgs#tlaplus -c sh -c 'readlink -f $(which tlc)')
  JAVA=$(grep -o '/nix/store/[^ "]*/bin/java' "$TLC"); JAR=$(grep -o '/nix/store/[^ "]*tla2tools.jar' "$TLC")
fi
export TMPDIR=$HOME/tmp/tlc-tmp-$$; mkdir -p "$TMPDIR" "$HOME/tmp/tlc-states"  # per-run: parallel TLCs race on extracted std modules
if [ "$1" = --sany ]; then exec "$JAVA" -Djava.io.tmpdir="$TMPDIR" -cp "$JAR" tla2sany.SANY "$2"; fi
spec=$1; cfg=$2; shift 2
exec "$JAVA" -Xmx32g -XX:+UseParallelGC -Djava.io.tmpdir="$TMPDIR" -cp "$JAR" tlc2.TLC \
  -metadir "$HOME/tmp/tlc-states/$(basename "$cfg" .cfg)-$$" -config "$cfg" "$@" "$spec"
