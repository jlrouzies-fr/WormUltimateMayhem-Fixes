#!/bin/sh
# gi_rate in motion: the free camera orbits a point on Apple Core in small steps (one per
# ~0.1 s) and a screenshot is taken mid-orbit, with gi_rate 1 and then 2 (same path).
cd "$(dirname "$0")"
INI=../../rt-sandbox/wumrt.ini
cp "$INI" out/motionprobe_ini_backup
setini() { if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi; }
VIEW="-120.62 136.89 320.27 -135.00 -22.00"
out=$(LEVEL="Apple Core" sh go.sh 2>&1); echo "$out" | grep -E "pop-up|BUSY|POPPED"
if echo "$out" | grep -q "BUSY\|POPPED"; then cp out/motionprobe_ini_backup "$INI"; exit 3; fi
sleep 10
py -3 rt.py key esc; sleep 2
for r in 1 2 4; do
  setini gi_rate $r; sleep 3
  py -3 rt.py freecam set $VIEW >/dev/null; sleep 4          # settle at the start of the path
  for k in 1 2 3 4 5 6; do py -3 rt.py freecam orbit 3 0 >/dev/null; done
  py -3 rt.py shot "out/motion_rate$r.png" >/dev/null 2>&1
  sleep 3; py -3 rt.py shot "out/motion_rate${r}_settled.png" >/dev/null 2>&1
  echo "gi_rate $r: $(py -3 rt.py rtstat 1 | grep 'cost per frame' | sed 's/.*trace/trace/')"
done
py -3 rt.py freecam off >/dev/null
py -3 sandbox.py stop >/dev/null 2>&1
cp out/motionprobe_ini_backup "$INI"
