"""
Ray trace sun shadows offline from a captured frame, to check that the capture holds
everything a real-time tracer needs (world-space geometry, camera, sun direction).

    python trace_capture.py CAPTURE.bin OUT_PREFIX [--screenshot SHOT.png] [--samples 16] [--radius 0.5]

Writes OUT_PREFIX_shaded.png (flat-shaded scene with traced shadows), OUT_PREFIX_mask.png
(shadow visibility only) and, with --screenshot, OUT_PREFIX_composite.png (the game's
own frame darkened by the traced shadows).
"""
import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import numpy as np

import capfile

HERE = Path(__file__).resolve().parent
RTCPU = HERE / "build" / "rtcpu.exe"

SCENE_FP = {"LandscapeFragmentMain": 1, "HeightMapFragmentMain": 2, "FFFragmentMainTexLit": 3,
            "FFFragmentMainTexLitCol": 3, "FFFragmentMainLit": 3, "FFFragmentMainLitCol": 3,
            "WaterFragmentMain": 4}
CLASS_COLOR = {1: (0.80, 0.72, 0.50), 2: (0.70, 0.62, 0.45), 3: (0.85, 0.55, 0.45), 4: (0.25, 0.45, 0.75)}


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\0" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def read_png(path):
    """Minimal reader for the 8-bit RGB, non-interlaced PNGs that rt.py writes."""
    d = Path(path).read_bytes()
    off, idat, w, h = 8, b"", 0, 0
    while off < len(d):
        n, tag = struct.unpack_from(">I4s", d, off)
        body = d[off + 8:off + 8 + n]
        if tag == b"IHDR":
            w, h = struct.unpack_from(">II", body)
        elif tag == b"IDAT":
            idat += body
        off += 12 + n
    raw = np.frombuffer(zlib.decompress(idat), dtype=np.uint8).reshape(h, 1 + w * 3)
    assert not raw[:, 0].any(), "filtered PNG rows are not supported"
    return raw[:, 1:].reshape(h, w, 3)


def world_tris(dr):
    t = capfile.triangles(dr)
    if t is None or not len(t):
        return None
    p = np.concatenate([dr.pos[:, :3], np.ones((dr.nverts, 1), np.float32)], axis=1) @ dr.model
    return (p[:, :3] / p[:, 3:4])[t].reshape(-1, 9)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("prefix")
    ap.add_argument("--screenshot")
    ap.add_argument("--samples", type=int, default=16)
    ap.add_argument("--mask-out", help="write the visibility mask for rt.py mask")
    ap.add_argument("--primary", choices=["scene", "casters"], default="scene",
                    help="what camera rays see: the main pass geometry, or (like the real-time tracer) the shadow casters")
    ap.add_argument("--radius", type=float, default=0.5, help="sun radius in degrees (real sun: 0.27)")
    args = ap.parse_args()

    progs, draws = capfile.load(args.capture)
    scene_fbo = max((d for d in draws if d.fp and d.fp.entry == "LandscapeFragmentMain"), key=lambda d: d.count)
    cam = scene_fbo
    w, h = cam.viewport[2], cam.viewport[3]
    # globalLightDir is an eye-space vector towards the sun (the shader dots it with the
    # eye-space normal); rotate it back into world space with the view matrix.
    light = cam.light.astype(np.float64) @ cam.view[:3, :3].astype(np.float64).T
    light /= np.linalg.norm(light)
    shadow_fbo = next((d.fbo for d in draws if d.vp and d.vp.entry.startswith("Lighting")), None)
    print(f"scene pass: fbo {cam.fbo} {w}x{h}; shadow pass: fbo {shadow_fbo}")
    print("globalLightDir (eye space):", cam.light, "-> towards the sun in world space:", light)

    blocks, flags, stats = [], [], {}
    for d in draws:
        cls = None
        if (args.primary == "scene" and d.fbo == cam.fbo and d.fp and d.fp.entry in SCENE_FP and d.depth_test
                and d.depth_mask and not d.blend):
            cls, bit = SCENE_FP[d.fp.entry], 1
        elif d.fbo == shadow_fbo and d.vp and d.vp.entry != "WaterVertexMainLighting":
            cls, bit = (1, 3) if args.primary == "casters" else (0, 2)
        if cls is None:
            continue
        t = world_tris(d)
        if t is None:
            continue
        blocks.append(t)
        flags.append(np.full(len(t), bit | cls << 8, np.uint32))
        key = ("camera" if bit == 1 else "caster", d.vp.entry)
        stats[key] = stats.get(key, 0) + len(t)
    tris = np.concatenate(blocks).astype(np.float32)
    flags = np.concatenate(flags)
    for k, v in sorted(stats.items()):
        print(f"  {k[0]:6s} {k[1]:40s} {v:7d} triangles")
    lo, hi = tris.reshape(-1, 3).min(0), tris.reshape(-1, 3).max(0)
    print("world bounds:", lo, hi)

    vp = cam.view.astype(np.float64) @ cam.proj.astype(np.float64)
    inv = np.linalg.inv(vp)
    eye_h = np.array([0, 0, 0, 1.0]) @ np.linalg.inv(cam.view.astype(np.float64))
    eye = eye_h[:3] / eye_h[3]

    def unproject(px, py):          # image coordinates, y down; point on the near plane
        ndc = np.array([2 * (px + 0.5) / w - 1, 1 - 2 * (py + 0.5) / h, -1.0, 1.0]) @ inv
        return ndc[:3] / ndc[3]

    base = unproject(0, 0)
    dx, dy = unproject(1, 0) - base, unproject(0, 1) - base
    print("eye:", eye)

    HERE.joinpath("build").mkdir(exist_ok=True)
    scene, out = HERE / "build" / "scene.bin", HERE / "build" / "trace.bin"
    with open(scene, "wb") as f:
        f.write(struct.pack("<4I", 0x43535452, len(tris), w, h))
        f.write(np.concatenate([eye, base, dx, dy, light]).astype("<f4").tobytes())
        rec = np.zeros(len(tris), dtype=[("v", "<f4", 9), ("flags", "<u4")])
        rec["v"], rec["flags"] = tris, flags
        f.write(rec.tobytes())
    r = subprocess.run([str(RTCPU), str(scene), str(out), str(args.samples), str(args.radius)],
                       capture_output=True, text=True)
    sys.stdout.write(r.stderr)
    if r.returncode:
        sys.exit("rtcpu failed")

    res = np.fromfile(out, dtype=[("t", "<f4"), ("ndotl", "<f4"), ("vis", "<f4"), ("tri", "<u4")]).reshape(h, w)
    hit = res["t"] > 0
    cls = np.where(hit, flags[np.minimum(res["tri"], len(flags) - 1)] >> 8, 0)
    albedo = np.zeros((h, w, 3), np.float32)
    albedo[...] = (0.55, 0.70, 0.90)
    for c, col in CLASS_COLOR.items():
        albedo[cls == c] = col
    direct = np.clip(res["ndotl"], 0, 1) * res["vis"]
    shade = np.where(hit, 0.35 + 0.65 * direct, 1.0)[..., None]
    write_png(args.prefix + "_shaded.png", (np.clip(albedo * shade, 0, 1) * 255).astype(np.uint8))
    mask = np.where(hit, res["vis"], 1.0)
    write_png(args.prefix + "_mask.png", np.repeat((mask * 255).astype(np.uint8)[..., None], 3, axis=2))
    if args.mask_out:
        with open(args.mask_out, "wb") as f:
            f.write(struct.pack("<3I", 0x4D545257, w, h))
            f.write((mask[::-1] * 255).astype(np.uint8).tobytes())    # bottom row first
    print(f"camera rays that hit geometry: {hit.mean() * 100:.1f}%, of those in shadow: "
          f"{(res['vis'][hit] < 0.5).mean() * 100:.1f}%")
    if args.screenshot:
        shot = read_png(args.screenshot).astype(np.float32) / 255
        if shot.shape[:2] == (h, w):
            # Darken only where the surface faces the sun but the sun is blocked.
            occl = np.where(hit & (res["ndotl"] > 0.05), 1 - res["vis"], 0.0)[..., None]
            comp = shot * (1 - 0.45 * occl)
            write_png(args.prefix + "_composite.png", (np.clip(comp, 0, 1) * 255).astype(np.uint8))
        else:
            print("screenshot size differs from the capture; no composite written")


if __name__ == "__main__":
    main()
