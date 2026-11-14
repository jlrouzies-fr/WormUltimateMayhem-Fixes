"""
Measure how noisy and how steady the traced indirect light is, on a paused view (the camera
and scene hold still, so any change between frames is noise the denoiser let through).

    python stability.py [LABEL]

Prints, for the red and green channels of the indirect light texture:
  spatial noise   mean difference between a pixel and the average of its four neighbours
  flicker         mean difference between the same pixel in two frames a moment apart
both as a percentage of the mean light level. Lower is better.
"""
import struct
import sys
import time
from pathlib import Path

import numpy as np

import rt

HERE = Path(__file__).resolve().parent


def grab(c, flags):
    raw = HERE / "out" / "stability.wrtf"
    c.arg[0] = flags
    rt.command(c, 7, raw)
    d = raw.read_bytes()
    _, w, h = struct.unpack_from("<3I", d, 0)
    raw.unlink()
    return np.frombuffer(d, dtype="<f4", offset=12).reshape(h, w, 2).astype(np.float64)


def main():
    c = rt.attach()
    hit = grab(c, 0)[..., 1] < 59999
    frames = []
    for _ in range(4):
        frames.append(grab(c, 0x400))           # indirect light, red and green
        time.sleep(0.25)
    a = frames[0]
    level = a[hit].mean()
    lap = np.abs(a[1:-1, 1:-1] - 0.25 * (a[:-2, 1:-1] + a[2:, 1:-1] + a[1:-1, :-2] + a[1:-1, 2:])).mean(axis=2)
    inner = hit[1:-1, 1:-1] & hit[:-2, 1:-1] & hit[2:, 1:-1] & hit[1:-1, :-2] & hit[1:-1, 2:]
    flick = np.mean([np.abs(frames[i + 1] - frames[i]).mean(axis=2)[hit].mean() for i in range(3)])
    label = sys.argv[1] if len(sys.argv) > 1 else ""
    print(f"{label:24s} light level {level:.3f}, spatial noise {lap[inner].mean() / level * 100:.2f}%, "
          f"flicker {flick / level * 100:.2f}%")


if __name__ == "__main__":
    main()
