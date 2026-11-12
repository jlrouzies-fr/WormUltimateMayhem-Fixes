"""
Read camfix's counters from a running, patched WormsMayhem.exe.

    python probe.py          print once
    python probe.py watch    print every second until Ctrl+C

Run it while in a match. Healthy output: `ticks` rising about 50 per second and
`interpolated` rising at roughly the frame rate whenever the camera is moving.
"""
import ctypes
import ctypes.wintypes as wt
import json
import struct
import sys
import time
from pathlib import Path

INFO = Path(__file__).resolve().parent / "build" / "camfix.json"
FIELDS = ["magic", "valid", "active", "tick_time", "q_last", "now_last", "frac",
          "ticks", "interpolated", "restored", "external", "no_xcam"]

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", wt.WCHAR * 260)]


def find_pid(name):
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
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


def read(handle, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    if not k32.ReadProcessMemory(handle, addr, buf, size, ctypes.byref(got)):
        raise OSError(ctypes.get_last_error(), "ReadProcessMemory failed")
    return buf.raw


def main():
    if not INFO.exists():
        sys.exit("build/camfix.json missing; run patch.py apply first")
    state_va = int(json.loads(INFO.read_text())["symbols"]["camfix_state"], 16)
    pid = find_pid("WormsMayhem.exe")
    if not pid:
        sys.exit("WormsMayhem.exe is not running")
    k32.OpenProcess.restype = wt.HANDLE
    h = k32.OpenProcess(0x0410, False, pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        sys.exit("could not open the game process")
    last = None
    while True:
        try:
            raw = read(h, state_va, 4 * 12 + 4 * 27)
        except OSError:
            sys.exit("could not read the hook state; is this exe patched?")
        v = list(struct.unpack("<6If5I", raw[:48]))
        if v[0] != 0x58494643:
            sys.exit("hook state not found at the expected address; exe is not patched with this build")
        d = dict(zip(FIELDS, v))
        prev = struct.unpack("<3f", raw[48:60])
        cur = struct.unpack("<3f", raw[84:96])
        line = (f"ticks={d['ticks']} interpolated={d['interpolated']} restored={d['restored']} "
                f"external={d['external']} no_xcam={d['no_xcam']} valid={d['valid']} "
                f"frac={d['frac']:.2f} cam_pos=({cur[0]:.1f}, {cur[1]:.1f}, {cur[2]:.1f})")
        if last:
            line += f"  | per second: ticks +{d['ticks'] - last['ticks']}, interpolated +{d['interpolated'] - last['interpolated']}"
        print(line, flush=True)
        if len(sys.argv) < 2 or sys.argv[1] != "watch":
            break
        last = d
        time.sleep(1)


if __name__ == "__main__":
    main()
