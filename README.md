# Worms Ultimate Mayhem fixes

Unofficial fixes for the Steam version of *Worms Ultimate Mayhem* (PC).

| Fix | What it does |
|---|---|
| [Camera stutter fix](#camera-stutter-fix) | Removes the regular camera stutter that is there at any frame rate, on any hardware |

## Camera stutter fix

### The problem

The camera stutters at a regular interval when it rotates or pans, even with a perfect
frame rate, on a fast PC as well as on a Steam Deck. Two things in the game cause it:

1. **The camera only moves 50 times per second.** The engine advances the camera on a
   fixed 20 ms task and never interpolates. On a 60 Hz display, one frame in six shows
   the same camera position as the frame before it.
2. **The frame limiter is imprecise.** The game limits its frame rate with `Sleep()`
   and never asks Windows for a fine timer. On current Windows, `Sleep()` wakes on a
   15.6 ms grid, which gives an uneven frame cadence (measured: 13 / 18 / 35 ms
   repeating, about 45 fps on average, on a PC that can render the game far faster).

### What the fix does

- **Camera:** the 50 Hz camera simulation is left exactly as it is. Once per rendered
  frame, the camera that is drawn is interpolated between the last two simulation
  steps. Mouse and stick sensitivity do not change.
- **Frame limiter:** the `Sleep()` limiter is replaced by a wait on the high-resolution
  clock, capped at 250 fps by default, so V-Sync sets the pace.

The patch adds one section of code to `WormsMayhem.exe` and redirects five
instructions to it. Nothing else in the game folder is touched, and the original exe
is kept as a backup.

### Results

Measured in-game on a PC (2560x1440, V-Sync on), by recording the camera transform of
every rendered frame while rotating the camera during a match:

| | Original | With camera interpolation |
|---|---|---|
| Frames repeating the previous camera position | 15% (6.8 per second) | 0% |
| Camera speed unevenness against the game clock | 48.7% | 11.5% |

Those numbers were taken with the camera part alone. The frame limiter replacement
has been tested in simulation (it holds a 16.667 ms target to within 0.002 ms) and
played in-game, but there are no before/after numbers for it from the game yet.

### Install

You need Python 3.8 or newer. The Steam Deck already has it. On Windows, install it
from [python.org](https://www.python.org/downloads/) if you do not have it.

1. Download `wum-camera-stutter-fix-<version>.zip` from the
   [Releases](https://github.com/jlrouzies-fr/WormUltimateMayhem-Fixes/releases) page and unzip it.
2. Close the game.
3. Apply the fix:
   - **Windows:** double-click `apply-fix.bat`.
   - **Steam Deck / Linux:** in Desktop Mode, open Konsole in the unzipped folder and run
     `python3 wum_camfix.py apply`.

The game is found automatically when it is in a Steam library. If it is not, pass the
game folder:

```
python wum_camfix.py apply --game "D:\SteamLibrary\steamapps\common\WormsXHD"
```

To undo, double-click `restore-original.bat` or run `python wum_camfix.py restore`.
`python wum_camfix.py status` shows whether the exe is patched.

### Options

| Option | Meaning |
|---|---|
| `--fps-cap N` | Upper frame-rate limit of the new limiter. Default 250, which leaves pacing to V-Sync. Use for example `--fps-cap 60` if V-Sync is forced off in the graphics driver. `0` removes the limit. |
| `--game DIR` | The folder that holds `WormsMayhem.exe`. |

### Good to know

- Only one build is supported: Steam build 1077 (2013-05-02), `WormsMayhem.exe` with
  SHA-1 `a2e3a569a798cf2f90185757578d013ec7a13dd1`. The patcher refuses anything else.
- Steam's "Verify integrity of game files" restores the original exe. Apply the fix
  again afterwards.
- Worms, projectiles and other objects still move at 50 steps per second. An object
  the camera follows can show a slight jitter against the now-smooth camera.
- Scripted camera moves (for example the fly-over at the start of a match) are not
  interpolated yet.
- The game simulation is not changed, but online play with the patch has not been tested.

### Building from source

Source is in [`camera-stutter/`](camera-stutter). See
[`camera-stutter/TECHNICAL.md`](camera-stutter/TECHNICAL.md) for how the engine works,
what is patched, and how to build, test and measure.

### Related

[heatray/WUMPatch](https://github.com/heatray/WUMPatch) is another community patch for
this game. It can change the frame limiter interval, but it does not address the 50 Hz
camera.
