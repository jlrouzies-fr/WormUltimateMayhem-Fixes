"""
Take before/after screenshots in the running sandbox match: the stock game, traced sun
shadows only, and the full set (sun shadows, sky occlusion, bounced light).

    python ab_capture.py NAME [DX DY]

Optionally turns the camera by DX, DY mouse pixels first. Writes captures/NAME_1_stock.png,
NAME_2_sun.png, NAME_3_full.png and NAME_stock_vs_full.png (side by side).
"""
import sys
import time
from pathlib import Path

import numpy as np

import rt
from trace_capture import read_png, write_png

HERE = Path(__file__).resolve().parent
OUT = HERE / "captures"


def shot(c, path):
    raw = path.with_suffix(".raw")
    rt.command(c, 1, raw)
    rt.png_from_raw(raw, path)
    raw.unlink()


def tracer(c, on, samples=8, radius=1.5, filt=2):
    c.arg[0], c.arg[1], c.arg[2], c.arg[3] = int(on), samples, int(radius * 1000), filt + 1
    rt.command(c, 5, ".")


def sky(c, rays, reach=80, strength=0.85, bounce=0.8):
    c.arg[0], c.arg[1], c.arg[2], c.arg[3] = rays, reach, int(strength * 1000), int(bounce * 1000)
    rt.command(c, 11, ".")


def main():
    name = sys.argv[1]
    c = rt.attach()
    if len(sys.argv) > 3:
        import subprocess
        subprocess.run([sys.executable, str(HERE / "rt.py"), "mouse", sys.argv[2], sys.argv[3]])
        time.sleep(1.2)
    OUT.mkdir(exist_ok=True)
    a, b, f = OUT / f"{name}_1_stock.png", OUT / f"{name}_2_sun.png", OUT / f"{name}_3_full.png"
    tracer(c, 0)
    time.sleep(0.5)
    shot(c, a)
    tracer(c, 1)
    sky(c, 0)
    time.sleep(0.5)
    shot(c, b)
    sky(c, 2)
    time.sleep(1.5)                 # let the accumulation settle
    shot(c, f)
    A, F = read_png(a), read_png(f)
    gap = np.full((A.shape[0], 8, 3), 255, np.uint8)
    write_png(OUT / f"{name}_stock_vs_full.png", np.ascontiguousarray(np.concatenate([A, gap, F], axis=1)))
    print(f"{name}: {A.shape[1]}x{A.shape[0]}")


if __name__ == "__main__":
    main()
