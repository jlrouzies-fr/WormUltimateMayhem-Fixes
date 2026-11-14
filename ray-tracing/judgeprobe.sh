#!/bin/sh
# One level with this copy's texture folder: a paused free-camera view on Apple Core plus two
# orbit steps, screenshots into out/judge_*.png, the texture load line from the log. Stops the game.
cd "$(dirname "$0")"
LOG=../../rt-sandbox/wumrt.log
VIEW="-120.62 136.89 320.27 -135.00 -22.00"
TAG=${1:-mid}
out=$(LEVEL="Apple Core" sh go.sh 2>&1); echo "$out" | grep -E "pop-up|BUSY|POPPED"
if echo "$out" | grep -q "BUSY\|POPPED"; then exit 3; fi
sleep 10
py -3 rt.py shot "out/judge_${TAG}_play.png" >/dev/null 2>&1
py -3 rt.py key esc; sleep 2
py -3 rt.py freecam set $VIEW >/dev/null; sleep 5
py -3 rt.py shot "out/judge_${TAG}_a.png" >/dev/null 2>&1
for k in 1 2 3 4 5 6 7 8 9 10; do py -3 rt.py freecam orbit 6 0 >/dev/null; done; sleep 5
py -3 rt.py shot "out/judge_${TAG}_b.png" >/dev/null 2>&1
grep "replacement textures loaded" "$LOG" | tail -1
grep -ci "error" "$LOG"
py -3 rt.py freecam off >/dev/null
py -3 sandbox.py stop >/dev/null 2>&1
