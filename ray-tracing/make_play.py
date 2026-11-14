"""Make or refresh WormsXHD/rt-play: a copy of the sandbox you can PLAY, with the latest look.

    py -3 make_play.py [SCALE]      SCALE = share of the screen the tracer works at (default 0.25)

rt-play = main's rt-sandbox as it is now (exe with the large-address flag, loader, core, compute
shaders, patched CG, ini files) in normal play mode (no `sandbox = 1`: the game keeps its own
window, mouse, keyboard and sound), at the resolution of the real game's local.cfg in full screen,
with the game's SSAA off (the mod does its own AA) and the tracer at SCALE of that resolution.
Data, EFMV, speech and the texture set are junctions, not copies. The real game folder is only
read (its configs), never written. Run it again after merges to get the newest look; it refuses
while rt-play's game runs, and it overwrites rt-play's wumrt.ini (edit PLAY_SETTINGS below
instead). Launch: double-click rt-play/WormsMayhem.exe with Steam running. F9 toggles tracing.
"""
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent                    # WormsXHD (main copy only: not from a stream)
SRC = ROOT / "rt-sandbox"
DST = ROOT / "rt-play"
FILES = ["WormsMayhem.exe", "MemoryBufferedFilter.dll", "cg.dll", "cgGL.dll", "fmod_event.dll",
         "fmodex.dll", "glut32.dll", "glut32_real.dll", "steam_api.dll", "wumrt_core.dll",
         "lang_eng", "steam_appid.txt"]
PATTERNS = ["wumrt_*.comp", "wumrt_*.ini"]       # wumrt.ini itself is written below
LINKS = {"Data": ROOT / "Data", "EFMV": ROOT / "EFMV", "speech": ROOT / "speech",
         "wumrt_textures": SRC / "wumrt_textures"}
PLAY_SETTINGS = {"sandbox": "0", "frame_timer": "0"}     # plus scale from the command line


def running_from(folder):
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process WormsMayhem -ErrorAction SilentlyContinue | ForEach-Object { $_.Path }"],
                         capture_output=True, text=True).stdout
    return [p for p in out.split() if Path(p).resolve().parent == folder.resolve()]


def is_link(p):
    return p.is_symlink() or getattr(os.path, "isjunction", lambda _: False)(p) \
        or (p.is_dir() and p.resolve() != p.absolute())


def junction(name, target):
    link = DST / name
    if is_link(link):
        if link.resolve() == target.resolve():
            return
        os.rmdir(link)              # removes the junction only, never what it points to
    elif link.exists():
        sys.exit(f"{link} exists and is not a link; remove it by hand")
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)


def play_ini(scale):
    keep = PLAY_SETTINGS | {"scale": f"{scale:g}"}
    lines = [f"# rt-play: written by make_play.py {time.strftime('%Y-%m-%d %H:%M')} from rt-sandbox/wumrt.ini.",
             "# The next refresh overwrites this file: change PLAY_SETTINGS in make_play.py instead.",
             "# It is re-read while the game runs, so a change here applies at once."]
    seen = set()
    for line in (SRC / "wumrt.ini").read_text().splitlines():
        m = re.match(r"\s*([A-Za-z_]+)\s*=", line)
        if m and m.group(1) in keep:
            if m.group(1) in seen:
                continue
            line = f"{m.group(1)} = {keep[m.group(1)]}"
            seen.add(m.group(1))
        lines.append(line)
    lines += [f"{k} = {v}" for k, v in keep.items() if k not in seen]
    return "\n".join(lines) + "\n"


def play_cfgs():
    """Default.cfg from the sandbox's (menus), resolution and options from the real local.cfg."""
    real = (ROOT / "local.cfg").read_text().split()
    opts = [o for o in real if not re.match(r"/(SSAA:|NOSSAA|WIN$|XPOS:|YPOS:|CONFIG:)", o, re.I)]
    w = next((o[3:] for o in opts if o.upper().startswith("/W:")), "1920")
    h = next((o[3:] for o in opts if o.upper().startswith("/H:")), "1080")
    local = " ".join(opts + ["/NOSSAA"])
    default = re.sub(r"/W:\d+ /H:\d+", f"/W:{w} /H:{h}", (SRC / "Default.cfg").read_text())
    return default, local, f"{w}x{h}"


def main():
    scale = float(sys.argv[1]) if len(sys.argv) > 1 else 0.25
    if not 0.1 <= scale <= 1:
        sys.exit("SCALE must be between 0.1 and 1")
    if not (SRC / "wumrt.ini").exists() or HERE.parent.parent.parent.name.lower() == "streams":
        sys.exit("run this from main's ray-tracing folder (WormsXHD/WormUltimateMayhem-Fixes/ray-tracing)")
    if running_from(DST):
        sys.exit("rt-play's game is running; quit it first")
    DST.mkdir(exist_ok=True)
    for name in FILES:
        shutil.copy2(SRC / name, DST / name)
    for pattern in PATTERNS:
        for f in SRC.glob(pattern):
            if f.name != "wumrt.ini":
                shutil.copy2(f, DST / f.name)
    if (DST / "CG").exists():
        shutil.rmtree(DST / "CG")
    shutil.copytree(SRC / "CG", DST / "CG")
    for name, target in LINKS.items():
        junction(name, target)
    (DST / "wumrt.ini").write_text(play_ini(scale), newline="\n")     # LF like the sandbox's
    default, local, size = play_cfgs()
    (DST / "Default.cfg").write_text(default)
    (DST / "local.cfg").write_text(local + "\n")
    print(f"rt-play ready: {size} full screen, tracer at {scale:g} of it, play mode. "
          f"Start {DST / 'WormsMayhem.exe'} with Steam running.")


if __name__ == "__main__":
    main()
