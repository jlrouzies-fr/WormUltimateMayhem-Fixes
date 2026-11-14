"""
Find the terrain textures that should carry shell layers (grass, moss) and list them in
wumrt_surfaces.ini.

    python surfaces.py candidates [OUT_PREFIX]   contact sheets of the library textures (out/matlib)
                                                 whose colour is mostly green, with their hashes
    python surfaces.py terrain [OUT_PREFIX]      contact sheet of the textures the running (or last)
                                                 sandbox match drew terrain with, from wumrt.log
    python surfaces.py reload                    make the game read wumrt_surfaces.ini again

A texture already listed in wumrt_surfaces.ini is marked with its kind. Labelling is by eye:
look at the sheets, then add `HASH grass` or `HASH moss` lines to the file.
"""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageStat

from ab_label import font

HERE = Path(__file__).resolve().parent
LIB = HERE / "out" / "matlib"
INI = HERE / "wumrt_surfaces.ini"
LOG = HERE.parent.parent / "rt-sandbox" / "wumrt.log"
COLS, CELL = 8, 150


def listed():
    kinds = {}
    if INI.exists():
        for line in INI.read_text().splitlines():
            word = line.split("#")[0].split()
            if len(word) >= 2:
                kinds[word[0].lower()] = word[1]
    return kinds


def greenness(path):
    """How far green leads the other two channels, 0 = not at all."""
    r, g, b = ImageStat.Stat(Image.open(path).convert("RGB")).mean
    return (g - max(r, b)) / max(g, 1.0)


def sheets(keys, prefix, notes=None):
    kinds = listed()
    f = font(13)
    per = COLS * 6
    for page in range(0, len(keys), per):
        part = keys[page:page + per]
        sheet = Image.new("RGB", (COLS * CELL, ((len(part) + COLS - 1) // COLS) * (CELL + 20)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for n, key in enumerate(part):
            x, y = n % COLS * CELL, n // COLS * (CELL + 20)
            src = LIB / f"{key}.png"
            if src.exists():
                sheet.paste(Image.open(src).resize((128, 128), Image.LANCZOS), (x + 11, y + 4))
            draw.text((x + CELL // 2, y + 133), key, font=f, fill=(255, 255, 255), anchor="ma")
            note = kinds.get(key, notes.get(key, "") if notes else "")
            draw.text((x + CELL // 2, y + 149), note, font=f,
                      fill=(120, 255, 120) if key in kinds else (190, 190, 190), anchor="ma")
        name = f"{prefix}_{page // per + 1}.png"
        sheet.save(name)
        print(name)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "candidates":
        scored = sorted(((greenness(p), p.stem) for p in LIB.glob("*.png")), reverse=True)
        keys = [k for s, k in scored if s > 0.06]
        sheets(keys, a[1] if len(a) > 1 else str(HERE / "out" / "green"),
               {k: f"green {s:.2f}" for s, k in scored})
        print(f"{len(keys)} mostly green textures of {len(scored)}")
    elif a[0] == "terrain":
        keys = []
        for m in re.finditer(r"rt: terrain texture ([0-9a-f]{8})", LOG.read_text(errors="replace")):
            if m.group(1) not in keys:
                keys.append(m.group(1))
        sheets(keys, a[1] if len(a) > 1 else str(HERE / "out" / "terrain"))
        print(f"{len(keys)} terrain textures in this match: {' '.join(keys)}")
    elif a[0] == "reload":
        import rt
        rt.command(rt.attach(), 6, ".")
        print("reloaded")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
