#!/bin/sh
# Cost and quality of the cheaper indirect light (gi_rate, gi_far) on one paused view:
# tracer GPU time (rt.py rtstat), noise and flicker of the indirect light (stability.py) and a
# screenshot per setting, in out/perf_*.png. Leaves the game stopped and the ini as it was.
#     LEVEL="Apple Core" sh perfprobe.sh
cd "$(dirname "$0")"
INI=../../rt-sandbox/wumrt.ini
cp "$INI" out/perfprobe_ini_backup
LEVEL=${LEVEL:-Apple Core}
setini() { if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi; }

out=$(LEVEL="$LEVEL" sh go.sh 2>&1); echo "$out" | grep -E "pop-up|BUSY|POPPED"
if echo "$out" | grep -q "BUSY\|POPPED"; then cp out/perfprobe_ini_backup "$INI" 2>/dev/null; exit 3; fi
sleep 10
py -3 rt.py key esc; sleep 2
for cfg in "1 0" "2 0" "4 0" "1 300" "2 300"; do
  set -- $cfg
  setini gi_rate $1; setini gi_far $2
  sleep 6                                   # settings poll + let the accumulation settle
  echo "== gi_rate $1, gi_far $2"
  py -3 rt.py rtstat 3 2>&1 | grep -E "cost per frame|fps"
  py -3 stability.py "rate$1 far$2" 2>&1 | tail -3
  py -3 rt.py shot "out/perf_rate$1_far$2.png" >/dev/null 2>&1
done
py -3 sandbox.py stop >/dev/null 2>&1
cp out/perfprobe_ini_backup "$INI"
