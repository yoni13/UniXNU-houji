#!/bin/bash
# After a gtr=1 boot on gpu42 (turnip-houji-diag.patch, m2v-houji-diag.patch, adrenometal-zzzzzzz-shader-names.patch): read the
# render server's diagnostic files over the console (readfile for the size, dump for the bytes; capture.py saves each dump as
# logs/N-dump-K.bin) and the census rows (notes). The phone stays up. Usage: t-diag-read.sh rootNNN -> logs/rootNNN-diag-*.{txt,bin}
cd /mnt/6f99f95d-0b93-4a42-84ea-c848405a2812/unixnu-work/ios27; W=$PWD; D=$W/../perf-test; N=${1:?rootNNN}; L=logs/$N.log
[ -e /dev/ttyACM0 ] || { echo "no console"; exit 1; }
c() { timeout 6 python3 $D/phone.py cmd "$1" >/dev/null 2>&1; }
M=/private/var/mobile/Library/Caches/com.apple.backboardd/m2v
for f in shaders.txt ir3.txt spirv.bin shadows.txt opens.txt attached.txt cpstamps.bin; do
  n=$(wc -l < $L); c "readfile $M/$f"; sleep 2
  sz=$(tail -n +$n $L | grep -a "HOUJI READFILE: $M/$f" | tail -1 | sed -n 's/.* \([0-9]*\) bytes.*/\1/p')
  echo "$f: ${sz:-?} bytes"
  [ -n "$sz" ] && [ "$sz" -gt 0 ] || continue
  before=$(ls logs/$N-dump-*.bin 2>/dev/null | wc -l)
  n=$(wc -l < $L); c "dump $M/$f 0 $(printf %x $sz)"
  timeout 120 tail -n +$n -f $L | grep -a -m1 -q 'HOUJI DUMP SAVED\|HOUJI DUMP: .*error'; sleep 1
  saved=$(ls -t logs/$N-dump-*.bin 2>/dev/null | head -1)
  dc=$(tail -n +$n $L | grep -a 'HOUJI DUMP: ' | tail -1 | sed -n 's/.*crc \([0-9a-f]*\),.*/\1/p')
  hc=$(tail -n +$n $L | grep -a 'HOUJI DUMP SAVED' | tail -1 | sed -n 's/.* crc \([0-9a-f]*\) in.*/\1/p')
  if [ "$(ls logs/$N-dump-*.bin 2>/dev/null | wc -l)" -gt "$before" ] && [ -n "$dc" ] && [ "$dc" = "$hc" ]; then
    cp "$saved" logs/$N-diag-$f; echo "  -> logs/$N-diag-$f (crc $dc match)"
  else
    echo "  dump of $f failed or crc mismatch (phone ${dc:-?}, host ${hc:-?})"
  fi
done
n=$(wc -l < $L); c 'notes 400'; sleep 4; tail -n +$n $L > logs/$N-diag-notes.log
echo "census rows: $(grep -a -c 'HOUJI CENS' logs/$N-diag-notes.log)"
