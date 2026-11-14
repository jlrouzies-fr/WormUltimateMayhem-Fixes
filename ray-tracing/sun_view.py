"""Point the free camera so the sun sits off-centre above the view (for sun shaft tests).
   python sun_view.py [DYAW] [DPITCH]   (the sun ends up DYAW degrees left/right, DPITCH above the centre)
Reads the sun direction from rt.py rtstat, keeps the game camera's position."""
import math
import re
import subprocess
import sys

dyaw = float(sys.argv[1]) if len(sys.argv) > 1 else 18.0
dpitch = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
st = subprocess.run([sys.executable, "rt.py", "rtstat", "1"], capture_output=True, text=True).stdout
m = re.search(r"sun direction \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", st)
if not m:
    sys.exit("no sun direction in rtstat")
sx, sy, sz = (float(v) for v in m.groups())
# free camera forward = (sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch))
yaw = math.degrees(math.atan2(sx, -sz))
pitch = math.degrees(math.asin(max(-1.0, min(1.0, sy))))
subprocess.run([sys.executable, "rt.py", "freecam", "take"], capture_output=True, text=True)
cur = subprocess.run([sys.executable, "rt.py", "freecam", "turn", "0", "0"], capture_output=True, text=True).stdout
nums = re.findall(r"freecam set ([-\d.]+) ([-\d.]+) ([-\d.]+)", cur)
if not nums:
    sys.exit("freecam take gave no pose: " + cur)
x, y, z = nums[-1]
if len(sys.argv) > 3:
    # from the level's far side: DIST units from the middle away from the sun, at HEIGHT,
    # so the island stands between the camera and the sun
    dist = float(sys.argv[3]); height = float(sys.argv[4]) if len(sys.argv) > 4 else 80.0
    h = math.hypot(sx, sz) or 1.0
    x, y, z = f"{-sx / h * dist:.2f}", f"{height:.2f}", f"{-sz / h * dist:.2f}"
pose = [x, y, z, f"{yaw + dyaw:.2f}", f"{max(-60.0, min(60.0, pitch - dpitch)):.2f}"]
print(f"sun direction {sx} {sy} {sz}: yaw {yaw:.1f} pitch {pitch:.1f}; camera", " ".join(pose))
subprocess.run([sys.executable, "rt.py", "freecam", "set"] + pose)
