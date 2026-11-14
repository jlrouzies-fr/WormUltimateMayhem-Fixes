"""
Free camera aimed at a world point, for close-ups of a character.

    python lookat.py X Y Z ANGLE DIST [HEIGHT [SHIFT]]

Puts the camera DIST world units from (X, Y, Z) in compass direction ANGLE (degrees, same
convention as the free camera's yaw: the camera then looks along yaw = ANGLE + 180), HEIGHT
above the point, looking straight at it; SHIFT (degrees) turns the view sideways afterwards so
the subject sits left (positive) or right (negative) of the paused game's menu box.
    python lookat.py X Y Z sweep DIST [HEIGHT] OUT.png     six views around the point, one sheet
    python lookat.py faces                                 the worms the harness logged (wumrt.log)
    python lookat.py face N ANGLE DIST [HEIGHT [SHIFT]]    the same aimed at worm N (also: face N sweep ...)
Uses `rt.py freecam set`; yaw convention: forward = (sin yaw cos pitch, sin pitch, -cos yaw cos pitch).
"""
import math
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def rt(*args):
    return subprocess.run([sys.executable, str(HERE / "rt.py"), *map(str, args)], capture_output=True, text=True).stdout


def place(x, y, z, angle, dist, height=0.0, shift=0.0):
    a = math.radians(angle)
    # the camera stands where it would be after moving DIST backwards from the point while looking along yaw
    fx, fz = math.sin(a + math.pi), -math.cos(a + math.pi)
    ex, ez = x - fx * dist, z - fz * dist
    ey = y + height
    pitch = math.degrees(math.atan2(-height, dist))
    yaw = (angle + 180 + shift + 180) % 360 - 180
    return rt("freecam", "set", f"{ex:.2f}", f"{ey:.2f}", f"{ez:.2f}", f"{yaw:.2f}", f"{pitch:.2f}")


def faces():
    """The worms' face centres from the newest 'characters:' line of the sandbox log."""
    import re
    log = HERE / ".." / ".." / "rt-sandbox" / "wumrt.log"
    lines = [l for l in log.read_text(errors="replace").splitlines() if "characters:" in l]
    if not lines:
        sys.exit("no 'characters:' line in the log yet (needs a character setting on and a worm drawn)")
    return [tuple(map(float, m)) for m in re.findall(r"\(([-\d.]+) ([-\d.]+) ([-\d.]+)\)", lines[-1])]


def main():
    a = sys.argv[1:]
    if a and a[0] == "faces":
        for i, f in enumerate(faces()):
            print(i, *f)
        return
    if a and a[0] == "face":
        # lookat.py face N ...: the same as X Y Z of worm N (see 'lookat.py faces'), aimed a little
        # above the face draw's centre
        x, y, z = faces()[int(a[1])]
        a = [str(x), str(y + 4), str(z)] + a[2:]
    x, y, z = map(float, a[:3])
    if a[3] == "sweep":
        from PIL import Image
        dist = float(a[4])
        height = float(a[5]) if len(a) > 6 else 0.0
        out = a[-1]
        sheet = Image.new("RGB", (480 * 3, 270 * 2))
        for k in range(6):
            place(x, y, z, k * 60, dist, height)
            tmp = HERE / "out" / "_sweep.png"
            rt("shot", tmp)
            im = Image.open(tmp).convert("RGB").resize((480, 270))
            sheet.paste(im, ((k % 3) * 480, (k // 3) * 270))
            print("angle", k * 60)
        sheet.save(out)
        return
    print(place(x, y, z, float(a[3]), float(a[4]), float(a[5]) if len(a) > 5 else 0.0,
                float(a[6]) if len(a) > 6 else 0.0).strip())


if __name__ == "__main__":
    main()
