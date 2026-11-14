#!/bin/sh
# Collect the textures of as many levels as possible and make replacements for all of them.
#
#   sh process_levels.sh [MAX_MATCHES]      (default 40)
#
# Quick Game picks a level at random, so this starts match after match in the sandbox (at its
# usual low resolution), dumps each one's textures, and stops when four matches in a row
# bring fewer than three new textures, or after MAX_MATCHES. Then it makes an AI normal map
# and a 4x upscale for every texture that has none yet and installs them in the sandbox.
# Progress goes to out/process_levels.log.
cd "$(dirname "$0")"
MAX=${1:-40}
SB=../../rt-sandbox
AIPY="${AIPY:-D:/AI-Upscale/venv/Scripts/python.exe}"
ESRGAN="${ESRGAN:-D:/AI-Upscale/bin/realesrgan-ncnn-vulkan/realesrgan-ncnn-vulkan.exe}"
LOG=out/process_levels.log
mkdir -p out/texdump out/ai_normals
: > $LOG

# The game must show its own textures while they are dumped, not earlier replacements.
py -3 sandbox.py stop >/dev/null 2>&1
[ -d $SB/wumrt_textures ] && mv $SB/wumrt_textures $SB/wumrt_textures_off

n=0; quiet=0
while [ $n -lt $MAX ] && [ $quiet -lt 4 ]; do
    n=$((n + 1))
    before=$(ls out/texdump | grep -c '^[0-9a-f]\{8\}\.png$')
    sh go.sh >/dev/null 2>&1
    sleep 7
    py -3 textures.py dump >/dev/null 2>&1
    py -3 materials.py collect >/dev/null 2>&1
    after=$(ls out/texdump | grep -c '^[0-9a-f]\{8\}\.png$')
    echo "match $n: $((after - before)) new textures, $after in all" >> $LOG
    if [ $((after - before)) -lt 3 ]; then quiet=$((quiet + 1)); else quiet=0; fi
done
py -3 sandbox.py stop >/dev/null 2>&1
mkdir -p $SB/wumrt_textures
[ -d $SB/wumrt_textures_off ] && cp -n $SB/wumrt_textures_off/* $SB/wumrt_textures/ && rm -rf $SB/wumrt_textures_off

echo "normal maps..." >> $LOG
"$AIPY" -I ai_maps.py models/pbrify_normal/1x-PBRify_NormalV3.pth n --flip-green >> $LOG 2>&1
cp out/ai_maps/*_n.png out/ai_normals/
py -3 textures.py import out/ai_normals >> $LOG 2>&1
echo "upscaling..." >> $LOG
py -3 textures.py upscale "$ESRGAN" 2>&1 | grep -c ' -> ' | sed 's/$/ textures upscaled/' >> $LOG
echo "done: $(ls $SB/wumrt_textures | grep -c '_n.wtx') normal maps, $(ls $SB/wumrt_textures | grep -c '_c.wtx') colour textures installed" >> $LOG
