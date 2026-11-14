#!/bin/sh
# One game slot for the effect lights, in one batch (about 10 minutes at most):
#   1. start this copy's sandbox on a level, pause;
#   2. test light where the view's centre looks: screenshots off / on, tracer time off / on;
#   3. resume, wait for a CPU-team explosion (fx_log GLOW), pause on it, screenshots off / on;
#   4. stop the game. nvlddmkm events are checked after the start and after the stop.
#   sh fx_slot.sh NAME [LEVEL]     -> out/NAME_*.png, out/NAME_cost.txt
cd "$(dirname "$0")"
NAME=${1:-slot}
INI=../../rt-sandbox/wumrt.ini
setini() {
  if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi
}
gpucheck() {
  powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'; StartTime=[datetime]'2026-11-14 13:27:10'} -ErrorAction SilentlyContinue | ForEach-Object { 'NVLDDMKM EVENT ' + \$_.TimeCreated + ' id ' + \$_.Id }"
}
stopall() { py -3 sandbox.py stop >/dev/null 2>&1; echo "stopped $(date +%T)"; gpucheck; [ -n "$KEEP" ] || sh stream.sh slot give; }
[ -n "$NOSLOT" ] || sh stream.sh slot take fx-light ${MIN:-12} || exit 3
setini fx 1; setini fx_log 2; setini fx_test 0
LEVEL="${2:-Apple Core}" sh go.sh || { stopall; exit 1; }
py -3 rt.py status >/dev/null 2>&1 || { echo "no game (BUSY?)"; exit 3; }
echo "started $(date +%T)"; gpucheck
T0=$(date +%s)

if [ -z "$SKIPTEST" ]; then
# 2. test light, paused: placed where the centre looks, then the free camera turns so the lit
#    spot is beside the pause box
py -3 rt.py key esc; sleep 1
setini fx_test ${TEST:-1500}; sleep 1.5
py -3 rt.py freecam take >/dev/null; py -3 rt.py freecam turn ${TURN:-22} -6 | tail -1
sleep 2
py -3 rt.py shot "out/${NAME}_test_on.png"
py -3 rt.py rtstat 3 > "out/${NAME}_cost.txt" 2>&1; echo "--- test light on above, off below" >> "out/${NAME}_cost.txt"
setini fx 0; sleep 2
py -3 rt.py shot "out/${NAME}_test_off.png"
py -3 rt.py rtstat 3 >> "out/${NAME}_cost.txt" 2>&1
setini fx 1; setini fx_test $(( ${TEST:-1500} * 4 )); sleep 2.5
py -3 rt.py shot "out/${NAME}_test_on4.png"
setini fx_test 0
py -3 rt.py freecam off >/dev/null
grep "fx: test\|fx: effect\|failed" ../../rt-sandbox/wumrt.log | tail -4
gpucheck
fi

# 3. a real explosion; first try Space (fire?) in the human turn
[ -z "$SKIPTEST" ] && py -3 rt.py key esc && sleep 1
LEFT=$((${WAITMAX:-540} - $(date +%s) + T0))
[ $LEFT -gt 60 ] || LEFT=60
if py -3 fx_watch.py "out/${NAME}_boom" --log "${WORD:-BOOM}" --look ${LOOK:-180} --capture --timeout $LEFT; then
  sleep 1
  py -3 rt.py shot "out/${NAME}_boom_on.png"
  setini fx 0; sleep 2; py -3 rt.py shot "out/${NAME}_boom_off.png"
  setini fx 1; sleep 2; py -3 rt.py shot "out/${NAME}_boom_on2.png"
  setini fx_knee 500; sleep 2; py -3 rt.py shot "out/${NAME}_boom_k500.png"
  setini fx_knee 1000; setini fx_strength 0.5; sleep 2; py -3 rt.py shot "out/${NAME}_boom_s05.png"
  setini fx_strength 1
  : > "out/${NAME}_boom_cost.txt"
  for v in 1 0 1 0; do setini fx $v; sleep 3; echo "--- fx $v" >> "out/${NAME}_boom_cost.txt"; py -3 rt.py rtstat 3 2>&1 | grep -E "cost per frame|^mask" >> "out/${NAME}_boom_cost.txt"; done
  setini fx 1
  grep "fx:" ../../rt-sandbox/wumrt.log | tail -30 > "out/${NAME}_boom_log.txt"
fi
stopall
