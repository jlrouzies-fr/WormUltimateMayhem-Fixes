"""
Repeatable, labelled before/after review sets: a few levels x three views x presets, all
presets of a level shot in the SAME paused match (same worms, same camera, same frame), so
only the rendering differs.

    python review.py kit NAME            snapshot this copy's build as a kit (core, patched CG, compute
                                         shaders, ini files; settings from the main sandbox's wumrt.ini)
    python review.py shoot MILESTONE [KIT ...] [--levels a,b] [--res 1280x720]
                                         shoot stock + the kits (default: baseline current) into
                                         main ray-tracing/captures/review/MILESTONE/
    python review.py label MILESTONE     side-by-side sheets (stock | kits) + compressed JPEGs

Presets:
  stock     the game's own CG shaders, rt = 0, no shells/scatter/relief, no replacement textures
  KIT       a kit made with `review.py kit NAME`: "baseline" (main before the streams merged) and
            "current" (latest main: `sh stream.sh sync review`, then `python review.py kit current`)

How one level is shot: the review sandbox starts (1280x720 by default, scale 0.5, hide_hud = 1) with its
wumrt_textures pointing at an empty folder and the stock files in place; the match is paused
(checked on screen); the views are set with the free camera; stock is shot; then textures are
pointed back at the main sandbox's and reloaded, and for each kit its files are copied in and
reload-core, cg-reload and reload swap them into the running game before its shots. The game
is stopped after each level (replacement textures cannot be taken out of a running game).

Views: "wide" is a stored free-camera pose per level (review_poses.json, made on the first
shoot from the match's start camera); "mid" and "close" are taken relative to the match's
start camera, which looks at the active worm (worms are placed at random, so these differ
between milestones; within one milestone all presets share them).
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
STREAM = HERE.parent.parent                         # streams/review
ROOT = STREAM.parent.parent                         # WormsXHD
SB = STREAM / "rt-sandbox"
MAIN = ROOT / "WormUltimateMayhem-Fixes" / "ray-tracing"
MAIN_SB = ROOT / "rt-sandbox"
KITS = STREAM / "kits"
EMPTY_TEX = STREAM / "empty_textures"
POSES = HERE / "review_poses.json"
OUT = MAIN / "captures" / "review"
PY = [sys.executable]

LEVELS = {                                          # short name -> name for rt.py level
    "apple": "Apple Core Island",
    "dragon": "This Game Does Dragon",
    "grave": "Grave Danger",
    "ice": "Ice, Ice, Maybe",
}
VIEWS = ["wide", "mid", "close"]
COMPS = ["wumrt_trace.comp", "wumrt_filter.comp", "wumrt_temporal.comp", "wumrt_atrous.comp"]
INIS = ["wumrt_materials.ini", "wumrt_surfaces.ini"]
STOCK_OFF = {"rt": 0, "shells": 0, "scatter": 0, "relief": 0, "sky": 0, "gi": 0, "reflections": 0,
             "specular": 0, "bump": 0}
LABELS = {"stock": "Stock game", "baseline": "Baseline (before the streams)", "current": "Current main"}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def run(args, timeout=300, check=False):
    p = subprocess.run(PY + args, cwd=HERE, capture_output=True, text=True, timeout=timeout)
    out = (p.stdout + p.stderr).strip()
    if check and p.returncode:
        raise RuntimeError(f"{args}: {out}")
    return p.returncode, out


def rt(*a, timeout=120):
    return run(["rt.py", *[str(x) for x in a]], timeout)[1]


# ---- kits ---------------------------------------------------------------------------------
def make_kit(name):
    k = KITS / name
    if k.exists():
        shutil.rmtree(k)
    (k / "CG").mkdir(parents=True)
    shutil.copy2(HERE / "build" / "wumrt_core.dll", k)
    for f in (SB / "CG").iterdir():
        shutil.copy2(f, k / "CG")
    for f in COMPS + INIS:
        shutil.copy2(HERE / f, k)
    shutil.copy2(MAIN_SB / "wumrt.ini", k / "wumrt.ini")
    (k / "made.txt").write_text(time.strftime("%Y-%m-%d %H:%M") + "\n")
    log(f"kit {name}: core, {len(list((k / 'CG').iterdir()))} CG files, settings from {MAIN_SB / 'wumrt.ini'}")


def write_ini(src, overrides):
    lines, seen = [], set()
    for line in src.read_text().splitlines():
        m = re.match(r"\s*([A-Za-z_]+)\s*=", line)
        if m and m.group(1) in overrides:
            line = f"{m.group(1)} = {overrides[m.group(1)]}"
            seen.add(m.group(1))
        lines.append(line)
    lines += [f"{k} = {v}" for k, v in overrides.items() if k not in seen]
    (SB / "wumrt.ini").write_text("\n".join(lines) + "\n")


def place(kit, overrides):
    """Copy a kit's files into the sandbox (the running game picks them up on reload)."""
    k = KITS / kit
    shutil.copy2(k / "wumrt_core.dll", SB / "wumrt_core.dll")
    for f in COMPS + INIS:
        shutil.copy2(k / f, SB / f)
    for f in (k / "CG").iterdir():
        shutil.copy2(f, SB / "CG" / f.name)
    write_ini(k / "wumrt.ini", overrides)


def place_stock(base_kit, overrides):
    place(base_kit, {**overrides, **STOCK_OFF})
    for f in (ROOT / "CG").iterdir():             # the game's own shaders
        shutil.copy2(f, SB / "CG" / f.name)


def textures(on):
    link = SB / "wumrt_textures"
    target = MAIN_SB / "wumrt_textures" if on else EMPTY_TEX
    EMPTY_TEX.mkdir(exist_ok=True)
    if link.exists() or link.is_symlink():
        subprocess.run(["cmd", "/c", "rmdir", str(link)], check=True, capture_output=True)
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)


def set_resolution(w, h):
    cfg = SB / "Default.cfg"
    t = cfg.read_text()
    t = re.sub(r"/W:\d+", f"/W:{w}", t)
    t = re.sub(r"/H:\d+", f"/H:{h}", t)
    cfg.write_text(t)


# ---- the game -----------------------------------------------------------------------------
def start_level(level):
    code, out = run(["sandbox.py", "slot", "take", "review", "15"])    # renew our hold (fails if not ours)
    if code:
        raise RuntimeError("the game slot is not review's (sh stream.sh slot take review): " + out)
    for attempt in range(1):
        env = dict(os.environ, LEVEL=LEVELS[level])
        p = subprocess.run(["sh", "go.sh"], cwd=HERE, capture_output=True, text=True, timeout=400, env=env)
        out = p.stdout + p.stderr
        log(" | ".join(l for l in out.splitlines() if "watch" in l or "match" in l or "BUSY" in l))
        if "POPPED UP" in out:
            raise RuntimeError("the game popped up: stopping")
        if "BUSY" in out:
            raise RuntimeError("the game slot is taken (see slot-queue.md in the notes folder); start again when it is yours")
        if f"match running on {LEVELS[level]}" in out:
            return
        raise RuntimeError("could not start the level:\n" + out)
    raise RuntimeError("no free sandbox slot for 20 minutes")


def pause():
    for _ in range(4):
        if rt("screen").strip() == "paused":
            return
        rt("key", "esc")
        time.sleep(2)
    raise RuntimeError("could not pause: screen says " + rt("screen"))


def pose_of(out):
    m = re.findall(r"freecam set ([-\d. ]+)", out)
    d = re.search(r"centre distance ([-\d.]+)", out)
    return (m[-1].split() if m else None), (float(d.group(1)) if d else 0.0)


def make_poses(level):
    out = rt("freecam", "take")
    start, d = pose_of(out)
    d = d if d > 5 else 120.0
    poses = {}
    rt("freecam", "set", *start); rt("freecam", "orbit", 35, 8, d)
    poses["mid"] = pose_of(rt("freecam", "move", 0, 0, -0.7 * d))[0]
    rt("freecam", "set", *start)                 # straight towards the worm the start camera looks at
    poses["close"] = pose_of(rt("freecam", "move", 0, 0, 0.6 * d))[0]
    stored = json.loads(POSES.read_text()) if POSES.exists() else {}
    if level not in stored:
        rt("freecam", "set", *start); rt("freecam", "move", 0, 1.5 * d, -2.7 * d)
        stored[level] = pose_of(rt("freecam", "turn", 0, -12))[0]
        POSES.write_text(json.dumps(stored, indent=1))
        log(f"{level}: new stored wide pose {stored[level]}")
    poses["wide"] = stored[level]
    return poses


def shoot_views(poses, folder, level, preset, settle):
    for v in VIEWS:
        rt("freecam", "set", *poses[v])
        time.sleep(settle)
        png = folder / f"{level}_{v}_{preset}.png"
        out = rt("shot", png)
        log(f"  {png.name}: {out.splitlines()[-1] if out else '?'}")


def shoot(milestone, kits, levels, res):
    w, h = res
    scale = 0.5 if w > 960 else 1
    folder = OUT / milestone
    folder.mkdir(parents=True, exist_ok=True)
    for k in kits:
        if not (KITS / k / "wumrt_core.dll").exists():
            sys.exit(f"no kit {k}: python review.py kit {k}")
    over = {"hide_hud": 1, "scale": scale, "sandbox": 1}
    notes = []
    try:
        set_resolution(w, h)
        for level in levels:
            log(f"=== {level} ({LEVELS[level]})")
            run(["sandbox.py", "stop"])
            textures(False)
            # navigate with the first kit's shaders and the tracer on (rt.py level waits for the tracer's
            # mask); the texture folder is empty, so nothing is replaced; menus and the pause box are
            # read from the screen, so the HUD stays visible until paused
            place(kits[0], {**over, **STOCK_OFF, "hide_hud": 0, "rt": 1})
            start_level(level)
            time.sleep(10)                   # past the opening fly-in: the camera settles on the active worm
            pause()
            place_stock(kits[0], over)                       # the game's own shaders, rt off, HUD hidden
            rt("cg-reload")
            time.sleep(2)
            poses = make_poses(level)
            (folder / f"{level}_poses.json").write_text(json.dumps(poses, indent=1))
            log("  stock")
            shoot_views(poses, folder, level, "stock", 2)
            textures(True)
            for k in kits:
                log(f"  {k}")
                place(k, over)
                rt("reload-core"); rt("cg-reload"); rt("reload")
                time.sleep(4)                    # BVH rebuild, merged BVH, denoiser history
                stat = rt("rtstat", 2)
                cost = [l for l in stat.splitlines() if l.startswith("cost per frame")]
                log("  " + (cost[0] if cost else stat.splitlines()[0]))
                notes.append(f"{level} {k}: " + (cost[0] if cost else ""))
                shoot_views(poses, folder, level, k, 3)
                if k == kits[-1] and level == levels[0]:      # one frame with the HUD, to check it
                    place(k, {**over, "hide_hud": 0})
                    time.sleep(2)
                    rt("shot", folder / f"{level}_hud_{k}.png")
            run(["sandbox.py", "stop"])
    finally:
        run(["sandbox.py", "stop"])
        set_resolution(480, 270)
        textures(True)
        place(kits[-1], {"hide_hud": 0, "scale": 1, "sandbox": 1})
        log("sandbox stopped, back at 480x270, scale 1")
    (folder / "notes.txt").write_text("\n".join(notes) + "\n")


# ---- sheets -------------------------------------------------------------------------------
def label(milestone):
    from PIL import Image, ImageDraw, ImageFont
    folder = OUT / milestone
    jpg = folder / "jpg"
    jpg.mkdir(exist_ok=True)

    def font(size):
        for name in ("segoeuib.ttf", "arialbd.ttf"):
            try:
                return ImageFont.truetype(name, size)
            except OSError:
                pass
        return ImageFont.load_default()

    sets = sorted({p.name.rsplit("_", 1)[0] for p in folder.glob("*_stock.png")})
    for s in sets:
        presets = [p for p in ["stock", "baseline", "current"] if (folder / f"{s}_{p}.png").exists()]
        views = [Image.open(folder / f"{s}_{p}.png").convert("RGB") for p in presets]
        for p, im in zip(presets, views):
            im.save(jpg / f"{s}_{p}.jpg", quality=85)
            im.resize((im.width // 4, im.height // 4), Image.LANCZOS).save(jpg / f"{s}_{p}_thumb.jpg", quality=80)
        W, H = views[0].size
        bar, gap = max(20, H // 19), 8
        sheet = Image.new("RGB", (W * len(views) + gap * (len(views) - 1), H + bar), (255, 255, 255))
        draw = ImageDraw.Draw(sheet)
        f = font(max(11, H // 36))
        for i, (im, p) in enumerate(zip(views, presets)):
            x = i * (W + gap)
            sheet.paste(im, (x, 0))
            draw.rectangle([x, H, x + W - 1, H + bar - 1], fill=(20, 20, 20))
            draw.text((x + W // 2, H + bar // 2), f"{LABELS.get(p, p)}  ({s.replace('_', ', ')})", font=f,
                      fill=(255, 255, 255), anchor="mm")
        sheet.save(folder / f"{s}_sheet.png")
        sheet.save(jpg / f"{s}_sheet.jpg", quality=82)
        print(s, presets)


GALLERY = STREAM / "gallery"
LOOKS = {"apple": "green island", "dragon": "grey fortress", "grave": "night", "ice": "snow"}


def gallery(milestone, label, notes=""):
    """Add (or redo) a milestone in the gallery folder: compressed images + gallery.json."""
    from PIL import Image
    src = OUT / milestone
    img = GALLERY / "img" / milestone
    img.mkdir(parents=True, exist_ok=True)
    shutil.copy2(HERE / "review_gallery.html", GALLERY / "index.html")
    man_path = GALLERY / "gallery.json"
    man = json.loads(man_path.read_text()) if man_path.exists() else {"milestones": []}
    presets = [p for p in ["stock", "baseline", "current"] if list(src.glob(f"*_{p}.png"))]
    levels = []
    for lv in LEVELS:
        views = []
        for v in VIEWS:
            images = {}
            for p in presets:
                png = src / f"{lv}_{v}_{p}.png"
                if not png.exists():
                    continue
                im = Image.open(png).convert("RGB")
                full = im.resize((1600, round(im.height * 1600 / im.width)), Image.LANCZOS) if im.width > 1600 else im
                full.save(img / f"{lv}_{v}_{p}.jpg", quality=82, optimize=True)
                im.resize((480, round(im.height * 480 / im.width)), Image.LANCZOS).save(
                    img / f"{lv}_{v}_{p}_t.jpg", quality=78, optimize=True)
                images[p] = {"full": f"img/{milestone}/{lv}_{v}_{p}.jpg", "thumb": f"img/{milestone}/{lv}_{v}_{p}_t.jpg"}
            if images:
                views.append({"id": v, "images": images})
        if views:
            levels.append({"id": lv, "name": LEVELS[lv], "look": LOOKS.get(lv, ""), "views": views})
    if (src / "notes.txt").exists() and not notes:
        notes = (src / "notes.txt").read_text().strip()
    first = next(iter(src.glob("*_stock.png")), None)
    res = "x".join(map(str, Image.open(first).size)) if first else "?"
    entry = {"id": milestone, "label": label, "time": time.strftime("%Y-%m-%d %H:%M"), "presets": presets, "res": res,
             "levels": levels, "notes": notes}
    old = next((m for m in man["milestones"] if m["id"] == milestone), None)
    if old:
        entry["time"] = old["time"]                  # keep when it was shot
    man["milestones"] = [m for m in man["milestones"] if m["id"] != milestone] + [entry]
    man_path.write_text(json.dumps(man, indent=1))
    files = sorted(str(p.relative_to(GALLERY)).replace("\\", "/") for p in GALLERY.rglob("*") if p.is_file())
    size = sum((GALLERY / f).stat().st_size for f in files)
    print(f"gallery: {len(files)} files, {size / 1e6:.1f} MB in {GALLERY}")
    (GALLERY / "files.txt").write_text("\n".join(f for f in files if f != "index.html") + "\n")


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    if a[0] == "kit" and len(a) == 2:
        make_kit(a[1])
    elif a[0] == "shoot" and len(a) >= 2:
        levels, res, kits = list(LEVELS), (1280, 720), []
        rest = a[2:]
        i = 0
        while i < len(rest):
            if rest[i] == "--levels":
                levels = rest[i + 1].split(","); i += 2
            elif rest[i] == "--res":
                res = tuple(int(x) for x in rest[i + 1].split("x")); i += 2
            else:
                kits.append(rest[i]); i += 1
        shoot(a[1], kits or ["baseline", "current"], levels, res)
    elif a[0] == "label" and len(a) == 2:
        label(a[1])
    elif a[0] == "gallery" and len(a) >= 3:
        gallery(a[1], a[2], " ".join(a[3:]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
