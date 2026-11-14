#!/bin/sh
# After film_textures.py has written the colour set (out/tex_mid, 2K ones in out/tex_mid/2k):
# normal maps (PBRify, 512) and model heights from the NEW colours, then install everything into
# a texture folder and pack it. GPU models here are small (PBRify, 1x); run this only when no
# SDXL run is going.
#     sh texset_finish.sh [FOLDER]      FOLDER defaults to this copy's sandbox wumrt_textures
set -e
cd "$(dirname "$0")"
DST=${1:-../../rt-sandbox/wumrt_textures}
PY=D:/AI-Upscale/venv/Scripts/python.exe
N=$(ls out/tex_mid/????????.png | wc -l)
echo "colour set: $N textures, $(ls out/tex_mid/2k/*.png 2>/dev/null | wc -l) at 2048"
# normal maps from the new colours (green flipped, as the installed ones), heights from the height model
$PY -I ai_maps.py models/pbrify_normal/1x-PBRify_NormalV3.pth n --gpu --flip-green --src out/tex_mid --out out/tex_mid_maps --size 512 | tail -1
$PY -I ai_maps.py models/pbrify_height/1x-PBRify_Height.pth h --gpu --src out/tex_mid --out out/ai_maps --size 512 | tail -1
# into the folder: colour (2048 where made), normal maps, heights into their alpha, packed copies
D:/AI-Upscale/venv-restyle/Scripts/python.exe -I film_textures.py install out/tex_mid --to "$DST"
py -3 - "$DST" <<'EOF'
import struct, sys
from pathlib import Path
from PIL import Image
dst = Path(sys.argv[1]); n = 0
for p in sorted(Path("out/tex_mid_maps").glob("????????_n.png")):
    img = Image.open(p).convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM)
    (dst / f"{p.stem}.wtx").write_bytes(struct.pack("<3I", 0x31585457, img.width, img.height) + img.tobytes())
    n += 1
print(n, "normal maps written to", dst)
EOF
py -3 relief.py model "$DST" | tail -1
py -3 texpack.py "$DST" --normals | tail -1
echo "done: $DST"
