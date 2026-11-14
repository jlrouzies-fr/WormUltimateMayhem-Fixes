#!/bin/sh
# Restart the sandbox until Quick Game lands on a level with plenty of grass or moss terrain
# (the level cannot be chosen). Needs shells > 0 in the sandbox's wumrt.ini: the harness logs
# how many indices the shell layers add per frame, and that is what is checked here.
#   sh grassy.sh [MIN_INDICES_PER_LAYER] [MAX_TRIES]
cd "$(dirname "$0")"
MIN=${1:-3000}
TRIES=${2:-10}
LOG=../../rt-sandbox/wumrt.log
n=0
while [ $n -lt $TRIES ]; do
  n=$((n + 1))
  sh go.sh >/dev/null 2>&1
  sleep 4
  line=$(grep "shells:" "$LOG" | tail -1)
  idx=$(echo "$line" | sed -n 's/.* draws, \([0-9]*\) extra indices.*(\([0-9]*\) layers.*/\1 \2/p')
  set -- $idx
  per=0
  [ -n "$2" ] && [ "$2" -gt 0 ] && per=$(($1 / $2))
  echo "try $n: $per shell indices per layer"
  if [ "$per" -ge "$MIN" ]; then exit 0; fi
done
echo "no grassy level in $TRIES tries"
exit 1
