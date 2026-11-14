"""
Restyle game textures towards an animated-film look, loosely guided by the original, in two
stages: a restyle at 1024, then that result enlarged and sharpened at 2048.

    D:\\AI-Upscale\\venv-restyle\\Scripts\\python.exe -I restyle_textures.py MODELS_DIR HASHES.txt

HASHES.txt holds one texture hash per line (for example the textures one level uses). Reads
out/texdump/HASH.png and HASH_c.png, writes out/tex_film/HASH.png (1024) and
out/tex_film2k/HASH.png (2048); textures already done are skipped. Repeating surfaces are
held loosely and stay tileable; textures fitted to a model are held tighter so their parts
keep lining up with the mesh. Nothing is installed into the game.
"""
import sys
from pathlib import Path

import numpy as np
import torch
from diffusers import AutoencoderKL, ControlNetUnionModel, StableDiffusionXLControlNetUnionImg2ImgPipeline
from PIL import Image

HERE = Path(__file__).resolve().parent
DUMP, OUT1, OUT2 = HERE / "out" / "texdump", HERE / "out" / "tex_film", HERE / "out" / "tex_film2k"
STYLE = ("stylized 3D animated feature film texture, clean simplified shapes, soft hand-painted detail, smooth gradients, "
         "appealing colours, crisp edges, high quality")
NEG = "photo, photorealistic, gritty, noisy, dirty, blurry, text, watermark, border, deformed"


def tiles(img):
    a = np.asarray(img.convert("L"), np.float32)
    inner_x, inner_y = np.abs(np.diff(a, axis=1)).mean() + 1.0, np.abs(np.diff(a, axis=0)).mean() + 1.0
    return np.abs(a[:, 0] - a[:, -1]).mean() < 2.0 * inner_x and np.abs(a[0, :] - a[-1, :]).mean() < 2.0 * inner_y


def main():
    models = Path(sys.argv[1])
    hashes = [w for w in Path(sys.argv[2]).read_text().split() if len(w) == 8]
    OUT1.mkdir(parents=True, exist_ok=True); OUT2.mkdir(parents=True, exist_ok=True)
    notes = {}
    for line in (HERE / "wumrt_materials.ini").read_text().splitlines():
        w = line.split("#")
        if len(w) == 2 and len(w[0].split()) >= 4:
            notes[w[0].split()[0]] = w[1].strip()
    todo = [h for h in hashes if (DUMP / f"{h}.png").exists() and not (OUT2 / f"{h}.png").exists()]
    print(len(todo), "textures to restyle", flush=True)
    if not todo:
        return
    vae = AutoencoderKL.from_pretrained(models / "sdxl-vae-fix", torch_dtype=torch.float16)
    cn = ControlNetUnionModel.from_pretrained(models / "cn-union", torch_dtype=torch.float16)
    pipe = StableDiffusionXLControlNetUnionImg2ImgPipeline.from_pretrained(
        models / "sdxl-base", controlnet=cn, vae=vae, torch_dtype=torch.float16, variant="fp16", use_safetensors=True).to("cuda")
    pipe.set_progress_bar_config(disable=True)
    pipe.vae.enable_tiling()
    convs = [m for net in (pipe.unet, pipe.vae, cn) for m in net.modules() if isinstance(m, torch.nn.Conv2d)]

    def run(base, desc, strength, scale, size):
        g = torch.Generator("cuda").manual_seed(5)
        return pipe(prompt=f"{desc}, {STYLE}", negative_prompt=NEG, image=base, control_image=[base], control_mode=[6],
                    strength=strength, controlnet_conditioning_scale=scale, guidance_scale=6.5, num_inference_steps=28,
                    generator=g, width=size, height=size).images[0]

    for n, h in enumerate(todo, 1):
        orig = Image.open(DUMP / f"{h}.png").convert("RGBA")
        if orig.width > 256:
            orig = orig.resize((orig.width // 4, orig.height // 4), Image.LANCZOS)
        up = DUMP / f"{h}_c.png"
        base = (Image.open(up) if up.exists() else orig).convert("RGB").resize((1024, 1024), Image.LANCZOS)
        rep = tiles(orig)
        for m in convs:
            m.padding_mode = "circular" if rep else "zeros"
        desc = notes.get(h, "game surface material")
        one = run(base, desc, 0.85 if rep else 0.6, 0.3 if rep else 0.45, 1024)
        two = run(one.resize((2048, 2048), Image.LANCZOS), desc, 0.38, 0.6, 2048)
        for img, folder in ((one, OUT1), (two, OUT2)):
            img.putalpha(orig.getchannel("A").resize(img.size, Image.LANCZOS))
            img.save(folder / f"{h}.png")
        print(f"{n}/{len(todo)} {h} {'tiles' if rep else 'fitted'}", flush=True)


if __name__ == "__main__":
    main()
