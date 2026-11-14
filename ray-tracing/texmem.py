"""
32-bit memory of this copy's sandbox game, and its large-address-aware flag.

    python texmem.py mem [LABEL]     virtual / private / peak memory of the sandbox game, appended
                                     with LABEL to out/texmem.log
    python texmem.py laa [on|off]    show or set IMAGE_FILE_LARGE_ADDRESS_AWARE on the SANDBOX exe
                                     copy (never the real game's exe; the game must be stopped)

A 32-bit exe without the flag gets 2 GB of address space; with it, 4 GB on 64-bit Windows.
The driver keeps copies of textures in the game's address space, so big replacement textures
can run it out (see the README's "Textures").
"""
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SANDBOX = (HERE.parent.parent / "rt-sandbox").resolve()
EXE = SANDBOX / "WormsMayhem.exe"
GAME = (HERE.parent.parent.parent / "WormsMayhem.exe").resolve() if (HERE.parent.parent.name == "WormsXHD") else None


def mem(label):
    ps = ("Get-Process WormsMayhem -ErrorAction SilentlyContinue | ForEach-Object { "
          "'{0}|{1}|{2}|{3}|{4}' -f $_.Path,$_.VirtualMemorySize64,$_.PrivateMemorySize64,$_.PeakVirtualMemorySize64,$_.WorkingSet64 }")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = line.strip().split("|")
        if len(f) == 5 and Path(f[0]).resolve() == EXE:
            v, p, pk, ws = (int(x) / 2**20 for x in f[1:])
            msg = (f"{time.strftime('%H:%M:%S')} {label}: virtual {v:.0f} MB, private {p:.0f} MB, "
                   f"peak virtual {pk:.0f} MB, working set {ws:.0f} MB, LAA {'on' if laa_flag() else 'off'}")
            print(msg)
            (HERE / "out").mkdir(exist_ok=True)
            with open(HERE / "out" / "texmem.log", "a") as log:
                print(msg, file=log)
            return
    print("this copy's sandbox game is not running")


def header():
    d = bytearray(EXE.read_bytes()[:4096])
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0", "not a PE file"
    return pe + 22


def laa_flag():
    off = header()
    with open(EXE, "rb") as f:
        f.seek(off)
        return struct.unpack("<H", f.read(2))[0] & 0x20 != 0


def laa(state):
    if state is None:
        print(f"{EXE}: large address aware {'on' if laa_flag() else 'off'}")
        return
    assert "rt-sandbox" in EXE.parts and EXE.parent.name == "rt-sandbox", "only a sandbox exe copy"
    if GAME is not None:
        assert EXE != GAME, "refusing to touch the real game's exe"
    off = header()
    with open(EXE, "r+b") as f:
        f.seek(off)
        c = struct.unpack("<H", f.read(2))[0]
        c = c | 0x20 if state == "on" else c & ~0x20
        f.seek(off)
        f.write(struct.pack("<H", c))
    print(f"{EXE}: large address aware {'on' if laa_flag() else 'off'}")


def main():
    a = sys.argv[1:]
    if a and a[0] == "mem":
        mem(" ".join(a[1:]) or "now")
    elif a and a[0] == "laa":
        laa(a[1] if len(a) > 1 else None)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
