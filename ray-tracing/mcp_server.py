"""
A small MCP server (stdio, no dependencies) for driving the sandbox game through rt.py:
screenshots, settings, live reloads, the free camera, pausing, changing level.

Register it with Claude Code, for example:
    claude mcp add wumrt -- py -3 E:/SteamLibrary/steamapps/common/WormsXHD/WormUltimateMayhem-Fixes/ray-tracing/mcp_server.py

Every tool runs `rt.py` (or `sandbox.py`) next to this file, so it talks to the same sandbox
those scripts do. The sandbox rules still hold: one game at a time, 480x270, 30 fps cap.
"""
import base64
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PY = [sys.executable]


def sandbox_dir():
    text = (HERE / "sandbox.py").read_text(encoding="utf-8")
    m = re.search(r'SANDBOX = HERE\.parent\.parent / "([^"]+)"', text)
    return HERE.parent.parent / (m.group(1) if m else "rt-sandbox")


def run(args, timeout=240):
    p = subprocess.run(PY + args, cwd=HERE, capture_output=True, text=True, timeout=timeout)
    out = (p.stdout + p.stderr).strip()
    return p.returncode, out or "(no output)"


def rt(*args, timeout=240):
    return run(["rt.py", *[str(a) for a in args]], timeout)


def text(s, error=False):
    return {"content": [{"type": "text", "text": s}], "isError": error}


def tool_screenshot(a):
    fd, png = tempfile.mkstemp(suffix=".png")
    os.close(fd)
    try:
        code, out = rt("shot", png)
        if code:
            return text(out, True)
        data = base64.b64encode(Path(png).read_bytes()).decode()
        return {"content": [{"type": "image", "data": data, "mimeType": "image/png"},
                            {"type": "text", "text": out}], "isError": False}
    finally:
        try:
            os.unlink(png)
        except OSError:
            pass


def tool_settings(a):
    ini = sandbox_dir() / "wumrt.ini"
    lines = ini.read_text(encoding="utf-8").splitlines()
    changes = a.get("set") or {}
    if changes:
        for name, value in changes.items():
            if not re.fullmatch(r"[A-Za-z_]+", name):
                return text(f"bad setting name {name!r}", True)
            if name == "sandbox":
                return text("'sandbox' only counts at start-up and must stay 1 for the sandbox", True)
            for i, line in enumerate(lines):
                if re.match(rf"\s*{name}\s*=", line):
                    lines[i] = f"{name} = {value}"
                    break
            else:
                lines.append(f"{name} = {value}")
        ini.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return text("wumrt.ini (re-read by the game within a second of a change):\n" + "\n".join(lines))


def simple(*cmd, timeout=240):
    def f(a):
        code, out = rt(*cmd, timeout=timeout)
        return text(out, code != 0)
    return f


def tool_rtstat(a):
    code, out = rt("rtstat", a.get("seconds", 2))
    return text(out, code != 0)


def tool_key(a):
    names = a["keys"]
    code, out = rt("keys", names, a.get("gap_ms", 400)) if "," in names else rt("key", names, a.get("ms", 120))
    return text(out if out != "(no output)" else f"pressed {names}", code != 0)


def tool_pause(a):
    code, s = rt("screen")
    paused = s.strip() == "paused"
    want = a.get("paused", True)
    if paused != want:
        rt("key", "esc")
        code, s = rt("screen")
    return text(f"screen now: {s.strip()}")


def tool_cg_reload(a):
    code, out = rt("cg-reload", *( [a["file"]] if a.get("file") else []))
    return text(out, code != 0)


def tool_freecam(a):
    action = a.get("action", "get")
    args = [str(x) for x in a.get("args", [])]
    code, out = rt("freecam", action, *args)
    return text(out, code != 0)


def tool_level(a):
    code, out = rt("level", a.get("name", "quick"), timeout=300)
    return text(out, code != 0)


def tool_sandbox(a):
    action = a.get("action", "status")
    if action == "start":
        env = dict(os.environ)
        if a.get("level"):
            env["LEVEL"] = a["level"]
        p = subprocess.run(["sh", "go.sh"], cwd=HERE, capture_output=True, text=True, timeout=300, env=env)
        return text((p.stdout + p.stderr).strip(), p.returncode != 0)
    if action not in ("stop", "status"):
        return text("action must be start, stop or status", True)
    code, out = run(["sandbox.py", action])
    return text(out, code != 0)


TOOLS = {
    "screenshot": ("Screenshot of the sandbox game's next frame (PNG).", {}, tool_screenshot),
    "status": ("Frame rate, draw counts and whether the core is loaded.", {}, simple("status")),
    "rtstat": ("Ray tracer statistics, averaged over some seconds.",
               {"seconds": {"type": "number", "description": "averaging time, default 2"}}, tool_rtstat),
    "settings": ("Read wumrt.ini, or change settings in it (the game re-reads it within a second).",
                 {"set": {"type": "object", "description": "name -> value, e.g. {\"bump\": 2, \"sky\": 4}",
                          "additionalProperties": {"type": ["number", "string"]}}}, tool_settings),
    "key": ("Press a key (enter esc space tab back up down left right a-z 0-9 f1-f12) or a comma-separated sequence.",
            {"keys": {"type": "string"}, "ms": {"type": "number"}, "gap_ms": {"type": "number"}}, tool_key),
    "pause": ("Pause or resume the match (Esc), checking the pause box on screen.",
              {"paused": {"type": "boolean", "description": "true = paused (default)"}}, tool_pause),
    "screen": ("What is on screen: match/other, paused, results or menu.", {}, simple("screen")),
    "reload_core": ("Swap in the wumrt_core.dll that build.sh just built, without restarting the game.", {},
                    simple("reload-core")),
    "reload_compute": ("Recompile the compute shaders and re-read the material/surface ini files.", {},
                       simple("reload")),
    "cg_reload": ("Compile the game's Cg shaders (sandbox CG folder) again; a file that fails keeps its old programs.",
                  {"file": {"type": "string", "description": "e.g. Landscape.cg; default all"}}, tool_cg_reload),
    "freecam": ("Free camera: action take|off|get|set (args X Y Z YAW PITCH)|move (RIGHT UP FORWARD)|"
                "turn (DYAW DPITCH)|orbit (DYAW DPITCH [DIST]).",
                {"action": {"type": "string"}, "args": {"type": "array", "items": {"type": "number"}}}, tool_freecam),
    "level": ("Leave the match through the menus and start another: a landscape name (part of it) or number from "
              "the Versus list, or 'quick' for a random Quick Game. Takes about a minute.",
              {"name": {"type": "string"}}, tool_level),
    "levels": ("The landscapes the Versus menu offers.", {}, simple("levels")),
    "sandbox": ("Start (restart into a match, optionally on a level), stop, or check the sandbox game.",
                {"action": {"type": "string", "enum": ["start", "stop", "status"]}, "level": {"type": "string"}},
                tool_sandbox),
}


def handle(msg):
    method, mid = msg.get("method"), msg.get("id")
    if method == "initialize":
        ver = (msg.get("params") or {}).get("protocolVersion", "2025-06-18")
        return {"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": ver, "capabilities": {"tools": {}},
            "serverInfo": {"name": "wumrt", "version": "1.0"}}}
    if method == "tools/list":
        tools = [{"name": n, "description": d, "inputSchema": {"type": "object", "properties": props}}
                 for n, (d, props, _) in TOOLS.items()]
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": tools}}
    if method == "tools/call":
        p = msg.get("params") or {}
        entry = TOOLS.get(p.get("name"))
        if not entry:
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": f"unknown tool {p.get('name')}"}}
        try:
            result = entry[2](p.get("arguments") or {})
        except subprocess.TimeoutExpired:
            result = text("timed out", True)
        except Exception as e:          # report, keep serving
            result = text(f"{type(e).__name__}: {e}", True)
        return {"jsonrpc": "2.0", "id": mid, "result": result}
    if method == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    if mid is None:
        return None                     # notifications
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"no method {method}"}}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        reply = handle(msg)
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
