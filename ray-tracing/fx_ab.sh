#!/bin/sh
# One test batch for the effect lights: start this copy's sandbox (unless it runs), wait for an
# explosion (the harness logs GLOW for glowing effect draws, fx_log = 1), pause on it, then
# screenshot the frozen frame with the effect lights off and on. Leaves the game paused.
#   sh fx_ab.sh NAME [LEVEL]      -> out/NAME_off.png, out/NAME_on.png, out/NAME.bin (draw capture)
cd "$(dirname "$0")"
NAME=${1:-fx}
INI=../../rt-sandbox/wumrt.ini
setini() {  # setini KEY VALUE
  if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi
}
setini fx_log 1
setini fx 1
if ! py -3 rt.py status >/dev/null 2>&1; then
  LEVEL="${2:-Apple Core}" sh go.sh || exit 1
fi
py -3 fx_watch.py "out/${NAME}_watch" --log GLOW --capture --timeout ${WAIT:-420} || exit 2
mv "out/${NAME}_watch.bin" "out/${NAME}.bin" 2>/dev/null
sleep 1
setini fx 0; sleep 2.5; py -3 rt.py shot "out/${NAME}_off.png"
setini fx 1; sleep 2.5; py -3 rt.py shot "out/${NAME}_on.png"
grep "fx:" ../../rt-sandbox/wumrt.log | tail -25
