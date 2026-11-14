"""
Run an image-to-image texture model (for example PBRify_Remix's normal or roughness model) on
the textures dumped by textures.py. Needs PyTorch and spandrel, so run it with a Python that
has them, not the one used for the other scripts:

    D:\\AI-Upscale\\venv\\Scripts\\python.exe ai_maps.py MODEL.pth KIND [--gpu] [--flip-green] [--src DIR] [--out DIR] [--size N] [HASH ...]

KIND is the suffix of the files written: n for a normal map, r for a roughness map, h for a
height map (PBRify's height model; `relief.py model` puts those into the normal maps' alpha).
For every
out/texdump/HASH.png it writes out/ai_maps/HASH_KIND.png; `python textures.py import out/ai_maps`
then hands the normal maps to the game. --src reads the colour textures from another folder
(for example the 1024 redo in out/tex_mid), --size scales them to N x N first, --out writes
elsewhere; maps already written there are skipped. Runs on the CPU unless --gpu is given (the textures
are small, and the GPU may be busy with a game).

The model file is loaded with spandrel, which reads weights only and does not run code from
the file.
"""
import sys
from pathlib import Path

import numpy as np
import torch
from PIL import Image
from spandrel import ImageModelDescriptor, ModelLoader

HERE = Path(__file__).resolve().parent
DUMP = HERE / "out" / "texdump"
OUT = HERE / "out" / "ai_maps"
PAD = 16


def main():
    opt = {k: sys.argv[sys.argv.index(k) + 1] for k in ("--src", "--out", "--size") if k in sys.argv}
    args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith("--") and sys.argv[i - 1] not in opt]
    gpu, flip = "--gpu" in sys.argv, "--flip-green" in sys.argv
    eq = next((a[6:] for a in sys.argv[1:] if a.startswith("--src=")), None)   # --src=FOLDER also works
    src, out, size = Path(eq or opt.get("--src", DUMP)), Path(opt.get("--out", OUT)), int(opt.get("--size", 0))
    model_path, kind, names = args[0], args[1], args[2:]
    model = ModelLoader().load_from_file(model_path)
    assert isinstance(model, ImageModelDescriptor), "not an image-to-image model"
    device = torch.device("cuda" if gpu and torch.cuda.is_available() else "cpu")
    model.to(device).eval()
    out.mkdir(parents=True, exist_ok=True)
    todo = [src / f"{h}.png" for h in names] if names else sorted(src.glob("????????.png"))
    todo = [p for p in todo if not (out / f"{p.stem}_{kind}.png").exists()]
    print(f"{model.architecture.name} x{model.scale} on {device}, {len(todo)} textures")
    for p in todo:
        img = Image.open(p).convert("RGB")
        if size:
            img = img.resize((size, size * img.height // img.width), Image.LANCZOS)
        rgb = np.asarray(img, dtype=np.float32) / 255.0
        pad = min(PAD, rgb.shape[0], rgb.shape[1])
        # A margin of the texture repeated around it, so the result tiles like the original.
        x = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="wrap")
        t = torch.from_numpy(x).permute(2, 0, 1).unsqueeze(0).to(device)
        with torch.no_grad():
            y = model(t).clamp(0, 1)[0].permute(1, 2, 0).cpu().numpy()
        k = model.scale * pad
        y = y[k:y.shape[0] - k, k:y.shape[1] - k]
        if flip:
            y[..., 1] = 1.0 - y[..., 1]
        Image.fromarray(np.uint8(y * 255 + 0.5)).save(out / f"{p.stem}_{kind}.png")
    print(f"written to {out}")


if __name__ == "__main__":
    main()
