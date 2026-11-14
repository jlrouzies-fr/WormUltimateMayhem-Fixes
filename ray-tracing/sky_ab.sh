#!/bin/sh
# Before/after shots of the matte-painting sky (sky_style) from a wide, sky-showing free camera.
#   sh sky_ab.sh TAG [X Y Z YAW PITCH]     (game running in this copy's sandbox; level already loaded)
# Writes captures/sky_TAG_off.png and captures/sky_TAG_on.png, then leaves sky_style = 1.
cd "$(dirname "$0")"
INI=../../rt-sandbox/wumrt.ini
TAG=${1:-test}; shift
mkdir -p captures
if [ $# -ge 5 ]; then py -3 rt.py freecam set "$@"; else py -3 rt.py freecam take; py -3 rt.py freecam turn 0 22; fi
setv() { if grep -q "^$1 = " $INI; then sed -i "s/^$1 = .*/$1 = $2/" $INI; else printf '%s = %s\n' "$1" "$2" >> $INI; fi; }
setv sky_style 0; sleep 3
py -3 rt.py shot captures/sky_${TAG}_off.png
setv sky_style 1; sleep 3
py -3 rt.py shot captures/sky_${TAG}_on.png
