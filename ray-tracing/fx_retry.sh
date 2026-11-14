#!/bin/sh
# Run fx_slot.sh NAME once fewer than 3 sandbox games run (checks every 20 s, gives up after
# MAXWAIT seconds, default 900). Same environment variables as fx_slot.sh.
cd "$(dirname "$0")"
NAME=${1:-slot}
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt ${MAXWAIT:-900} ]; do
  n=$(tasklist //FI "IMAGENAME eq WormsMayhem.exe" 2>/dev/null | grep -c Worms)
  if [ "$n" -lt 3 ]; then
    echo "games running: $n, starting $(date +%T)"
    sh fx_slot.sh "$@"
    echo "fx_slot exit $? $(date +%T)"
    grep -q "^started" "out/${NAME}_run.txt" 2>/dev/null || true
    exit 0
  fi
  sleep 20
done
echo "gave up waiting for a free game $(date +%T)"
