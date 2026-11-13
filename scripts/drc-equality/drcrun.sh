#!/bin/bash
# drcrun.sh KICAD_CLI BOARD THREADS OUT.json [extra kicad-cli args...]
# One DRC run with a private config (runtime's bundled lib tables, MaximumThreads=THREADS).
# Prints: wall user sys maxrss_kb md5(report without date line)
set -u
K=$1; BOARD=$2; T=$3; OUT=$4; shift 4
RT=$(cd "$(dirname "$K")/.." && pwd)
C=${DRC_CFG_ROOT:-/tmp/drc-lane/cfg}/rt-$(basename "$RT")-t$T
if [ ! -e "$C/10.99/kicad_advanced" ]; then
  mkdir -p "$C/10.99"
  for t in fp-lib-table sym-lib-table design-block-lib-table; do cp "$RT/share/kicad/$t" "$C/10.99/$t" 2>/dev/null || true; done
  echo "MaximumThreads=$T" > "$C/10.99/kicad_advanced"
fi
TF=$(mktemp)
KICAD_CONFIG_HOME=$C /usr/bin/time -o "$TF" -f "%e %U %S %M" "$K" pcb drc --format json "$@" -o "$OUT" "$BOARD" > "$OUT.log" 2>&1
rc=$?
H=$(grep -v '"date"' "$OUT" 2>/dev/null | md5sum | cut -c1-12)
echo "$(cat $TF) $H rc=$rc"
rm -f "$TF"
