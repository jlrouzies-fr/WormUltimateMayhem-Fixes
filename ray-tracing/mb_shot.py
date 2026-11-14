"""Motion-blur check shot: turn the game camera with the mouse at a steady rate (DX, DY virtual
mouse pixels every frame; the game turns ~0.25 deg of yaw per pixel) and take a screenshot in the
middle of the turn, so the frame is captured while the camera moves.

    py -3 mb_shot.py OUT.png [DX] [DY] [FRAMES_BEFORE] [FRAMES_TOTAL]
    py -3 mb_shot.py --free OUT.png DYAW DPITCH [STEPS] [MOVE_RIGHT]
        free camera instead: turn by DYAW/DPITCH degrees (and move MOVE_RIGHT world units)
        every frame for STEPS frames (default 8), then shoot; the pose is left where it ended

Defaults: DX 8, DY 0, shot after 12 frames of turning, 30 frames of turning in all. Uses only
PostMessage for the mouse (no control-block command), so the shot command runs alongside it.
"""
import ctypes
import ctypes.wintypes as wt
import sys
import threading
import time
from pathlib import Path

import rt


def turn(c, dx, dy, frames, started):
    u32 = ctypes.WinDLL("user32")
    u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
    x, y = c.mouse_dx, c.mouse_dy
    for i in range(frames):
        x, y = x + dx, y + dy
        c.mouse_dx, c.mouse_dy = x, y
        f = c.frame
        u32.PostMessageW(c.hwnd, 0x0200, 0, (y & 0xFFFF) << 16 | (x & 0xFFFF))
        started.append(i)
        t0 = time.time()
        while c.frame < f + 1 and time.time() - t0 < 5:
            time.sleep(0.002)


def free(a):
    import math
    out, dyaw, dpitch = a[0], float(a[1]), float(a[2])
    steps = int(a[3]) if len(a) > 3 else 8
    mr = float(a[4]) if len(a) > 4 else 0.0
    c = rt.attach()
    cur = rt.freecam_io(c, 3)
    if not cur[5]:
        cur = rt.freecam_io(c, 2)
    eye, yaw, pitch = list(cur[0:3]), cur[3], cur[4]
    for i in range(steps):
        yaw += dyaw
        pitch = max(-89.5, min(89.5, pitch + dpitch))
        y = math.radians(yaw)
        eye = [eye[0] + mr * math.cos(y), eye[1], eye[2] + mr * math.sin(y)]
        f = c.frame
        rt.freecam_io(c, 1, eye + [yaw, pitch])
        t0 = time.time()
        while c.frame < f + 1 and time.time() - t0 < 5:
            time.sleep(0.002)
    raw = Path(out).with_suffix(".raw")
    rt.command(c, 1, raw)
    w, h = rt.png_from_raw(raw, out)
    raw.unlink()
    print(f"{out}: {w}x{h}, frame {c.frame}, after {steps} free-camera steps of {dyaw} deg yaw, {mr} units right")


def main():
    a = sys.argv[1:]
    if a and a[0] == "--free":
        return free(a[1:])
    out = a[0]
    dx = int(a[1]) if len(a) > 1 else 8
    dy = int(a[2]) if len(a) > 2 else 0
    before = int(a[3]) if len(a) > 3 else 12
    total = int(a[4]) if len(a) > 4 else 30
    c = rt.attach()
    started = []
    th = threading.Thread(target=turn, args=(c, dx, dy, total, started), daemon=True)
    th.start()
    t0 = time.time()
    while len(started) < before and time.time() - t0 < 10:
        time.sleep(0.002)
    raw = Path(out).with_suffix(".raw")
    rt.command(c, 1, raw)
    w, h = rt.png_from_raw(raw, out)
    raw.unlink()
    print(f"{out}: {w}x{h}, frame {c.frame}, shot after {len(started)} turning frames")
    th.join()


if __name__ == "__main__":
    main()
