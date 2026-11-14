#!/bin/sh
# What each feature costs on the GPU: one fixed, paused free-camera view on Apple Core, every
# setting as in main's ini, then each feature turned off on its own. Per line: the tracer's
# GPU time (rt.py rtstat, tracer + temporal + filters) and the whole frame's GPU time from
# frame_timer (mean and fastest of 60 frames; the fastest is the least disturbed by other
# programs on the GPU). Leaves the game stopped and the ini as it was.
cd "$(dirname "$0")"
INI=../../rt-sandbox/wumrt.ini
LOG=../../rt-sandbox/wumrt.log
cp "$INI" out/costprobe_ini_backup
cp ../../../../rt-sandbox/wumrt.ini "$INI"          # measure main's current settings
setini() { if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi; }
VIEW="-120.62 136.89 320.27 -135.00 -22.00"
FEATURES=${*:-"round shell_shadow skin_translucency skin_wrap rim_light catchlights relief post hdr dof bloom shells scatter reflections specular"}
setini frame_timer 1
out=$(LEVEL="Apple Core" sh go.sh 2>&1); echo "$out" | grep -E "pop-up|BUSY|POPPED"
if echo "$out" | grep -q "BUSY\|POPPED"; then cp out/costprobe_ini_backup "$INI"; exit 3; fi
sleep 10
py -3 rt.py key esc; sleep 2
py -3 rt.py freecam set $VIEW >/dev/null
measure() {
  sleep 5
  tr=$(py -3 rt.py rtstat 2 | grep 'cost per frame' | sed 's/.*trace (GPU) //')
  ft=$(grep "frame timer: GPU" "$LOG" | tail -1 | sed 's/.*frame timer: GPU //; s/ (60 frames.*//')
  echo "trace $tr | frame $ft | $1"
}
measure "baseline (main's settings)"
# Each feature off between two baselines, so a drift in what else runs on the GPU cancels out:
# the feature's cost is the mean of the two baselines minus the time with it off.
for f in $FEATURES; do
  old=$(sed -n "s/^$f *= *//p" "$INI")
  [ -z "$old" ] && { echo "(no $f in the ini)"; continue; }
  setini "$f" 0
  measure "$f = 0 (was $old)"
  setini "$f" "$old"
  measure "baseline"
done
setini gi_rate 2; measure "gi_rate = 2 (new)"; setini gi_rate 1
measure "baseline again"
py -3 rt.py freecam off >/dev/null
py -3 sandbox.py stop >/dev/null 2>&1
cp out/costprobe_ini_backup "$INI"
