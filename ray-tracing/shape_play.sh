#!/bin/sh
# In-play cost of the tracer's copy of rounded worms at 480x270: starts the sandbox, waits for
# the turn to start, takes the game camera DIST world units from what the centre pixel shows
# (the match keeps running), then logs the "round:" lines and the tracer's cost for:
#   round_casters 1 | 2 with the defaults (round_caster_pixels 40, round_caster_steps 2) |
#   2 with every part from round_min_pixels at the main pass's steps (round_caster_pixels 10,
#   round_caster_steps 6: how it was first merged).
# Stops the game and puts round_casters 2 with the defaults back afterwards.
# Usage: sh shape_play.sh [DIST]
cd "$(dirname "$0")"
DIST=${1:-22}
SB=../../rt-sandbox
setv() {
  grep -q "^$1 " $SB/wumrt.ini || echo "$1 = $2" >> $SB/wumrt.ini
  sed -i "s/^$1 = .*/$1 = $2/" $SB/wumrt.ini
}
trap 'setv round_casters 2; setv round_caster_pixels 40; setv round_caster_steps 2; py -3 sandbox.py stop' EXIT
setv round_caster_pixels 40; setv round_caster_steps 2
sh go.sh | grep -E "start watch|POPPED|BUSY"
sleep 18
D=$(py -3 rt.py freecam take | sed -n 's/.*centre distance \([0-9.]*\).*/\1/p' | head -1)
echo "centre distance $D"
if [ "$(py -3 -c "print(1 if 0 < float('${D:-0}') < 400 else 0)")" = 1 ]; then
  py -3 rt.py freecam move 0 0 "$(py -3 -c "print(float('$D') - $DIST)")" | tail -1
fi
for cfg in "1 40 2" "2 40 2" "2 10 6"; do
  set -- $cfg
  setv round_casters $1; setv round_caster_pixels $2; setv round_caster_steps $3
  sleep 4
  n0=$(grep -c "round: [0-9]* draws" $SB/wumrt.log)
  sleep 22
  echo "== round_casters $1, round_caster_pixels $2, round_caster_steps $3, in play"
  grep "round: [0-9]* draws" $SB/wumrt.log | tail -n +$((n0 + 1)) | cut -c10-230
  py -3 rt.py rtstat 3 | grep -E "cost"
done
