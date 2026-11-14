"""
Wait for an explosion in the running sandbox match and freeze it: polls screenshots, and when
the share of fire-coloured pixels jumps, pauses the game (Esc), takes a screenshot and
(optionally) a draw capture of the paused frame. Explosions come from the CPU team's turns;
the human team's turn just runs out.

    python fx_watch.py OUTPREFIX [--capture] [--timeout S] [--threshold F]
    python fx_watch.py OUTPREFIX --log WORD [--capture] [--timeout S]   wait for WORD in wumrt.log
                                                    (fx_log = 1 logs "GLOW" for glowing effect draws)

Writes OUTPREFIX.png (and OUTPREFIX.bin). Leaves the game paused: python rt.py key esc resumes.
"""
import sys
import time
from pathlib import Path

import numpy as np

import rt


def frame_rgb(c, raw):
    rt.command(c, 1, raw)
    d = raw.read_bytes()
    w, h = np.frombuffer(d, "<u4", 2, 4)
    img = np.frombuffer(d, np.uint8, int(w) * int(h) * 3, 12).reshape(int(h), int(w), 3)[::-1, :, ::-1]
    return img


def fire_share(img):
    r, g, b = (img[..., k].astype(np.int16) for k in range(3))
    hud = np.zeros(img.shape[:2], bool)
    h, w = hud.shape
    hud[int(0.2 * h):int(0.8 * h), int(0.15 * w):int(0.85 * w)] = True    # the HUD sits at the edges
    fire = (r > 220) & (g > 90) & (g < 235) & (b < 140) & (r - b > 110)
    return float((fire & hud).mean())


LOOK = 0.0


def look_at_boom(c, text, dist):
    """Point the free camera at the last BOOM light: the game camera's heading, 25 degrees down."""
    import math
    import re
    m = None
    for m in re.finditer(r"BOOM new bright light \d+ at (\S+) (\S+) (\S+)", text):
        pass
    if not m:
        return
    lx, ly, lz = (float(v) for v in m.groups())
    v = rt.freecam_io(c, 2)            # take: start from the game's view
    yaw, pitch = v[3], -25.0
    y, p = math.radians(yaw), math.radians(pitch)
    f = (math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p))
    pose = (lx - f[0] * dist, ly - f[1] * dist, lz - f[2] * dist, yaw, pitch)
    pose = pose[:3] + (yaw + 20.0, pitch)      # the light ends up left of the pause box
    rt.freecam_io(c, 1, pose)
    print("free camera on the light: python rt.py freecam set %.2f %.2f %.2f %.2f %.2f" % pose, flush=True)


def wait_log(c, out, cap, timeout, word):
    """Wait until the harness logs an effect draw that glows (fx_log = 1), then pause."""
    pos = rt.log_size()
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(0.1)
        size = rt.log_size()
        if size <= pos:
            continue
        with open(rt.LOG, "rb") as f:
            f.seek(pos)
            text = f.read(size - pos).decode("mbcs", "replace")
        pos = size
        if any(w in text for w in word.split("|")):
            rt.press(c, "esc", 120)
            time.sleep(0.4)
            print(f"'{word}' logged after {time.time() - t0:.0f} s: paused", flush=True)
            raw = Path(out).with_suffix(".watch.raw")
            rt.command(c, 1, raw)
            rt.png_from_raw(raw, out + ".png")
            raw.unlink()
            if cap:
                rt.command(c, 2, out + ".bin", timeout=60)
            print("screen:", rt.screen(c))
            if LOOK:
                look_at_boom(c, text, LOOK)
            return True
    return False


def main():
    out = sys.argv[1]
    if "--log" in sys.argv:
        word = sys.argv[sys.argv.index("--log") + 1]
        global LOOK
        if "--look" in sys.argv:
            LOOK = float(sys.argv[sys.argv.index("--look") + 1])
        c = rt.attach()
        timeout = float(sys.argv[sys.argv.index("--timeout") + 1]) if "--timeout" in sys.argv else 240
        if not wait_log(c, out, "--capture" in sys.argv, timeout, word):
            sys.exit("nothing logged")
        return
    cap = "--capture" in sys.argv
    timeout = float(sys.argv[sys.argv.index("--timeout") + 1]) if "--timeout" in sys.argv else 240
    thr = float(sys.argv[sys.argv.index("--threshold") + 1]) if "--threshold" in sys.argv else 0.004
    c = rt.attach()
    raw = Path(out).with_suffix(".watch.raw")
    hist = []
    t0 = time.time()
    while time.time() - t0 < timeout:
        img = frame_rgb(c, raw)
        s = fire_share(img)
        base = float(np.median(hist[-10:])) if hist else s
        if len(hist) >= 3 and s - base > thr:
            rt.press(c, "esc", 120)
            time.sleep(0.4)
            print(f"fire {s:.4f} (baseline {base:.4f}) after {time.time() - t0:.0f} s: paused", flush=True)
            rt.command(c, 1, raw)
            rt.png_from_raw(raw, out + ".png")
            if cap:
                rt.command(c, 2, out + ".bin", timeout=60)
            raw.unlink()
            print("screen:", rt.screen(c))
            return
        hist.append(s)
        time.sleep(0.15)
    raw.unlink(missing_ok=True)
    sys.exit("no explosion seen")


if __name__ == "__main__":
    main()
