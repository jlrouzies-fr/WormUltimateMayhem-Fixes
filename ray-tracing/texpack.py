"""
Pack replacement textures as S3TC (DXT1 / DXT5) with their mip levels, so the game uploads
them as they are instead of making the driver compress them on load (see the README's
"Textures"). CPU only (Pillow's BCn encoder); no GPU, no AI.

    python texpack.py [FOLDER] [--normals] [--force] [--check HASH]

For every HASH_c.wtx in FOLDER (default: this copy's sandbox wumrt_textures) it writes
HASH_c.wtc: DXT1 when the picture is opaque, DXT5 when it has alpha. --normals also packs
HASH_n.wtx (DXT5: the alpha holds the relief height). Files already packed and newer than
their .wtx are skipped unless --force. The game uses a .wtc only with `tex_compress = 1`
(colour) or 2 (colour and normal maps), and only when it is at least as new as its .wtx.
--check HASH prints how far the packed colour map is from the original (PSNR, in dB).

.wtc layout: 'WTC1', width, height, GL format, levels (u32 each), then per level a u32 byte
count and the blocks. Rows run bottom-up, like the .wtx.
"""
import io
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
TEX = HERE.parent.parent / "rt-sandbox" / "wumrt_textures"
DXT1, DXT5 = 0x83F0, 0x83F3


def read_wtx(p):
    b = p.read_bytes()
    magic, w, h = struct.unpack("<3I", b[:12])
    assert magic == 0x31585457, p
    return Image.frombytes("RGBA", (w, h), b[12:12 + w * h * 4])


def blocks(img, fmt):
    """The BCn blocks of one level (padded to whole 4 x 4 blocks by repeating the edge)."""
    w, h = img.size
    pw, ph = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    if (pw, ph) != (w, h):
        a = np.asarray(img)
        a = np.pad(a, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")
        img = Image.fromarray(a, img.mode)
    buf = io.BytesIO()
    img.convert("RGB" if fmt == DXT1 else "RGBA").save(buf, "DDS", pixel_format="DXT1" if fmt == DXT1 else "DXT5")
    data = buf.getvalue()[128:]
    want = (pw // 4) * (ph // 4) * (8 if fmt == DXT1 else 16)
    assert len(data) == want, (len(data), want)
    return data


def pack(src, dst, alpha_matters):
    img = read_wtx(src)
    opaque = img.getchannel("A").getextrema()[0] == 255
    fmt = DXT1 if opaque and not alpha_matters else DXT5
    w, h = img.size
    levels = []
    lv = img
    while True:
        levels.append(blocks(lv, fmt))
        if lv.width == 1 and lv.height == 1:
            break
        lv = lv.resize((max(1, lv.width // 2), max(1, lv.height // 2)), Image.BOX)
    out = bytearray(struct.pack("<5I", 0x31435457, w, h, fmt, len(levels)))
    for d in levels:
        out += struct.pack("<I", len(d)) + d
    tmp = dst.with_suffix(".tmp")
    tmp.write_bytes(out)
    tmp.replace(dst)
    return fmt, len(out)


def unpack_level0(p):
    b = p.read_bytes()
    _, w, h, fmt, n = struct.unpack("<5I", b[:20])
    size = struct.unpack("<I", b[20:24])[0]
    data = b[24:24 + size]
    # wrap the blocks in a DDS header so Pillow can decode them
    hdr = io.BytesIO()
    Image.new("RGB" if fmt == DXT1 else "RGBA", (w, h)).save(hdr, "DDS", pixel_format="DXT1" if fmt == DXT1 else "DXT5")
    return Image.open(io.BytesIO(hdr.getvalue()[:128] + data)).convert("RGBA")


def check(folder, h):
    a = np.asarray(read_wtx(folder / f"{h}_c.wtx"), np.float32)[..., :3]
    b = np.asarray(unpack_level0(folder / f"{h}_c.wtc"), np.float32)[..., :3]
    mse = ((a - b) ** 2).mean()
    print(f"{h}: PSNR {10 * np.log10(255 ** 2 / max(mse, 1e-6)):.1f} dB")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--check" in sys.argv:
        folder = Path(args[1]) if len(args) > 1 else TEX
        return check(folder, args[0])
    folder = Path(args[0]) if args else TEX
    kinds = ["c"] + (["n"] if "--normals" in sys.argv else [])
    n = skipped = total = 0
    for kind in kinds:
        for src in sorted(folder.glob(f"????????_{kind}.wtx")):
            dst = src.with_suffix(".wtc")
            if "--force" not in sys.argv and dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
                skipped += 1
                continue
            fmt, size = pack(src, dst, kind == "n")
            n += 1
            total += size
    print(f"{n} packed ({total / 2**20:.0f} MB), {skipped} already up to date, in {folder}")


if __name__ == "__main__":
    main()
