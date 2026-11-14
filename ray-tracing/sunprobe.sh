#!/bin/sh
# Which setting moves the GPU-vs-reference sun difference that selftest.sh reports? One level,
# one paused view, the selftest's hard-sun mode; the compare runs with everything as in the
# ini, then with one setting at a time turned off, then with tex_compress 0 (needs a level
# reload, so last). Output: one line per run. Leaves the game stopped and the ini as it was.
#     LEVEL="Apple Core" sh sunprobe.sh [SETTING ...]
cd "$(dirname "$0")"
INI=../../rt-sandbox/wumrt.ini
cp "$INI" out/sunprobe_ini_backup
SETTINGS=${*:-"round scatter shells relief catchlights skin_wrap skin_translucency rim_light post hdr reflections specular bump"}
LEVEL=${LEVEL:-Apple Core}

setini() { if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi; }
cmp1() {
  py -3 compare_mask.py "out/sunprobe" 2>&1 | grep -E "sun visibility|disagree" | sed 's/^/    /' | tr '\n' ' '
  echo " [$1]"
}

out=$(LEVEL="$LEVEL" sh go.sh 2>&1); echo "$out" | grep -E "pop-up|BUSY|POPPED"
if echo "$out" | grep -q "BUSY\|POPPED"; then cp out/sunprobe_ini_backup "$INI" 2>/dev/null; exit 3; fi
sleep 10
py -3 rt.py key esc; sleep 2                         # pause: the view stays still
py -3 rt.py rt on 1 0.5 0 >/dev/null; py -3 rt.py sky 0 >/dev/null
setini smooth 0; sleep 3
cmp1 "baseline (ini as is, smooth 0)"
cmp1 "baseline again"
for s in $SETTINGS; do
  old=$(sed -n "s/^$s *= *//p" "$INI")
  [ -z "$old" ] && { echo "    (no $s in the ini)"; continue; }
  setini "$s" 0; sleep 3
  cmp1 "$s = 0 (was $old)"
  setini "$s" "$old"; sleep 2
done
for s in $SETTINGS; do grep -q "^$s *=" "$INI" && setini "$s" 0; done; sleep 3
cmp1 "all of: $SETTINGS = 0"
cp out/sunprobe_ini_backup "$INI"; sleep 2
cmp1 "baseline restored"
# textures are only replaced on load: compare tex_compress 1 and 0 after a reload of the same level each
for tc in 1 0; do
  setini tex_compress $tc; setini smooth 1
  py -3 rt.py level "$LEVEL" >/dev/null 2>&1; sleep 10
  py -3 rt.py key esc; sleep 2
  py -3 rt.py rt on 1 0.5 0 >/dev/null; py -3 rt.py sky 0 >/dev/null
  setini smooth 0; sleep 3
  cmp1 "after level reload, tex_compress $tc"
done
py -3 sandbox.py stop >/dev/null 2>&1
cp out/sunprobe_ini_backup "$INI"
rm -f out/sunprobe.bin out/sunprobe_gpu.wrtf
