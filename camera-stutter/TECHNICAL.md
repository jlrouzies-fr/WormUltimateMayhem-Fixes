# Camera stutter fix: technical notes

Target: `WormsMayhem.exe`, Steam build 1077 (2013-05-02), 32-bit, MSVC 9, not packed,
fixed image base `0x400000`, SHA-1 `a2e3a569a798cf2f90185757578d013ec7a13dd1`.
All addresses below are virtual addresses in that build.

## How the engine keeps time

| What | Where |
|---|---|
| Engine clock: `QueryPerformanceCounter` converted to whole milliseconds | `0x63a903` |
| Main loop (`XomAppBase::Run`) | `0x6f5161` |
| Per-frame update, takes elapsed ms, clamps it to 100 | `0x6f51df` |
| Task manager step | `0x68d4d4` |
| Frame limiter, inside `AppDataService::Run` | `0x4d918e`..`0x4d91ab` |
| The single `SwapBuffers` call | `0x795548` |

The task manager runs two kinds of tasks. Timed tasks run when their scheduled
millisecond is reached and return the delay until their next run. Per-frame tasks run
every frame, but the time they are given is rounded up to a multiple of 20 ms.

## Cause 1: the camera is a 50 Hz timed task

`ZCamUpdateFudgeService::Run` (`0x533c90`) is the only thing that advances the camera.
It calls `CameraManagerService::Update` (`0x51da00`) twice with a constant time step of
0.02 s and reschedules itself 20 ms later. There is no interpolation between two
updates, so at any frame rate that is not exactly 50 fps, some frames repeat the
previous camera transform and others jump by two steps.

The render camera is an `XCamera` object (vtable `0x893814`) at `[[0x95a100] + 8]`. It
holds position, look and up vectors at `+0x120`, `+0x12c` and `+0x138`, followed by two
dirty flags at `+0x144`.

## Cause 2: the frame limiter sleeps on a coarse timer

```
elapsed = GetTimeMs() - last;
if (elapsed < 16) Sleep(16 - elapsed);
last = GetTimeMs();
```

The exe never requests a fine timer (it does not import `winmm`). Since Windows 10
version 2004, a process that does not ask for one gets the default 15.6 ms timer
grid, so the sleep overshoots by a varying amount. Measured on a PC with V-Sync on:
frame times repeat 13 / 18 / 35 ms, about 45 fps on average.

## The patch

A new section `.camfix` is appended at `0x9b2000` and five instructions are redirected
to it:

| Address | Original | Now calls | Purpose |
|---|---|---|---|
| `0x68d4f0` | `call 0x68d5ee` | `glue_frame_begin` | Start of the task manager step: put the true camera transform back |
| `0x68d5ac` | `call 0x68d91f` | `glue_frame_present` | Before the per-frame tasks: write the interpolated transform |
| `0x533c90` | `ZCamUpdateFudgeService::Run` | `glue_camera_tick` | Record the camera transform before and after each 50 Hz step |
| `0x4d918e` | limiter code | `camfix_limiter` | Precise frame limiter |
| `0x795548` | `call [SwapBuffers]` | `glue_swap` | Timestamp each present (used by `measure.py`) |

### Camera interpolation

The camera simulation is not touched. At each camera tick the hook saves the transform
before and after the update. Before each frame is drawn, it writes
`previous + (current - previous) * a` into the `XCamera`, where `a` is the time since
the tick divided by 20 ms. The engine clock only has whole milliseconds, so the
sub-millisecond part is taken from `QueryPerformanceCounter`.

Two details matter:

- The camera code reads the `XCamera` back as part of its own state. The true
  transform is therefore restored at the start of every task manager step, before any
  game logic runs. The simulation ends up bit-for-bit where it would without the patch
  (checked by `test_camfix.c`).
- Other code can write the `XCamera` too (scripted cameras). If the transform is no
  longer the one the hook wrote, the hook leaves it alone.

Driving the camera once per frame with a variable time step was rejected: the chase
camera multiplies mouse movement by the time step, so sensitivity would change with
the frame rate.

The interpolation shows the camera about one tick (20 ms) late.

### Frame limiter

`camfix_limiter` waits until a deadline on the `QueryPerformanceCounter` clock. It asks
for a 1 ms timer through `NtSetTimerResolution`, sleeps 1 ms at a time while more than
2 ms remain, then spins. The default cap is 250 fps (`limit_us = 4000`), which is only
a safety limit: with V-Sync on, the buffer swap sets the pace. The original limiter is
still in the code and can be switched on at run time for comparison.

### Settings block

`camfix_log` starts with a small header that tools can change in the running game:

| Offset | Field | Meaning |
|---|---|---|
| +0 | magic | `0x32474C43` |
| +4 | enabled | 1 = camera interpolation on |
| +8 | widx | number of frames recorded so far |
| +12 | limiter_original | 1 = behave like the engine's own limiter |
| +16 | limit_us | minimum frame time of the precise limiter in microseconds, 0 = none |

It is followed by a ring of 2048 per-frame records (camera transform, game clock, swap
timestamps, limiter wait).

## Files

| File | Purpose |
|---|---|
| `camfix.c`, `glue.s` | The hooks. Freestanding C and a few assembly stubs |
| `wum_camfix.py` | The patcher. Python standard library only. This is what the release ships, with the built hooks embedded |
| `patch.py` | Builds the hooks, makes the release files, and forwards `apply`/`restore`/`status` to `wum_camfix.py` |
| `test_camfix.c` | Test harness that runs the hooks against a simulated engine loop |
| `measure.py` | Records frames from the running game and compares original / limiter only / full patch (Windows) |
| `probe.py` | Prints the hooks' counters from the running game (Windows) |

## Building

Needs LLVM (`clang` and `lld-link` on `PATH`) and `pip install pefile`.

```
python patch.py build     # build/camfix.bin and build/camfix.json
python patch.py apply     # build, then patch the game
python patch.py dist      # dist/wum_camfix.py with the hooks embedded, and the release zip
```

The hooks are compiled for `i686-pc-windows-msvc` with no runtime library and linked
as a one-section image based so that the section lands at `0x9b2000`. They import
nothing; Windows functions are called through the game's own import table.

## Testing

`test_camfix.c` includes `camfix.c` with `-DCAMFIX_TEST`, which replaces the game
addresses with mocks. Build it as a 32-bit program and run it; it ends with
`RESULT: PASS`. It checks, among other things:

- camera steps are even within 0.02% at 50 to 144 fps (in the original, the worst
  frame is off by 100% or more at every rate above 50 fps)
- the simulation's own state is identical with and without the hooks
- writes by other code are not undone
- the precise limiter holds its target within 0.002 ms on a simulated 15.6 ms timer
  grid, and adds no delay when frames are already slower than the cap

`measure.py` does the same comparison in the real game: start it, launch the game,
begin a match and keep the camera moving. It switches between the three configurations
every few seconds and reports frame times, repeated camera frames and how even the
camera speed is.
