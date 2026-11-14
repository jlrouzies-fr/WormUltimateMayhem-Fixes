#!/bin/sh
# Atmos as the player sees it (game camera) and sun shafts (free camera turned towards the sun)
# on one level. sh shafts_test.sh NAME   (LEVEL1="" = a random Quick Game level)
#   -> out/NAME_game_{off,on}.png, out/NAME_sun_{s0,s1,s2}.png (+ _sunN for more sun views)
cd "$(dirname "$0")"
NAME=${1:-sh}
INI=../../rt-sandbox/wumrt.ini
setini() {
  if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi
}
gpucheck() {
  powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'; StartTime=[datetime]'2026-11-14 13:27:10'} -ErrorAction SilentlyContinue | ForEach-Object { 'NVLDDMKM EVENT ' + \$_.TimeCreated + ' id ' + \$_.Id }"
  echo "gpucheck $(date +%T)"
}
setini atmos 1; setini atmos_log 1; setini atmos_shafts 0
LEVEL="${LEVEL1-Apple Core}" sh go.sh || { py -3 sandbox.py stop; gpucheck; exit 1; }
echo "started $(date +%T)"; gpucheck
sleep 4
# 1. the game's own camera, unpaused (no pause box), atmos off / on
setini atmos 0; sleep 2; py -3 rt.py shot "out/${NAME}_game_off.png"
setini atmos 1; sleep 2; py -3 rt.py shot "out/${NAME}_game_on.png"
# 2. paused, free camera towards the sun, shafts 0 / 1 / 2, for a few sun placements
py -3 rt.py key esc; sleep 1.5
i=0
for v in "${V1:-22 12}" "${V2:--25 6}" "${V3:-30 20}"; do
  i=$((i+1))
  py -3 sun_view.py $v | head -1; sleep 1.5
  setini atmos_shafts 0; sleep 1.5; py -3 rt.py shot "out/${NAME}_sun${i}_s0.png"
  setini atmos_shafts 1; sleep 1.5; py -3 rt.py shot "out/${NAME}_sun${i}_s1.png"
  setini atmos_shafts 2; sleep 1.5; py -3 rt.py shot "out/${NAME}_sun${i}_s2.png"
done
setini atmos_shafts 0
py -3 rt.py freecam off >/dev/null
sleep 8
grep "atmos:" ../../rt-sandbox/wumrt.log | tail -2
py -3 sandbox.py stop >/dev/null 2>&1; echo "stopped $(date +%T)"; gpucheck
