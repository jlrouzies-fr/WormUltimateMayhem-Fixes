"""
Talk to the wumrt harness inside the running sandbox game.

    python rt.py status
    python rt.py shot OUT.png              screenshot of the next presented frame
    python rt.py key NAME [MS]             hold a key for MS milliseconds (default 120)
    python rt.py keys NAME,NAME,... [GAP]  press keys one after another, GAP ms apart (default 400)
    python rt.py capture OUT.bin           dump every draw call of the next frame
    python rt.py rt on|off [SAMPLES RADIUS_DEG [FILTER]]   real-time traced sun shadows; FILTER = blur radius in pixels (default 2)
    python rt.py sky RAYS [REACH STRENGTH BOUNCE]  sky occlusion: rays per pixel (0 = off), reach in world units,
                                           strength 0..1; BOUNCE > 0 adds sunlight reflected by nearby surfaces
    python rt.py scale FACTOR              trace the mask at FACTOR x the render resolution (1 = same)
    python rt.py rtstat [SECONDS]          tracer statistics (averaged over SECONDS)
    python rt.py reload                    recompile the compute shader (wumrt_trace.comp in the sandbox)
    python rt.py dumpmask OUT.png [CAM]    save a camera's current mask (red = lit, plus a depth view)
    python rt.py mouse DX DY               move the game's mouse by DX, DY pixels (turns the camera)
    python rt.py cam                       main camera position and direction (needs the tracer on)
    python rt.py verify [DRAWS]            check the harness's tracked GL state against OpenGL (default 3000 draws)
    python rt.py pacing INTERVAL [CAP]     swap interval (0 = V-Sync off) and frame cap in fps (0 = none)
    python rt.py wake                      tell the game its window is active (no real focus change)

  Live editing (no restart):
    python rt.py reload-core               swap in the wumrt_core.dll that build.sh just made (end of the next frame)
    python rt.py cg-reload [FILE]          compile the game's .cg shaders again (FILE: e.g. Landscape.cg; default all);
                                           a file that fails keeps its old programs, the compiler's message is printed
    python rt.py cgstat                    Cg reloads so far, programs, cached parameter values
    python rt.py freecam take              free camera, starting from the game's current view
    python rt.py freecam off | get
    python rt.py freecam set X Y Z YAW PITCH
    python rt.py freecam move RIGHT UP FORWARD     move along the camera's own axes (world units)
    python rt.py freecam turn DYAW DPITCH          degrees
    python rt.py freecam orbit DYAW DPITCH [DIST]  swing around the point DIST ahead (default: what the
                                           centre of the view shows, from the traced mask)
    python rt.py level NAME|INDEX|quick    leave the match through the menus and start another one: a Versus
                                           deathmatch on that landscape (NAME: any part of it, e.g. "annelids"),
                                           or a Quick Game (random level); works from a match, the pause menu,
                                           the results screen or the main menu
    python rt.py levels                    the landscapes the Versus menu offers, with their numbers
    python rt.py screen                    what the harness thinks is on screen (match, paused, results, main menu)

Key names: enter esc space tab back up down left right, a-z, 0-9, f1-f12.
"""
import ctypes
import ctypes.wintypes as wt
import mmap
import struct
import sys
import time
import zlib
from pathlib import Path

MAGIC = 0x31545257
SANDBOX = Path(__file__).resolve().parent.parent.parent / "rt-sandbox"


def ctl_name(sandbox):
    """'wumrt_ctl' for WormsXHD/rt-sandbox, 'wumrt_ctl_NAME' for WormsXHD/streams/NAME/rt-sandbox
    (a parallel work stream); the loader derives the same from the exe's path."""
    import re
    p = Path(sandbox).resolve()
    if p.name.lower() == "rt-sandbox" and p.parent.parent.name.lower() == "streams" and p.parent.name:
        return "wumrt_ctl_" + re.sub(r"[^A-Za-z0-9_-]", "_", p.parent.name[:60])
    return "wumrt_ctl"


CTL_NAME = ctl_name(SANDBOX)
LOG = SANDBOX / "wumrt.log"


class Ctl(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("magic", ctypes.c_uint32), ("size", ctypes.c_uint32), ("frame", ctypes.c_uint32),
                ("cmd_seq", ctypes.c_uint32), ("ack_seq", ctypes.c_uint32),
                ("cmd", ctypes.c_uint32), ("result", ctypes.c_uint32), ("arg", ctypes.c_uint32 * 4),
                ("stat", ctypes.c_uint32 * 8),
                ("kbd_state_calls", ctypes.c_uint32), ("kbd_data_calls", ctypes.c_uint32),
                ("kbd_devices", ctypes.c_uint32), ("pad_enum_blocked", ctypes.c_uint32),
                ("shot_w", ctypes.c_uint32), ("shot_h", ctypes.c_uint32), ("hwnd", ctypes.c_uint32),
                ("swap_tick", ctypes.c_uint32), ("mouse_x", ctypes.c_int32), ("mouse_y", ctypes.c_int32),
                ("verify_checked", ctypes.c_uint32), ("verify_bad", ctypes.c_uint32),
                ("mouse_seq", ctypes.c_uint32), ("mouse_dx", ctypes.c_int32), ("mouse_dy", ctypes.c_int32),
                ("core_loads", ctypes.c_uint32), ("core_ok", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 9),
                ("dik", ctypes.c_uint8 * 256), ("vk", ctypes.c_uint8 * 256),
                ("path", ctypes.c_char * 260),
                ("rt", ctypes.c_uint32 * 29), ("rt_sun", ctypes.c_float * 3), ("rt_radius", ctypes.c_float),
                ("cam_eye", ctypes.c_float * 3), ("cam_fwd", ctypes.c_float * 3)]


RT_FIELDS = ["enabled", "ok", "instances", "static_tris", "frame_tris", "static_nodes", "frame_nodes",
             "cache_entries", "built_static", "built_frame", "skipped", "collect_us", "finalize_us",
             "trace_cpu_us", "trace_gpu_us", "traces", "mask_w", "mask_h", "pool_bytes", "shadow_bytes",
             "error", "samples", "alpha_instances", "alpha_skipped", "merged_instances", "merged_tris",
             "merge_ms", "merges", "tlas_instances"]


# name: (DirectInput scan code, virtual key)
KEYS = {"esc": (0x01, 0x1B), "enter": (0x1C, 0x0D), "space": (0x39, 0x20), "tab": (0x0F, 0x09),
        "back": (0x0E, 0x08), "up": (0xC8, 0x26), "down": (0xD0, 0x28), "left": (0xCB, 0x25),
        "right": (0xCD, 0x27)}
for _row, _start in (("1234567890", 0x02), ("qwertyuiop", 0x10), ("asdfghjkl", 0x1E), ("zxcvbnm", 0x2C)):
    for _i, _c in enumerate(_row):
        KEYS[_c] = (_start + _i, ord(_c.upper()))
for _i in range(10):
    KEYS[f"f{_i + 1}"] = (0x3B + _i, 0x70 + _i)
KEYS["f11"], KEYS["f12"] = (0x57, 0x7A), (0x58, 0x7B)


def attach():
    mm = mmap.mmap(-1, ctypes.sizeof(Ctl), tagname=CTL_NAME)
    c = Ctl.from_buffer(mm)
    if c.magic != MAGIC:
        sys.exit("harness not found: is the sandbox game running?")
    return c


def command(c, cmd, path, timeout=20, check=True, raw=None):
    if raw is not None:
        ctypes.memmove(ctypes.addressof(c) + Ctl.path.offset, raw, len(raw))
    else:
        c.path = str(Path(path).resolve()).encode("mbcs") if path else b""
    c.cmd = cmd
    seq = (c.cmd_seq + 1) & 0xFFFFFFFF
    c.cmd_seq = seq
    t0 = time.time()
    while c.ack_seq != seq:
        if time.time() - t0 > timeout:
            sys.exit(f"no answer from the game within {timeout} s (frame counter {c.frame}; is it rendering?)")
        time.sleep(0.01)
    if c.result and check:
        sys.exit(f"command failed with code {c.result}")
    return c.result


def log_size():
    try:
        return LOG.stat().st_size
    except OSError:
        return 0


def print_log_since(pos, keep=lambda line: True):
    try:
        with open(LOG, "rb") as f:
            f.seek(pos)
            text = f.read().decode("mbcs", "replace")
    except OSError:
        return
    for line in text.splitlines():
        if keep(line):
            print(line)


def freecam_io(c, mode, pose=None):
    raw = struct.pack("<5f", *pose) if pose else b"\0" * 20
    c.arg[0] = mode
    r = command(c, 14, None, raw=raw, check=False)
    if r == 2:
        sys.exit("no scene camera seen yet (is a match on screen?)")
    return struct.unpack("<8f", ctypes.string_at(ctypes.addressof(c) + Ctl.path.offset, 32))


def freecam(c, a):
    import math
    sub = a[0] if a else "get"
    if sub == "off":
        v = freecam_io(c, 0)
    elif sub == "take":
        v = freecam_io(c, 2)
    elif sub == "set":
        v = freecam_io(c, 1, [float(x) for x in a[1:6]])
    elif sub in ("move", "turn", "orbit"):
        cur = freecam_io(c, 3)
        if not cur[5]:
            cur = freecam_io(c, 2)          # start from the game's view
        eye, yaw, pitch = list(cur[0:3]), cur[3], cur[4]
        y, p = math.radians(yaw), math.radians(pitch)
        fwd = [math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p)]
        right = [math.cos(y), 0.0, math.sin(y)]
        up = [right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
              right[0] * fwd[1] - right[1] * fwd[0]]
        if sub == "move":
            dr, du, df = (float(x) for x in a[1:4])
            eye = [eye[k] + dr * right[k] + du * up[k] + df * fwd[k] for k in range(3)]
        elif sub == "turn":
            yaw, pitch = yaw + float(a[1]), max(-89.5, min(89.5, pitch + float(a[2])))
        else:
            dist = float(a[3]) if len(a) > 3 else (cur[6] or 30.0)
            pivot = [eye[k] + dist * fwd[k] for k in range(3)]
            yaw, pitch = yaw + float(a[1]), max(-89.5, min(89.5, pitch + float(a[2])))
            y, p = math.radians(yaw), math.radians(pitch)
            fwd = [math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p)]
            eye = [pivot[k] - dist * fwd[k] for k in range(3)]
            print(f"orbit around ({pivot[0]:.1f}, {pivot[1]:.1f}, {pivot[2]:.1f}) at distance {dist:.1f}")
        v = freecam_io(c, 1, eye + [yaw, pitch])
    else:
        v = freecam_io(c, 3)
    state = "on" if v[5] else "off (showing the game's camera)"
    print(f"free camera {state}: eye {v[0]:.2f} {v[1]:.2f} {v[2]:.2f}  yaw {v[3]:.2f}  pitch {v[4]:.2f}"
          f"  centre distance {v[6]:.1f}  matrices replaced {int(v[7])}")
    print(f"  python rt.py freecam set {v[0]:.2f} {v[1]:.2f} {v[2]:.2f} {v[3]:.2f} {v[4]:.2f}")


def png_from_raw(raw_path, png_path):
    d = Path(raw_path).read_bytes()
    magic, w, h = struct.unpack_from("<3I", d, 0)
    assert magic == 0x53545257, "not a wumrt screenshot"
    rows = []
    for y in range(h - 1, -1, -1):          # stored bottom-up, BGR
        row = d[12 + y * w * 3:12 + (y + 1) * w * 3]
        rgb = bytearray(row)
        rgb[0::3], rgb[2::3] = row[2::3], row[0::3]
        rows.append(b"\0" + bytes(rgb))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
    Path(png_path).write_bytes(png)
    return w, h


def rtstat(c, seconds):
    acc = {k: [] for k in RT_FIELDS}
    t0, f0 = time.time(), c.frame
    while True:
        for k, v in zip(RT_FIELDS, list(c.rt)):
            acc[k].append(v)
        if time.time() - t0 >= seconds:
            break
        time.sleep(0.02)
    last = {k: v[-1] for k, v in acc.items()}
    mean = {k: sum(v) / len(v) for k, v in acc.items()}
    fps = (c.frame - f0) / max(time.time() - t0, 1e-6) if seconds else 0
    err = {0: "none", 1: "compute shaders unavailable", 2: "shader failed (see wumrt.log)"}[last["error"]]
    print(f"tracer: {'on' if last['enabled'] else 'off'}, mask produced last frame: {bool(last['ok'])}, error: {err}"
          + (f", {fps:.1f} fps" if seconds else ""))
    print(f"scene: {last['instances']} instances, {last['static_tris']} cached + {last['frame_tris']} per-frame triangles, "
          f"{last['cache_entries']} meshes known, skipped draws {last['skipped']}; "
          f"alpha-tested casters: {last['alpha_instances']} traced, {last['alpha_skipped']} left out")
    print(f"merged static BVH: {last['merged_instances']} instances, {last['merged_tris']} triangles (built {last['merges']} times, last in {last['merge_ms']} ms); top level holds {last['tlas_instances']} instances")
    print(f"builds per frame: cached {mean['built_static']:.2f}, per-frame {mean['built_frame']:.1f}")
    print(f"cost per frame: collect {mean['collect_us'] / 1000:.2f} ms, top level + upload {mean['finalize_us'] / 1000:.2f} ms, "
          f"dispatch (CPU) {mean['trace_cpu_us'] / 1000:.2f} ms, trace (GPU) {mean['trace_gpu_us'] / 1000:.2f} ms")
    print(f"mask {last['mask_w']}x{last['mask_h']}, {last['traces']} trace(s) per frame, sun samples {last['samples']}, "
          f"radius {c.rt_radius:.2f} deg, sun direction ({c.rt_sun[0]:.3f}, {c.rt_sun[1]:.3f}, {c.rt_sun[2]:.3f})")
    print(f"memory: BVH pools {last['pool_bytes'] / 1e6:.1f} MB, buffer mirror {last['shadow_bytes'] / 1e6:.1f} MB")


def mask_png(raw_path, png_path):
    d = Path(raw_path).read_bytes()
    magic, w, h = struct.unpack_from("<3I", d, 0)
    assert magic == 0x46545257, "not a wumrt mask dump"
    import array
    f = array.array("f")
    f.frombytes(d[12:12 + w * h * 8])
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray(w * 3)
        base = y * w * 2
        for x in range(w):
            vis, dist = f[base + 2 * x], f[base + 2 * x + 1]
            g = 0 if dist >= 59999 else max(0, min(255, int(255 - dist * 0.25)))
            row[3 * x] = max(0, min(255, int(vis * 255)))
            row[3 * x + 1] = g
            row[3 * x + 2] = 255 if dist >= 59999 else 0
        rows.append(b"\0" + bytes(row))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    Path(png_path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                               + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
    print(f"{png_path}: {w}x{h} (red = sun visibility, green = nearness, blue = no caster hit)")


# The Versus landscape list, in menu order (the first five make a random level of that theme).
LEVELS = ["Arabian (random)", "Camelot (random)", "Construction (random)", "Prehistoric (random)",
          "Wild West (random)", "Desert Oasis", "Riverboat Harbour", "This Game Does Dragon",
          "Wormwigs Big Rig Jig", "A Leek in a Vegetable Patch", "Alien Juice Suckers", "All Cooped Up",
          "Apple Core Island", "Costa Del Danger", "Crate Britain", "Crop Circle", "D-Day", "Earn Your Crust",
          "Grave Danger", "Helter Skelter", "High Stakes", "Hold Until Relieved", "Hook, Line and Skimmer",
          "Ice, Ice, Maybe", "Movie Mayhem", "Nobody Rides For Free", "Plaice Holder", "Ragnarok and Roll",
          "Schools in for Summer", "Shiver Me Timbers", "Showdown at the OK Corale Reef", "Tree Village Trouble",
          "When Annelids Collide"]


def screen(c):
    """Classify the current frame from a screenshot: 'paused' (pause menu or a yes/no box),
    'results' (end of a Versus round), 'menu' (main menu) or 'other' (a match, loading, ...)."""
    import tempfile
    raw = Path(tempfile.gettempdir()) / f"wumrt_screen_{CTL_NAME}.raw"
    command(c, 1, raw)
    d = raw.read_bytes()
    raw.unlink()
    _, w, h = struct.unpack_from("<3I", d, 0)

    def frac(box, test):
        x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)
        n = hit = 0
        for y in range(y0, y1, 2):
            row = 12 + (h - 1 - y) * w * 3          # bottom-up BGR
            for x in range(x0, x1, 2):
                b, g, r = d[row + 3 * x], d[row + 3 * x + 1], d[row + 3 * x + 2]
                n += 1
                hit += test(r, g, b)
        return hit / max(n, 1)

    teal = lambda r, g, b: r < 70 and 70 < g < 130 and 80 < b < 140 and b > r + 30
    yellow = lambda r, g, b: r > 170 and g > 150 and b < 110
    if frac((0.76, 0.76, 0.86, 0.93), yellow) > 0.08:
        return "results"
    if frac((0.38, 0.30, 0.62, 0.68), teal) > 0.4:
        return "paused"
    if frac((0.20, 0.10, 0.50, 0.45), yellow) > 0.08:
        return "menu"
    return "other"


def keys(c, names, gap=0.45):
    for n in names.split(","):
        press(c, n, 120)
        time.sleep(gap)


def to_main_menu(c):
    for _ in range(8):
        s = screen(c)
        print("  screen:", s, flush=True)
        if s == "menu":
            return
        if s == "results":
            keys(c, "esc")
            time.sleep(1.0)
            keys(c, "down,enter")
            time.sleep(9)
        elif s == "paused":
            keys(c, "down,down,down,down,down,enter", 0.3)    # the menus stop at the last item:
            time.sleep(1.2)                                     # Quit in a pause menu, Yes in a yes/no box
            if screen(c) == "paused":
                keys(c, "down,down,enter", 0.3)                 # "Exit Game? Are you sure?" -> Yes
            time.sleep(9)
        else:
            keys(c, "esc")                  # a match: pause it; a menu: go back
            time.sleep(1.5)
    sys.exit("could not get back to the main menu")


def wait_for_match(c, timeout=60):
    t0 = time.time()
    good = 0
    while time.time() - t0 < timeout:
        time.sleep(1)
        good = good + 1 if c.rt[0] and c.rt[2] and c.rt[1] else 0      # enabled, instances, mask made
        if good >= 3:
            return True
    return False


def level(c, which):
    if which.lower() in ("quick", "random"):
        index = None
    elif which.isdigit():
        index = int(which)
    else:
        found = [i for i, n in enumerate(LEVELS) if which.lower() in n.lower()]
        if len(found) != 1:
            sys.exit(f"'{which}' matches {len(found)} landscapes; see python rt.py levels")
        index = found[0]
    if index is not None and not 0 <= index < len(LEVELS):
        sys.exit("no such landscape number; see python rt.py levels")
    to_main_menu(c)
    keys(c, "enter")                        # Local Game (focus starts on Quick Game)
    settle(c, 2)
    if index is None:
        keys(c, "enter")                    # Quick Game
        settle(c, 1.5)
        keys(c, "enter")                    # 1 Player
    else:
        print(f"  starting a deathmatch on {LEVELS[index]}", flush=True)
        keys(c, "down,down,down,down,enter")    # Versus
        settle(c, 1.5)
        keys(c, "enter")                    # Deathmatch; focus on the first team slot
        settle(c, 2)
        keys(c, "enter")                    # first team: the first in the list (a human team)
        settle(c, 1.0)
        keys(c, "enter")
        settle(c, 1.2)
        keys(c, "down,enter")               # second slot: CPU Normal
        settle(c, 1.0)
        keys(c, "down,down,down,down,enter")
        settle(c, 1.2)
        keys(c, "left,enter")               # the landscape list
        settle(c, 1.5)
        for _ in range(len(LEVELS) + 2):    # to the top (the list does not wrap)
            press(c, "up", 60)
            time.sleep(0.12)
        for _ in range(index):
            press(c, "down", 60)
            time.sleep(0.15)
        settle(c, 0.4)
        keys(c, "enter")
        settle(c, 1.5)
        keys(c, "down,right,down,enter")    # Game Style -> Wormpot -> start
    if not wait_for_match(c):
        sys.exit("no match after 60 s: look at a screenshot (python rt.py shot out/where.png)")
    print("match running" + (f" on {LEVELS[index]}" if index is not None else " (Quick Game)"))


def wait_frames(c, n, timeout=3.0):
    """Until the game has drawn n more frames (or timeout): the game reads input once per frame."""
    f0, t0 = c.frame, time.time()
    while c.frame - f0 < n and time.time() - t0 < timeout:
        time.sleep(0.005)


def press(c, name, ms):
    """Hold a key for ms AND at least 2 game frames, then let 2 frames pass released, so a slow
    game (heavy textures, a busy GPU) cannot miss the press or merge two presses."""
    dik, vk = KEYS[name.lower()]
    c.dik[dik] = 1
    c.vk[vk] = 1
    time.sleep(ms / 1000)
    wait_frames(c, 2)
    c.dik[dik] = 0
    c.vk[vk] = 0
    wait_frames(c, 2)


def settle(c, seconds):
    """A menu wait that stretches when the game runs slowly: at least `seconds` and at least
    as many frames as the game draws in that time at 30 fps (at most 4x the time)."""
    t0, f0 = time.time(), c.frame
    time.sleep(seconds)
    while c.frame - f0 < seconds * 30 and time.time() - t0 < seconds * 4:
        time.sleep(0.02)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    c = attach()
    if a[0] == "status":
        f0, t0 = c.frame, time.time()
        time.sleep(1)
        fps = (c.frame - f0) / (time.time() - t0)
        s = list(c.stat)
        print(f"frame {c.frame}, {fps:.1f} fps, window {c.hwnd:#x}")
        print(f"last frame draws: elements {s[0]}, arrays {s[1]}, range {s[2]}, begin {s[3]}, "
              f"call lists {s[4]}; vertices submitted {s[5]}; draws using the traced mask {s[6]}")
        print(f"keyboard: devices {c.kbd_devices}, state reads {c.kbd_state_calls}, "
              f"buffered reads {c.kbd_data_calls}; pad enumerations blocked {c.pad_enum_blocked}")
        print(f"core: {'loaded' if c.core_ok else 'NOT loaded'}, loads in this process {c.core_loads}")
    elif a[0] == "shot":
        raw = Path(a[1]).with_suffix(".raw")
        command(c, 1, raw)
        w, h = png_from_raw(raw, a[1])
        raw.unlink()
        print(f"{a[1]}: {w}x{h}, frame {c.frame}")
    elif a[0] == "key":
        press(c, a[1], int(a[2]) if len(a) > 2 else 120)
    elif a[0] == "keys":
        gap = int(a[2]) if len(a) > 2 else 400
        for name in a[1].split(","):
            press(c, name, 120)
            time.sleep(gap / 1000)
    elif a[0] == "capture":
        command(c, 2, a[1], timeout=60)
        print(f"{a[1]}: {Path(a[1]).stat().st_size} bytes, frame {c.frame}")
    elif a[0] == "rt":
        c.arg[0] = 1 if a[1] == "on" else 0
        c.arg[1] = int(a[2]) if len(a) > 2 else 0
        c.arg[2] = int(float(a[3]) * 1000) if len(a) > 3 else 500
        c.arg[3] = int(a[4]) + 1 if len(a) > 4 else 0
        command(c, 5, ".")
        print("real-time traced shadows", a[1])
    elif a[0] == "sky":
        c.arg[0] = int(a[1])
        c.arg[1] = int(float(a[2])) if len(a) > 2 else 80
        c.arg[2] = int(float(a[3]) * 1000) if len(a) > 3 else 700
        c.arg[3] = int(float(a[4]) * 1000) if len(a) > 4 else 0
        command(c, 11, ".")
    elif a[0] == "scale":
        c.arg[0] = int(float(a[1]) * 1000)
        command(c, 10, ".")
    elif a[0] == "rtstat":
        rtstat(c, float(a[1]) if len(a) > 1 else 0)
    elif a[0] == "reload":
        command(c, 6, ".")
        time.sleep(0.5)
        rtstat(c, 0)
    elif a[0] == "dumpmask":
        raw = Path(a[1]).with_suffix(".wrtf")
        c.arg[0] = int(a[2]) if len(a) > 2 else 0
        command(c, 7, raw)
        mask_png(raw, a[1])
        raw.unlink()
    elif a[0] == "cam":
        import math
        f = list(c.cam_fwd)
        yaw, pitch = math.degrees(math.atan2(f[0], -f[2])), math.degrees(math.asin(max(-1, min(1, f[1]))))
        print(f"eye ({c.cam_eye[0]:.1f}, {c.cam_eye[1]:.1f}, {c.cam_eye[2]:.1f})  yaw {yaw:.1f}  pitch {pitch:.1f}")
    elif a[0] == "mouse":
        # The game turns the camera by the difference between consecutive WM_MOUSEMOVE
        # positions (about 0.25 degrees of yaw and 0.054 of pitch per pixel). A virtual
        # cursor position is kept in the control block and moved in small steps.
        dx, dy = int(a[1]), int(a[2])
        u32 = ctypes.WinDLL("user32")
        u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
        n = max(1, (max(abs(dx), abs(dy)) + 39) // 40)
        x0, y0 = c.mouse_dx, c.mouse_dy
        for i in range(1, n + 1):
            x, y = x0 + dx * i // n, y0 + dy * i // n
            c.mouse_dx, c.mouse_dy = x, y
            f = c.frame
            u32.PostMessageW(c.hwnd, 0x0200, 0, (y & 0xFFFF) << 16 | (x & 0xFFFF))
            t0 = time.time()
            while c.frame < f + 1 and time.time() - t0 < 10:
                time.sleep(0.005)
    elif a[0] == "verify":
        c.arg[0] = int(a[1]) if len(a) > 1 else 3000
        command(c, 8, ".")
        t0 = time.time()
        while c.verify_checked < c.arg[0] and time.time() - t0 < 60:
            time.sleep(0.2)
        print(f"draws checked {c.verify_checked}, with a difference {c.verify_bad}")
    elif a[0] == "pacing":
        c.arg[0] = int(a[1])
        c.arg[1] = int(a[2]) if len(a) > 2 else 0
        command(c, 9, ".")
        time.sleep(1.5)
        f0, t0 = c.frame, time.time()
        time.sleep(1.5)
        print(f"{(c.frame - f0) / (time.time() - t0):.1f} fps")
    elif a[0] == "reload-core":
        pos = log_size()
        loads = c.core_loads
        r = command(c, 100, None, timeout=60, check=False)
        print_log_since(pos, lambda l: "loader:" in l or "core " in l or "rt: " in l and ("fail" in l or "mirror" in l))
        if r or c.core_loads == loads:
            sys.exit("core reload failed (see above); the game runs without a core until the next reload-core")
        print(f"core reloaded (load {c.core_loads} in this process)")
    elif a[0] == "cg-reload":
        pos = log_size()
        name = a[1] if len(a) > 1 else ""
        r = command(c, 101, None, timeout=120, check=False, raw=name.encode("mbcs") + b"\0")
        print_log_since(pos, lambda l: True)
        print(f"{c.arg[0]} files compiled again, {c.arg[1]} programs replaced, {r} files failed (reload {c.arg[2]})")
        if r:
            sys.exit(1)
    elif a[0] == "cgstat":
        command(c, 102, None)
        print(f"Cg reloads {c.arg[0]}, programs {c.arg[1]}, cached parameter values {c.arg[2]}, translated parameters {c.arg[3]}")
    elif a[0] == "freecam":
        freecam(c, a[1:])
    elif a[0] == "level":
        level(c, " ".join(a[1:]) if len(a) > 1 else "quick")
    elif a[0] == "levels":
        for i, n in enumerate(LEVELS):
            print(f"{i:3d}  {n}")
    elif a[0] == "screen":
        print(screen(c))
    elif a[0] == "wake":
        u32 = ctypes.WinDLL("user32")
        u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
        hwnd = c.hwnd
        u32.PostMessageW(hwnd, 0x001C, 1, 0)     # WM_ACTIVATEAPP
        u32.PostMessageW(hwnd, 0x0006, 1, 0)     # WM_ACTIVATE (WA_ACTIVE)
        u32.PostMessageW(hwnd, 0x0007, 0, 0)     # WM_SETFOCUS
        print("activation messages posted")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
