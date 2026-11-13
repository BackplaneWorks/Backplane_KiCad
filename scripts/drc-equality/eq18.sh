#!/bin/bash
# eq18.sh RT TAG : every board, RT at 1 and 8 threads vs s14 (bp.13 without the guard) at 1 thread.
# Per board: raw (byte, date line removed) and normalised (cmp17.py) equality of RT t1 vs t8, and
# normalised equality of RT t1 vs s14 t1.  s14's own t1/t8 raw equality is recorded for reference.
RT=$1; TAG=$2; D=/home/user/drc; O=$D/out/eq-$TAG; S=$D/out/eq-all17; mkdir -p $O; cd $D/src
md5() { grep -v '"date"' "$1" | md5sum | cut -c1-12; }
while read -r b; do
  [ -z "$b" ] && continue
  n=$(echo "$b" | tr '/ ' '__' | sed 's/.kicad_pcb$//')
  sn=$(echo "$b" | tr '/' '_' | sed 's/.kicad_pcb$//')
  w=$(mktemp -d); cp -r "$(dirname "$b")"/. $w/ 2>/dev/null; s=$(basename "$b" .kicad_pcb); bb="$w/$s.kicad_pcb"
  for t in 1 8; do bash $D/drcrun.sh $D/rts/$RT/bin/kicad-cli "$bb" $t $O/$n-t$t.json >/dev/null; done
  if [ ! -s "$S/$sn-s14.json" ]; then bash $D/drcrun.sh $D/rts/s14/bin/kicad-cli "$bb" 1 "$S/$sn-s14.json" >/dev/null; fi
  if [ ! -s "$S/$sn-s14-t8.json" ]; then bash $D/drcrun.sh $D/rts/s14/bin/kicad-cli "$bb" 8 "$S/$sn-s14-t8.json" >/dev/null; fi
  if [ ! -s $O/$n-t1.json ] || [ ! -s "$S/$sn-s14.json" ]; then echo "NOREPORT $b"; rm -rf $w; continue; fi
  raw=DIFF; [ "$(md5 $O/$n-t1.json)" = "$(md5 $O/$n-t8.json)" ] && raw=SAME
  rawb=DIFF; [ -s "$S/$sn-s14-t8.json" ] && [ "$(md5 "$S/$sn-s14.json")" = "$(md5 "$S/$sn-s14-t8.json")" ] && rawb=SAME
  g=$(python3 -I $D/cmp17.py "$bb" "$S/$sn-s14.json" $O/$n-t1.json)
  t=$(python3 -I $D/cmp17.py "$bb" $O/$n-t1.json $O/$n-t8.json)
  echo "guard:${g%% *} threads:${t%% *} raw:$raw base-raw:$rawb $b ${g#SAME} ${t#SAME}"
  rm -rf $w
done < $D/boards-all.txt > $O/cmp.txt
awk '{print $1, $2, $3, $4}' $O/cmp.txt | sort | uniq -c
grep -v "^guard:SAME threads:SAME raw:SAME" $O/cmp.txt | cut -c1-400
