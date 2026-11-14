"""
Replacement textures for the game: dump what a match uses, make normal maps, upscale colours.

    python textures.py dump                    full-size textures of the running match -> out/texdump/HASH.png
    python textures.py normals [STRENGTH]      a normal map for every dumped texture -> wumrt_textures/HASH_n.wtx
    python textures.py upscale EXE [HASH ...]  upscale dumped textures 4x with a Real-ESRGAN ncnn executable
                                               -> wumrt_textures/HASH_c.wtx (all of them, or the named ones)
    python textures.py import DIR              take normal maps or colour images made by another tool:
                                               DIR/HASH_n.png and DIR/HASH_c.png -> wumrt_textures

The game reads wumrt_textures (in its own folder) when a texture is first used; restart the
match to see changes. `bump = 1` in wumrt.ini turns normal mapping on.

The normal maps made here come from the picture's brightness, taken as height: no neural
network, and painted-on shading counts as relief. `import` is the way in for a tool that does
better; the file names it must produce are in out/texdump. ai_maps.py is one such tool.
"""
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

HERE = Path(__file__).resolve().parent
DUMP = HERE / "out" / "texdump"
GAME = HERE.parent.parent / "rt-sandbox" / "wumrt_textures"


def write_wtx(path, img):
    img = img.convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM)      # bottom row first, as OpenGL wants it
    path.write_bytes(struct.pack("<3I", 0x31585457, img.width, img.height) + img.tobytes())


def dump():
    import rt
    DUMP.mkdir(parents=True, exist_ok=True)
    c = rt.attach()
    rt.command(c, 13, DUMP / "t_", timeout=60)       # the game writes t_HASH.wtx
    n = 0
    for p in DUMP.glob("t_*.wtx"):
        d = p.read_bytes()
        _, w, h = struct.unpack_from("<3I", d, 0)
        Image.frombytes("RGBA", (w, h), d[12:]).transpose(Image.FLIP_TOP_BOTTOM).save(DUMP / (p.stem[2:] + ".png"))
        p.unlink()
        n += 1
    print(f"{n} textures written, {len(list(DUMP.glob('*.png')))} in {DUMP}")


def normal_map(img, strength):
    """Tangent-space normal map from brightness as height; wraps around like the texture tiles."""
    grey = np.asarray(img.convert("L").filter(ImageFilter.GaussianBlur(0.7)), dtype=np.float32) / 255.0
    dx = np.zeros_like(grey)
    dy = np.zeros_like(grey)
    for step, weight in ((1, 1.0), (2, 0.6), (4, 0.35)):            # fine detail plus broader forms
        dx += weight * (np.roll(grey, -step, axis=1) - np.roll(grey, step, axis=1)) / step
        dy += weight * (np.roll(grey, step, axis=0) - np.roll(grey, -step, axis=0)) / step   # image rows run downwards
    scale = strength * 2.5
    n = np.stack([-dx * scale, -dy * scale, np.ones_like(grey)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return Image.fromarray(np.uint8(np.clip(n * 0.5 + 0.5, 0, 1) * 255 + 0.5), "RGB")


def normals(strength):
    GAME.mkdir(exist_ok=True)
    n = 0
    for p in sorted(DUMP.glob("????????.png")):
        img = Image.open(p)
        if img.width < 16 or img.height < 16:
            continue
        nm = normal_map(img, strength)
        nm.save(DUMP / f"{p.stem}_n.png")
        write_wtx(GAME / f"{p.stem}_n.wtx", nm)
        n += 1
    print(f"{n} normal maps written to {GAME}")


def upscale(exe, names):
    GAME.mkdir(exist_ok=True)
    todo = [DUMP / f"{h}.png" for h in names] if names else sorted(DUMP.glob("????????.png"))
    for p in todo:
        out = DUMP / f"{p.stem}_c.png"
        if out.exists() and not names:      # done in an earlier run
            continue
        src = Image.open(p).convert("RGBA")
        rgb = DUMP / f"{p.stem}_rgb.png"
        # The upscaler gets colour only (alpha is resized separately), with a margin of the
        # texture repeated around it so that the result still tiles without a seam.
        pad = min(16, src.width, src.height)
        Image.fromarray(np.pad(np.asarray(src.convert("RGB")), ((pad, pad), (pad, pad), (0, 0)), mode="wrap")).save(rgb)
        r = subprocess.run([exe, "-i", str(rgb), "-o", str(out), "-s", "4"], capture_output=True, text=True, cwd=Path(exe).parent)
        rgb.unlink()
        if r.returncode or not out.exists():
            print(f"{p.stem}: upscaler failed: {(r.stderr or r.stdout)[-200:]}")
            continue
        big = Image.open(out).convert("RGB")
        big = big.crop((pad * 4, pad * 4, big.width - pad * 4, big.height - pad * 4))
        big.putalpha(src.getchannel("A").resize(big.size, Image.LANCZOS))
        big.save(out)
        write_wtx(GAME / f"{p.stem}_c.wtx", big)
        print(f"{p.stem}: {src.width}x{src.height} -> {big.width}x{big.height}")


def import_dir(folder):
    GAME.mkdir(exist_ok=True)
    n = 0
    for p in Path(folder).glob("????????_[nc].png"):
        write_wtx(GAME / f"{p.stem}.wtx", Image.open(p))
        n += 1
    print(f"{n} files imported into {GAME}")


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
    elif a[0] == "dump":
        dump()
    elif a[0] == "normals":
        normals(float(a[1]) if len(a) > 1 else 1.0)
    elif a[0] == "upscale":
        upscale(a[1], a[2:])
    elif a[0] == "import":
        import_dir(a[1])


if __name__ == "__main__":
    main()
