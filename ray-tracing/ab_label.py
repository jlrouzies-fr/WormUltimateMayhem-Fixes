"""
Build labelled comparison images from the screenshots ab_capture.py took.

    python ab_label.py [NAME ...]

For every capture set in captures/ (or only the named ones) writes
NAME_stock_vs_full.png (two views) and NAME_three_way.png (stock, sun shadows only, full),
each view with a caption bar underneath so nothing in the picture is covered.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).resolve().parent / "captures"
GAP = 8
LABELS = {
    "1_stock": "Stock game",
    "2_sun": "Ray-traced sun shadows",
    "3_full": "Ray-traced: sun shadows, sky and bounced light, reflections",
}


def font(size):
    for name in ("segoeuib.ttf", "arialbd.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def strip(name, parts):
    views = [Image.open(OUT / f"{name}_{p}.png").convert("RGB") for p in parts]
    w, h = views[0].size
    BAR = max(20, h // 19)              # caption bar and text scale with the picture
    sheet = Image.new("RGB", (w * len(views) + GAP * (len(views) - 1), h + BAR), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    f = font(max(11, h // 36))
    for i, (view, part) in enumerate(zip(views, parts)):
        x = i * (w + GAP)
        sheet.paste(view, (x, 0))
        draw.rectangle([x, h, x + w - 1, h + BAR - 1], fill=(20, 20, 20))
        draw.text((x + w // 2, h + BAR // 2), LABELS[part], font=f, fill=(255, 255, 255), anchor="mm")
    return sheet


def main():
    names = sys.argv[1:] or sorted(p.name[:-len("_1_stock.png")] for p in OUT.glob("*_1_stock.png"))
    for name in names:
        strip(name, ["1_stock", "3_full"]).save(OUT / f"{name}_stock_vs_full.png")
        strip(name, ["1_stock", "2_sun", "3_full"]).save(OUT / f"{name}_three_way.png")
        print(name)


if __name__ == "__main__":
    main()
