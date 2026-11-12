Worms Ultimate Mayhem - camera stutter fix
==========================================

Fixes the regular camera stutter of the Steam version of Worms Ultimate Mayhem
(WormsMayhem.exe build 1077, 2013-05-02). Details and source code:
https://github.com/jlrouzies-fr/WormUltimateMayhem-Fixes

Needs Python 3.8 or newer. Close the game first.

Windows
  1. Unzip these files anywhere (inside the game folder is fine).
  2. Double-click apply-fix.bat.
  To undo: double-click restore-original.bat.

Steam Deck / Linux (Desktop Mode, Konsole)
  python3 wum_camfix.py apply
  To undo:  python3 wum_camfix.py restore

If the game is not found automatically, add the game folder:
  python wum_camfix.py apply --game "D:\SteamLibrary\steamapps\common\WormsXHD"

Options
  --fps-cap N   upper frame-rate limit (default 250, so V-Sync sets the pace).
                If V-Sync is forced off in the graphics driver, use e.g. --fps-cap 60.
  status        python wum_camfix.py status   shows whether the exe is patched.

Notes
  - The original exe is kept next to it as WormsMayhem.exe.orig-camfix.
  - Steam's "Verify integrity of game files" (and game updates) undo the patch;
    run apply again afterwards.
  - Only the camera shown on screen and the frame pacing change. The game's
    simulation still runs exactly as before.
