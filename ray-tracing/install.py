"""
Install or remove the experimental ray-traced sun shadows in a Worms Ultimate Mayhem folder.

    python install.py install   --game DIR
    python install.py uninstall --game DIR
    python install.py status    --game DIR

install puts four things in the game folder, keeping the originals:
  glut32.dll            the harness's loader (the game's own becomes glut32_real.dll)
  wumrt_core.dll        the rest of the harness (the loader runs a copy from wumrt_core_copies/)
  CG/FixedFunction.cg   edited copies of two shaders (originals kept as *.orig-wumrt)
  CG/Landscape.cg
  wumrt_trace.comp, wumrt_filter.comp, wumrt_temporal.comp, wumrt.ini

Build the harness first (sh build.sh). Close the game before running this. Only Steam
build 1077 is supported; with any other exe the harness does nothing.
"""
import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HARNESS = HERE / "build" / "glut32.dll"
CORE = HERE / "build" / "wumrt_core.dll"
SHADERS = ["FixedFunction.cg", "Landscape.cg", "water.cg"]
EXTRA = ["wumrt_trace.comp", "wumrt_filter.comp", "wumrt_temporal.comp", "wumrt_atrous.comp", "wumrt_materials.ini"]
EXE_TIMESTAMP = 0x51828619
MARKS = (b"wumrt loader attached", b"wumrt attached")    # strings only our glut32.dll contains (now, before the split)
DEFAULT_INI = ("rt = 1\nsamples = 8\nradius = 1.5\nfilter = 2\nscale = 0.5\nmax_megapixels = 4\n"
               "sky = 0\nsky_reach = 80\nsky_strength = 0.85\nbounce = 0\naccumulate = 0.95\n"
               "gi = 0\ngi_sky = 1\ngi_bounce = 0.7\ngi_tint = 0.5\ndenoise = 3\nreflections = 0\nreflection_strength = 1\n")


def is_ours(path):
    if not path.exists():
        return False
    data = path.read_bytes()
    return any(m in data for m in MARKS)


def check_game(game):
    exe = game / "WormsMayhem.exe"
    if not exe.exists():
        sys.exit(f"WormsMayhem.exe not found in {game}")
    d = exe.read_bytes()[:1024]
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if struct.unpack_from("<I", d, pe + 8)[0] != EXE_TIMESTAMP:
        sys.exit("this is not the supported game build (Steam build 1077)")
    for f in ["glut32.dll"] + ["CG/" + s for s in SHADERS]:
        if not (game / f).exists():
            sys.exit(f"{f} is missing from the game folder")
    try:
        with open(exe, "r+b"):
            pass
    except PermissionError:
        sys.exit("the game is running; close it first")


def install(game):
    check_game(game)
    if not HARNESS.exists():
        sys.exit("build/glut32.dll not found; run build.sh first")
    dll, real = game / "glut32.dll", game / "glut32_real.dll"
    if not is_ours(dll):
        if real.exists():
            sys.exit("glut32_real.dll already exists but glut32.dll is not the harness; not touching it")
        shutil.copy2(dll, real)
    elif not real.exists():
        sys.exit("glut32.dll is the harness but glut32_real.dll is missing; restore the game's glut32.dll first")
    if not CORE.exists():
        sys.exit("build/wumrt_core.dll not found; run build.sh first")
    shutil.copy2(HARNESS, dll)
    shutil.copy2(CORE, game / "wumrt_core.dll")

    with tempfile.TemporaryDirectory() as tmp:
        for name in SHADERS:
            cur, orig = game / "CG" / name, game / "CG" / (name + ".orig-wumrt")
            if not orig.exists():
                shutil.copy2(cur, orig)
            shutil.copy2(orig, Path(tmp) / name)
        r = subprocess.run([sys.executable, str(HERE / "patch_shaders.py"), tmp], capture_output=True, text=True)
        if r.returncode:
            sys.exit("the shaders are not the expected originals:\n" + r.stdout + r.stderr)
        for name in SHADERS:
            shutil.copy2(Path(tmp) / name, game / "CG" / name)
    for name in EXTRA:
        shutil.copy2(HERE / name, game / name)
    ini = game / "wumrt.ini"
    if not ini.exists():
        ini.write_text(DEFAULT_INI)
    print(f"installed in {game}")
    print("settings: wumrt.ini (rt = 0 switches back to the game's own shadows); remove with: install.py uninstall")


def uninstall(game):
    check_game(game)
    dll, real = game / "glut32.dll", game / "glut32_real.dll"
    if is_ours(dll):
        if not real.exists():
            sys.exit("glut32_real.dll is missing; use Steam's 'Verify integrity of game files'")
        shutil.copy2(real, dll)
    if real.exists() and not is_ours(dll):
        real.unlink()
    for name in SHADERS:
        orig = game / "CG" / (name + ".orig-wumrt")
        if orig.exists():
            shutil.copy2(orig, game / "CG" / name)
            orig.unlink()
    for name in EXTRA + ["wumrt.ini", "wumrt.log", "wumrt_core.dll"]:
        (game / name).unlink(missing_ok=True)
    shutil.rmtree(game / "wumrt_core_copies", ignore_errors=True)
    print(f"removed from {game}")


def status(game):
    print("harness:", "installed" if is_ours(game / "glut32.dll") else "not installed")
    for name in SHADERS:
        edited = b"RtShadow" in (game / "CG" / name).read_bytes()
        print(f"CG/{name}:", "edited" if edited else "original")
    ini = game / "wumrt.ini"
    print("wumrt.ini:", ini.read_text().replace("\n", "  ").strip() if ini.exists() else "absent")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["install", "uninstall", "status"])
    ap.add_argument("--game", required=True, help="folder that holds WormsMayhem.exe")
    args = ap.parse_args()
    game = Path(args.game).resolve()
    {"install": install, "uninstall": uninstall, "status": status}[args.command](game)


if __name__ == "__main__":
    main()
