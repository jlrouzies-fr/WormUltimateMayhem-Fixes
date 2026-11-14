"""
Measure the noise in the traced sky visibility: mean absolute difference between each pixel
and the average of its four neighbours, over pixels that hit geometry. Lower is smoother.

    python noise.py [LABEL]
"""
import struct
import sys
from pathlib import Path

import numpy as np

import rt

HERE = Path(__file__).resolve().parent


def grab(c, flags):
    raw = HERE / "out" / "noise.wrtf"
    c.arg[0] = flags
    rt.command(c, 7, raw)
    d = raw.read_bytes()
    _, w, h = struct.unpack_from("<3I", d, 0)
    raw.unlink()
    return np.frombuffer(d, dtype="<f4", offset=12).reshape(h, w, 2)


def main():
    c = rt.attach()
    m = grab(c, 0)              # sun visibility, distance
    s = grab(c, 0x200)          # sky visibility, -
    hit = m[..., 1] < 59999
    sky = s[..., 0]
    lap = np.abs(sky[1:-1, 1:-1] - 0.25 * (sky[:-2, 1:-1] + sky[2:, 1:-1] + sky[1:-1, :-2] + sky[1:-1, 2:]))
    inner = hit[1:-1, 1:-1] & hit[:-2, 1:-1] & hit[2:, 1:-1] & hit[1:-1, :-2] & hit[1:-1, 2:]
    label = sys.argv[1] if len(sys.argv) > 1 else ""
    print(f"{label:22s} sky visibility: mean {sky[hit].mean():.3f}, pixel-to-neighbour noise {lap[inner].mean() * 100:.3f}% "
          f"({inner.mean() * 100:.0f}% of the image measured)")


if __name__ == "__main__":
    main()
