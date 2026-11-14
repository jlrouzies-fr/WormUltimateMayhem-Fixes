#!/bin/sh
# 1080p before/after of rounded shapes (round 0 / 1) on the active worm: starts the sandbox at
# 1920x1080 with the traced mask at half resolution (scale = 0.5), waits for the turn to start
# (the game camera then looks at the worm), pauses, takes the game camera, moves to DIST world
# units from what the centre pixel shows, optionally turns by YAW (the HUD and pause box are hidden: hide_hud = 1), and
# shoots both settings. Afterwards it puts 480x270 and scale = 1 back and stops the game.
# Needs the game slot. Usage: sh shape_hd.sh NAME [DIST [YAW]]
cd "$(dirname "$0")"
NAME=${1:-shape_hd}
DIST=${2:-22}
YAW=${3:-0}
SB=../../rt-sandbox
restore() {
  sed -i 's|/W:1920 /H:1080|/W:480 /H:270|' $SB/Default.cfg
  sed -i 's/^scale = .*/scale = 1/' $SB/wumrt.ini
  sed -i 's/^hide_hud = .*/hide_hud = 0/' $SB/wumrt.ini
  py -3 sandbox.py stop
}
trap restore EXIT
sed -i 's/^scale = .*/scale = 0.5/' $SB/wumrt.ini
grep -q '^hide_hud' $SB/wumrt.ini || echo 'hide_hud = 0' >> $SB/wumrt.ini
sed -i 's/^hide_hud = .*/hide_hud = 1/' $SB/wumrt.ini      # no HUD or pause box in the shots
sed -i 's|/W:480 /H:270|/W:1920 /H:1080|' $SB/Default.cfg
sh go.sh | grep -E "start watch|POPPED|BUSY|fps"
sleep 18
py -3 rt.py rtstat 2 | grep -E "mask|cost"
py -3 rt.py key esc; sleep 2
py -3 rt.py screen
D=$(py -3 rt.py freecam take | sed -n 's/.*centre distance \([0-9.]*\).*/\1/p' | head -1)
echo "centre distance $D"
if [ "$(py -3 -c "print(1 if 0 < float('${D:-0}') < 400 else 0)")" != 1 ]; then echo "nothing near the centre of the view: no shots"; exit 1; fi
py -3 rt.py freecam move 0 0 "$(py -3 -c "print(float('$D') - $DIST)")" | tail -1
py -3 rt.py freecam turn "$YAW" 0 | tail -1
py -3 ab_setting.py ${NAME} ${SET:-round} ${A:-0} ${B:-1} "${LA:-stock mesh}" "${LB:-rounded}" | tail -1
# PLAY=1: then let the match run and log the rounding's CPU cost and the tracer's with the
# setting at B, then at A (about 12 s each; the harness logs "round:" every 300 frames)
if [ "${PLAY:-0}" = 1 ]; then
  py -3 rt.py key esc; sleep 2
  for v in ${B:-1} ${A:-0}; do
    sed -i "s/^${SET:-round} = .*/${SET:-round} = $v/" $SB/wumrt.ini
    sleep 13
    echo "== ${SET:-round} = $v, in play"
    grep "round: [0-9]* draws" $SB/wumrt.log | tail -1
    py -3 rt.py rtstat 3 | grep -E "fps|cost"
  done
fi
