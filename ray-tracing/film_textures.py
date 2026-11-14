"""
Redo every dumped game texture at 1024 x 1024 in a "middle" animated-film style: cleaner and
more painted than redo_textures.py (realistic detail), but held much tighter than
restyle_textures.py (which turned grass into leaves). How hard each texture is pushed depends
on its class, read from wumrt_texstyle.ini (HASH CLASS # description):

    F  featureless fill (plain grass, sand, dirt, water, ice, gradients): low strength, strong
       tile guidance, so it stays the same material
    S  structured repeating surface (bricks, planks, roof tiles, metal plates, bark)
    A  atlas fitted to a model (parts must keep lining up with the mesh)
    W  worm skin / face: lowest of the model-fitted ones
    G  graphic (signs, posters, flags, glows, cutouts): barely touched, no new shapes

    D:\\AI-Upscale\\venv-restyle\\Scripts\\python.exe -I film_textures.py MODELS_DIR [--out DIR]
        [--only HASH,HASH] [--size2k HASHFILE] [--duty 1] [--variant NAME]

Reads out/texdump/HASH.png (+ HASH_c.png, the 4x upscale). The guide picture is the upscale
smoothed with an edge-preserving filter first, so the model copies shapes and colours rather
than pixel noise. Writes OUT/HASH.png (default out/tex_mid); textures already done are skipped
(resumable). --size2k lists hashes (large terrain surfaces) that get a second pass at 2048
into OUT/2k. Variant C is the default (picked 2026-11-14; A and B are milder, mid+ is A with
more smoothing).

    python film_textures.py install [OUT] [--no2k] [--to DIR]

writes OUT/HASH.png (or OUT/2k/HASH.png when there is one) as DIR/HASH_c.wtx; DIR defaults to
this copy's sandbox wumrt_textures. `tex_max` in wumrt.ini can still cap the 2K ones on load.
"""
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import torch
from diffusers import AutoencoderKL, ControlNetUnionModel, StableDiffusionXLControlNetUnionImg2ImgPipeline
from PIL import Image

HERE = Path(sys.argv[sys.argv.index("--root") + 1]) if "--root" in sys.argv else Path(__file__).resolve().parent
DUMP = HERE / "out" / "texdump"
STYLE = ("stylized 3D animated feature film texture, hand-painted, clean readable shapes, simplified detail, "
         "soft painted gradients, appealing colours, high quality")
NEG = ("photo, photorealistic, gritty, grainy, noisy, dirty, blurry, text, letters, watermark, border, frame, "
       "deformed, extra objects")
# class: (strength, tile guidance, smoothing) per variant
VARIANTS = {
    "mid": {"F": (0.50, 0.80, 1), "S": (0.66, 0.55, 1), "A": (0.48, 0.65, 1), "W": (0.36, 0.80, 1), "G": (0.28, 0.85, 0)},
    "mid+": {"F": (0.60, 0.70, 2), "S": (0.74, 0.45, 2), "A": (0.54, 0.60, 2), "W": (0.40, 0.75, 1), "G": (0.30, 0.85, 0)},
    "B": {"F": (0.66, 0.60, 0), "S": (0.78, 0.42, 1), "A": (0.56, 0.55, 1), "W": (0.40, 0.75, 0), "G": (0.30, 0.85, 0)},
    "C": {"F": (0.72, 0.50, 0), "S": (0.82, 0.38, 1), "A": (0.60, 0.50, 1), "W": (0.44, 0.70, 0), "G": (0.32, 0.85, 0)},
}
GENTLE = {"ad1c5680", "59a767f4"}      # user picked the gentler version for wood planks and the hazard barrel
WHAT = {"F": "same material all over, soft painted strokes", "S": "", "A": "game model texture atlas", "W": "smooth cartoon skin",
        "G": "flat graphic"}


def arg(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def tiles(img):
    a = np.asarray(img.convert("L"), np.float32)
    ix, iy = np.abs(np.diff(a, axis=1)).mean() + 1.0, np.abs(np.diff(a, axis=0)).mean() + 1.0
    return np.abs(a[:, 0] - a[:, -1]).mean() < 2.0 * ix and np.abs(a[0, :] - a[-1, :]).mean() < 2.0 * iy


def classes():
    out = {}
    for line in (Path(__file__).resolve().parent / "wumrt_texstyle.ini").read_text().splitlines():
        w = line.split("#", 1)
        f = w[0].split()
        if len(f) >= 2 and len(f[0]) == 8:
            out[f[0]] = (f[1], w[1].strip() if len(w) == 2 else "")
    return out


def smooth(img, level, rep):
    """Edge-preserving smoothing (painted look). Wraps around for repeating textures."""
    if level == 0:
        return img
    a = np.asarray(img.convert("RGB"))
    p = 32
    if rep:
        a = np.pad(a, ((p, p), (p, p), (0, 0)), mode="wrap")
    for _ in range(level):
        a = cv2.bilateralFilter(a, 9, 40, 7)
    a = cv2.edgePreservingFilter(a, flags=cv2.RECURS_FILTER, sigma_s=12 * level, sigma_r=0.25)
    if rep:
        a = a[p:-p, p:-p]
    return Image.fromarray(a)


def install():
    import struct
    a = [x for i, x in enumerate(sys.argv[2:], 2) if not x.startswith("--") and sys.argv[i - 1] != "--to"]
    src = Path(a[0]) if a else HERE / "out" / "tex_mid"
    dst = Path(arg("--to", str(HERE.parent.parent / "rt-sandbox" / "wumrt_textures")))
    dst.mkdir(exist_ok=True)
    n = n2 = 0
    for p in sorted(src.glob("????????.png")):
        big = src / "2k" / p.name
        use2k = big.exists() and "--no2k" not in sys.argv
        img = Image.open(big if use2k else p).convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM)
        (dst / f"{p.stem}_c.wtx").write_bytes(struct.pack("<3I", 0x31585457, img.width, img.height) + img.tobytes())
        n += 1
        n2 += use2k
    print(f"{n} colour textures ({n2} at 2048) written to {dst}")


def main():
    if sys.argv[1] == "install":
        return install()
    models = Path(sys.argv[1])
    variant = arg("--variant", "C")
    out = Path(arg("--out", str(HERE / "out" / "tex_mid")))
    duty = float(arg("--duty", "1"))
    big = set(Path(arg("--size2k", "")).read_text().split()) if "--size2k" in sys.argv else set()
    only = arg("--only", "")
    out.mkdir(parents=True, exist_ok=True)
    (out / "2k").mkdir(exist_ok=True)
    cls = classes()
    hs = only.split(",") if only else sorted(p.stem for p in DUMP.glob("????????.png"))
    todo = [h for h in hs if not (out / f"{h}.png").exists() or (h in big and not (out / "2k" / f"{h}.png").exists())]
    log = (out / "progress.log").open("a")
    print(f"{len(todo)} textures to do, variant {variant}, {len(big)} at 2K", file=log, flush=True)
    if not todo:
        return
    vae = AutoencoderKL.from_pretrained(models / "sdxl-vae-fix", torch_dtype=torch.float16)
    cn = ControlNetUnionModel.from_pretrained(models / "cn-union", torch_dtype=torch.float16)
    pipe = StableDiffusionXLControlNetUnionImg2ImgPipeline.from_pretrained(
        models / "sdxl-base", controlnet=cn, vae=vae, torch_dtype=torch.float16, variant="fp16", use_safetensors=True).to("cuda")
    fast = "--fast" in sys.argv          # SDXL Lightning 8-step LoRA: ~6x less GPU time, for a busy GPU
    if fast:
        from diffusers import EulerDiscreteScheduler
        pipe.load_lora_weights(str(models / "lightning"), weight_name="sdxl_lightning_8step_lora.safetensors")
        pipe.fuse_lora()
        pipe.scheduler = EulerDiscreteScheduler.from_config(pipe.scheduler.config, timestep_spacing="trailing")
    pipe.set_progress_bar_config(disable=True)
    pipe.vae.enable_tiling()
    convs = [m for net in (pipe.unet, pipe.vae, cn) for m in net.modules() if isinstance(m, torch.nn.Conv2d)]

    def run(base, prompt, strength, scale, size, steps=28):
        g = torch.Generator("cuda").manual_seed(5)
        if fast:            # Lightning: 8-step schedule, no classifier-free guidance
            steps = max(8, round(8 / max(strength, 0.3)))
        return pipe(prompt=prompt, negative_prompt=None if fast else NEG, image=base, control_image=[base],
                    control_mode=[6], strength=strength, controlnet_conditioning_scale=scale,
                    guidance_scale=1.0 if fast else 6.0, num_inference_steps=steps, generator=g,
                    width=size, height=size).images[0]

    for n, h in enumerate(todo, 1):
        t0 = time.time()
        orig = Image.open(DUMP / f"{h}.png").convert("RGBA")
        if orig.width > 256:
            orig = orig.resize((orig.width // 4, orig.height // 4), Image.LANCZOS)
        up = DUMP / f"{h}_c.png"
        rep = tiles(orig)
        for m in convs:
            m.padding_mode = "circular" if rep else "zeros"
        c, desc = cls.get(h, ("S", "game surface material"))
        strength, scale, sm = VARIANTS[variant][c]
        if h in GENTLE:
            strength -= 0.12
        base = (Image.open(up) if up.exists() else orig).convert("RGB").resize((1024, 1024), Image.LANCZOS)
        base = smooth(base, sm, rep)
        prompt = ", ".join(x for x in (desc, WHAT[c], STYLE) if x)
        one = Image.open(out / f"{h}.png").convert("RGB") if (out / f"{h}.png").exists() else run(base, prompt, strength, scale, 1024)
        if h not in big or not (out / f"{h}.png").exists():
            one.putalpha(orig.getchannel("A").resize(one.size, Image.LANCZOS))
            one.save(out / f"{h}.png")
        if h in big:
            two = run(one.convert("RGB").resize((2048, 2048), Image.LANCZOS), prompt, 0.30, 0.7, 2048, 20)
            two.putalpha(orig.getchannel("A").resize(two.size, Image.LANCZOS))
            two.save(out / "2k" / f"{h}.png")
        work = time.time() - t0
        print(f"{n}/{len(todo)} {h} {c} {'tiles' if rep else 'fitted'} s={strength:.2f} cn={scale:.2f} {work:.1f}s",
              file=log, flush=True)
        if duty < 1:
            time.sleep(work * (1.0 / duty - 1.0))
    print("done", file=log, flush=True)


if __name__ == "__main__":
    main()
