#!/usr/bin/env python3
"""
Camera stutter fix for Worms Ultimate Mayhem (Steam, WormsMayhem.exe build 1077).

    python wum_camfix.py apply   [--game DIR] [--fps-cap N]
    python wum_camfix.py restore [--game DIR]
    python wum_camfix.py status  [--game DIR]

apply    backs up WormsMayhem.exe and patches it
restore  puts the original exe back
status   says whether the exe is original or patched

--game DIR    the game folder (the one holding WormsMayhem.exe). Found automatically
              when this script sits in or under the game folder, or the game is in a
              Steam library.
--fps-cap N   upper frame-rate limit of the new frame limiter (default 250, so V-Sync
              sets the pace; 0 = no limit). Use e.g. 60 if V-Sync is forced off.

Needs only Python 3.8 or newer. Close the game before running it.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import struct
import sys
import zlib
from pathlib import Path

VERSION = "1.0.0"
EXE_NAME = "WormsMayhem.exe"
BACKUP_NAME = "WormsMayhem.exe.orig-camfix"
GAME_FOLDER = "WormsXHD"

ORIG_SHA1 = "a2e3a569a798cf2f90185757578d013ec7a13dd1"
IMAGE_BASE = 0x400000
NEW_RVA = 0x5B2000            # first free RVA after .rsrc; the hooks are linked for it
NEW_VA = IMAGE_BASE + NEW_RVA
SECTION_NAME = b".camfix"

# (address, expected original bytes, opcode, symbol the rel32 points at, trailing bytes)
SITES = [
    (0x68D4F0, "e8f9000000", 0xE8, "glue_frame_begin", ""),       # call 0x68d5ee
    (0x68D5AC, "e86e030000", 0xE8, "glue_frame_present", ""),     # call 0x68d91f
    (0x533C90, "568b3570c3", 0xE9, "glue_camera_tick", ""),       # ZCamUpdateFudgeService::Run
    # Engine frame limiter 0x4d918e..0x4d91ab: call ours, then jump over the rest.
    (0x4D918E, "e8701716002b05", 0xE8, "camfix_limiter", "eb17"),
    (0x795548, "ff1524508100", 0xE8, "glue_swap", "90"),          # call [SwapBuffers]
]

# Settings block at the start of camfix_log (see camfix.c).
LOG_MAGIC = 0x32474C43
LOG_ENABLED, LOG_LIMITER_ORIGINAL, LOG_LIMIT_US = 4, 12, 16
DEFAULT_FPS_CAP = 250

HERE = Path(__file__).resolve().parent

# Filled in by `patch.py dist` for the single-file release build. In a source checkout
# the hooks are read from build/ (made by `patch.py build`).
EMBEDDED = None


class Problem(Exception):
    pass


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def load_hooks():
    """Returns (blob bytes, virtual size, {symbol: VA})."""
    if EMBEDDED:
        blob = zlib.decompress(base64.b64decode(EMBEDDED["blob"]))
        return blob, EMBEDDED["size"], dict(EMBEDDED["symbols"])
    info, binf = HERE / "build" / "camfix.json", HERE / "build" / "camfix.bin"
    if not info.exists() or not binf.exists():
        raise Problem("the hooks are not built; run `patch.py build` first "
                      "(or use the release download, which has them built in)")
    meta = json.loads(info.read_text())
    return binf.read_bytes(), meta["size"], {k: int(v, 16) for k, v in meta["symbols"].items()}


def configure_hooks(blob, syms, fps_cap):
    """Returns the blob with the frame limiter's cap set."""
    off = syms["camfix_log"] - NEW_VA
    if struct.unpack_from("<I", blob, off)[0] != LOG_MAGIC:
        raise Problem("hook blob has an unexpected layout")
    if fps_cap != 0 and not 20 <= fps_cap <= 1000:
        raise Problem("--fps-cap must be 0 (no limit) or between 20 and 1000")
    b = bytearray(blob)
    struct.pack_into("<I", b, off + LOG_ENABLED, 1)
    struct.pack_into("<I", b, off + LOG_LIMITER_ORIGINAL, 0)
    struct.pack_into("<I", b, off + LOG_LIMIT_US, round(1_000_000 / fps_cap) if fps_cap else 0)
    return bytes(b)


class PE:
    """Just enough of the PE32 header layout to add a section."""

    def __init__(self, data):
        if data[:2] != b"MZ":
            raise Problem("not an executable")
        self.pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe:self.pe + 4] != b"PE\0\0":
            raise Problem("not a PE executable")
        self.nsec_off = self.pe + 6
        self.nsec = struct.unpack_from("<H", data, self.nsec_off)[0]
        opt_size = struct.unpack_from("<H", data, self.pe + 20)[0]
        self.opt = self.pe + 24
        if struct.unpack_from("<H", data, self.opt)[0] != 0x10B:
            raise Problem("not a 32-bit executable")
        self.image_base = struct.unpack_from("<I", data, self.opt + 28)[0]
        self.sec_align, self.file_align = struct.unpack_from("<II", data, self.opt + 32)
        self.size_of_image_off = self.opt + 56
        self.size_of_image, self.size_of_headers = struct.unpack_from("<II", data, self.opt + 56)
        self.checksum_off = self.opt + 64
        self.table = self.opt + opt_size
        self.sections = []      # (name, virtual size, rva, raw size, raw pointer)
        for i in range(self.nsec):
            name, vsize, rva, rsize, rptr = struct.unpack_from("<8sIIII", data, self.table + 40 * i)
            self.sections.append((name.rstrip(b"\0"), vsize, rva, rsize, rptr))

    def offset(self, rva):
        for _, vsize, sec_rva, rsize, rptr in self.sections:
            if sec_rva <= rva < sec_rva + rsize:
                return rva - sec_rva + rptr
        raise Problem(f"address {rva + self.image_base:#x} is not in the file")


def pe_checksum(data, checksum_off):
    d = bytearray(data)
    d[checksum_off:checksum_off + 4] = b"\0\0\0\0"
    if len(d) % 2:
        d.append(0)
    s = sum(struct.unpack(f"<{len(d) // 2}H", d))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (s + len(data)) & 0xFFFFFFFF


def patch_bytes(orig, blob, vsize, syms):
    """Returns the patched image: original + .camfix section + redirected call sites."""
    d = bytearray(orig)
    pe = PE(orig)
    last = pe.sections[-1]
    if pe.image_base != IMAGE_BASE or pe.size_of_image != NEW_RVA:
        raise Problem("unexpected image layout")
    if last[4] + last[3] != len(orig):
        raise Problem("file has data after the last section; not patching")

    # New section header goes right after the existing ones.
    hdr_off = pe.table + 40 * pe.nsec
    if hdr_off + 40 > pe.size_of_headers or any(d[hdr_off:hdr_off + 40]):
        raise Problem("no room for another section header")
    raw_size = (len(blob) + pe.file_align - 1) // pe.file_align * pe.file_align
    virt_size = (vsize + pe.sec_align - 1) // pe.sec_align * pe.sec_align
    d[hdr_off:hdr_off + 40] = struct.pack(
        "<8sIIIIIIHHI", SECTION_NAME, vsize, NEW_RVA, raw_size, len(d), 0, 0, 0, 0,
        0xE0000060)  # code | initialized data | execute | read | write
    d += blob.ljust(raw_size, b"\0")
    struct.pack_into("<H", d, pe.nsec_off, pe.nsec + 1)
    struct.pack_into("<I", d, pe.size_of_image_off, NEW_RVA + virt_size)

    for va, expect, opcode, name, tail in SITES:
        expect = bytes.fromhex(expect)
        off = pe.offset(va - IMAGE_BASE)
        if bytes(d[off:off + len(expect)]) != expect:
            raise Problem(f"bytes at {va:#x} are not the expected original")
        rel = (syms[name] - (va + 5)) & 0xFFFFFFFF
        new = bytes([opcode]) + struct.pack("<I", rel) + bytes.fromhex(tail)
        if len(new) != len(expect):
            raise Problem(f"patch for {va:#x} has the wrong length")
        d[off:off + len(new)] = new

    struct.pack_into("<I", d, pe.checksum_off, pe_checksum(d, pe.checksum_off))
    return bytes(d)


def steam_libraries():
    roots = []
    if os.name == "nt":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                roots.append(Path(winreg.QueryValueEx(k, "SteamPath")[0]))
        except OSError:
            pass
        roots.append(Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Steam")
    else:
        home = Path.home()
        roots += [home / ".steam" / "steam", home / ".local" / "share" / "Steam",
                  home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
    libs = []
    for root in roots:
        libs.append(root)
        vdf = root / "steamapps" / "libraryfolders.vdf"
        try:
            for p in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(errors="replace")):
                libs.append(Path(p.replace("\\\\", "\\")))
        except OSError:
            pass
    return libs


def find_game(explicit):
    if explicit:
        game = Path(explicit)
        if not (game / EXE_NAME).exists():
            raise Problem(f"{EXE_NAME} not found in {game}")
        return game
    candidates = [HERE, *HERE.parents, Path.cwd()]
    candidates += [lib / "steamapps" / "common" / GAME_FOLDER for lib in steam_libraries()]
    for c in candidates:
        try:
            if (c / EXE_NAME).exists():
                return c
        except OSError:
            pass
    raise Problem(f"could not find the game; pass --game with the folder that holds {EXE_NAME}")


def original_bytes(exe, backup):
    data = exe.read_bytes()
    if sha1(data) == ORIG_SHA1:
        return data
    if backup.exists():
        data = backup.read_bytes()
        if sha1(data) == ORIG_SHA1:
            return data
    raise Problem(f"{EXE_NAME} is not the supported build (Steam build 1077, 2013-05-02) and no "
                  f"valid backup exists.\nIts SHA-1 is {sha1(exe.read_bytes())}; expected {ORIG_SHA1}.")


def write_exe(exe, data):
    try:
        exe.write_bytes(data)
    except PermissionError:
        raise Problem(f"could not write {exe.name}; close the game and try again") from None


def cmd_apply(game, fps_cap):
    exe, backup = game / EXE_NAME, game / BACKUP_NAME
    orig = original_bytes(exe, backup)
    blob, vsize, syms = load_hooks()
    patched = patch_bytes(orig, configure_hooks(blob, syms, fps_cap), vsize, syms)
    if not backup.exists():
        backup.write_bytes(orig)
    elif sha1(backup.read_bytes()) != ORIG_SHA1:
        raise Problem(f"{backup.name} exists but is not the original exe; move it away first")
    write_exe(exe, patched)
    cap = f"{fps_cap} fps cap" if fps_cap else "no fps cap"
    print(f"patched {exe} ({len(SITES)} call sites redirected, {cap})")
    print(f"original saved as {backup.name}; undo with: {Path(sys.argv[0]).name} restore")


def cmd_restore(game):
    exe, backup = game / EXE_NAME, game / BACKUP_NAME
    if sha1(exe.read_bytes()) == ORIG_SHA1:
        print("exe is already the original")
        return
    if not backup.exists() or sha1(backup.read_bytes()) != ORIG_SHA1:
        raise Problem("no valid backup; use Steam's 'Verify integrity of game files' instead")
    write_exe(exe, backup.read_bytes())
    print(f"original exe restored in {game}")


def cmd_status(game):
    exe, backup = game / EXE_NAME, game / BACKUP_NAME
    data = exe.read_bytes()
    print(f"game folder: {game}")
    if sha1(data) == ORIG_SHA1:
        print("original (unpatched)")
        return
    try:
        sec = next((s for s in PE(data).sections if s[0] == SECTION_NAME), None)
    except Problem:
        sec = None
    if not sec:
        print(f"unknown build (SHA-1 {sha1(data)})")
    else:
        line = "patched with camfix"
        try:
            _, _, syms = load_hooks()
            off = sec[4] + syms["camfix_log"] - NEW_VA
            magic, enabled, _, lim_orig, limit_us = struct.unpack_from("<5I", data, off)
            if magic == LOG_MAGIC:
                cap = f"{round(1_000_000 / limit_us)} fps cap" if limit_us else "no fps cap"
                line += (f" (camera interpolation {'on' if enabled else 'off'}, "
                         f"{'engine limiter' if lim_orig else cap})")
            else:
                line += " (a different version of it)"
        except (Problem, struct.error, KeyError):
            pass
        print(line)
    print("backup:", "present" if backup.exists() else "missing")


def main(argv=None):
    ap = argparse.ArgumentParser(description=f"Worms Ultimate Mayhem camera stutter fix {VERSION}")
    ap.add_argument("command", choices=["apply", "restore", "status"])
    ap.add_argument("--game", help=f"folder that holds {EXE_NAME}")
    ap.add_argument("--fps-cap", type=int, default=DEFAULT_FPS_CAP,
                    help=f"frame limiter cap for apply (default {DEFAULT_FPS_CAP}, 0 = none)")
    args = ap.parse_args(argv)
    try:
        game = find_game(args.game)
        if args.command == "apply":
            cmd_apply(game, args.fps_cap)
        elif args.command == "restore":
            cmd_restore(game)
        else:
            cmd_status(game)
    except Problem as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
