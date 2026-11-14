#!/bin/sh
# Atmospheric depth A/B on two levels: start the sandbox (Apple Core), pause, shots with atmos
# off / on / stronger / shafts; change level (LEVEL2), the same; stop. nvlddmkm checked after
# the start and the stop.   sh atmos_test.sh NAME [LEVEL2]   -> out/NAME_<level>_*.png
cd "$(dirname "$0")"
NAME=${1:-at}
L2=${2:-}
INI=../../rt-sandbox/wumrt.ini
setini() {
  if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi
}
gpucheck() {
  powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'; StartTime=[datetime]'2026-11-14 13:27:10'} -ErrorAction SilentlyContinue | ForEach-Object { 'NVLDDMKM EVENT ' + \$_.TimeCreated + ' id ' + \$_.Id }"
  echo "gpucheck $(date +%T)"
}
shots() {   # $1 = label
  py -3 rt.py key esc; sleep 1.5
  py -3 rt.py freecam take >/dev/null; py -3 rt.py freecam move 0 ${UP:-0} ${FWD:-0} >/dev/null; py -3 rt.py freecam turn ${TURN:-0} ${PITCH:-8} | tail -1; sleep 1.5
  setini atmos 0; setini atmos_shafts 0; sleep 2; py -3 rt.py shot "out/${NAME}_$1_off.png"
  setini atmos 1; sleep 2.5; py -3 rt.py shot "out/${NAME}_$1_on.png"
  setini atmos_strength 0.7; sleep 2; py -3 rt.py shot "out/${NAME}_$1_s07.png"
  setini atmos_strength 0.5; setini atmos_shafts 1; sleep 2; py -3 rt.py shot "out/${NAME}_$1_shafts.png"
  setini atmos_shafts 0
  py -3 rt.py rtstat 1 2>&1 | grep -E "^mask|cost per frame"
  py -3 rt.py freecam off >/dev/null
  py -3 rt.py key esc; sleep 1
}
setini atmos 1; setini atmos_log 1; setini atmos_shafts 0; setini atmos_strength 0.5
LEVEL="${LEVEL1-Apple Core}" sh go.sh || { py -3 sandbox.py stop; gpucheck; exit 1; }
echo "started $(date +%T)"; gpucheck
sleep 3
shots apple
if [ -n "$L2" ]; then
  py -3 rt.py level "$L2" 2>&1 | tail -2; sleep 12
  shots l2
fi
grep "atmos:" ../../rt-sandbox/wumrt.log | tail -6
py -3 sandbox.py stop >/dev/null 2>&1; echo "stopped $(date +%T)"; gpucheck
