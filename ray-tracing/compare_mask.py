"""
Check the real-time (GPU) tracer against the offline CPU reference on the same frame.

    python compare_mask.py OUT_PREFIX

Captures one frame from the running sandbox game, dumps the GPU mask of the frame after
it, traces the capture offline with the same rules (camera rays against the shadow
casters, one hard sun ray) and reports how the two masks differ. Keep the camera still.
"""
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

import rt
import trace_capture

HERE = Path(__file__).resolve().parent


def main():
    prefix = sys.argv[1]
    c = rt.attach()
    cap, raw = Path(prefix + ".bin"), Path(prefix + "_gpu.wrtf")
    rt.command(c, 2, cap, timeout=60)
    c.arg[0] = 0
    rt.command(c, 7, raw)
    r = subprocess.run([sys.executable, str(HERE / "trace_capture.py"), str(cap), prefix + "_ref",
                        "--samples", "1", "--radius", "0", "--primary", "casters"], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)
    d = raw.read_bytes()
    magic, w, h = struct.unpack_from("<3I", d, 0)
    gpu = np.frombuffer(d, dtype="<f4", offset=12).reshape(h, w, 2)[::-1]      # to top-down
    ref = np.fromfile(HERE / "build" / "trace.bin",
                      dtype=[("t", "<f4"), ("ndotl", "<f4"), ("vis", "<f4"), ("tri", "<u4")]).reshape(h, w)
    g_hit, r_hit = gpu[..., 1] < 59999, ref["t"] > 0
    both = g_hit & r_hit
    print(f"{w}x{h}: camera rays hitting a caster: GPU {g_hit.mean() * 100:.2f}%, reference {r_hit.mean() * 100:.2f}%, "
          f"disagree on {np.mean(g_hit != r_hit) * 100:.3f}% of pixels")
    rel = np.abs(gpu[..., 1][both] - ref["t"][both]) / ref["t"][both]
    print(f"distance where both hit: median relative difference {np.median(rel) * 100:.4f}%, "
          f"pixels off by more than 1%: {np.mean(rel > 0.01) * 100:.3f}%")
    same_surface = both.copy()
    same_surface[both] = rel < 0.01
    dv = np.abs(gpu[..., 0] - ref["vis"])[same_surface]
    print(f"sun visibility on the same surface: differs on {np.mean(dv > 0.5) * 100:.3f}% of pixels "
          f"(reference in shadow: {np.mean(ref['vis'][same_surface] < 0.5) * 100:.1f}%)")
    diff = np.zeros((h, w, 3), np.uint8)
    diff[..., 0] = np.where(same_surface, np.abs(gpu[..., 0] - ref["vis"]) * 255, 0)
    diff[..., 1] = np.where(same_surface, gpu[..., 0] * 120, 0)
    diff[..., 2] = np.where(g_hit != r_hit, 255, 0)
    trace_capture.write_png(prefix + "_diff.png", diff)
    print(f"{prefix}_diff.png: red = visibility differs, blue = hit/miss differs, green = lit (GPU)")


if __name__ == "__main__":
    main()
