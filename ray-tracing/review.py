"""
Repeatable, labelled before/after review sets: a few levels x three views x presets, all
presets of a level shot in the SAME paused match (same worms, same camera, same frame), so
only the rendering differs.

    python review.py kit NAME [--main]   snapshot this copy's build (or with --main: main's build, the main
                                         sandbox's patched CG and main's shaders) as a kit; settings always
                                         from the main sandbox's wumrt.ini
    python review.py shoot MILESTONE [KIT ...] [--levels a,b] [--res WxH]
                                         shoot stock + the kits (default: baseline current) into
                                         main ray-tracing/captures/review/MILESTONE/
    python review.py label MILESTONE     side-by-side sheets (stock | kits) + compressed JPEGs

Presets:
  stock     the game's own CG shaders, rt = 0, no shells/scatter/relief, no replacement textures
  KIT       a kit made with `review.py kit NAME`: "baseline" (main before the streams merged) and
            "current" (latest main: `sh stream.sh sync review`, then `python review.py kit current`)

How one level is shot: the review sandbox starts (480x270, the only size sandboxes may use; scale 1, hide_hud = 1) with its
wumrt_textures pointing at an empty folder and the stock files in place; the match is paused
(checked on screen); the views are set with the free camera; stock is shot; then textures are
pointed back at the main sandbox's and reloaded, and for each kit its files are copied in and
reload-core, cg-reload and reload swap them into the running game before its shots. The game
is stopped after each level (replacement textures cannot be taken out of a running game).

Views: "wide" is a stored free-camera pose per level (review_poses.json, made on the first
shoot from the match's start camera); "mid" and "close" are taken relative to the match's
start camera, which looks at the active worm (worms are placed at random, so these differ
between milestones; within one milestone all presets share them). "hero" swings 65 degrees round
that worm and comes in low and nearly level, the worm left of centre, the level behind it (worm in front, background far: depth of field,
atmosphere, rim light).

Gallery: a milestone folder may hold columns.json ({"stock": "...", "baseline": "...",
"current": "..."}); the page shows it on top as what each column contains.
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
VIEWS = ["wide", "mid", "close", "hero"]
COMPS = ["wumrt_trace.comp", "wumrt_filter.comp", "wumrt_temporal.comp", "wumrt_atrous.comp"]
INIS = ["wumrt_materials.ini", "wumrt_surfaces.ini"]
STOCK_OFF = {"rt": 0, "shells": 0, "scatter": 0, "relief": 0, "sky": 0, "gi": 0, "reflections": 0,
             "specular": 0, "bump": 0, "post": 0, "fx": 0}
LABELS = {"stock": "Stock game", "baseline": "Baseline (before the streams)", "current": "Current main",
          "late": "Late additions"}
PRESETS = list(LABELS)                  # column order
EXTRA_VIEWS = ["shore", "smoke"]        # shot only where available (see SHORE, smoke())
SHORE = {"apple": ["704.05", "90.08", "-270.53", "-145.00", "-4.97"]}   # cliffs, sand, sea (m1 mid pose)


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
def make_kit(name, from_main=False):
    """from_main: main's own build, main sandbox's patched CG and main's shaders (latest main)."""
    src, sb = (MAIN, MAIN_SB) if from_main else (HERE, SB)
    k = KITS / name
    if k.exists():
        shutil.rmtree(k)
    (k / "CG").mkdir(parents=True)
    shutil.copy2(src / "build" / "wumrt_core.dll", k)
    for f in (sb / "CG").iterdir():
        shutil.copy2(f, k / "CG")
    for f in [p.name for p in src.glob("*.comp")] + INIS:
        shutil.copy2(src / f, k)
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
    for f in [p.name for p in k.glob("*.comp")] + INIS:
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
    for attempt in range(80):
        env = dict(os.environ, LEVEL=LEVELS[level])
        p = subprocess.run(["sh", "go.sh"], cwd=HERE, capture_output=True, text=True, timeout=400, env=env)
        out = p.stdout + p.stderr
        if "games already run" in out:          # all game places in use (slot not enforced): wait
            if attempt % 8 == 0:
                log("all sandbox game places in use; waiting")
            time.sleep(15)
            continue
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
    rt("freecam", "set", *start)                 # hero: swing 65 degrees round the worm, nearly level,
    rt("freecam", "orbit", 65, 9, d)             # then in to ~0.4 d with the worm left of centre: low,
    poses["hero"] = pose_of(rt("freecam", "move", 0.07 * d, 0, 0.62 * d))[0]   # level far behind
    stored = json.loads(POSES.read_text()) if POSES.exists() else {}
    if level not in stored:
        rt("freecam", "set", *start); rt("freecam", "move", 0, 1.5 * d, -2.7 * d)
        stored[level] = pose_of(rt("freecam", "turn", 0, -12))[0]
        POSES.write_text(json.dumps(stored, indent=1))
        log(f"{level}: new stored wide pose {stored[level]}")
    poses["wide"] = stored[level]
    if level in SHORE:
        poses["shore"] = SHORE[level]
    return poses


def shoot_views(poses, folder, level, preset, settle):
    for v in [v for v in VIEWS + EXTRA_VIEWS if v in poses]:
        rt("freecam", "set", *poses[v])
        time.sleep(settle)
        png = folder / f"{level}_{v}_{preset}.png"
        out = rt("shot", png)
        log(f"  {png.name}: {out.splitlines()[-1] if out else '?'}" + ("  BLACK FRAME" if black(png) else ""))


def black(png):
    from PIL import Image, ImageStat
    return max(ImageStat.Stat(Image.open(png).convert("L")).extrema[0]) < 8


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
            for attempt in (1, 2):           # a flaky screen read or a black start gets one more go
                try:
                    start_level(level)
                    time.sleep(10)           # past the opening fly-in: the camera settles on the active worm
                    pause()
                    probe = folder / f"{level}_probe.png"
                    rt("shot", probe)
                    if black(probe):
                        raise RuntimeError("the game shows a black frame")
                    probe.unlink()
                    break
                except RuntimeError as e:
                    if attempt == 2 or "popped up" in str(e) or "slot" in str(e):
                        raise
                    log(f"  {e}; starting the level again")
                    run(["sandbox.py", "stop"])
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


def smoke(milestone, kits, level, res=(480, 270), timeout=300):
    """Start LEVEL with the last kit, let the match play until an explosion (fx_watch.py: fire-coloured
    pixels jump), which pauses it; then shoot the game's own view of that frozen moment with each kit
    (view "smoke"). No stock frame: textures cannot be taken out of a running game."""
    w, h = res
    scale = 0.5 if w > 960 else 1
    folder = OUT / milestone
    folder.mkdir(parents=True, exist_ok=True)
    over = {"hide_hud": 1, "scale": scale, "sandbox": 1}
    try:
        set_resolution(w, h)
        run(["sandbox.py", "stop"])
        textures(True)
        place(kits[-1], {**over, "hide_hud": 0, "fx_log": 1})   # HUD visible while menus are read;
        # fx_log makes the harness log GLOW for glowing effect draws (fire, explosions)
        start_level(level)
        code, out = run(["fx_watch.py", str(folder / f"{level}_smoke_watch"), "--log", "GLOW", "--timeout", str(timeout)],
                        timeout=timeout + 60)
        log("  " + " | ".join(out.strip().splitlines()[-2:]))
        if rt("screen").strip() != "paused":
            raise RuntimeError("no explosion caught (or the game did not pause)")
        for k in kits:
            place(k, over)
            rt("reload-core"); rt("cg-reload"); rt("reload")
            time.sleep(4)
            png = folder / f"{level}_smoke_{k}.png"
            out = rt("shot", png)
            if black(png):                       # hide_hud = 1 gives a black frame in some paused states
                place(k, {**over, "hide_hud": 0})
                time.sleep(2)
                out = rt("shot", png)
                log("  (HUD hidden gave a black frame: shot with the HUD shown)")
            log(f"  {png.name}: {out.splitlines()[-1] if out else '?'}")
    finally:
        run(["sandbox.py", "stop"])
        set_resolution(480, 270)
        place(kits[-1], {"hide_hud": 0, "scale": 1, "sandbox": 1})
        log("sandbox stopped, back at 480x270, scale 1")


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

    sets = sorted({p.name.rsplit("_", 1)[0] for p in folder.glob("*.png")
                   if p.stem.rsplit("_", 1)[-1] in PRESETS and "_hud_" not in p.name})
    for s in sets:
        presets = [p for p in PRESETS if (folder / f"{s}_{p}.png").exists()]
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
LOOKS = {"apple": "green island", "dragon": "green castle island", "grave": "night", "ice": "snow"}


def gallery(milestone, label, notes=""):
    """Add (or redo) a milestone in the gallery folder: compressed images + gallery.json."""
    from PIL import Image
    src = OUT / milestone
    img = GALLERY / "img" / milestone
    img.mkdir(parents=True, exist_ok=True)
    shutil.copy2(HERE / "review_gallery.html", GALLERY / "index.html")
    man_path = GALLERY / "gallery.json"
    man = json.loads(man_path.read_text()) if man_path.exists() else {"milestones": []}
    presets = [p for p in PRESETS if list(src.glob(f"*_{p}.png"))]
    levels = []
    for lv in LEVELS:
        views = []
        for v in VIEWS + EXTRA_VIEWS:
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
    columns = json.loads((src / "columns.json").read_text()) if (src / "columns.json").exists() else None
    if (src / "notes.txt").exists() and not notes:
        notes = (src / "notes.txt").read_text().strip()
    first = next(iter(src.glob("*_stock.png")), None)
    res = "x".join(map(str, Image.open(first).size)) if first else "?"
    entry = {"id": milestone, "label": label, "time": time.strftime("%Y-%m-%d %H:%M"), "presets": presets, "res": res,
             "levels": levels, "notes": notes}
    if columns:
        entry["columns"] = columns
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
    if a[0] == "kit" and len(a) in (2, 3):
        make_kit(a[1], from_main=a[2:] == ["--main"])
    elif a[0] == "shoot" and len(a) >= 2:
        levels, res, kits = list(LEVELS), (480, 270), []     # user 18:31: sandboxes at 480x270 only
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
    elif a[0] == "smoke" and len(a) >= 4:          # smoke MILESTONE LEVEL KIT [KIT...]
        smoke(a[1], a[3:], a[2])
    elif a[0] == "label" and len(a) == 2:
        label(a[1])
    elif a[0] == "gallery" and len(a) >= 3:
        gallery(a[1], a[2], " ".join(a[3:]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
