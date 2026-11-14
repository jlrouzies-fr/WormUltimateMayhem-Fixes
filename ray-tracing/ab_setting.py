"""
Screenshot the running sandbox match with one wumrt.ini setting at two values, side by side.

    python ab_setting.py NAME SETTING VALUE_A VALUE_B [LABEL_A LABEL_B]

Writes captures/NAME_SETTING.png. The setting is left at VALUE_B. The match is not paused,
so the two views are a few seconds apart.
"""
import re
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw

import rt
from ab_capture import shot
from ab_label import font

HERE = Path(__file__).resolve().parent
INI = HERE.parent.parent / "rt-sandbox" / "wumrt.ini"
OUT = HERE / "captures"


def set_value(name, value):
    text = INI.read_text()
    new, n = re.subn(rf"(?m)^{re.escape(name)}\s*=.*$", f"{name} = {value}", text)
    INI.write_text(new if n else text.rstrip("\n") + f"\n{name} = {value}\n")
    time.sleep(3.0)             # the harness re-reads the file once a second; let the result settle


def main():
    name, setting, a, b = sys.argv[1:5]
    labels = sys.argv[5:7] if len(sys.argv) >= 7 else [f"{setting} = {a}", f"{setting} = {b}"]
    c = rt.attach()
    OUT.mkdir(exist_ok=True)
    views = []
    for value in (a, b):
        set_value(setting, value)
        tmp = OUT / f"{name}_{setting}_tmp.png"
        shot(c, tmp)
        views.append(Image.open(tmp).convert("RGB"))
        tmp.unlink()
    w, h = views[0].size
    bar, gap = max(20, h // 19), 8
    sheet = Image.new("RGB", (w * 2 + gap, h + bar), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    f = font(max(11, h // 36))
    for i, (view, label) in enumerate(zip(views, labels)):
        x = i * (w + gap)
        sheet.paste(view, (x, 0))
        draw.rectangle([x, h, x + w - 1, h + bar - 1], fill=(20, 20, 20))
        draw.text((x + w // 2, h + bar // 2), label, font=f, fill=(255, 255, 255), anchor="mm")
    sheet.save(OUT / f"{name}_{setting}.png")
    print(OUT / f"{name}_{setting}.png")


if __name__ == "__main__":
    main()
