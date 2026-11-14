#!/bin/sh
# In-play cost of round_casters (1 vs 2) at 480x270: starts the sandbox, waits for the turn to
# start, takes the game camera DIST world units from what the centre pixel shows (match keeps
# running), then logs the "round:" lines and the tracer's cost with round_casters = 2, then 1.
# Stops the game afterwards. Usage: sh shape_play.sh [DIST]
cd "$(dirname "$0")"
DIST=${1:-22}
SB=../../rt-sandbox
trap 'sed -i "s/^round_casters = .*/round_casters = 1/" $SB/wumrt.ini; py -3 sandbox.py stop' EXIT
grep -q '^round_casters' $SB/wumrt.ini || echo 'round_casters = 1' >> $SB/wumrt.ini
sh go.sh | grep -E "start watch|POPPED|BUSY"
sleep 18
D=$(py -3 rt.py freecam take | sed -n 's/.*centre distance \([0-9.]*\).*/\1/p' | head -1)
echo "centre distance $D"
if [ "$(py -3 -c "print(1 if 0 < float('${D:-0}') < 400 else 0)")" = 1 ]; then
  py -3 rt.py freecam move 0 0 "$(py -3 -c "print(float('$D') - $DIST)")" | tail -1
fi
for v in 2 1; do
  sed -i "s/^round_casters = .*/round_casters = $v/" $SB/wumrt.ini
  sleep 4
  n0=$(grep -c "round: [0-9]* draws" $SB/wumrt.log)
  sleep 22
  echo "== round_casters = $v, in play"
  grep "round: [0-9]* draws" $SB/wumrt.log | tail -n +$((n0 + 1)) | cut -c1-230
  py -3 rt.py rtstat 3 | grep -E "fps|cost"
done
py -3 rt.py shot captures/shape_play_view.png | tail -1
