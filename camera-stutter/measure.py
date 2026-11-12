"""
Measure frame pacing and camera smoothness in a running, camfix-patched WormsMayhem.exe.

    python measure.py [--frames 900] [--timeout 300] [--period 4] [--wait 900]
                            [--limit-us 4000]

The hooks record, for every rendered frame, the transform the render camera held and
when the frame was presented (SwapBuffers). This script reads that record and cycles
through three configurations every --period seconds, so all are measured in the same
session:

    original      engine's own Sleep limiter, no camera interpolation (unpatched behaviour)
    limiter only  precise limiter, no camera interpolation
    full patch    precise limiter and camera interpolation

Only frames where the camera is moving count, so keep the camera rotating or panning
while it runs. It stops once each configuration has --frames moving frames, or after
--timeout seconds of recording, and always leaves the full patch switched on.

What the numbers mean:
  frame time  time between consecutive presents (SwapBuffers returning)
  long frames frames lasting more than 1.5x the median frame time
  swap wait   time spent inside SwapBuffers (large and steady = V-Sync is pacing)
  frozen      moving frames whose camera transform equals the previous frame's
  unevenness  how far each frame's camera speed is from the local average speed,
              judged against present times (0% = perfectly even motion on screen)
"""
import argparse
import csv
import ctypes
import ctypes.wintypes as wt
import json
import math
import statistics
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
INFO = HERE / "build" / "camfix.json"
CSV_OUT = HERE / "build" / "measure_frames.csv"

LOG_N = 2048
HEADER_SIZE = 32
FRAME_SIZE = 56
MAGIC = 0x32474C43
QPC_FREQ_VA = 0x962758
OFF_ENABLED, OFF_LIM_ORIG, OFF_LIMIT_US = 4, 12, 16
FLAG_ENABLED, FLAG_XCAM, FLAG_ORIG = 4, 8, 32
WINDOW = 12          # frames either side used for "is it moving" and local speed
SETTLE = 20          # frames dropped after each configuration switch

# name, limiter_original, interpolation
CONFIGS = [("original", 1, 0), ("limiter only", 0, 0), ("full patch", 0, 1)]

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = k32.ReadProcessMemory.argtypes


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    pid = None
    ok = k32.Process32FirstW(wt.HANDLE(snap), ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            pid = e.th32ProcessID
            break
        ok = k32.Process32NextW(wt.HANDLE(snap), ctypes.byref(e))
    k32.CloseHandle(wt.HANDLE(snap))
    return pid


class Frame:
    __slots__ = ("q", "now", "ticks", "flags", "v", "bits", "sq0", "sq1", "wait")

    def __init__(self, raw, off):
        self.q, self.now, self.ticks, self.flags = struct.unpack_from("<4I", raw, off)
        self.v = struct.unpack_from("<6f", raw, off + 16)
        self.bits = raw[off + 16:off + 40]
        self.sq0, self.sq1, self.wait = struct.unpack_from("<3I", raw, off + 40)


class Game:
    def __init__(self, pid, log_va):
        self.h = k32.OpenProcess(0x0438, False, pid)  # QUERY | VM_READ | VM_WRITE | VM_OPERATION
        if not self.h:
            sys.exit("could not open the game process")
        self.log_va = log_va

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, addr, buf, size, ctypes.byref(got)):
            raise OSError("game memory not readable (game closed?)")
        return buf.raw

    def write_u32(self, off, value):
        v = ctypes.c_uint32(value)
        got = ctypes.c_size_t()
        if not k32.WriteProcessMemory(self.h, self.log_va + off, ctypes.byref(v), 4, ctypes.byref(got)):
            raise OSError("could not change hook settings")

    def configure(self, lim_orig, interp, limit_us):
        self.write_u32(OFF_LIMIT_US, limit_us)
        self.write_u32(OFF_LIM_ORIG, lim_orig)
        self.write_u32(OFF_ENABLED, interp)

    def snapshot(self):
        raw = self.read(self.log_va, HEADER_SIZE + LOG_N * FRAME_SIZE)
        magic, _, widx = struct.unpack_from("<3I", raw, 0)
        if magic != MAGIC:
            sys.exit("frame recorder not found or wrong version; re-apply with patch.py apply")
        return widx, raw


def cfg_of(flags):
    return (1 if flags & FLAG_ORIG else 0, 1 if flags & FLAG_ENABLED else 0)


def collect(game, args, segments):
    """Fills segments ({config index: [segments]}; a segment is a gap-free list of
    Frames). Whatever was recorded stays in it if the game goes away mid-run."""
    try:
        _collect(game, args, segments)
    finally:
        cur, ci = _collect.pending
        if cur:
            segments[ci].append(cur)
        _collect.pending = ([], 0)


def _collect(game, args, segments):
    widx, _ = game.snapshot()
    last = widx
    cur = []
    ci = 0
    game.configure(CONFIGS[ci][1], CONFIGS[ci][2], args.limit_us)
    t_switch = t_begin = t_report = time.time()
    t_start = None
    drop = SETTLE

    def close():
        nonlocal cur
        if cur:
            segments[ci].append(cur)
        cur = []

    while True:
        _collect.pending = (cur, ci)
        time.sleep(0.2)
        widx, raw = game.snapshot()
        n_new = (widx - last) & 0xFFFFFFFF
        if n_new > LOG_N:               # fell behind; start a fresh segment
            close()
            last, drop, n_new = widx, SETTLE, 0
        want = (CONFIGS[ci][1], CONFIGS[ci][2])
        for i in range(last, last + n_new):
            f = Frame(raw, HEADER_SIZE + (i % LOG_N) * FRAME_SIZE)
            ok = f.flags & FLAG_XCAM and cfg_of(f.flags) == want
            if ok and drop > 0:
                drop -= 1
                ok = False
            if ok:
                cur.append(f)
            else:
                close()
        last = (last + n_new) & 0xFFFFFFFF

        moving = []
        for i in range(len(CONFIGS)):
            segs = segments[i] + ([cur] if i == ci and cur else [])
            moving.append(sum(len(analyse_segment(s, 1)) for s in segs))
        if t_start is None and any(moving):
            t_start = time.time()
            print("camera motion detected, recording", flush=True)
        if time.time() - t_report > 5:
            t_report = time.time()
            state = "recording" if t_start else "waiting for camera motion"
            print(f"  {state}: moving frames so far  " +
                  ", ".join(f"{CONFIGS[i][0]} {moving[i]}" for i in range(len(CONFIGS))), flush=True)
        if all(m >= args.frames for m in moving):
            break
        if t_start and time.time() - t_start > args.timeout:
            print("timeout reached", flush=True)
            break
        if t_start is None and time.time() - t_begin > args.wait:
            print("no camera motion seen; giving up", flush=True)
            break
        if time.time() - t_switch > args.period:
            close()
            # Next configuration that still needs frames.
            for step in range(1, len(CONFIGS) + 1):
                nxt = (ci + step) % len(CONFIGS)
                if moving[nxt] < args.frames:
                    break
            ci = nxt
            game.configure(CONFIGS[ci][1], CONFIGS[ci][2], args.limit_us)
            t_switch = time.time()
            drop = SETTLE
    _collect.pending = (cur, ci)


_collect.pending = ([], 0)


def analyse_segment(seg, freq):
    """One row per moving frame: (frame_time, frozen, uneven_vs_present, uneven_vs_game,
    swap_wait, limiter_wait). Frame i's present time is entry i+1's swap timestamp."""
    n = len(seg) - 1                    # last entry has no present time yet
    if n < 2 * WINDOW + 3:
        return []
    have_swap = all(seg[i].sq1 for i in range(1, n + 1))
    pres = [seg[i + 1].sq1 if have_swap else seg[i + 1].q for i in range(n)]
    dts, dgs, steps, frozen = [0.0] * n, [0] * n, [0.0] * n, [False] * n
    for i in range(1, n):
        dts[i] = ((pres[i] - pres[i - 1]) & 0xFFFFFFFF) / freq
        dgs[i] = seg[i].now - seg[i - 1].now
        steps[i] = math.dist(seg[i].v, seg[i - 1].v)
        frozen[i] = seg[i].bits == seg[i - 1].bits
    rows = []
    for i in range(1 + WINDOW, n - WINDOW):
        lo, hi = i - WINDOW, i + WINDOW + 1
        if freq > 1 and max(dts[lo:hi]) > 0.06:
            continue                    # hitch (loading, alt-tab): not what we measure
        if sum(1 for j in range(lo, hi) if not frozen[j]) < 0.3 * (hi - lo):
            continue
        tot_s, tot_t, tot_g = sum(steps[lo:hi]), sum(dts[lo:hi]), sum(dgs[lo:hi])
        if tot_s <= 0 or tot_t <= 0 or dts[i] <= 0:
            continue
        local = tot_s / tot_t
        un_p = abs(steps[i] / dts[i] - local) / local
        un_g = None
        if tot_g > 0 and dgs[i] > 0:
            lg = tot_s / tot_g
            un_g = abs(steps[i] / dgs[i] - lg) / lg
        nxt = seg[i + 1]
        rows.append((dts[i], frozen[i], un_p, un_g,
                     ((nxt.sq1 - nxt.sq0) & 0xFFFFFFFF) / freq, nxt.wait / freq))
    return rows


def pct(vals, p):
    s = sorted(vals)
    return s[min(len(s) - 1, int(p / 100 * len(s)))]


def report(segments, freq):
    summary = []
    for i, (name, _, _) in enumerate(CONFIGS):
        rows = []
        for seg in segments[i]:
            rows += analyse_segment(seg, freq)
        print(f"\n{name}")
        if len(rows) < 100:
            print(f"  not enough moving frames ({len(rows)}); keep the camera moving longer")
            continue
        dts = [r[0] for r in rows]
        t = sum(dts)
        med = statistics.median(dts)
        longf = sum(1 for d in dts if d > 1.5 * med)
        nfro = sum(1 for r in rows if r[1])
        un_p = [r[2] for r in rows]
        un_g = [r[3] for r in rows if r[3] is not None]
        print(f"  moving frames measured : {len(rows)} ({t:.1f} s)")
        print(f"  frame rate             : {len(rows) / t:.1f} fps")
        print(f"  frame time             : median {med * 1000:.2f} ms, 1st pct {pct(dts, 1) * 1000:.2f}, "
              f"99th pct {pct(dts, 99) * 1000:.2f}, std dev {statistics.pstdev(dts) * 1000:.2f} ms")
        print(f"  long frames (>1.5x med): {longf / len(rows) * 100:.1f}% of frames, {longf / t:.1f} per second")
        print(f"  swap wait              : median {statistics.median(r[4] for r in rows) * 1000:.2f} ms")
        print(f"  limiter wait           : median {statistics.median(r[5] for r in rows) * 1000:.2f} ms")
        print(f"  frozen camera frames   : {nfro / len(rows) * 100:.1f}% of frames, {nfro / t:.1f} per second")
        print(f"  camera speed unevenness: mean {statistics.fmean(un_p) * 100:.1f}% vs present times"
              + (f", {statistics.fmean(un_g) * 100:.1f}% vs game clock" if un_g else ""))
        summary.append((name, len(rows) / t, longf / t, nfro / t, statistics.fmean(un_p)))
    if len(summary) > 1:
        print("\nsummary")
        print(f"  {'configuration':14s} {'fps':>6s} {'long frames/s':>14s} {'frozen/s':>9s} {'unevenness':>11s}")
        for name, fps, lf, fr, un in summary:
            print(f"  {name:14s} {fps:6.1f} {lf:14.1f} {fr:9.1f} {un * 100:10.1f}%")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=900, help="moving frames wanted per configuration")
    ap.add_argument("--timeout", type=float, default=300, help="max seconds of recording")
    ap.add_argument("--period", type=float, default=4, help="seconds between configuration switches")
    ap.add_argument("--wait", type=float, default=900, help="max seconds to wait for the game and camera motion")
    ap.add_argument("--limit-us", type=int, default=4000,
                    help="precise limiter's minimum frame time in microseconds (0 = no limiter)")
    args = ap.parse_args()

    if not INFO.exists():
        sys.exit("build/camfix.json missing; run patch.py apply first")
    syms = json.loads(INFO.read_text())["symbols"]
    if "camfix_log" not in syms:
        sys.exit("this build has no frame recorder; run patch.py apply")
    log_va = int(syms["camfix_log"], 16)

    t0 = time.time()
    pid = find_pid("WormsMayhem.exe")
    if not pid:
        print("waiting for WormsMayhem.exe to start...", flush=True)
    while not pid:
        if time.time() - t0 > args.wait:
            sys.exit("game did not start in time")
        time.sleep(2)
        pid = find_pid("WormsMayhem.exe")
    time.sleep(3)
    game = Game(pid, log_va)
    game.snapshot()
    print("attached; start a match and keep the camera moving", flush=True)

    segments = {i: [] for i in range(len(CONFIGS))}
    try:
        collect(game, args, segments)
    except OSError as e:
        print(f"stopped: {e}", flush=True)
    finally:
        try:
            game.configure(0, 1, args.limit_us)     # leave the full patch on
        except OSError:
            pass

    try:
        freq = struct.unpack("<Q", game.read(QPC_FREQ_VA, 8))[0] or 10_000_000
    except OSError:
        freq = 10_000_000

    with open(CSV_OUT, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "segment", "qpc", "game_ms", "ticks", "flags", "px", "py", "pz",
                    "lx", "ly", "lz", "swap_q0", "swap_q1", "limiter_wait_ticks"])
        for ci, segs in segments.items():
            for si, seg in enumerate(segs):
                for fr in seg:
                    w.writerow([CONFIGS[ci][0], si, fr.q, fr.now, fr.ticks, fr.flags]
                               + [repr(x) for x in fr.v] + [fr.sq0, fr.sq1, fr.wait])
    print(f"raw frames saved to {CSV_OUT}")
    report(segments, freq)


if __name__ == "__main__":
    main()
