"""
Redo every dumped game texture at 1024 x 1024: the 4x upscale is resized to 1024 and given a
Stable Diffusion XL pass guided by that same picture (ControlNet tile mode), which adds fine
detail while keeping layout and colours. Needs PyTorch and diffusers:

    D:\\AI-Upscale\\venv-restyle\\Scripts\\python.exe -I redo_textures.py MODELS_DIR [--duty 0.33] [--steps 24]

Reads out/texdump/HASH.png (+ HASH_c.png, the 4x upscale), writes out/tex1k/HASH.png and a
line per texture to out/tex1k/progress.log. Textures already done are skipped, so it can be
stopped and started again. --duty is the share of time the GPU works: after each texture the
script sleeps long enough to keep to it, so a game running alongside keeps most of the GPU.
Nothing is installed into the game; that is a separate step.
"""
import sys
import time
from pathlib import Path

import numpy as np
import torch
from diffusers import AutoencoderKL, ControlNetUnionModel, StableDiffusionXLControlNetUnionImg2ImgPipeline
from PIL import Image

HERE = Path(__file__).resolve().parent
DUMP, OUT = HERE / "out" / "texdump", HERE / "out" / "tex1k"
NEG = "blurry, flat, text, watermark, border, frame, deformed"


def arg(name, default):
    return float(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


def tiles(img):
    """Does the picture repeat? True if its opposite edges match about as well as neighbouring rows and columns do."""
    a = np.asarray(img.convert("L"), np.float32)
    inner_x = np.abs(np.diff(a, axis=1)).mean() + 1.0
    inner_y = np.abs(np.diff(a, axis=0)).mean() + 1.0
    seam_x = np.abs(a[:, 0] - a[:, -1]).mean()
    seam_y = np.abs(a[0, :] - a[-1, :]).mean()
    return seam_x < 2.0 * inner_x and seam_y < 2.0 * inner_y


def main():
    models = Path(sys.argv[1])
    duty, steps = arg("--duty", 0.33), int(arg("--steps", 24))
    OUT.mkdir(parents=True, exist_ok=True)
    notes = {}
    ini = HERE / "wumrt_materials.ini"
    if ini.exists():                    # the labels double as a description of what the texture shows
        for line in ini.read_text().splitlines():
            w = line.split("#")
            if len(w) == 2 and len(w[0].split()) >= 4:
                notes[w[0].split()[0]] = w[1].strip()
    todo = [p for p in sorted(DUMP.glob("????????.png")) if not (OUT / p.name).exists()]
    log = (OUT / "progress.log").open("a")
    print(f"{len(todo)} textures to do, duty {duty}, {steps} steps", file=log, flush=True)
    if not todo:
        return
    vae = AutoencoderKL.from_pretrained(models / "sdxl-vae-fix", torch_dtype=torch.float16)
    cn = ControlNetUnionModel.from_pretrained(models / "cn-union", torch_dtype=torch.float16)
    pipe = StableDiffusionXLControlNetUnionImg2ImgPipeline.from_pretrained(
        models / "sdxl-base", controlnet=cn, vae=vae, torch_dtype=torch.float16, variant="fp16", use_safetensors=True).to("cuda")
    pipe.set_progress_bar_config(disable=True)
    convs = [m for net in (pipe.unet, pipe.vae, cn) for m in net.modules() if isinstance(m, torch.nn.Conv2d)]
    for n, p in enumerate(todo, 1):
        t0 = time.time()
        orig = Image.open(p).convert("RGBA")
        if orig.width > 256:            # a dump taken while the 4x version was loaded
            orig = orig.resize((orig.width // 4, orig.height // 4), Image.LANCZOS)
        up = DUMP / f"{p.stem}_c.png"
        base = (Image.open(up) if up.exists() else orig).convert("RGB").resize((1024, 1024), Image.LANCZOS)
        rep = tiles(orig)
        for m in convs:
            m.padding_mode = "circular" if rep else "zeros"
        desc = notes.get(p.stem, "game surface material")
        g = torch.Generator("cuda").manual_seed(5)
        r = pipe(prompt=f"{desc}, texture, sharp fine detail, realistic material, high resolution", negative_prompt=NEG,
                 image=base, control_image=[base], control_mode=[6], strength=0.68 if rep else 0.48,
                 controlnet_conditioning_scale=0.6, guidance_scale=6.0, num_inference_steps=steps, generator=g,
                 width=1024, height=1024).images[0]
        r.putalpha(orig.getchannel("A").resize((1024, 1024), Image.LANCZOS))
        r.save(OUT / p.name)
        work = time.time() - t0
        print(f"{n}/{len(todo)} {p.stem} {'tiles' if rep else 'fitted'} {work:.1f}s", file=log, flush=True)
        time.sleep(work * (1.0 / duty - 1.0))       # leave the GPU to the game most of the time
    print("done", file=log, flush=True)


if __name__ == "__main__":
    main()
