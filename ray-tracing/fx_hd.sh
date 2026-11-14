#!/bin/sh
# One explosion batch at 1920x1080 with the tracer at scale 0.5 (rule: 1080p only with scale 0.5),
# then 480x270 / scale 1 are restored whatever happened.  sh fx_hd.sh NAME
cd "$(dirname "$0")"
NAME=${1:-hd}
SB=../../rt-sandbox
INI=$SB/wumrt.ini
restore() {
  sed -i 's#^/RUN:WormsX /W:[0-9]* /H:[0-9]*#/RUN:WormsX /W:480 /H:270#' $SB/Default.cfg
  sed -i 's/^scale *=.*/scale = 1/' "$INI"
  echo "restored: $(head -1 $SB/Default.cfg | tr -d '\r'), $(grep '^scale' "$INI")"
}
trap restore EXIT
grep -q '^scale *=' "$INI" || echo "scale = 1" >> "$INI"
sed -i 's/^scale *=.*/scale = 0.5/' "$INI"
sed -i 's#^/RUN:WormsX /W:[0-9]* /H:[0-9]*#/RUN:WormsX /W:1920 /H:1080#' $SB/Default.cfg
echo "set: $(head -1 $SB/Default.cfg | tr -d '\r'), $(grep '^scale' "$INI")"
sh fx_retry.sh "$NAME"
