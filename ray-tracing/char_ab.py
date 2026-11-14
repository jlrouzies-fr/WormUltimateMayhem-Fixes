"""
Before/after of the character lighting on the current (paused, free camera) view.

    python char_ab.py NAME [X0 Y0 X1 Y1] [--only KEY[,KEY...]]

Shoots the view with the character settings (skin_wrap, skin_translucency, rim_light,
catchlights, contact_shadow) at 0, then as they are in the sandbox's wumrt.ini; with --only KEY
only that one is switched off for the first shot. Writes captures/char_NAME_off.png,
captures/char_NAME_on.png and captures/char_NAME_pair.png (the two side by side, labelled, cropped
to X0 Y0 X1 Y1 of the frame if given, enlarged to at least 900 pixels wide).
"""
import re
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw

from ab_label import font

HERE = Path(__file__).resolve().parent
INI = HERE / ".." / ".." / "rt-sandbox" / "wumrt.ini"
KEYS = ["skin_wrap", "skin_translucency", "rim_light", "catchlights", "contact_shadow"]


def rt(*args):
    return subprocess.run([sys.executable, str(HERE / "rt.py"), *map(str, args)], capture_output=True, text=True).stdout


def shot(path):
    time.sleep(2.5)        # the ini is polled once a second; let the history settle
    rt("shot", path)


def main():
    a = sys.argv[1:]
    only = None
    if "--only" in a:
        i = a.index("--only")
        only = a[i + 1]
        del a[i:i + 2]
    name = a[0]
    box = tuple(int(v) for v in a[1:5]) if len(a) >= 5 else None
    keys = only.split(",") if only else KEYS     # --only A,B: several
    text = INI.read_text()
    off = text
    for k in keys:
        off = re.sub(r"(?m)^%s\s*=.*$" % k, "%s = 0" % k, off)
    cap = HERE / "captures"
    cap.mkdir(exist_ok=True)
    try:
        INI.write_text(off)
        shot(cap / f"char_{name}_off.png")
    finally:
        INI.write_text(text)
    shot(cap / f"char_{name}_on.png")
    ims = [Image.open(cap / f"char_{name}_{s}.png").convert("RGB") for s in ("off", "on")]
    if box:
        ims = [im.crop(box) for im in ims]
    w, h = ims[0].size
    k = max(1, 900 // (2 * w) + (1 if 900 % (2 * w) else 0)) if 2 * w < 900 else 1
    ims = [im.resize((w * k, h * k), Image.LANCZOS) for im in ims]
    w, h = ims[0].size
    out = Image.new("RGB", (2 * w + 8, h + 34), (24, 24, 24))
    d = ImageDraw.Draw(out)
    f = font(20)
    for i, (im, label) in enumerate(zip(ims, ("before (character lighting off)" if not only else f"{only} = 0",
                                              "after: " + ", ".join(f"{kk} {v}" for kk, v in re.findall(
                                                  r"(?m)^(%s)\s*=\s*(\S+)" % "|".join(keys), text))))):
        out.paste(im, (i * (w + 8), 34))
        d.text((i * (w + 8) + 6, 6), label, fill=(240, 240, 240), font=f)
    out.save(cap / f"char_{name}_pair.png")
    print(cap / f"char_{name}_pair.png")


if __name__ == "__main__":
    main()
