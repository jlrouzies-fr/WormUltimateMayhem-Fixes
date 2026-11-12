"""
Developer front end for the camera stutter fix: builds the hooks from source, then
hands over to wum_camfix.py.

    python patch.py build                 compile and link the hooks into build/
    python patch.py dist                  build, then write the release files into dist/
    python patch.py apply [--game DIR] [--fps-cap N]    build, then patch the game
    python patch.py restore|status [--game DIR]         same as wum_camfix.py

Building needs clang and lld-link (LLVM) and the pefile Python package. Patching itself
(wum_camfix.py) needs neither.
"""
import base64
import json
import shutil
import subprocess
import sys
import textwrap
import zipfile
import zlib
from pathlib import Path

import wum_camfix as core

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
DIST = HERE / "dist"
INFO = BUILD / "camfix.json"
BLOB = BUILD / "camfix.bin"

LINK_BASE = 0x9B0000          # blob is linked as its own image; /ALIGN:0x2000 puts
LINK_ALIGN = 0x2000           # its single section at LINK_BASE + 0x2000 == NEW_VA
EXPORTS = ["glue_frame_begin", "glue_frame_present", "glue_camera_tick", "camfix_limiter",
           "glue_swap", "camfix_state,DATA", "camfix_log,DATA"]


def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    for base in (r"C:\Program Files\LLVM\bin", "/usr/bin", "/usr/local/bin"):
        for cand in (Path(base) / (name + ".exe"), Path(base) / name):
            if cand.exists():
                return str(cand)
    sys.exit(f"{name} not found; install LLVM and add its bin folder to PATH")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("command failed: " + " ".join(map(str, cmd)) + "\n" + r.stdout + r.stderr)
    return r.stdout + r.stderr


def build():
    """Compile and link the hooks. Returns (blob bytes, virtual size, {symbol: VA})."""
    import pefile

    BUILD.mkdir(exist_ok=True)
    clang, lld = find_tool("clang"), find_tool("lld-link")
    cobj, sobj, img = BUILD / "camfix.obj", BUILD / "glue.obj", BUILD / "camfix_image.exe"
    tgt = "--target=i686-pc-windows-msvc"
    run([clang, tgt, "-O2", "-ffreestanding", "-fno-builtin", "-nostdinc",
         "-fno-stack-protector", "-mno-stack-arg-probe", "-Wall",
         "-c", str(HERE / "camfix.c"), "-o", str(cobj)])
    run([clang, tgt, "-c", str(HERE / "glue.s"), "-o", str(sobj)])
    out = run([lld, "/NOLOGO", "/NODEFAULTLIB", "/SUBSYSTEM:WINDOWS", "/ENTRY:glue_frame_begin",
               "/FIXED", f"/BASE:{LINK_BASE:#x}", f"/ALIGN:{LINK_ALIGN:#x}", "/FILEALIGN:0x200",
               "/SAFESEH:NO", "/IGNORE:4281",
               "/MERGE:.rdata=.text", "/MERGE:.data=.text", "/SECTION:.text,ERW",
               *("/EXPORT:" + e for e in EXPORTS),
               f"/OUT:{img}", str(sobj), str(cobj)])
    out = "\n".join(l for l in out.splitlines() if "/align specified without /driver" not in l)
    if out.strip():
        print(out.strip())

    pe = pefile.PE(str(img))
    if len(pe.sections) != 1:
        sys.exit("hook image should have exactly one section, has: "
                 + ", ".join(s.Name.rstrip(b"\0").decode() for s in pe.sections))
    sec = pe.sections[0]
    va = pe.OPTIONAL_HEADER.ImageBase + sec.VirtualAddress
    if va != core.NEW_VA:
        sys.exit(f"hook image section is at {va:#x}, expected {core.NEW_VA:#x}")
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT") or hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        sys.exit("hook image has imports or relocations; it must be self-contained")
    vsize = max(sec.Misc_VirtualSize, sec.SizeOfRawData)
    blob = sec.get_data()[:sec.SizeOfRawData].ljust(vsize, b"\0")
    syms = {e.name.decode(): pe.OPTIONAL_HEADER.ImageBase + e.address
            for e in pe.DIRECTORY_ENTRY_EXPORT.symbols}
    for _, _, _, name, _ in core.SITES:
        if name not in syms:
            sys.exit(f"missing symbol {name}")
    pe.close()
    BLOB.write_bytes(blob)
    INFO.write_text(json.dumps({"section_va": core.NEW_VA, "size": vsize,
                                "symbols": {k: hex(v) for k, v in syms.items()}}, indent=2))
    return blob, vsize, syms


BAT = """@echo off
setlocal
cd /d "%~dp0"
set PY=
where py >nul 2>nul && set PY=py -3
if not defined PY (where python >nul 2>nul && set PY=python)
if not defined PY (
  echo Python 3 is needed. Install it from https://www.python.org/downloads/ and run this again.
  pause
  exit /b 1
)
%PY% wum_camfix.py {command} %*
pause
"""


def dist():
    """Writes dist/wum_camfix.py (hooks built in) and the release zip."""
    blob, vsize, syms = build()
    packed = base64.b64encode(zlib.compress(blob, 9)).decode()
    lines = ["EMBEDDED = {", f'    "size": {vsize},',
             '    "symbols": {' + ", ".join(f'"{k}": {v:#x}' for k, v in sorted(syms.items())) + "},",
             '    "blob": (']
    lines += [f'        "{chunk}"' for chunk in textwrap.wrap(packed, 88)]
    lines += ["    ),", "}"]
    src = (HERE / "wum_camfix.py").read_text(encoding="utf-8")
    marker = "EMBEDDED = None\n"
    if src.count(marker) != 1:
        sys.exit("wum_camfix.py: EMBEDDED marker not found exactly once")
    DIST.mkdir(exist_ok=True)
    single = DIST / "wum_camfix.py"
    single.write_text(src.replace(marker, "\n".join(lines) + "\n"), encoding="utf-8", newline="\n")
    readme = HERE / "RELEASE-README.txt"
    name = f"wum-camera-stutter-fix-{core.VERSION}.zip"
    with zipfile.ZipFile(DIST / name, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(single, "wum_camfix.py")
        z.writestr("apply-fix.bat", BAT.format(command="apply").replace("\n", "\r\n"))
        z.writestr("restore-original.bat", BAT.format(command="restore").replace("\n", "\r\n"))
        if readme.exists():
            z.write(readme, "README.txt")
    print(f"wrote {single} and {DIST / name}")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "build":
        print("built", {k: hex(v) for k, v in build()[2].items()})
    elif cmd == "dist":
        dist()
    elif cmd in ("apply", "restore", "status"):
        if cmd == "apply":
            build()
        core.main()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
