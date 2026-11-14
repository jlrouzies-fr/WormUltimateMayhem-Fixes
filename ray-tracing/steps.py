"""
Measure how much work the tracer does per pixel, independent of GPU load: the number of
BVH nodes visited plus triangles tested ("steps"), for the camera + sun rays and for the
sky + bounce rays.

    python steps.py [VARIANT.comp ...]

With no arguments it measures wumrt_trace.comp; each extra file is measured too, for
comparing shader variants on the same (paused) view. Run it with the sandbox game paused.
"""
import struct
import sys
import time
from pathlib import Path

import numpy as np

import rt

HERE = Path(__file__).resolve().parent
LIVE = HERE.parent.parent / "rt-sandbox" / "wumrt_trace.comp"


def instrument(src):
    def rep(s, old, new, count=1):
        assert s.count(old) == count, (old[:50], s.count(old))
        return s.replace(old, new)
    s = src
    s = rep(s, "const float T_MIN = 1e-3;\n", "const float T_MIN = 1e-3;\nuint gSteps = 0u, gStepsSun = 0u;\n")
    s = rep(s, "        Node nd = nodes[nodeBase + ni];\n", "        Node nd = nodes[nodeBase + ni];\n        gSteps++;\n")
    s = rep(s, "        Tri t = tris[first + k];\n", "        Tri t = tris[first + k];\n        gSteps++;\n")
    s = rep(s, "        Node nd = tlas[ni];\n", "        Node nd = tlas[ni];\n        gSteps++;\n")
    s = rep(s, "        int skyRays = int(uSkyParams.x);\n", "        gStepsSun = gSteps;\n        int skyRays = int(uSkyParams.x);\n")
    s = rep(s, "    imageStore(uMask, pix, vec4(vis, dist, nOut));",
            "    if (gStepsSun == 0u) gStepsSun = gSteps;\n"
            "    imageStore(uMask, pix, vec4(float(gStepsSun) / 16.0, float(gSteps - gStepsSun) / 16.0, nOut));")
    return s


def measure(c, src):
    LIVE.write_text(instrument(src), encoding="utf-8", newline="\n")
    rt.command(c, 6, ".")
    time.sleep(1.0)
    raw = HERE / "out" / "steps.wrtf"
    c.arg[0] = 0x100
    rt.command(c, 7, raw)
    d = raw.read_bytes()
    _, w, h = struct.unpack_from("<3I", d, 0)
    m = np.frombuffer(d, dtype="<f4", offset=12).reshape(h, w, 2) * 16.0
    raw.unlink()
    return float(m[..., 0].mean()), float(m[..., 1].mean())


def main():
    c = rt.attach()
    base = (HERE / "wumrt_trace.comp").read_text(encoding="utf-8")
    files = [HERE / "wumrt_trace.comp"] + [Path(a) for a in sys.argv[1:]]
    try:
        for f in files:
            sun, sky = measure(c, f.read_text(encoding="utf-8"))
            print(f"{f.name:28s} steps per pixel: camera + sun rays {sun:7.1f}, sky + bounce rays {sky:7.1f}, total {sun + sky:7.1f}")
    finally:
        LIVE.write_text(base, encoding="utf-8", newline="\n")
        rt.command(c, 6, ".")


if __name__ == "__main__":
    main()
