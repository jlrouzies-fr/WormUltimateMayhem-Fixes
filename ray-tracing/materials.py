"""
Look at the material textures the running sandbox match uses, for labelling them.

    python materials.py sheet [OUT_PREFIX]     contact sheets OUT_PREFIX_1.png, ... and a list on stdout
    python materials.py reload                 make the game read wumrt_materials.ini again
    python materials.py collect                add the match's textures to the library in out/matlib
    python materials.py todo [OUT_PREFIX]      sheets of library textures not labelled or reviewed yet

Each texture is shown with the hash of its 128-pixel copy (the key used in
wumrt_materials.ini) and its current material: r = roughness, k = reflectivity, m = metal,
e = emissive; a '*' marks values that come from the file rather than from the game.

wumrt_materials.ini, one texture per line:

    HASH ROUGHNESS REFLECTIVITY METAL [EMISSIVE]   # note

roughness 0.05 (mirror) .. 1 (matte); reflectivity 0..1 scales how much a non-metal mirrors;
metal 1 makes it mirror in its own colour; emissive is how much light it gives off.
"""
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import rt
from ab_label import font

HERE = Path(__file__).resolve().parent
COLS, ROWS, CELL = 8, 6, 150


def layers(c):
    raw = HERE / "out" / "layers.wrtl"
    raw.parent.mkdir(exist_ok=True)
    rt.command(c, 12, raw)
    d = raw.read_bytes()
    raw.unlink()
    magic, count, edge = struct.unpack_from("<3I", d, 0)
    assert magic == 0x4C545257
    off, out = 12, []
    for i in range(count):
        h, rough, refl, metal, emis, flag = struct.unpack_from("<I4fI", d, off)
        off += 24
        img = Image.frombytes("RGBA", (edge, edge), d[off:off + edge * edge * 4]).transpose(Image.FLIP_TOP_BOTTOM)
        off += edge * edge * 4
        out.append((i, h, rough, refl, metal, emis, flag, img))
    return out


LIB = HERE / "out" / "matlib"       # every texture seen so far: HASH.png, and index.txt with the game's values


def labelled():
    done = set()
    for line in (HERE / "wumrt_materials.ini").read_text().splitlines():
        word = line.split("#")[0].split()
        if len(word) >= 4:
            done.add(word[0].lower())
    return done


def collect(c):
    """Add the running match's textures to the library."""
    LIB.mkdir(parents=True, exist_ok=True)
    index = LIB / "index.txt"
    known = {l.split()[0] for l in index.read_text().splitlines()} if index.exists() else set()
    new = 0
    with index.open("a") as f:
        for i, h, rough, refl, metal, emis, flag, img in layers(c):
            key = f"{h:08x}"
            if i < 2 or key in known:
                continue
            known.add(key)
            img.convert("RGB").save(LIB / f"{key}.png")
            f.write(f"{key} {rough:.2f} {refl:.2f} {emis:.2f}\n")
            new += 1
    print(f"{new} new textures, {len(known)} in the library")


def todo(prefix):
    """Contact sheets of the library's textures that wumrt_materials.ini does not list yet."""
    done = labelled()
    seen = LIB / "reviewed.txt"           # looked at and left as the game has them (matte)
    if seen.exists():
        done |= set(seen.read_text().split())
    rows = [l.split() for l in (LIB / "index.txt").read_text().splitlines()]
    rows = [r for r in rows if r[0] not in done]
    f = font(13)
    for page in range(0, len(rows), COLS * ROWS):
        part = rows[page:page + COLS * ROWS]
        sheet = Image.new("RGB", (COLS * CELL, ((len(part) + COLS - 1) // COLS) * (CELL + 20)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for n, (key, rough, refl, emis) in enumerate(part):
            x, y = n % COLS * CELL, n // COLS * (CELL + 20)
            sheet.paste(Image.open(LIB / f"{key}.png").resize((128, 128), Image.LANCZOS), (x + 11, y + 4))
            draw.text((x + CELL // 2, y + 133), key, font=f, fill=(255, 255, 255), anchor="ma")
            draw.text((x + CELL // 2, y + 149), f"game: r{rough} k{refl} e{emis}", font=f, fill=(190, 190, 190), anchor="ma")
        name = f"{prefix}_{page // (COLS * ROWS) + 1}.png"
        sheet.save(name)
        print(name)
    print(f"{len(rows)} textures not labelled yet")


def main():
    a = sys.argv[1:]
    if a and a[0] == "todo":
        todo(a[1] if len(a) > 1 else str(HERE / "out" / "todo"))
        return
    c = rt.attach()
    if a and a[0] == "collect":
        collect(c)
        return
    if a and a[0] == "reload":
        rt.command(c, 6, ".")
        print("reloaded")
        return
    prefix = Path(a[1]) if len(a) > 1 else HERE / "out" / "materials"
    ls = [x for x in layers(c) if x[0] >= 2]            # layers 0 and 1 are plain grey and white
    f = font(13)
    for page in range(0, len(ls), COLS * ROWS):
        part = ls[page:page + COLS * ROWS]
        rows = (len(part) + COLS - 1) // COLS
        sheet = Image.new("RGB", (COLS * CELL, rows * (CELL + 20)), (40, 40, 40))
        draw = ImageDraw.Draw(sheet)
        for n, (i, h, rough, refl, metal, emis, flag, img) in enumerate(part):
            x, y = n % COLS * CELL, n // COLS * (CELL + 20)
            sheet.paste(img.convert("RGB").resize((128, 128), Image.LANCZOS), (x + 11, y + 4))
            draw.text((x + CELL // 2, y + 133), f"{h:08x}", font=f, fill=(255, 255, 255), anchor="ma")
            draw.text((x + CELL // 2, y + 149), f"r{rough:.2f} k{refl:.2f} m{metal:.0f} e{emis:.1f}{'*' if flag else ''}",
                      font=f, fill=(190, 190, 190), anchor="ma")
        name = f"{prefix}_{page // (COLS * ROWS) + 1}.png"
        sheet.save(name)
        print(name)
    for i, h, rough, refl, metal, emis, flag, img in ls:
        print(f"{h:08x} {rough:.2f} {refl:.2f} {metal:.0f} {emis:.2f}{'   # from file' if flag else ''}")


if __name__ == "__main__":
    main()
