"""
Run the sandbox copy of the game (rt-sandbox/) without disturbing whatever else is on
screen: small window, no border, never activated, kept at the bottom of the window stack.

    python sandbox.py start [extra game switches...]
    python sandbox.py stop
    python sandbox.py status
    python sandbox.py slot [take NAME [MIN] | give [NEXT] | free [--force] | queue NAME]

The ONE game slot (shared by main and every stream) is enforced: `start` refuses unless the
slot belongs to this copy's stream (its name comes from streams/NAME/...; main counts as the
stream that holds merge.lock, so a merging session can run main's selftest). Take it with
`sh stream.sh slot take NAME [MIN]` (default 10 min, at most 15; renew by taking again), hand
it on with `slot give NEXT` (NEXT has 5 min to take it, then it lapses to free), or `slot free`.
`slot` alone shows the owner, the expiry and the queue (`slot queue NAME` to join it).

Several sandboxes may run at once (the main one and parallel streams' ones, see stream.sh), at
most MAX_GAMES (1 - user asked 13:45: their PC stutters -, or WUMRT_MAX_GAMES) together; `start` exits with code 3 and a message when
that many already run, so try again later. It never starts while the real game
(WormsXHD/WormsMayhem.exe) runs.
"""
import ctypes
import os
import ctypes.wintypes as wt
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SANDBOX = HERE.parent.parent / "rt-sandbox"
EXE = SANDBOX / "WormsMayhem.exe"
# WormsXHD itself: rt-sandbox's parent, or streams/NAME/rt-sandbox's grandparent's parent.
ROOT = (SANDBOX.parent.parent.parent if SANDBOX.parent.parent.name.lower() == "streams" else SANDBOX.parent).resolve()
REAL_GAME = ROOT / "WormsMayhem.exe"
MAX_GAMES = int(os.environ.get("WUMRT_MAX_GAMES", "16"))  # user 18:25: every session may run its game for 35 min (was 3)
SLOT_ENFORCED = os.environ.get("WUMRT_SLOT", "0") == "1"    # the one-slot rule is off since 15:38
START_LOCK = ROOT / ".wumrt_start.lock"     # held while one start checks the count and launches
SLOT_FILE = ROOT / ".wumrt_slot"            # the one game slot: owner, expiry, hand-over, queue
MERGE_LOCK = ROOT / "WormUltimateMayhem-Fixes" / "merge.lock"

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32.GetForegroundWindow.restype = wt.HWND
u32.GetWindow.restype = wt.HWND
u32.MonitorFromRect.restype = wt.HANDLE
u32.MonitorFromRect.argtypes = [ctypes.POINTER(wt.RECT), wt.DWORD]
u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetWindow.argtypes = [wt.HWND, ctypes.c_uint]
u32.GetParent.restype = wt.HWND
u32.GetParent.argtypes = [wt.HWND]
u32.SetWinEventHook.restype = wt.HANDLE
u32.SetWinEventHook.argtypes = [wt.DWORD, wt.DWORD, wt.HMODULE, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.DWORD]
u32.UnhookWinEvent.argtypes = [wt.HANDLE]
u32.MsgWaitForMultipleObjects.argtypes = [wt.DWORD, ctypes.c_void_p, wt.BOOL, wt.DWORD, wt.DWORD]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
u32.SetWindowLongW.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_long]
u32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                             ctypes.c_int, ctypes.c_uint]
u32.PostMessageW.argtypes = [wt.HWND, ctypes.c_uint, wt.WPARAM, wt.LPARAM]
k32.OpenProcess.restype = wt.HANDLE
k32.TerminateProcess.argtypes = [wt.HANDLE, ctypes.c_uint]

GWL_STYLE, GWL_EXSTYLE = -16, -20
WS_POPUP, WS_VISIBLE = 0x80000000, 0x10000000
WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW = 0x08000000, 0x00000080
SWP_NOSIZE, SWP_NOMOVE, SWP_NOACTIVATE, SWP_FRAMECHANGED = 1, 2, 0x10, 0x20
HWND_BOTTOM = 1


def window_size():
    """The size the sandbox's Default.cfg asks for. Keep it very low: the sandbox shares the
    GPU with whatever else is running."""
    import re
    try:
        t = (SANDBOX / "Default.cfg").read_text()
        return int(re.search(r"/W:(\d+)", t).group(1)), int(re.search(r"/H:(\d+)", t).group(1))
    except (OSError, AttributeError):
        return 480, 270


def sandbox_pids():
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-Process WormsMayhem -ErrorAction SilentlyContinue | "
         "ForEach-Object { \"$($_.Id)|$($_.Path)\" }"],
        capture_output=True, text=True).stdout
    pids = []
    for line in out.split():
        pid, _, path = line.partition("|")
        if path and Path(path).resolve() == EXE.resolve():
            pids.append(int(pid))
    return pids


def windows_of(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    u32.EnumWindows(cb, 0)
    return found


def title(hwnd):
    buf = ctypes.create_unicode_buffer(200)
    u32.GetWindowTextW(hwnd, buf, 200)
    cls = ctypes.create_unicode_buffer(200)
    u32.GetClassNameW(hwnd, cls, 200)
    r = wt.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    return f"'{buf.value}' class {cls.value} at {r.left},{r.top} size {r.right - r.left}x{r.bottom - r.top}"


class StartWatch(threading.Thread):
    """Watches the game's windows from before it starts until stop(): every time one is shown,
    takes the foreground, or sits above the window that had the foreground (checked every few
    milliseconds), so a start can be checked for any pop-up, however short."""
    EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_SHOW = 0x0003, 0x8002
    PROC = ctypes.WINFUNCTYPE(None, wt.HANDLE, wt.DWORD, wt.HWND, wt.LONG, wt.LONG, wt.DWORD, wt.DWORD)

    def __init__(self, fg):
        super().__init__(daemon=True)
        self.fg, self.pid, self.done = fg, None, False
        self.shown, self.took_focus, self.above = [], [], []
        self.t0 = time.time()

    def owner(self, hwnd):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        return p.value

    def mine(self, hwnd):
        return self.pid and hwnd and self.owner(hwnd) == self.pid

    def is_above_fg(self, hwnd):
        """On a monitor and above the foreground window in the Z order."""
        if not self.fg or hwnd == self.fg:
            return False
        r = wt.RECT()
        u32.GetWindowRect(hwnd, ctypes.byref(r))
        if not u32.MonitorFromRect(ctypes.byref(r), 0):     # MONITOR_DEFAULTTONULL: off every screen
            return False
        h = hwnd
        for _ in range(4000):
            h = u32.GetWindow(h, 2)         # GW_HWNDNEXT: the next window below
            if not h:
                return False
            if h == self.fg:
                return True
        return False

    def run(self):
        def cb(hook, event, hwnd, obj, child, thread, ms):
            if obj != 0 or not self.mine(hwnd):
                return
            t = round(time.time() - self.t0, 3)
            if event == self.EVENT_SYSTEM_FOREGROUND:
                self.took_focus.append(t)
            elif event == self.EVENT_OBJECT_SHOW and not u32.GetParent(hwnd):
                self.shown.append((t, self.is_above_fg(hwnd)))
        self.cb = self.PROC(cb)
        hooks = [u32.SetWinEventHook(e, e, None, self.cb, 0, 0, 0)
                 for e in (self.EVENT_SYSTEM_FOREGROUND, self.EVENT_OBJECT_SHOW)]
        msg = wt.MSG()
        while not self.done:
            while u32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
                u32.TranslateMessage(ctypes.byref(msg))
                u32.DispatchMessageW(ctypes.byref(msg))
            if self.pid:
                for hwnd in windows_of(self.pid):
                    if self.is_above_fg(hwnd):
                        self.above.append(round(time.time() - self.t0, 3))
            u32.MsgWaitForMultipleObjects(0, None, False, 3, 0x04FF)
        for h in hooks:
            u32.UnhookWinEvent(h)

    def stop(self):
        self.done = True
        self.join(2)

    def report(self):
        ok = not self.took_focus and not self.above and not any(a for _, a in self.shown)
        print(f"start watch: shown {len(self.shown)}x (above the foreground window: "
              f"{sum(1 for _, a in self.shown if a)}), took the foreground {len(self.took_focus)}x, "
              f"seen above the foreground window in {len(self.above)} checks -> "
              + ("no pop-up" if ok else "POPPED UP"), flush=True)
        if not ok:
            print("  times (s):", self.shown, self.took_focus, self.above[:10], flush=True)
        return ok


OFF_SCREEN = -10000     # x of the game window: off every monitor, so it can never be seen


def tame(hwnd, w, h):
    """Borderless, never activated, bottom of the Z order, client area w x h, OFF every screen.
    It used to sit at 0,0 under everything, and showed whenever the window above it went away
    (18:24 pop-up: a 1926x1109 window over the desktop). The game renders, is screenshotted and
    takes injected keys the same off-screen (tested 18:42: 28 fps, normal frames)."""
    u32.SetWindowLongW(hwnd, GWL_STYLE, ctypes.c_long(WS_POPUP | WS_VISIBLE).value)
    ex = u32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    u32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW)
    u32.SetWindowPos(hwnd, HWND_BOTTOM, OFF_SCREEN, 0, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED)


def any_game_running():
    """Every WormsMayhem process, whichever folder it runs from (the other sandbox, the real game)."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process WormsMayhem -ErrorAction SilentlyContinue | ForEach-Object { $_.Path }"],
                         capture_output=True, text=True).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


def take_start_lock():
    """Only one start at a time checks how many games run and launches (mkdir is atomic)."""
    t0 = time.time()
    while True:
        try:
            os.mkdir(START_LOCK)
            return
        except FileExistsError:
            try:
                if time.time() - START_LOCK.stat().st_mtime > 120:    # left behind by a killed start
                    os.rmdir(START_LOCK)
                    continue
            except OSError:
                pass
            if time.time() - t0 > 90:
                sys.exit(f"another sandbox start has held {START_LOCK} for 90 s; try again later")
            time.sleep(0.5)


def release_start_lock():
    try:
        os.rmdir(START_LOCK)
    except OSError:
        pass


def caller_name():
    """This copy's stream name; main is the stream that holds merge.lock (or 'main')."""
    if SANDBOX.parent.parent.name.lower() == "streams":
        return SANDBOX.parent.name
    try:
        return (MERGE_LOCK / "owner").read_text().split()[0]
    except (OSError, IndexError):
        return "main"


def slot_read():
    import json
    try:
        s = json.loads(SLOT_FILE.read_text())
    except (OSError, ValueError):
        s = {}
    return {"owner": s.get("owner"), "until": s.get("until", 0), "offer": s.get("offer"),
            "offer_until": s.get("offer_until", 0), "queue": s.get("queue", [])}


def slot_write(s):
    import json
    SLOT_FILE.write_text(json.dumps(s))


def slot_state(s):
    now = time.time()
    if s["owner"] and s["until"] > now:
        return "held", s["owner"]
    if s["offer"] and s["offer_until"] > now:
        return "offered", s["offer"]
    return "free", None


def slot_text(s):
    state, who = slot_state(s)
    t = time.strftime("%H:%M:%S", time.localtime(s["until"] if state == "held" else s["offer_until"]))
    head = {"held": f"slot: held by {who} until {t}", "offered": f"slot: given to {who}, who can take it until {t}",
            "free": "slot: free"}[state]
    return head + "\nqueue: " + (", ".join(s["queue"]) if s["queue"] else "(empty)")


def slot_cmd(args):
    """slot [take NAME [MIN] | give [NEXT] | free [--force] | queue NAME]"""
    me = caller_name()
    take_start_lock()
    try:
        s = slot_read()
        state, who = slot_state(s)
        op = args[0] if args else ""
        if op == "take":
            name = args[1] if len(args) > 1 else me
            minutes = min(15.0, float(args[2])) if len(args) > 2 else 10.0
            if state != "free" and who != name:
                print(slot_text(s), file=sys.stderr)
                print(f"BUSY: the slot is not free; join the queue with: sh stream.sh slot queue {name}", file=sys.stderr)
                sys.exit(3)
            s.update(owner=name, until=time.time() + minutes * 60, offer=None, offer_until=0)
            s["queue"] = [q for q in s["queue"] if q != name]
        elif op == "give":
            if state == "held" and who != me and "--force" not in args:
                sys.exit(f"the slot is held by {who}, not {me}; only its owner gives it on")
            nxt = next((a for a in args[1:] if a != "--force"), None) or (s["queue"][0] if s["queue"] else None)
            if not nxt:
                s.update(owner=None, until=0, offer=None, offer_until=0)
            else:
                s.update(owner=None, until=0, offer=nxt, offer_until=time.time() + 300)
                s["queue"] = [q for q in s["queue"] if q != nxt]
        elif op == "free":
            if state == "held" and who != me and "--force" not in args:
                sys.exit(f"the slot is held by {who}, not {me}; use --force only if it is clearly abandoned")
            s.update(owner=None, until=0, offer=None, offer_until=0)
        elif op == "queue":
            name = args[1] if len(args) > 1 else me
            if name not in s["queue"] and name != who:
                s["queue"].append(name)
        elif op:
            sys.exit(slot_cmd.__doc__)
        if op:
            slot_write(s)
        print(slot_text(s))
    finally:
        release_start_lock()


def start(extra):
    if sandbox_pids():
        sys.exit("sandbox game is already running")
    ini = SANDBOX / "wumrt.ini"
    if not ini.exists() or "sandbox = 1" not in ini.read_text():
        sys.exit(f"{ini} must contain 'sandbox = 1' (otherwise the game would take keyboard, mouse and focus)")
    take_start_lock()
    try:
        launch(extra)
    finally:
        release_start_lock()


def launch(extra):
    s = slot_read()
    state, who = slot_state(s)
    me = caller_name()
    if SLOT_ENFORCED and (who != me or state != "held"):
        print(slot_text(s), file=sys.stderr)
        print(f"BUSY: the game slot is not {me}'s. Take it first: sh stream.sh slot take {me} [MIN] "
              f"(or join the queue: sh stream.sh slot queue {me}).", file=sys.stderr)
        sys.exit(3)
    w, h = window_size()
    if (w, h) != (480, 270) and os.environ.get("WUMRT_ALLOW_SIZE") != "1":
        # user 18:30: "Keep many, 480x270 only" (a 1920x1080 start popped up at 18:24)
        sys.exit(f"refused: Default.cfg asks for {w}x{h}; sandboxes start at 480x270 only "
                 "(set /W:480 /H:270 in this sandbox's Default.cfg)")
    games = [Path(g) for g in any_game_running()]
    if any(g.resolve() == REAL_GAME.resolve() for g in games if g.exists()):
        sys.exit(f"the real game ({REAL_GAME}) is running; sandboxes never start beside it")
    if len(games) >= MAX_GAMES:
        print(f"BUSY: {len(games)} sandbox games already run (at most {MAX_GAMES} at once): "
              + ", ".join(str(g.parent) for g in games) + ". Try again later.", file=sys.stderr)
        sys.exit(3)
    fg = u32.GetForegroundWindow()
    watch = StartWatch(fg)
    watch.start()
    si = subprocess.STARTUPINFO()
    si.dwFlags = subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 4          # SW_SHOWNOACTIVATE
    p = subprocess.Popen([str(EXE), *extra], cwd=str(SANDBOX), startupinfo=si, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)   # never holds the caller's pipes
    watch.pid = p.pid
    release_start_lock()            # the new process is now counted by any other start
    print(f"started pid {p.pid}", flush=True)
    hwnd = None
    t0 = time.time()
    while time.time() - t0 < 60 and p.poll() is None:
        ws = windows_of(p.pid)
        if ws:
            hwnd = ws[0]
            break
        time.sleep(0.1)
    if p.poll() is not None:
        watch.stop()
        sys.exit(f"game exited early with code {p.returncode}")
    if not hwnd:
        watch.stop()
        sys.exit("no window after 60 s")
    print("window:", title(hwnd), flush=True)
    tame(hwnd, *window_size())
    for _ in range(20):         # keep it down while the game finishes starting
        time.sleep(0.25)
        u32.SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
        now = u32.GetForegroundWindow()
        if now != fg and fg:
            print("foreground changed; putting it back", flush=True)
            u32.SetForegroundWindow(fg)
    print("after taming:", title(hwnd))
    watch.stop()
    watch.report()
    print("foreground unchanged:", u32.GetForegroundWindow() == fg)


def stop():
    pids = sandbox_pids()
    if not pids:
        print("sandbox game is not running")
        return
    for pid in pids:
        for hwnd in windows_of(pid):
            u32.PostMessageW(hwnd, 0x0010, 0, 0)    # WM_CLOSE
    time.sleep(3)
    for pid in sandbox_pids():
        h = k32.OpenProcess(1, False, pid)          # PROCESS_TERMINATE
        k32.TerminateProcess(h, 0)
        k32.CloseHandle(h)
        print(f"terminated pid {pid}")
    print("stopped")


def status():
    pids = sandbox_pids()
    if not pids:
        print("not running")
    for pid in pids:
        print(f"pid {pid}:", "; ".join(title(h) for h in windows_of(pid)) or "no visible window")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "start":
        start(sys.argv[2:])
    elif cmd == "stop":
        stop()
    elif cmd == "status":
        status()
    elif cmd == "slot":
        slot_cmd(sys.argv[2:])
    else:
        sys.exit(__doc__)
