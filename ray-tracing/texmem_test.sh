#!/bin/sh
# One short batch measuring the sandbox game's 32-bit memory with replacement textures
# (README "Textures"). Three starts on Apple Core Island, each stopped again:
#   1. 1024 RGBA8 as installed, no large-address flag          (baseline)
#   2. tex_compress = 1 (colour maps S3TC: texpack.py's .wtc files, else compressed by the driver)
#   3. tex_compress = 2, large-address flag on, terrain at 2048 (out/wtx_2k_test: plain resizes
#      of the installed 1024 maps, made only to measure memory)
# Afterwards the 1024 terrain maps, the flag (on: tested) and tex_compress = 2 stay as the test
# leaves them unless it failed. Results: out/texmem.log. Refuses to start if any game runs.
set -u
cd "$(dirname "$0")"
SB=../../rt-sandbox
INI=$SB/wumrt.ini
TX=$SB/wumrt_textures
BK=out/wtx_1k_backup
WAIT=${WAIT:-25}

setini() {
  if grep -q "^$1 *=" "$INI"; then sed -i "s/^$1 *=.*/$1 = $2/" "$INI"; else echo "$1 = $2" >> "$INI"; fi
}

one() {
  label=$1
  : > "$SB/wumrt.log" 2>/dev/null
  out=$(LEVEL="Apple Core" sh go.sh 2>&1)
  echo "$out" | grep -E "no pop-up|POPPED|BUSY|refus|error|Error" | head -3
  if echo "$out" | grep -q "BUSY\|POPPED"; then py -3 sandbox.py stop >/dev/null 2>&1; echo "not started ($label)"; return 3; fi
  sleep "$WAIT"
  py -3 rt.py screen 2>&1 | head -1
  py -3 texmem.py mem "$label"
  py -3 rt.py rtstat 2 2>&1 | head -2
  sleep 15
  py -3 texmem.py mem "$label +15s"
  py -3 rt.py shot "out/texmem_$(echo "$label" | tr -c 'A-Za-z0-9' '_').png" >/dev/null 2>&1
  grep -E "replacement textures loaded|out of memory|failed|reason 14" "$SB/wumrt.log" | tail -3
  py -3 sandbox.py stop >/dev/null 2>&1
  return 0
}

mkdir -p "$BK"
setini tex_max 0
py -3 texmem.py laa off
if [ -z "${SKIP_BASE:-}" ]; then              # SKIP_BASE=1: baseline already measured
  setini tex_compress 0
  one "1K RGBA8, LAA off" || exit $?
fi
setini tex_compress 1
one "1K compress 1, LAA off" || exit $?
for f in out/wtx_2k_test/*_c.wtx; do b=$(basename "$f"); [ -f "$BK/$b" ] || cp "$TX/$b" "$BK/$b"; cp "$f" "$TX/$b"; done
cp out/wtx_2k_test/*_c.wtc "$TX/" 2>/dev/null     # packed versions (texpack.py), copied after the .wtx
py -3 texmem.py laa on
setini tex_compress 2
one "terrain 2K, compress 2, LAA on"; r=$?
cp "$BK"/*_c.wtx "$TX/"
py -3 texpack.py "$TX" --normals | tail -1        # re-pack the restored 1024 maps (their .wtc are now older)
echo "1024 terrain maps restored; exit $r"
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'} -MaxEvents 1 | Format-List TimeCreated,Id" | grep -v "^$"
exit $r
