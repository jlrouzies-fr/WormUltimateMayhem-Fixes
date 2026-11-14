"""
Height maps for parallax relief, made from the normal maps already installed (no download).

    python relief.py heights [FOLDER]    write a height into the alpha of every HASH_n.wtx
    python relief.py model [FOLDER]      the same, shaped by PBRify's height model (out/ai_maps/HASH_h.png,
                                         from ai_maps.py ... h --src=out/tex1k) and as deep as the
                                         normal map says; textures without a model height as `heights`
    python relief.py clear [FOLDER]      set that alpha back to 255 (flat: the relief does nothing)
    python relief.py sheet [FOLDER]      contact sheet out/relief_sheet.png: colour | height, for checking

FOLDER defaults to the main sandbox's wumrt_textures. The shaders read the alpha as a height
(1 = the top of the relief), `relief` in wumrt.ini sets how deep 0 is, and `rt.py reload`
loads changed maps into the running game.

How: the slopes the normal map gives (-x/z, -y/z per texel) are integrated over the
repeating texture with an FFT (Frankot-Chellappa: the height whose gradient is closest to
them). Two things the map does not say are worked out per texture: which way its green
channel points (the convention whose slopes integrate best, that is with the smaller
non-integrable remainder, wins) and which way is up (the sign whose height goes with the
colour texture's brightness: cavities are darker). The height is in texture repeats and
stored as 1 - depth / MAX_DEPTH, so weak normal maps give shallow relief and strong ones
deep relief; deeper than MAX_DEPTH is clipped. Grass, moss and sand (wumrt_surfaces.ini) are
left flat: shells and scattered stones cover them.
"""
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
TEX = HERE.parent.parent / "rt-sandbox" / "wumrt_textures"
SURF = HERE / "wumrt_surfaces.ini"
MAX_DEPTH = 0.04            # texture repeats that alpha 0 stands for (the shader's `relief` scales it)


def read_wtx(p):
    b = p.read_bytes()
    magic, w, h = struct.unpack("<3I", b[:12])
    assert magic == 0x31585457, p
    return np.frombuffer(b, np.uint8, offset=12).reshape(h, w, 4).copy()


def write_wtx(p, a):
    h, w = a.shape[:2]
    p.write_bytes(struct.pack("<3I", 0x31585457, w, h) + np.ascontiguousarray(a, np.uint8).tobytes())


def soft_kinds():
    out = set()
    for line in SURF.read_text(encoding="utf-8").splitlines():
        f = line.split("#")[0].split()
        if len(f) >= 2 and f[1] in ("grass", "moss", "sand"):
            out.add(f[0].lower())
    return out


def integrate(p, q):
    """Height (texels) from slopes p = dz/dx, q = dz/dy on a repeating image, and the remainder."""
    h, w = p.shape
    wx = np.fft.fftfreq(w) * 2 * np.pi
    wy = np.fft.fftfreq(h) * 2 * np.pi
    WX, WY = np.meshgrid(wx, wy)
    P, Q = np.fft.fft2(p), np.fft.fft2(q)
    den = WX ** 2 + WY ** 2
    den[0, 0] = 1
    Z = (-1j * WX * P - 1j * WY * Q) / den
    Z[0, 0] = 0
    z = np.real(np.fft.ifft2(Z))
    # how far the slopes of that height are from the ones asked for (non-integrable part)
    zp = np.real(np.fft.ifft2(1j * WX * Z)); zq = np.real(np.fft.ifft2(1j * WY * Z))
    rest = np.mean((zp - p) ** 2 + (zq - q) ** 2) / max(np.mean(p ** 2 + q ** 2), 1e-12)
    return z, rest


def height_of(nrm, colour):
    """nrm: HxWx4 uint8 (bottom row first). Returns depth below the top in texture repeats."""
    n = nrm[..., :3].astype(np.float64) / 127.5 - 1
    nz = np.maximum(n[..., 2], 0.15)
    p = -n[..., 0] / nz
    best = None
    for gsign in (1, -1):                       # the green channel's convention
        q = -gsign * n[..., 1] / nz
        z, rest = integrate(p, q)
        if best is None or rest < best[1]:
            best = (z, rest, gsign)
    z = best[0] / nrm.shape[1]                  # texels -> texture repeats
    if colour is not None:
        lum = colour[..., :3].astype(np.float64).mean(2)
        if lum.shape != z.shape:
            from PIL import Image
            lum = np.asarray(Image.fromarray(lum.astype(np.float32)).resize((z.shape[1], z.shape[0])), np.float64)
        if np.corrcoef(lum.ravel(), z.ravel())[0, 1] < 0:
            z = -z
    depth = z.max() - z                          # 0 at the top
    return depth, best[1], best[2]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "heights"
    folder = Path(sys.argv[2]) if len(sys.argv) > 2 else TEX
    files = sorted(folder.glob("*_n.wtx"))
    soft = soft_kinds()
    if cmd == "clear":
        for f in files:
            a = read_wtx(f)
            if a[..., 3].min() < 255:
                a[..., 3] = 255
                write_wtx(f, a)
        print(f"{len(files)} normal maps flat again")
        return
    if cmd == "sheet":
        from PIL import Image
        picks = files[:: max(1, len(files) // 24)][:24]
        cell = 128
        sheet = Image.new("RGB", (cell * 2 * 4, cell * 6), (30, 30, 30))
        for i, f in enumerate(picks):
            c = f.with_name(f.name.replace("_n.wtx", "_c.wtx"))
            col = Image.fromarray(read_wtx(c)[::-1, :, :3]).resize((cell, cell)) if c.exists() else Image.new("RGB", (cell, cell))
            hgt = Image.fromarray(read_wtx(f)[::-1, :, 3]).resize((cell, cell)).convert("RGB")
            x, y = (i % 4) * cell * 2, (i // 4) * cell
            sheet.paste(col, (x, y)); sheet.paste(hgt, (x + cell, y))
        out = HERE / "out" / "relief_sheet.png"
        sheet.save(out)
        print(out)
        return
    use_model = cmd == "model"
    flipped = 0
    depths, done, skipped = [], 0, 0
    for f in files:
        h = f.name[:8].lower()
        a = read_wtx(f)
        if h in soft:
            a[..., 3] = 255
            write_wtx(f, a)
            skipped += 1
            continue
        c = f.with_name(f.name.replace("_n.wtx", "_c.wtx"))
        colour = read_wtx(c) if c.exists() else None
        depth, rest, g = height_of(a, colour)
        hp = HERE / "out" / "ai_maps" / f"{f.name[:8]}_h.png"
        if use_model and hp.exists():
            from PIL import Image
            m = Image.open(hp).convert("L").resize((a.shape[1], a.shape[0]), Image.BILINEAR)
            hm = np.asarray(m, np.float64)[::-1]          # bottom row first, like the .wtx
            lo, hi = np.percentile(hm, 2), np.percentile(hm, 98)
            hn = np.clip((hm - lo) / max(hi - lo, 1e-6), 0, 1)
            if np.corrcoef(hn.ravel(), -depth.ravel())[0, 1] < 0:
                hn = 1 - hn                                # the model's up is the other way here
                flipped += 1
            depth = (1 - hn) * np.percentile(depth, 95)
        depths.append(np.percentile(depth, 95))
        a[..., 3] = np.clip(255 * (1 - depth / MAX_DEPTH), 0, 255).round().astype(np.uint8)
        write_wtx(f, a)
        done += 1
    d = np.array(depths) if depths else np.zeros(1)
    print(f"{done} normal maps got heights, {skipped} left flat (grass, moss, sand); depth (95th percentile, "
          f"texture repeats): median {np.median(d):.4f}, 90% {np.percentile(d, 90):.4f}, max {d.max():.4f}; "
          f"clipped at {MAX_DEPTH}" + (f"; model heights turned over for {flipped}" if use_model else ""))


if __name__ == "__main__":
    main()
