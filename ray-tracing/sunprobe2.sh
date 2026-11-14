#!/bin/sh
# Same fixed free-camera view on Apple Core, compare GPU vs CPU reference (selftest's hard-sun
# mode) with tex_compress 1, 0, 1 (a level reload before each, since textures load once).
cd "$(dirname "$0")"
INI=../../rt-sandbox/wumrt.ini
cp "$INI" out/sunprobe2_ini_backup
setini() { if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi; }
VIEW="-120.62 136.89 320.27 -135.00 -22.00"
setini tex_compress 1
out=$(LEVEL="Apple Core" sh go.sh 2>&1); echo "$out" | grep -E "pop-up|BUSY|POPPED"
if echo "$out" | grep -q "BUSY\|POPPED"; then cp out/sunprobe2_ini_backup "$INI"; exit 3; fi
first=1
for tc in 1 0 1; do
  setini tex_compress $tc; setini smooth 1
  if [ $first = 0 ]; then py -3 rt.py level "Apple Core" >/dev/null 2>&1; fi
  first=0
  sleep 10
  py -3 rt.py key esc; sleep 2
  py -3 rt.py freecam set $VIEW >/dev/null; sleep 2
  py -3 rt.py rt on 1 0.5 0 >/dev/null; py -3 rt.py sky 0 >/dev/null
  setini smooth 0; sleep 3
  for k in 1 2; do
    py -3 compare_mask.py out/sunprobe2 2>&1 | grep -E "sun visibility|disagree" | tr '\n' ' '; echo " [tex_compress $tc, run $k]"
  done
  py -3 rt.py shot "out/sunprobe2_tc$tc.png" >/dev/null 2>&1
  grep "alpha bits" ../../rt-sandbox/wumrt.log | tail -4 | sed 's/^/    /'
  py -3 rt.py freecam off >/dev/null; py -3 rt.py key esc; sleep 1
done
py -3 sandbox.py stop >/dev/null 2>&1
cp out/sunprobe2_ini_backup "$INI"
rm -f out/sunprobe2.bin out/sunprobe2_gpu.wrtf
