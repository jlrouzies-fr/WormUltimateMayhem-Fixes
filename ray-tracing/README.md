# Ray-traced sun shadows and indirect light (experimental)

Real-time ray-traced sun and moon shadows, plus ray-traced sky and bounced light, for Worms Ultimate Mayhem. This is a working
prototype. It has only been run in a sandbox copy of the game; an install script for the
real game exists but has not been tried on one.

## What it does

- Every object and the terrain cast and receive sharp or soft shadows. The stock game only
  puts a blurry shadow map on the terrain.
- Shadows are traced on the GPU with OpenGL compute shaders. No ray-tracing hardware is needed.
- Grass fringes and other alpha-textured pieces cast shadows shaped by their texture.
- Optional sky occlusion: ambient light is dimmed where the sky is blocked (under objects,
  in corners), traced with short rays over the hemisphere above each surface.
- Optional bounced sunlight: where those rays find a nearby surface the sun reaches, some
  of that light is added, tinted by the surface (a sunlit grass bank turns the rock next to
  it slightly green). The tint is the average colour of the surface's texture.
- Optional traced sky light (`gi = 1`), which replaces the two options above and the game's
  constant ambient colour: rays over the hemisphere above each surface return the colour of
  the sky where they get away and the lit colour of what they hit otherwise, read from the
  hit surface's own texture. Light bounces more than once, because a hit surface also passes
  on the indirect light it received in the previous frame.
- Optional traced reflections in the sea (`reflections = 1`): where the camera sees the
  water, a mirrored ray returns the sky or the lit, textured surface it finds, and the
  game's water shader blends that in by viewing angle, shifted by its own ripples.
- Optional materials and glossy reflections (`specular = 1`): every material texture has a
  roughness, a reflectivity, a metal flag and an emissive strength. They start from the
  game's own material data (specular power and colour, emissive colour) and are overridden
  per texture by `wumrt_materials.ini`. A surface that is glossy enough gets one mirrored
  ray; rougher materials read blurrier textures and sky instead of spreading rays, so the
  result has no noise. Emissive surfaces light their surroundings through the sky rays.

- Optional replacement textures (`bump = 1` for the normal maps): the game's textures are
  small, mostly 128 x 128. `textures.py` dumps the ones a match uses, upscales them 4x with
  a Real-ESRGAN executable and makes a normal map for each; the harness loads the upscaled
  image into the game's own texture and gives the normal map to the object and terrain
  shaders, which work out tangents per pixel because the meshes have none.

- Optional shell-layer grass and moss (`shells = 16`): terrain whose texture is listed as
  grass or moss in `wumrt_surfaces.ini` is drawn again in layers pushed out along the surface
  normal, and each layer keeps only the pixels inside a blade at that height, so the painted
  grass gets real thickness, hides the feet of what stands in it and has a ragged outline.
  It does not need the tracer (`rt = 0` works), only the edited terrain shader. Before/after:
  `captures/shells_apple_shells.png` and the full-resolution crop `captures/shells_apple_crop.png`.

- Smooth normals in the tracer (`smooth = 1`): rays start from the curved surface that the
  meshes' vertex normals describe rather than from the flat triangles, so round low-polygon
  shapes (worms, boulders, hulls) lose the shadow steps and creases that followed their
  triangles. Before/after: `captures/worm_normals_smooth.png` (crop `worm_normals_crop.png`).

- Optional rounded shapes (`round = 1`): worms and props are drawn as curved patches through
  their triangles (PN triangles), cut into smaller triangles, so their outlines become curves
  instead of polygons; the tracer gets the same rounded props. See "Rounded shapes" below.

- Optional scattered stones and tufts (`scatter = 1`): chunky low-poly pebbles on sand and
  rock, rubble where a rock wall stands on sand or grass, and tufts of blades along the border
  of grass, made from the terrain's own triangles by what each texture is
  (`wumrt_surfaces.ini`), drawn with the game's terrain shading and traced like everything
  else. Before/after: `captures/scatter_sand_scatter.png`, `scatter_beach_scatter.png`,
  `scatter_lip_scatter.png`.

## How it works

1. The game draws its shadow map first. Those draws are the complete list of shadow casters,
   so the harness collects them: meshes that do not change are cached, static ones are merged
   into one BVH on a worker thread, animated ones keep their tree and have its boxes refitted.
2. When the first sun-lit draw of the main view arrives, a compute shader traces one camera
   ray and one or more sun rays per pixel into a mask texture (sun visibility and distance).
3. With sky light on, the game's sky draws are repeated into six small views around the
   camera (a cube map in one texture), which is what the rays read where they leave the
   scene. Every material texture gets a 128 x 128 layer in an array texture, and every
   triangle keeps its texture coordinates, so a ray knows the colour of what it hit. The
   sky's brightness is scaled so that open ground receives as much light as the game's
   ambient colour gave it.
4. A temporal pass accumulates sun visibility, sky visibility and indirect light per pixel
   (reprojected to where the surface was last frame, with a per-pixel count of frames
   accumulated) and measures how much the indirect light still varies. The sun's history
   is clamped to what the pixel's neighbourhood shows now, so moving shadows leave no
   trail. A small blur then smooths sun and sky visibility, and the indirect light goes
   through several wider passes that blur where variance remains and keep detail where
   it has settled (the SVGF scheme).
5. The game's own shaders (edited copies of `FixedFunction.cg` and `Landscape.cg`) read the
   traced textures: sun visibility scales the sun's diffuse and specular light, sky
   visibility scales ambient light, and bounced light is added to the diffuse term.

## Files

| File | Purpose |
|---|---|
| `loader.c`, `wumrt_api.h`, `ctl.h` | The permanent part of the harness (`glut32.dll`): import-table thunks, the control block, input isolation, the Cg program record and handle translation, loading and swapping the core |
| `wumrt.c` | The core's hooks (`wumrt_core.dll` with `rt_core.c` and `bvh.c`): GL state tracking, screenshots, commands, settings, shell layers, free camera |
| `rt_core.c` | Caster collection, mesh cache, merged static BVH, compute dispatch |
| `bvh.c` | BVH builder |
| `wumrt_trace.comp`, `wumrt_temporal.comp`, `wumrt_filter.comp`, `wumrt_atrous.comp` | Tracer, accumulation over time, and the spatial filters (GLSL) |
| `patch_shaders.py` | Edits copies of the game's Cg shaders (objects, terrain, water) to use the traced textures |
| `install.py` | Installs or removes the mod in a game folder, keeping the originals |
| `sandbox.py`, `rt.py`, `go.sh` | Run the sandbox game in the background and drive it (including the live reloads, the free camera and changing level) |
| `mcp_server.py` | MCP server (stdio, no dependencies) offering the `rt.py` commands as tools, screenshots as images |
| `stream.sh` | Parallel work streams: own copy, merge base and sandbox per stream; new, sync, merge (with `merge.lock`), unlock, list, delete |
| `capfile.py`, `rtcpu.c`, `trace_capture.py`, `compare_mask.py` | Frame capture and the CPU reference tracer used to validate the GPU one |
| `selftest.sh` | Smoke test: build, run the sandbox, check against the reference, look for errors |
| `steps.py`, `noise.py`, `stability.py` | Work per pixel (BVH steps), noise of the sky term, and noise and flicker of the indirect light on a paused view; none depends on how busy the GPU is |
| `relief.py` | Heights for the parallax relief, integrated from the installed normal maps into their alpha (`heights`, `clear`, `sheet`) |
| `textures.py` | Dump a match's textures, make normal maps, upscale colours, import maps made by other tools; output goes to `wumrt_textures/` in the game folder |
| `ai_maps.py`, `models/` | Run a texture model on the dumped textures (needs PyTorch and spandrel); `models/` holds PBRify_Remix's normal and roughness models (CC0, from github.com/Kim2091/PBRify_Remix) |
| `materials.py`, `wumrt_materials.ini` | Contact sheets of a level's material textures for labelling, and the per-texture overrides (keyed by a hash of the texture's pixels) |
| `surfaces.py`, `wumrt_surfaces.ini` | Which terrain textures are grass, moss, rock or sand (same hash key), and contact sheets for choosing them: green textures of the library, or the textures the last match drew terrain with |
| `scatter.c` | Scattered stones and tufts (included by `rt_core.c`) |
| `redo_textures.py`, `restyle_textures.py`, `film_textures.py`, `wumrt_texstyle.ini` | AI redo of the replacement colours at 1024: realistic, loose film style, and the middle film style with a strength per texture class (see "Textures") |
| `texpack.py` | Packs replacement textures as DXT1/DXT5 with mip levels (`.wtc`), so the game uploads them without the driver compressing them |
| `texmem.py`, `texmem_test.sh` | The sandbox game's 32-bit memory, its large-address-aware flag (sandbox exe copy only), and the one-batch memory measurement |
| `texset_finish.sh`, `texinstall_main.sh`, `judgeprobe.sh` | After a colour run: normal maps, model heights, install and pack into a texture folder; the swap of main's sandbox folder (old set kept as `wumrt_textures_redo`); screenshots of one level with a folder |
| `costprobe.sh`, `perfprobe.sh`, `motionprobe.sh`, `sunprobe.sh`, `sunprobe2.sh` | GPU cost per feature (feature off between two baselines), `gi_rate` noise/flicker, `gi_rate` in motion, and the fixed-view sun visibility checks |
| `round.c` | Rounded shapes: PN-triangle subdivision of worm and prop draws (included by `wumrt.c`) |
| `shape_hd.sh` | 1080p before/after on the active worm (paused, scale = 0.5, puts 480x270 back); any setting with SET/A/B, PLAY=1 also logs the cost in play |
| `shape_play.sh` | In-play cost of `round_casters` 1 vs 2 at 480x270, camera close to the active worm |
| `grassy.sh` | Restart the sandbox until Quick Game lands on a level with a lot of grass terrain (`LEVEL=NAME sh go.sh` or `rt.py level NAME` now pick one directly: Apple Core Island has plenty) |
| `wumrt_characters.ini`, `char_ab.py`, `lookat.py` | Which textures are the worms' skin, face and eyes (same hash key); before/after of the character lighting on the paused view; aim the free camera at a world point (the worms' positions are in the log) |
| `ab_capture.py`, `ab_label.py`, `ab_setting.py` | Before/after screenshots of the running sandbox match (stock, sun shadows, everything) and captioned comparison images, in `captures/` |
| `post.c`, `post.h`, `wumrt_post.comp` | Film post-processing: tone curve, grade, bloom, depth of field, FXAA on the scene before the HUD |
| `bench.sh`, `imgtool.py` | Timing and image helpers |
| `fx.c`, `fx.h`, `wumrt_fx.comp`, `fx_watch.py`, `fx_ab.sh`, `fx_slot.sh` | Light from effects (see that section), and catching an explosion in the sandbox to test it |

## Running it

The sandbox is a separate folder (`rt-sandbox/` next to the game) with its own copy of the
exe, shaders and config, and links to the game's data. `build.sh` compiles the harness into
it as `glut32.dll`, the loader (the game's own is kept as `glut32_real.dll`), and
`wumrt_core.dll`, everything else (see "Live editing" below: most changes no longer need a
restart). `sandbox.py start` refuses to run while any copy of the game is running.

The sandbox game never comes up over what you are doing, not even for a frame. With
`sandbox = 1` in `wumrt.ini` the loader hooks window creation (a CBT hook on the main thread,
because the exe looks `CreateWindowEx` up at run time) and the game's `ShowWindow` and
`SetWindowPos`: its windows are created off-screen (-10000, -10000), at the bottom of the Z
order, with `WS_EX_NOACTIVATE` and no taskbar button; activating them is refused, and every
show or move keeps them at the bottom without activation. `sandbox.py` then moves the window to
0,0, still at the bottom. It also watches the start (window events and the Z order every few
milliseconds) and prints `start watch: ... -> no pop-up` or `POPPED UP`. Screenshots read the
framebuffer back, so the window never needs to be seen; it renders at the capped 30 fps.

```
sh build.sh
python patch_shaders.py ../../rt-sandbox/CG     # on fresh copies of the two .cg files
sh go.sh                                        # start the sandbox (480x270, 30 fps cap) and enter a Quick Game
python rt.py rtstat 2                           # tracer statistics
python rt.py shot out/view.png                  # screenshot
```

Settings are read from `wumrt.ini` in the sandbox folder:

```
rt = 1          # 0 = the game's own shadows
samples = 8     # sun rays per pixel; 1 = hard shadows
radius = 1.5    # sun radius in degrees (the real sun is 0.27)
filter = 2      # blur radius in pixels for soft shadows
scale = 1       # mask resolution relative to the render resolution (0.5 = half)
max_megapixels = 4    # the traced textures never exceed this many million pixels
sky = 8         # sky occlusion rays per pixel; 0 = off (the default)
sky_reach = 80  # how far those rays look, in world units
sky_strength = 0.85   # how strongly blocked sky dims ambient light (0..1)
bounce = 0.6    # strength of bounced sunlight; 0 = off (the default); needs sky rays
accumulate = 0.95  # largest share of the accumulated sky and bounce light kept each frame (0 = off)
gi = 1          # traced sky light instead of constant ambient (needs sky > 0; replaces sky_strength and bounce)
gi_sky = 1      # brightness of the sky's light, relative to the game's ambient colour
gi_bounce = 0.7 # strength of light bounced by surfaces
gi_tint = 0.5   # how much of the sky's own colour its light keeps (0 = the ambient colour's hue)
gi_rate = 1     # 2 or 4: each block of 8x4 pixels traces sky rays every 2nd / 4th frame (see "Measured cost")
gi_far = 0      # > 0: half the sky rays beyond this distance (world units)
denoise = 3     # filter passes for the indirect light (1..5); each reaches twice as far as the last
reflections = 1 # traced reflections in the sea (0 = the game's own water)
reflection_strength = 1
water_style = 0 # stylised sea: depth colour, shore foam, sun glints (needs reflections = 1); see "Stylised sea"
specular = 1    # materials and traced reflections on glossy surfaces
specular_strength = 1
smooth = 1      # trace from the smooth surface the vertex normals describe (0 = flat triangles)
round = 1       # draw worms and props as rounded patches (0 = off, the default); see "Rounded shapes"
round_level = 3 # small triangles along each edge (2..6)
round_casters = 1     # the tracer traces the rounded mesh: 0 never, 1 props, 2 also worms (removes the bright fringe on close worms; cost below)
round_bevel = 1       # rounded highlights along the hard edges of props (0 = off, the default)
round_min_pixels = 10 # meshes smaller than this radius on screen are drawn as they are
round_full_pixels = 40  # worms smaller than this radius on screen get 2 steps instead of round_level
bump = 2        # strength of normal maps from wumrt_textures (0 = off); 1 is barely visible, 4 is heavy
relief = 0      # parallax relief from the heights in the normal maps' alpha (0 = off, the default; 1 = as relief.py wrote them)
relief_shadow = 6    # how dark relief in the sun's way makes a point (0 = no self-shadowing)
shells = 16     # layers drawn over grass and moss terrain (0 = off, the default)
shell_height = 3.5    # height of the outermost layer, world units
shell_density = 0.3   # blade cells per world unit (0.5 was the old default; 2 gives fine fur, which does not suit the game's style)
shell_width = 0.45    # radius of a blade's foot, in cells (at most 0.45)
shell_shadow = 0      # 1 = the blades shade each other and the ground from the sun
grass_wind = 0        # wind sway of the shells and the border tufts (0 = still, 1 = gentle)
grass_patches = 0     # painted colour patches over the grass (0 = one flat green, 1 = full)
cloud_shadows = 0     # soft drifting cloud shadows, strength 0..1 (0 = off)
cloud_scale = 160     # size of a cloud, world units
cloud_speed = 6       # drift, world units a second
frame_timer = 0 # 1 = log the GPU time per frame every 60 frames (timestamp queries)
skin_wrap = 1   # worms: sunlight wraps past the shadow line, skin in shade stays warm (0 = off, the default)
skin_translucency = 1 # worms: warm light through thin parts, traced (0 = off, the default; < 0 = debug view)
skin_depth = 4  # world units of worm that let 1/e of that light through (a worm is about 10 across)
rim_light = 1   # worms: soft rim of sky or back light along their outline (0 = off, the default)
catchlights = 1 # worms: the glint painted in their pupils shown at full brightness (0 = off, the default)
contact_shadow = 2   # worms: soft dark patch where they touch the ground (0 = off, the default; 2 judged good; < 0 = debug view)
contact_reach = 20   # how close, in world units
foliage_glow = 0     # leaves and grass cut-outs glow warm green with the sun behind them (0 = off, the default; < 0 = debug view)
foliage_wrap = 0     # soft light wrap on foliage (0 = off, the default)
foliage_depth = 40   # world units of leafy mass that let 1/e of the light through
blade_glow = 0       # grass and canopy shell blades glow warm green with the sun behind them (0 = off, the default; 1 judged good)
blade_tip = 0        # bright rim on the blade tips when backlit (0 = off, the default; 0.5 judged good)
scatter = 0     # 1 = stones and tufts on the terrain (0 = off, the default)
scatter_density = 1   # how many, relative
scatter_size = 1      # how big, relative (a worm is about 15 units tall; pebbles are 1.2 to 4.5, rubble 3.5 to 8.5)
sandbox = 1     # cut the game off from keyboard, mouse, pads and focus (testing only)
```

The file is read again whenever it changes, so settings can be tuned while the game runs.
Outside sandbox mode, F9 switches the tracing on and off for comparison.

To try it in the real game (untested there): `sh build.sh`, close the game, then
`python install.py install --game <game folder>`. `python install.py uninstall --game <game folder>`
puts everything back.

## Live editing (no restarts)

The harness is split in two. `glut32.dll` (`loader.c`) is a small permanent loader: it owns
the import-table slots, the control block, the input isolation and the record of the game's
Cg programs. Everything else is `wumrt_core.dll`, which the loader runs from a uniquely
named copy in `wumrt_core_copies/`, so `build.sh` can overwrite it while the game runs.
Only a change to `loader.c`, `ctl.h` or `wumrt_api.h` still needs a restart (`build.sh` says
so when the loader changed while the game holds it).

| Change | Command |
|---|---|
| C code of the core (`wumrt.c`, `rt_core.c`, `bvh.c`) | `sh build.sh` then `python rt.py reload-core` |
| Compute shaders, `wumrt_materials.ini`, `wumrt_surfaces.ini`, replacement textures | `python rt.py reload` (as before) |
| Settings | edit `wumrt.ini` (as before) |
| The game's Cg shaders (`CG/*.cg` in the sandbox, e.g. after `patch_shaders.py`) | `python rt.py cg-reload [Landscape.cg]` |
| Camera | `python rt.py freecam take`, then `turn`, `move`, `orbit`, `set`, `off` |
| Level | `python rt.py level NAME` (or `quick`); `LEVEL=NAME sh go.sh` to start on one |

**reload-core** happens at the end of the next frame: the old core zeroes our uniforms in
the game's shaders, stops its worker thread, deletes its GL objects and is unloaded; the new
one is loaded, re-hooked, given every Cg program the engine has, and starts with empty
caches. It copies the engine's existing vertex buffers into its CPU mirror (about 20 ms),
then rebuilds its meshes and the merged BVH over the next second. The loader keeps a few
things across reloads so that a swap is invisible at 30 fps: the frame cap, the free camera,
the normal-map textures and the original hash of every game texture (once a replacement
image is loaded into a texture its pixels no longer give the same hash), and compiled
compute programs whose source and defines are unchanged (compiling the tracer takes the
driver several seconds; a changed shader is compiled as usual). The core allocates from a
heap of its own (`malloc` and friends are redirected in `rt.h` when building with
`WUMRT_CORE`), which goes away with it. A core that fails to load leaves the game running
on the loader alone (no tracing) until the next `reload-core`. Measured: about 0.15 s per
swap including the Python round trip, no dropped frames; `rt.py verify` finds the tracked
GL state right after a swap.

**cg-reload** compiles every program of the `.cg` files again with the engine's own
arguments (context, profile, entry point, compiler options). A file is replaced only if all
of its programs compile and load; otherwise the old programs stay in use and the compiler's
message is printed (from `cgGetLastListing`) and logged. The engine keeps using the handles
it created at start-up: the loader translates every call that names a program or a parameter
(`cgGLBindProgram`, `cgGLLoadProgram`, `cgUpdateProgramParameters`, `cgDestroyProgram`,
`cgGLSetParameter1f..4f`, `cgGLSetMatrixParameterfr`, `cgGLSetTextureParameter`,
`cgGLEnable/DisableTextureParameter`, `cgGetFirstParameter`, `cgGetNextParameter`,
`cgGetFirstStructParameter`, `cgGetArrayParameter`) to the new program, parameters by name.
It also remembers the last value the engine gave every parameter and gives it to the new
program. A parameter the new program no longer has is dropped (logged once). Our own
uniforms (`rtSomething`) are hidden from the engine's parameter walks by the loader, which
is why they must keep the `rt` + capital letter naming. All four files compile in about
0.35 s, one file in about 50 ms. Old programs are kept (not destroyed), a few kilobytes
each. Editors may briefly find a `.cg` file locked right after a reload; try again.

**freecam** replaces the view matrix of the scene cameras (those that draw the landscape;
not the shadow pass, not the HUD's models) at the moment the engine gives it to a program, so
the shaders and the tracer both use it. Light directions and positions that the engine
passes in eye space, `viewProjection`, the sky's view and the sea's matrices and camera
position are corrected to match (checked: the corrected sun direction equals the tracer's
world-space sun seen through the free view, cosine 1.0000). It works while the match is
paused, which is the way to get repeatable close-ups: `freecam take` starts at the game's
view, `turn DYAW DPITCH`, `move RIGHT UP FORWARD` (camera axes, world units), `orbit DYAW
DPITCH [DIST]` (around the point DIST ahead; by default what the centre pixel shows, from the
traced mask), `set X Y Z YAW PITCH` (every command prints the `set` line that reproduces the
current view), `off`. Limits: the engine still culls and picks levels of detail with its own
camera, so objects outside its view are missing; the stock shadow map is fitted to the game's
camera (the traced shadows are not affected). While the game's own camera is moving (not
paused), things drawn before the terrain in a frame can still show the game's view for that
frame.

**level** goes back to the main menu (from a match, the pause menu, a round's results or
any menu) and starts a Versus deathmatch on the landscape asked for (a part of its name or
its number in `python rt.py levels`: five random themes, four prebuilt and 24 Worms 3D
maps), with a human team and a CPU team, or a Quick Game (`quick`). It reads the screen to
know where it is (pause box, results tick, main-menu logo) and checks that the tracer has a
scene afterwards. About 45-60 s per change, mostly loading. The engine does not create new
Cg programs per match, so changing level many times in one process costs nothing extra.

**MCP server**: `mcp_server.py` offers screenshot (as an image), status, rtstat, settings
(read or change `wumrt.ini`), key, pause, screen, reload_core, reload_compute, cg_reload,
freecam, level, levels and sandbox (start, optionally on a level; stop; status). Register it
with `claude mcp add wumrt -- py -3 <this folder>/mcp_server.py`.

## Parallel streams

Several sessions can work at once, each in a stream of its own made by `stream.sh`:

```
sh stream.sh new NAME            # WormsXHD/streams/NAME: copy of ray-tracing, merge base, own sandbox, built
cd ../../streams/NAME/WormUltimateMayhem-Fixes/ray-tracing
sh go.sh                         # its own game; rt.py, build.sh, selftest.sh ... work as usual there
sh stream.sh sync NAME           # bring main's changes in (merge-file), rebuild, re-patch its CG
sh stream.sh merge NAME          # into main: takes merge.lock, merges, builds main, KEEPS the lock
(cd <main ray-tracing> && sh selftest.sh)  then  sh stream.sh unlock
sh stream.sh list | delete NAME
```

A stream is `streams/NAME/WormUltimateMayhem-Fixes/ray-tracing` (the top-level files of
ray-tracing; `out/`, `captures/` and `build/` start empty, `models/` is a junction to main's),
`streams/NAME/base` (main's files as of the last fork, sync or merge: the merge base) and
`streams/NAME/rt-sandbox` (own exe and DLLs, `CG/` patched from the game's own shaders,
480x270, `sandbox = 1`, `scale = 1`; `Data`, `EFMV`, `speech` and `wumrt_textures` are
junctions, the textures to the main sandbox's folder, so don't write to them from a stream;
`new NAME --private-textures` copies them instead). Because of that layout the relative paths
in the scripts (`../../rt-sandbox`) point at the stream's own sandbox. The control block is
`wumrt_ctl_NAME` for a stream's game and `wumrt_ctl` for the main sandbox: the loader derives
it from the exe's path, `rt.py` from its own folder.

**merge** goes file by file over the stream's top-level files: unchanged since the base,
skipped; main still equal to the base, copied; otherwise `git merge-file` (README.md with
`--union`); new files copied. A file that conflicts is not written; the list is printed and
`merge` exits with code 2 (run `sync`, resolve in your copy, merge again: the lock stays
yours meanwhile). Files deleted in the stream are listed, not deleted in main. Then
`build.sh` runs in main (the loader is only replaced if main's game is stopped). The merging
session runs main's `selftest.sh` while it holds `WormUltimateMayhem-Fixes/merge.lock`, then
`sh stream.sh unlock`. **sync** is the reverse (main into the stream), under the same lock
for its duration (it stops if another stream's merge holds it); like git, a conflicting file
gets `<<<<<<<` / `=======` / `>>>>>>>` markers in your copy and the base moves on to main's
version, so once you edit the markers out the next merge carries only your side; `merge`
refuses a file that still has markers. Then `cg-reload` / `reload-core` in the stream's
running game pick up what sync brought in.

**Games at once:** sandbox games of different folders can run side by side (tested: two at
once, each traced at its 30 fps cap while the user's game held the GPU), but the user's PC
stutters with more than one, so `WUMRT_MAX_GAMES` defaults to 1 and the one game slot is
enforced: `sandbox.py start` (and `go.sh`, `selftest.sh`) exits with code 3 and a `BUSY`
message unless this copy's stream holds it. `sh stream.sh slot` shows the owner, its expiry
and the queue; `slot take NAME [MIN]` (default 10 minutes, at most 15, take again to renew;
only when the slot is free, expired or given to NAME), `slot give [NEXT]` (to the first in the
queue by default; NEXT then has 5 minutes to take it), `slot free`, `slot queue NAME`. Main
counts as the stream that holds `merge.lock`, so a merging session takes the slot under its
own name for main's selftest. The state is `WormsXHD/.wumrt_slot`; it is never started beside
the real game (`WormsXHD/WormsMayhem.exe`).

## Review sets

`review.py` (with `review_gallery.html`) shoots repeatable before/after sets: four levels
(Apple Core Island, This Game Does Dragon, Grave Danger, Ice, Ice, Maybe) x three views (wide:
a stored free-camera pose per level in `review_poses.json`; mid and close: relative to the
match's start camera, which looks at the active worm) x presets, every preset of a level in
the same paused match so only the rendering differs:

```
python review.py kit baseline|current       # snapshot this copy's build: core, patched CG, compute shaders,
                                            # ini files, settings from the main sandbox's wumrt.ini
python review.py shoot MILESTONE [KITS]     # 1280x720, scale 0.5; into captures/review/MILESTONE/
python review.py label MILESTONE            # side-by-side sheets with captions
python review.py gallery MILESTONE "LABEL"  # compressed images + gallery.json for the gallery page
```

"stock" is the game's own CG with `rt = 0`, no shells, scatter or relief and no replacement
textures (the review sandbox's `wumrt_textures` points at an empty folder until stock is shot,
since a replaced texture cannot be put back in a running game; one game process per level). A
kit is swapped into the running, paused game with `reload-core`, `cg-reload` and `reload`.

`hide_hud = 1` in `wumrt.ini` (default 0, re-read live) keeps the HUD, the worm labels, the
pause box and the menus out of the picture: the engine draws them into its scene framebuffer
after the scene, starting with the first fixed-function draw after the sun-lit pass, and every
draw from there on is skipped except the copy to the window. Keep it at 0 while navigating
menus (`rt.py screen` and `level` read the pause box and menus from the screen).
`hide_hud = 2` also logs one frame's draws after the shadow pass.

## Measured cost

RTX 5090 with nothing else using it, paused match, about 100,000 static triangles. "4K-equivalent" means the mask was
traced at 3840x2160 while the sandbox window stayed at 960x540.

| | 960x540 | 4K-equivalent |
|---|---|---|
| Hard shadows (1 sun ray) | 0.3 ms | 3.1 ms |
| Soft shadows (8 samples, filtered) | 1.1 ms | 10.5 ms |

Sky occlusion and bounce add about 65% more traversal work (BVH nodes visited plus triangles
tested per pixel, with 4 sky rays) than soft sun shadows alone; their time has not been
measured on an idle GPU. With accumulation, 2 sky rays per pixel are as smooth as 8 without.

CPU cost on the game's thread is about 0.9 ms per frame during play (collecting casters and
refitting about 50 animated meshes). The merged BVH is rebuilt on a worker thread in 60 to
120 ms a few times a minute, when static objects appear, move or are destroyed. Half-resolution soft shadows halve the GPU time at 960x540; that
mode has not been timed at 4K.

With sky light on (2 rays per pixel, unlimited reach, several bounces) the work per pixel is
about 144 BVH steps, of which 54 are the sky rays; GPU time is not measured on an idle GPU.
On a paused view the filtered indirect light flickers by about 0.2% from frame to frame
with three filter passes, against 0.6% with one (`stability.py`).

With everything on, tracing at the full 1920x1080 took about 90 ms per frame while the GPU
was shared with another game, against about 10 ms with `scale = 0.5` (traced at 960x540 and
upsampled by the game's shaders along surfaces). Half resolution is the setting to use at
1080p and above until the indirect light gets a resolution of its own.

**Cheaper indirect light (`gi_rate`, `gi_far`; 2026-11-14).** The sky and bounce rays are most
of the tracer's GPU time. With `gi_rate = 2` (or 4) only every 2nd (4th) block of 8 x 4
pixels traces them each frame, in turn; the blocks are one GPU warp, so whole warps skip
together and the time really drops. A pixel that skips keeps what the temporal pass has
accumulated for it (the tracer writes sky = -1 and the temporal pass keeps its history
without blending); a pixel without history (just came into view, same reprojection test as
the temporal pass) always traces. `gi_far = D` halves the sky rays beyond D world units.
Both default to off (`gi_rate = 1`, `gi_far = 0`). Measured on a paused Apple Core view at
480x270 with everything on, the GPU shared with three other sandbox games and an SDXL run
(timings noisy; tracer + temporal + filters, `rt.py rtstat 3`; noise and flicker of the
indirect light from `stability.py`):

| setting | trace (GPU) | spatial noise | flicker |
|---|---|---|---|
| `gi_rate = 1` (as before) | 24.2 ms | 1.71% | 0.59% |
| `gi_rate = 2` | 19.1 ms | 1.72% | 0.47% |
| `gi_rate = 4` | 17.6 ms | 1.75% | 0.48% |
| `gi_rate = 1`, `gi_far = 300` | 24.2 ms | 1.95% | 0.68% |
| `gi_rate = 2`, `gi_far = 300` | 18.3 ms | 1.91% | 0.55% |

So `gi_rate = 2` saves about a fifth of the tracer's time at the same noise; screenshots of
the paused view cannot be told apart (`out/perf_*.png`). `gi_far` saves little here and adds
noise. The cost of skipping is lag: indirect light that changes (an explosion's glow, a
destroyed wall) catches up over 2 or 4 frames more; an orbiting free camera showed no visible
lag (`motionprobe.sh`, `out/motion_rate*.png`). Not timed at
1080p (`scale = 0.5`). This is not yet a separate lower-resolution target with a depth/normal
aware upsample; that would mean splitting the tracer into two dispatches.

Screen-sized textures: 13 RGBA16F per scene camera before, 12 now (this frame's sky light
shares a texture with the indirect-light filter's second temporary, which is first written
after the temporal pass has read it). At 960x540 that is 4 MB per texture. Packing more
channels into smaller formats (R11G11B10F for light, RG16F) needs the shaders' image formats
changed together and is not done.

**What each feature costs (`costprobe.sh`, 2026-11-14 16:23).** One paused free-camera view on
Apple Core, 480x270, every setting as in main's ini, nothing else on the GPU. Each feature is
turned off on its own between two baselines (the baseline drifts about 1 ms over the run, so
under 0.5 ms is noise). Baseline: tracer 6.5-8.4 ms, whole frame 10.1-12.2 ms.

| turned off | tracer | whole frame |
|---|---|---|
| `scatter` | -0.5 to -1.3 ms | -1.4 ms |
| `specular` | -0.6 ms | -0.3 ms |
| `reflections` | -0.4 ms | -0.5 ms |
| `round`, `shell_shadow`, `skin_translucency`, `skin_wrap`, `rim_light`, `catchlights`, `relief`, `post`, `hdr`, `dof`, `bloom`, `shells` | each within noise | each within noise |
| (`gi_rate = 2` turned on) | -1.6 ms | -1.3 ms |

At this size only the tracer's rays cost much. With a worm filling about half of the screen
(16:49, same method) `round` costs about 1 ms of the tracer (1.9 ms of the frame); the skin
features, `rim_light`, `catchlights`, `contact_shadow`, `specular` and `reflections` stay within
noise there too. Not measured at 1080p.

## Shell layers (grass and moss)

The terrain is drawn per material. For a terrain draw whose colour texture is listed in
`wumrt_surfaces.ini` (keyed by the same pixel hash as the materials file; `surfaces.py
candidates` makes contact sheets of the green textures, `surfaces.py terrain` of the ones the
last match drew terrain with, since the harness logs every terrain texture's hash once), the
core draws the same mesh `shells` more times just before the game's own draw, outermost
first, so pixels under blades are rejected by the depth test instead of being shaded. Each
copy sets `rtShell` on the terrain programs: the vertex program pushes the vertices out along
their normals by the layer's height; the fragment program keeps a pixel only if it lies inside
a blade at that height. A blade is a cone in a cell of a grid over the texture coordinates
(random place, height and lean, one colour: the texture's at its foot, dark at the root and
light at the tip). Two grids overlap, the second turned by 37 degrees and 1.31 times finer, so
blades never line up in rows. The grid is sized in world units, along u and along v
separately, from how far the surface moves per unit of each texture coordinate (worked out
from how texture coordinate and position change across the screen, and rounded to steps of
the square root of two so it holds still): blades stay round where the texture is stretched,
as on steep faces. Each layer's height is moved by up to half a layer by a fixed fine noise
over the surface, so at grazing angles the layers show as grain instead of bands. With
`shell_shadow = 1` (off by default) the blades shade each other and the ground from the sun:
from each pixel in the lower 60% of the blades (and the ground) the shader steps towards the
sun up to a layer 35% higher and looks for a blade there (one more blade test per pixel).
Bigger blades are now the default (`shell_density 0.3`, `shell_height 3.5`; before/after
`captures/grass_bigger_shell_density.png`). Blade shadows: `captures/grass_shadow_grazing_shell_shadow.png`
(darker blade sides and roots; the ledge pair caught a turn change, so its right half is dimmed
by the game's overlay). Their GPU cost was not separable from the shared GPU's noise. Where a cell would get smaller than about three pixels the grid gets coarser instead,
in further steps of the square root of two, so the blades cover the whole map, bigger in the
distance (they used to fade out there; before/after `captures/shells_far_compare.png`, crop
`captures/shells_far_crop.png`). The ground under the blades is darkened. A blade pixel reads the traced light of the ground behind it
(the mask's distance check allows for the layer's height along the line of sight).

Cost, 1080p, paused view of Apple Core Island with 50 grass draws (800 extra draws for 16
layers): 0.4 to 0.7 ms of CPU per frame; GPU time is within the noise of the shared GPU
(fastest frame 22 to 27 ms without shells, 26 to 35 ms with). A level full of grass adds
23,000 to 45,000 indices per layer.

Wind (`grass_wind`, off by default): every blade bends along a fixed direction of the texture,
more towards its tip (the square of the height), by up to 0.7 cells at strength 1. How much
comes from a gust field over the surface: waves about 180 world units apart rolling across at
about 30 units a second, slowly swelling and fading, plus a flutter of each blade's own. All
sines from a time uniform (`rtWind`), no loops. The border tufts sway with the same gusts in
the terrain vertex program: their tip vertices carry colour alpha 0 (roots 1), and the scatter
draw sets the vertex `rtShell` to (0, time, wind, 1), which also puts the alpha back to 1.
Painted colour (`grass_patches`, off by default): big soft patches, 40 to 80 units across, of
drier yellower grass and lusher bluer green, and a second set a little darker or lighter. A
blade takes the colour at its foot, so it stays one colour. The tufts do not get the patches.

Known flaws: where grass meets another material it still ends in a step (the scattered border
tufts, see below, lean out over it); the blade shadows are a shader test against the other
blades, not traced (the tracer does not see the blades; they get the ground's traced light
and shadow); edges are aliased like the rest of the game (no anti-aliasing).

## Cloud shadows

`cloud_shadows` (strength, off by default), `cloud_scale`, `cloud_speed`: in the trace shader
(`cloudLight`, marked "moss: cloud shadows") each traced point is carried along the sun
direction down to the height-0 plane, and a smooth two-octave value noise there, thresholded
softly, says how much sun a cloud takes. It multiplies the sun visibility written to the mask,
so every lit draw (terrain, props, worms) gets it through `RtLight`, and the sunlight that
sky rays find at their hits, so bounce light follows. It drifts with the time. No loops. The
sea is not in the traced scene and stays unshadowed. Captures `captures/clouds_beach_cloud_shadows.png`,
`clouds_wide_cloud_shadows.png`, `clouds_motion_cloud_shadows.png` (paused game, menu in view).

## Scattered stones and tufts

`scatter.c` makes small objects from each terrain draw's triangles (read from the CPU mirror
of the vertex buffers), by what its texture is in `wumrt_surfaces.ini`:

- `rock`: pebbles on its flat parts, and rubble along the foot of its walls: the level bottom
  edge of a steep triangle with flat ground that is not rock (sand, grass, earth) just in front
  of it and within about 1.5 units of its height. Shelves of the rock itself do not count,
  otherwise the stones stick to the sculpted walls.
- `sand`: pebbles, tinted darker than the sand they lie on.
- `grass`, `moss`: tufts of 5 to 8 three-sided blades (3.5 to 6 units tall, about as tall as
  the shells), leaning well out over edges that no other grass triangle shares (where grass
  meets rock, sand or a cliff's lip), so the shells' hard step at the border is overhung.
  Seen in `captures/tufts_ledge_scatter.png`: the tufts show as long blades poking out at the
  border; together with the rubble they break up the step a little, but the shells' own
  border still reads as a hard edge.

A stone is a squashed, jittered icosahedron with round normals, half sunk; its texture
coordinates continue the ground's (shifted per stone), so it is made of the material it lies
on. An edge map over all terrain draws (world space, corners matched after rounding to 1/16
unit, plus points sampled every ~1.5 units on flat ground in 3-unit cells) tells which edges
are shared and where the flat ground is; a draw is only filled once the map has held still
for three frames, and filled again when its vertex data changes (destruction) or a neighbour
appears. Each set is drawn right after its terrain draw with the same programs (one
`glDrawElements` from a buffer of its own, client array state pushed and popped; on grass the
shell uniform is cleared for it), so it gets the game's lighting, normal map and traced light.
Stones (not tufts) go into the traced scene as cached static casters keyed by the set, so they
cast and receive traced shadows and end up in the merged BVH; tufts stay out because they are
thin, numerous and memory in this 32-bit game is short (adding them made the static pool fail
to grow).

Apple Core Island (labels for its 3 rock and 2 sand textures are in the file): about 3,000
pebbles on the sand, 250 on rock, 1,060 rubble stones and 5,000 tufts; 52,000 vertices for
the stones. Making them takes about 10 ms once, spread over the first frames (at most 24
draws per frame). Per frame: one extra draw per terrain draw that has objects; collect time
unchanged (0.9 ms). GPU: once the stones are merged into the static BVH, the difference is
inside the noise of the shared GPU (21 to 22 ms traced at 960x540 with them; a run without them
measured 29 ms only because less of the scene had been merged). Until they are merged (for
two minutes after they were switched off and on again: anything that leaves the merged BVH is
kept out of it that long), every stone set is traced on its own across the whole island and
costs about 10 ms.

Judged at 1080p: the pebbles read as chunky stones on the sand and suit the game's look; the
rubble at a wall's foot on grass ledges looks right; the tufts barely show next to the shell
grass (which already gives the grass border a ragged edge). Not right yet: the scatter is
even (no clusters), no rubble where a beach's cliffs meet the sand on Apple Core (their bottom
edges are not level enough or the sand in front is not flat enough), sand pebbles on other
levels need their sand labelled, and the stones' colour comes from the ground's texture
times a tint (a sand pebble is darkened sand).

## Characters

The worms are the stars of every shot, so they get lighting of their own, in the style of an
animated film rather than realism. Which draws are worms is decided by texture:
`wumrt_characters.ini` lists the hashes of their skin textures (`skin`), the skin texture that
also holds the eye whites (`face`) and eye textures of their own (`eye`). Hats, glasses and
other accessories are not listed and are lit as before. Every effect has its own setting and is
off by default.

- `skin_wrap`: the sun's light reaches past the line where the skin turns away from the sun,
  tinted warm (red-orange), as light that went under the skin and came out again; and skin in
  shade keeps the brightness of the light that falls on it but takes a warm hue instead of the
  colour of its surroundings (without it, a worm on grass in shade turns grey-green).
- `skin_translucency`: for skin turned away from the sun the tracer follows the sun's direction
  from just under the surface through the body to where it comes out (one closest-hit ray), and
  checks with one more ray whether the sun reaches that far side. The length crossed gives the
  share of light that gets through (`exp(-length / skin_depth)`), stored as a negative alpha in
  the reflection texture (the sea uses alpha 1 there); the Cg helper adds it as warm light,
  strongest when the viewer looks towards the sun. The far side's sun visibility also replaces
  the sun visibility of these pixels, because the light wrap needs to know about other things'
  shadows there (smooth normals otherwise leave them lit). Only worm pixels pay for the two
  rays. `skin_translucency = -1` shows what the tracer left on worm pixels instead of the
  picture: red = light through, green = sky visibility, blue = sun visibility.
- `rim_light`: a soft rim along the worm's outline (`(1 - n.v)^3`), as strong as the point sees
  the sky, so it does not glow in caves, a little stronger on upward-facing parts; it has the
  sky's (ambient) colour, turning to the sun's warm colour as the sun gets behind the worm.
- `catchlights`: the worms' pupil texture (listed as `eye`) has a small glint painted in; its
  near-white texels are added again as light (times `catchlights`, a little stronger where the
  sky is open), so the glint reads as a real reflection instead of being shaded with the eye.
  A computed highlight on the eye whites was tried first and dropped: it landed as an odd mark
  at the top of the eye. Judged a modest gain (`captures/char_eye4_zoom.png`).
- `contact_shadow`: every worm draw becomes a capsule along its bounding box's longest side
  (radius half the mean of the other two sides; the harness collects them each frame and hands
  them to the tracer, at most 16). For every surface point that is not on a worm, the share of
  the sky each capsule hides (cosine weighted, as a sphere of its radius at its nearest point
  would), fading out within `contact_reach` of its surface, darkens all of the point's sky
  light, twice over, and (because sky light is only a small share of the light on open ground)
  also takes up to 70% of the sun there: stylised and free of noise, so worms sit on the
  ground; their true shadows are traced as before. Points inside a capsule belong to that worm
  (goggles, hats) and are left alone. `contact_shadow` is the strength (1 is barely visible on
  grass, 2 judged good: `captures/char_cs6_zoom.png`, `char_cs7_pair.png`); `contact_shadow =
  -1` paints the occlusion as red light instead. No measurable cost (16 capsules, trace time
  unchanged at 6.7 ms at 480x270). (A first version counted sky rays that hit a worm; from the
  ground those never registered as worm hits, cause not found, so it was replaced.)

The harness gives the object shaders `rtChar` = (kind: 1 skin, 2 face, 3 eye; light wrap; rim;
light through) and `rtChar2` = (catchlight strength, the world's up direction in eye space) per
draw, and the tracer `uChar`; `mats[].w` in the tracer's material table is the kind.
`char_ab.py NAME [X0 Y0 X1 Y1] [--only KEY]` shoots the paused view with the settings off and on
(captures/char_NAME_*.png). Finding a worm for close-ups: the harness logs the centres of the
worms' face draws every 3 s (`characters: N faces at (x y z) ...` in `wumrt.log`), and
`lookat.py X Y Z ANGLE DIST [HEIGHT [SHIFT]]` puts the free camera around such a point (`sweep`
shoots six angles into one sheet; `lookat.py faces` lists the logged worms and `lookat.py face N
ANGLE DIST ...` aims at worm N). Worms are about 30 world units tall. The pause menu box
covers the middle of the frame, so SHIFT turns the view to put the worm beside it.

Foliage uses the same path. Textures labelled `foliage` in `wumrt_characters.ini` (leaf, plant,
grass and flower cut-outs, the tree canopies' leaves) are kind 4. Where such a surface faces
away from the sun, the tracer follows the sun's direction through it: a single card with
nothing behind it lets everything through, a leafy mass `exp(-length / foliage_depth)` of it.
The result goes into the same channel as the worms' light through skin. Unlike skin, the sun
visibility stays as the game's shading expects for faces turned away from the sun (the far
side's visibility made canopies darker). The Cg helper adds it times `foliage_glow`, tinted warm
yellow-green and stronger looking into the sun, plus a soft wrap (`foliage_wrap`). Status: works
on the game's own leaf and grass cut-outs (Apple Core Island, grass strips along the edges:
`captures/char_leaf1_diff.png`), but on the levels tried most of the visible foliage is drawn
by the harness (moss shells over the canopies and grass, scatter tufts) and does not get it, so
it is off in the sandbox. The tracer already leaves the light through at canopy pixels when
`foliage_glow` is on (`-refl.w`); the shells could add it.

Backlit blades (`blade_glow`, `blade_tip`): the shells (grass, moss, the canopies' leaves) do
get a glow of their own. A block in the terrain fragment program, marked "characters stream",
adds sunlight through the blades when the sun is behind them as seen from the camera
(`dot(-view, sun)^2`). It is tinted warm yellow-green, more towards the thin tips (`rtShell.y`),
times the traced sun visibility and half-weighted by the blades' shade on each other. A bright
rim on the top third of the blades comes from `blade_tip`. The harness passes `rtBlade = (glow,
tip, 0, 0)` with each shell draw. Judged good at 1 / 0.5 on Tree Village Trouble (low sun): a
backlit canopy turns from a dark mass into glowing leaves (`captures/char_blade1_pair.png`),
nothing changes with the sun behind the camera (`char_blade2_pair.png`). It costs no tracing,
only a few shader operations per shell pixel. Scatter tufts don't get it yet.

Before/after (480x270 crops, Apple Core Island, paused): `captures/char_t4_pair.png` (front,
sun on the face: warmer, fleshier skin and a soft bright outline), `captures/char_t5_pair.png`
(from behind against the sun: the shaded back is warm instead of grey-brown, warm edge).
Cost: two rays per worm pixel turned away from the sun; not measured (the GPU was shared).
Known limits: accessories are not covered; light through the body is often blocked by the
worm's own hat or head (correctly, but it then shows little); the 480x270 judgement needs a
1080p check.
## Parallax relief

Surfaces with a normal map can look carved rather than painted: `relief.py heights` turns
every installed normal map's slopes into a height (an FFT finds the height whose slopes are
closest; per texture it picks the green-channel convention that integrates best and the sign
that goes with the colour texture's brightness, since cavities are darker) and stores it in
the map's alpha, 1 at the top. Grass, moss and sand stay flat. Depths come out in texture
repeats (median 0.014, nine in ten under 0.034; `relief.py` clips at 0.04, which alpha 0
stands for). `relief.py model` does better where the model's maps exist (`ai_maps.py`, kind
`h`, PBRify's height model): the model's height gives the shape, the integrated normals give
how deep it is (their 95th percentile), and the sign is checked against the integration. The
sandbox textures were done that way (420 maps; 31 left flat; 4 model heights turned over).

In the object and terrain shaders `RtRelief` walks the view ray into the relief in texture
space (16 steps, then one secant step), using the true change of position per unit of u and
v worked out from screen derivatives (the meshes have no tangents); the colour texture and
the normal map are read at the point found. From there 8 steps towards the light measure how
far relief rises above the ray, which darkens the sunlight (self-shadowing). Both loops have
fixed bounds; at grazing angles the shift is limited to four times the depth.

Settings: `relief` (depth scale, 0 = off, the default) and `relief_shadow` (strength of the
self-shadowing, 6). Seen at 1080p on Apple Core Island (`captures/relief_wall_relief.png`,
`relief_ledge_relief.png`, `relief_beach_relief.png`): the rock walls get darker cracks and
read more carved; the effect is subtle, and `relief = 2` looks almost the same as 1
(`relief_wall2_relief.png`): most of what shows is the self-shadowing, not the shift. GPU cost
not measured apart from the shared GPU's noise. Compressed textures keep the height (normal maps
stay RGBA8 with `tex_compress = 1`; DXT5 with `tex_compress = 2` or packed `.wtc`), but DXT5
alpha has 8 levels per 4x4 block, so heights can look slightly stepped: keep `tex_compress` at
1 if that shows. A packed `_n.wtc` older than its `_n.wtx` is ignored, so new heights from
`relief.py` are used until the textures are packed again.

## Smooth normals

The game shades its meshes with vertex normals, so a worm looks round, but the tracer used to
see flat triangles: the sun's shadow edge and the sky light broke into steps along them. Now
every triangle in the traced scene has the normals of its three corners next to it (a buffer
parallel to the triangle buffer, 16 bytes per triangle, octahedral 16 + 16 bits per normal),
kept in step with the triangles through every BVH build, refit and merge (the merged BVH
transforms them by the inverse of each mesh's model matrix).

Where they come from: the game's own normal array if the caster draw has one and at least 75%
of its normals agree with their triangles (they do not in the shadow pass for skinned meshes,
whose normals are zero there); otherwise they are computed from the mesh: indexed meshes
average the face normals around each vertex index, others weld corners by position and keep
edges sharper than 60 degrees hard. Computing them costs about 0.09 ms per frame for the
animated meshes of a match (the position weld cost 1.9 ms, hence the index path).

At a hit the tracer interpolates the normal, and:
- moves the ray's start from the flat triangle onto the curved surface (each corner's tangent
  plane pushes the point outwards, weighted by the barycentric coordinates, at most half the
  triangle's size), so neighbouring triangles of a convex shape no longer block it;
- leaves a point whose smooth normal faces away from the sun lit, since the game's own shading
  already takes the sunlight away there and a traced shadow would only add a jagged edge;
- on a triangle that faces away from the sun while its smooth normal faces it (the lit edge of
  a round shape), ignores hits closer than the triangle's size;
- sends sky rays around the smooth normal, mirrored above the flat triangle where they would
  go under it, and hands the smooth normal to the filters and the glossy reflection rays.

Result: on the worms, the stepped shadow bands and dark creases along triangles are gone;
silhouettes are still polygonal (that is the mesh), and the overall brightness is unchanged.
On flat-sided objects there is nothing to see. The CPU reference tracer has no smooth normals,
so `selftest.sh` compares with `smooth = 0`; with `smooth = 1` about 30% of mask pixels
differ from it, almost all of them points facing away from the sun that are now left lit.

## Film post-processing

`post.c` and `wumrt_post.comp` run a chain of compute passes over the game's scene picture
before the HUD is drawn on it: a filmic tone curve with a colour grade, bloom, depth of field
and anti-aliasing. Every piece has its own setting and all of them are off unless `post = 1`.

Where in the frame: the engine draws the main view into a framebuffer of its own (the
picture-in-picture view goes to another one first), then the worm silhouettes, particles and
the worms' name labels with the same camera, then the 2D HUD and the HUD's 3D models with a
camera of their own, and finally copies the picture to the window. The harness follows the
framebuffer binds and classifies every draw: depth of field runs at the first unlit draw after
the lit scene (so labels and effects stay sharp), everything else at the first draw with a
camera that is not the scene's (the HUD), or when the engine leaves the scene's framebuffer.
Only the framebuffer that held the last scene of the previous frame (the main view) is
processed.

HDR (`hdr = 1`): the game's lit shaders clamped their colour to 1; `patch_shaders.py` now
lets it go higher (`RtHdr`, which changes nothing in an 8-bit target because the hardware
clamps the same way), and the harness gives the scene texture 16-bit float storage between
two frames. Sunlit surfaces, highlights and stacked additive effects then keep values above
1, which the tone curve rolls off instead of clipping and the bloom picks up.

```
post = 1            # master switch for everything below (0 = the game's picture)
hdr = 1             # float scene texture, lit colours above 1 are kept
exposure = 1.0      # multiplies the linear colour before the curve
tone_contrast = 1.0 # contrast around middle grey (log space)
tone_knee = 0.6     # linear value where the soft shoulder starts (1 never reached)
saturation = 1.0    # saturation, half as strong on colours that are already strong
grade_warm = 0      # warm light, cool shade (0..1)
vignette = 0        # darkened corners (0..1)
bloom = 0           # bloom strength (0 = off)
bloom_threshold = 1.0   # linear brightness where bloom starts, after exposure
bloom_knee = 0.5    # softness of that threshold
bloom_levels = 5    # blur levels, each half the size of the last (1..7): more = wider
dof = 0             # depth of field: largest blur radius as a share of the picture height (0 = off; needs rt = 1)
dof_range = 1.0     # how fast blur grows with distance behind the focus
dof_near = 0.15     # share of that blur in front of the focus
dof_focus = 0       # fixed focus distance in world units (0 = the worm nearest the middle of the view, else the nearest thing there)
dof_speed = 0.15    # how fast the focus follows what is at the centre of the view (per frame)
aa = 0              # FXAA on the scene (not on the HUD)
glare = 0           # sun glare: a bright core and a wide halo around the sun when it is on screen and open sky (needs rt = 1)
glare_size = 1      # size of that glare
auto_exposure = 0   # 0..1: how far the exposure follows the scene's brightness towards auto_key (at most -20% / +25%)
auto_key = 0.35     # the mean linear brightness auto exposure aims at (this game's daylight scenes measure 0.28 to 0.53)
grade_auto = 0      # 1 = the grade follows the level's mood: a dim or bluish sun (night) gets a cool moonlit tint instead of the warm one, and less saturation
dither = 1          # break up banding with +-1/255 of noise
post_timer = 0      # 1 = log the GPU time of the tone, bloom and AA passes
```

Values chosen (set in the sandbox ini): `post = 1`, `hdr = 1`, `exposure = 1.0`, `tone_contrast = 1.2`,
`tone_knee = 0.85`, `saturation = 1.2`, `grade_warm = 0.25`, `vignette = 0.2`, `bloom = 0.15`, `bloom_threshold = 1.2`,
`bloom_knee = 0.3`, `dof = 0.005`, `aa = 1`, `glare = 0.4`, `auto_exposure = 0.4`, `auto_key = 0.35`,
`grade_auto = 1`. Earlier choices looked hazy: bloom 0.35 from 0.85 bloomed the sky itself to pale cream, and
with the shoulder starting at linear 0.6 the bright sky and sea were compressed and lost colour (whites capped
near 0.88, a milky, low-contrast look). With the shoulder at 0.85, bloom only above 1.2 (sunlit highlights,
stacked effects) and less warmth, whites reach white again and the sky keeps its blue: on Apple Core at 1080p
the 99th percentile luminance went 0.88 -> 0.93, contrast (standard deviation) 0.233 -> 0.255, sky saturation
0.138 -> 0.153, blacks unchanged (`film_m1fix2_compare.png`, labelled).

Cost (GPU timer queries, the GPU shared with another game, so upper bounds): at 1920x1080 the
tone curve, grade, five bloom levels and FXAA take about 0.08 ms per frame and the depth of
field (40 taps) about 0.24 ms; at 480x270 everything together takes 0.01 to 0.03 ms. CPU cost
is a few GL calls per frame.

Before/after (1920x1080, tracing at half size; in the film-post stream's `captures/`): worm in focus, background
soft: `captures/film_cp3_off.png` / `film_cp3_filmC.png` / `film_cp3_mood.png`; earlier `captures/film_cp2_off.png`,
`film_cp2_filmA.png` (moderate), `film_cp2_filmB.png` (strong: more contrast, saturation,
warmth, bloom and blur); the sun glare `film_cp2_sun_compare.png`; paused, snow level
`film_cp1_compare.png`.

How the pieces look: the grade warms the light and slightly cools the shade and lifts
saturation without pushing the already strong greens; with `hdr = 1` sunlit highlights roll
off instead of clipping flat; the depth of field keeps what is at the centre of the view (the
worm the camera follows) sharp and softens the far background and sky; the glare is a soft
halo where the sun is in open sky. When the sun is just off the screen its visibility is read at the
nearest screen edge, so its halo still reaches in over open sky (`film_edge_compare.png`: sun in view,
just above the top edge, far above; glare 0 / 0.6) but not over a leaf or cliff in front of it.

Focus: the worm nearest the middle of the view (worms are recognised by their textures listed in
`wumrt_characters.ini`, their centre from the draw's bounding box); without one, the nearest thing in
the middle 12% x 16% of the view. The focus follows changes over a few frames.

Known limits: the worm nearest the middle is not always the one being played; the depth of field uses the traced
distance, so things that cast no shadow (the sea, particles, labels) take the distance of
what is behind them (the labels and particles are drawn after it and stay sharp); the sea takes
the distance of the sea floor or the sky behind it; the picture-in-picture view is not
processed; FXAA softens fine texture a little. The night grade (`grade_auto`) was seen on a night Apple Core match (sun colour 0.38 0.31 0.75 -> night 1.00,
scene mean luminance 0.055, auto exposure x1.25): the night stays purple, slightly cooler and lifted, not crushed
(`film_exp3_compare.png`); Grave Danger's dusk has a greenish sun and counts as day. Auto exposure lifts dark
levels by up to 25%. Explosions (`film_exp2_off/on.png`, a fire jet frozen at 1080p with `fx = 1`): with
`bloom_threshold = 1.0` and `bloom = 0.25` the jet gets a soft halo at its hot core and keeps its shape, no haze
over the scene (judged with the earlier bloom 0.25 from 1.0; the chosen 0.15 from 1.2 is gentler); particles keep animating while paused, so the two shots are about 2 s apart. Not tested in play mode or at real 1080p
without `scale = 0.5`.
### Per-level colour script (`grade_levels`, `wumrt_grades.ini`)

Each level can have its own hand-tuned grade on top of the global one (like a film's colour
script): `grade_levels = 1` (default 0; needs `post = 1`) reads `wumrt_grades.ini` (copied to the
sandbox by `build.sh`; `rt.py reload` reads it again). A level is a block:

```
[Apple Core Island]          # golden afternoon
textures 1234abcd 5678ef01   # terrain textures that mark the level
gain 1.06 1.0 0.9            # multiplies everything (the light's colour), per channel
lift 0.002 0.001 0.0         # added in the shadows (linear, small)
gamma 1.02 1.0 0.98          # midtones (above 1 brighter)
saturation 1.05              # on top of the global saturation
night 0                      # 1: keep it on night matches too (default: fades out with grade_auto's night)
```

How it works (`grade.c`, included from `post.c`): wumrt.c reports every landscape draw's
texture hash (`rt_terrain_hash` in rt_core.c); the level whose listed textures were drawn most in
the last 2 s wins, and its grade blends in over ~2 s (`grade_speed`, share per frame, default
0.04). Unknown terrain blends back to no level grade; menus (no terrain) keep the last one. New
terrain hashes go to the log as `grade: terrain HASH`, the chosen level as `grade: level NAME`, so
adding a level is: `rt.py level NAME`, copy its `grade: terrain` lines into a new block, tune,
`rt.py reload`. In the shader the level grade runs after the warm/cool split and before the
vignette and shoulder: `c *= gain; c += lift * shadows; c = pow(c, 1/gamma); saturation`. Cost:
a few ALU per pixel; the hash lookup is a few dozen compares per terrain draw.

## Rounded shapes

`round.c` replaces every sun-lit mesh draw that is not terrain, not alpha-tested and not
skinned on the GPU by a finer mesh on a curved surface. Each triangle becomes a cubic Bezier
patch through its three corners that follows their normals (PN triangles, Vlachos et al.
2001) and is cut into `round_level` x `round_level` triangles (9 per triangle at level 3).
Edges decide the shape, and are computed so that neighbours agree bit for bit (no cracks):

- a smooth edge (both triangles have the same normals at its ends) bends along those normals;
- a crease (the normals differ: the rim of a barrel, the edge of a crate) follows the line
  both tangent planes share (cross product of the two normals), so a round rim becomes a
  circle and a straight edge stays straight;
- an edge with one triangle, or more than two, stays straight.

Corners are welded by position (to a grid of 1/1000 of the mesh's size) to find the
neighbour across each edge, since texture seams and hard edges split the game's vertices. Meshes whose control points all lie on their flat
triangles (crates, walls) are drawn as they are; meshes that bend little per triangle (already
dense) get 2 steps per edge (`round_dense`). New vertices interpolate the corners' normals,
texture coordinates and colours. Static meshes are rounded once and kept in GL buffers of our
own; the worms are skinned on the CPU every frame into one shared scratch array (every worm's
parts come by with the same pointers and other contents), so their results are kept per
content (a hash of the positions) and remade when the worm moves.

The tracer: static props are handed to it rounded (their shadow-pass draw is replaced like the
main-pass one, with a key that never changes, so they are cached and merged as before). The
worms are not by default (`round_casters = 1`), and that shows up close: the traced worm keeps
its stock outline, so the thin band where the rounded outline sticks out past it has nothing
of the worm in the mask (the camera ray there hits whatever is behind), the lit shaders find no
match for the pixel and leave it fully lit: a bright one-pixel fringe along part of the
outline, seen at 1080p on a close worm (`captures/shape_hd_worm_round_seam_crop.png`).
`round_casters = 2` hands the tracer the rounded worms too. The shadow pass comes before the
main pass and its copy of the worm's normals is zero, so it first looks for a result the main
pass made from the same positions (a paused or still worm) and otherwise rounds the worm
itself, with normals worked out from the positions (area-weighted face normals summed per
welded corner, turned away from the part's middle) and the same size test as the main pass,
measured from the camera the main pass had one frame earlier. Offline, on a 1,012-triangle
sphere, those normals are within 1 - cos = 0.0003 of the true ones and the rounded surface
within 0.0004 of the radius of the main pass's; they take about 19 us per part. Hard edges on a
worm get rounded over in the tracer's copy (it cannot see them), a small shape difference well
inside the shaders' matching tolerance (3% of the eye distance).

Checked in the game: on a paused close worm at 1080p (scale 0.5) the bright band along the
outline is there with `round_casters = 1` and gone with 2
(`captures/shape_hd_worm_round_casters.png`, 2x crop `shape_hd_worm_round_casters_edge_crop.png`;
the contact shadow where the body folds over itself also follows the rounded shape,
`shape_hd_worm_round_casters_crop.png`). In play at 480x270 with the camera 22 units from a
worm (`shape_play.sh`): 12 to 13 worm parts per frame rounded in the shadow pass; rounding CPU
2.0 ms per frame against 1.1 ms with 1, the tracer's collect step 2.1 ms against 0.9 ms, so
about 2 ms of CPU per frame more, still at the 30 fps cap; GPU time not measurable (shared
GPU). Default in code stays 1.

Offline check of the patch code (a test program, not part of the build): an icosahedron's
face centres move from radius 0.79 to 0.94 (sphere = 1); an 8-sided cylinder's rim goes from
0.924 (octagon) to 0.993..1.0; a cube with split normals is left alone; no cracks with shared
or split vertices.

Before the distance test, in the game (a Quick Game level with a tower in the sea; name not checked), 480x270: 128 draws rounded, 23,000 -> 211,000
triangles. CPU on the game's thread: 3 to 4 ms per frame during play (about 75 worm parts
remade per frame), 0.1 ms on a paused view; making everything once after a reload, 44 ms.
GPU cost not measurable here (the GPU was shared with three other sandboxes and a game; frame
times moved by more than the effect). Before/after: `captures/shape_worm_close_round.png`,
3x crop `captures/shape_worm_close_crop.png`: the helmet's dome and brim lose their corners;
the body, already dense, changes less. At 480x270 from the game's own camera distance the
difference is one or two pixels. Paused close-up on the swamp level:
`captures/shape_worm2_paused_round.png`, 3x crop `shape_worm2_paused_round_crop.png` - the
helmet's faceted dome becomes one smooth arc and the worm's back a smooth curve. Oil drum
(`shape_drum_paused_round_crop.png`): rounder by a pixel at that size.

Distance: a draw whose bounding sphere is under `round_min_pixels` in radius on screen is
drawn as it is, and worm parts under `round_full_pixels` get 2 steps per edge (one index
pattern is kept per level, since one worm mesh is drawn near and far in the same frame). The
bounding box is taken from the draw's vertices each time (the scratch array holds a different
worm at every draw). Measured offline, one rounding of a 1,012-triangle part takes 0.2 to 0.3
ms (control points about 0.08 us per triangle, then about 11 ns per new vertex); compiler
flags make no difference. In the game (swamp level with jetty and oil drums, 480x270,
defaults): 45 to 48 draws rounded, about 165 left alone as too small, 0.4 to 0.5 ms of CPU per
frame in play and 0.06 ms paused (it was 3 to 4 ms in play before the distance test, on
another level). At 1080p more draws pass the size test, so expect more.

Bevels (`round_bevel = 1`): a prop with hard edges (crates, barrel rims, posts; not the
worms, which change every frame, and not meshes over `round_bevel_max` = 40 world units in
radius) is cut 5 x 5 with the rows pulled towards its edges (at 6%, 31%, 69% and 94% of each
triangle), and the vertices on a hard edge get the mean of the normals on both sides of it;
corners where hard edges meet get the mean of all the different normals there. The outline
does not change, but the edge catches the light like a bevelled edge. Offline check: a cube
gets 132 bevel normals along its edges and keeps 120 face normals, no cracks. In the game it
reaches fewer props than hoped: on the swamp level 3 meshes got bevels; the wooden crate did
not, because its faces do not share edges (each is a separate piece, open on all sides), and
large level pieces are excluded by size. Its look has not been judged in the game, so it stays
off.

Not done: GPU-skinned meshes (programs named `*Skinned`) are left alone (none seen in use).

## Light from effects

Explosions, fire and other glowing effects light their surroundings with traced, shadowed,
coloured light that fades out (`fx = 1`; off by default in code). The game draws its effects
after the scene with `FFVertexMainTexCol`, from plain memory or a vertex buffer, with a colour
per corner that fades them out. Explosions are triangle lists with ordinary alpha blending
(orange fire textures, a yellow flash texture) plus one additive shock-ring strip; smoke is
white or grey. `fx.c` turns every such draw of the main view into sources:

- additive draws count as they are;
- alpha-blended draws count when their colour (vertex colour x the texture's average colour)
  is fire-coloured: red-orange to yellow, not grey or blue (`fx_alpha` is that share).

A source's power is its area x vertex colour x alpha x the texture's average colour, at its
centre. Draws wider than 150 units (the shock ring) are skipped. Overlapping sources are merged
into one light, a ball up to 30 units across, and lights are followed from frame to frame:

- a light that appears flashes (`fx_flash`: extra brightness that dies away in about 0.1 s);
- when its sprites have gone it fades instead of stopping (`fx_decay`, seconds);
- an explosion (a light younger than 4 s) leaves a dim red glow on the spot (`fx_ember` share
  of its peak, fading over `fx_ember_time` seconds), the heat of the scorched ground;
- a light that burns longer than 0.6 s flickers (`fx_flicker`: two fast waves and noise);
- above `fx_knee` the power only grows with its log. A big blast sums 20000-40000 over its
  sprites (a single fireball is about 1500), which without the knee lit the whole view flat
  yellow.

The four brightest lights keep a stable slot each (`fx_shadows`, 0..4). After the tracer's
own passes, `wumrt_trace.comp` compiled with `FX_PASS` sends one shadow ray per pixel towards a
random point of each slot's ball (soft shadows). `wumrt_fx.comp` then:

1. accumulates those rays over time like the sun's visibility (reprojected, clamped to the
   neighbourhood, restarted when a slot gets another light);
2. blurs them along surfaces;
3. adds the light of all lights (up to `fx_max`, 16; lights without a slot are unshadowed) to
   the indirect light texture: inverse square with the ball's size, a horizon wrap for the
   ball, faded to zero at the light's reach.

The light's brightness itself is never accumulated, so a flash shows in the frame it happens.
The indirect light texture is what the game's shaders use in place of the ambient colour, so the
light is tinted by every surface's own colour. The next frame's sky rays also read it where they
hit something on screen, so the light bounces too. Needs `gi = 1`. Every loop has a constant
bound (4 shadow rays, 16 lights, 5 x 5 blur).

Settings:

```
fx = 1            # light from effects (0 = off, the default)
fx_strength = 1   # brightness of the effects' light
fx_knee = 1000    # powers above this grow only with the log (0 = no knee)
fx_flash = 2      # extra brightness when a light appears (x3 at first), gone in ~0.1 s
fx_decay = 0.35   # seconds a light takes to fade after its sprites are gone
fx_ember = 0.15   # red glow an explosion leaves behind, share of its peak brightness
fx_ember_time = 1.5   # seconds that glow takes to fade
fx_flicker = 0.2  # flicker of lights that burn longer than 0.6 s
fx_shadows = 4    # how many of the brightest lights get traced shadows (0..4)
fx_max = 16       # lights in all
fx_alpha = 1      # share of alpha-blended fire-coloured sprites that counts (0 = additive only)
fx_log = 0        # 1 = log lights, BOOMs and the cost; 2 = also every new kind of unlit draw
fx_test = 0       # >0: a steady orange test light of this power where the view's centre looks (placed once)
fx_test_height = 30   # how far above that point (a worm is about 25 units tall)
```

How bright: a single fireball is about 1500. A big blast is about 4000 after the knee, x3 in
its first 0.1 s. The crates' yellow halo rings also count, as steady small lights (about 230):
a faint warm glow around each crate. Light added to the indirect texture is power / (distance^2
+ ball radius^2): about 1.2 at 50 units from a 4000 blast, 0.4 at 100 units. In the paused test
shots that raised the ground near a blast by about 20/255; the flash frame is brighter than the
paused captures show.

Getting explosions on demand: the weapon keys are unknown (Space did not visibly fire), so the
tools wait for the CPU team's turn. With `fx_log >= 1`, a new light brighter than 400 logs
`fx: BOOM ... at x y z`. `fx_watch.py OUT --log BOOM --look 180 --capture` waits for that line,
pauses the game at once (the explosion stays frozen while paused), points the free camera at
the light from 180 units, and saves a screenshot and a draw capture. `fx_slot.sh NAME` runs a
whole batch: start, wait for a blast, fx off / on / knee / strength shots, cost, stop.
`fx_hd.sh NAME` does the same at 1920 x 1080 with `scale = 0.5`, then restores 480 x 270.

Cost (480x270, `fx_log = 1` logs it every 90 frames):

- GPU: timestamp queries around the three passes give 0.05-0.15 ms per frame with only crate
  halos, and 0.2-0.65 ms with an explosion and 4 shadowed lights. The passes do not run at all
  while there is no light.
- CPU: 0.03 ms per frame with nothing burning, 0.17 ms with an explosion (the draw scan reads
  the effect vertices).
- The tracer's whole-frame GPU time with fx on vs off was 18-31 ms either way on the shared GPU
  (other games running), so it is noise.

Captures: `captures/fx_*` (explosion paused, fx off / on).

## Lit smoke

The game draws its smoke puffs as unlit, alpha-blended sprites (`FFVertexMainTexCol` /
`FFFragmentMainTexCol`), so they keep the same cream colour by day and at night. With
`smoke = 1` the patched `FFFragmentMainTexCol` shades them like soft volumes:

- which draws: main-view, SA/1-SA blended draws of the unlit textured-colour programs whose
  texture averages to a white or grey colour with a partly transparent alpha
  (`fx_smoke_texture` in fx.c). Fire, flash and sparkle textures are coloured and stay unlit
  (they glow, and fx.c turns them into lights). Every other draw of that program gets
  `rtSmoke.x = 0` and is unchanged.
- normal: from the puff's alpha slope (4 extra texture taps), so it bends outwards at the edge
  and along the billows.
- light: the sun's colour (as the game gives it to the terrain's shader) times a wrapped,
  squared Lambert term towards the sun (direction in view space from the harness), plus the
  game's ambient colour, darker on the underside.

| setting | default | meaning |
|---|---|---|
| `smoke` | 0 | on/off |
| `smoke_strength` | 1 | overall scale of the lit colour |
| `smoke_step` | 1/32 | texture step of the alpha slope |
| `smoke_gain` | 6 | alpha slope -> normal tilt (negative flips the texture's v) |
| `smoke_under` | 0.55 | ambient the underside keeps |
| `smoke_sun`, `smoke_ambient` | 1, 1 | scales of the sun and the ambient part |

Limits: no shadow on the puff (it takes the sun as seen from the open sky), no light from
explosions on the smoke yet, no traced shadow cast by the smoke.

## Atmospheric depth

Distant surfaces fade towards the sky's own colour, more so low down (aerial perspective plus
height haze), so the backgrounds separate in layers like a matte painting (`atmos = 1`; off by
default in code). `atmos.c` runs `wumrt_atmos.comp` on the game's scene texture from post.c's
`run_post`, so it needs `post = 1`. It runs before bloom, auto exposure and the tone curve, and
before the HUD.

How it works:

1. Pass 1 (one work group) averages the linear colour of the sky pixels (nothing hit in the
   tracer's mask), weighted towards the horizon and smoothed over time. That is the haze colour,
   so it follows each level's sky: blue by day, purple at night.
2. Pass 2 blends every surface pixel towards that colour by its eye distance from the mask:
   `strength x (1 - exp(-(distance - start) / range))`, scaled by `exp(-(height - base) / height)`
   by the `atmos_low` share.

Pixels with no hit (sky, and the sea, which is not in the traced scene) are left alone. Labels
and particles over a surface take that surface's haze.

Optional sun shafts (`atmos_shafts`, off) count the open sky on 16 taps towards the sun's place
on screen and add sun-coloured light where that line is partly blocked. Not judged yet: the sun
was never on screen in the test views.

Settings (world units: a worm is about 25 tall, a level about +-600):

```
atmos = 1             # 0 = off, the default
atmos_strength = 0.5  # most haze a far pixel gets (0..1); 0.7 washes islands out
atmos_start = 150     # distance where it starts
atmos_range = 500     # distance scale of the build-up
atmos_height = 250    # the haze thins over this much above atmos_base
atmos_base = 0
atmos_low = 0.6       # how much the height matters (0 = distance only)
atmos_shafts = 0      # sun shafts strength (experimental)
atmos_shaft_len = 0.5 # share of the way to the sun the shaft taps cover
atmos_log = 0         # 1 = log the haze colour and the GPU time every 300 frames
```

Cost (480x270, timestamp queries): 0.01-0.06 ms GPU per frame. Every loop has a constant bound
(4 + 64 sky samples, 16 shaft taps).

Captures: `captures/atmos_apple_core_night_480.png`, `captures/atmos_random_level_day_480.png`
(off / on / 0.7). Tested from a free camera pulled back 350 units and raised 150 units.
`atmos_test.sh NAME` runs the batch.

## Camera motion blur

When the camera pans, flies or shakes, the scene smears softly along the motion, like a film
camera's shutter; a camera that holds still changes nothing (`motion_blur = 0.5` is a 180 degree
shutter: the smear is half of the frame-to-frame motion; 0 = off, the default in code). `motion.c`
runs `wumrt_motion.comp` on the game's scene texture from post.c's `run_dof`, right after the lit
scene, so it needs `post = 1` and the tracer. Particles, name labels and the HUD are drawn later
and stay sharp.

- Motion vectors are the camera's only: each pixel's surface point (eye distance from the
  tracer's mask) projected with this and last frame's camera. Both ends are projected from the
  same point, so a camera that holds still gives exactly zero, and when the camera matrices are
  unchanged the pass does not run at all (a paused, held frame is untouched).
- A surface that was not there last frame (something farther was, in last frame's mask, in all
  four nearest texels) moved by itself, e.g. a fast shell or worm the camera follows: it is not
  smeared and does not bleed into its neighbours. Slow movers (walking worms) get the camera's
  smear, about a pixel.
- Gather: up to 16 taps along the pixel's own smear (box shutter, jittered). A nearer tap only
  counts where its own smear reaches this pixel, so the foreground does not bleed onto the
  background behind it.
- A camera cut (turn over `motion_blur_cut` degrees, default 20, or a jump over
  `motion_blur_jump` world units, default 400) gives no blur that frame.
- Settings: `motion_blur` (shutter), `motion_blur_max` (longest smear, share of the view
  height, default 0.04), `motion_blur_taps` (2..16, default 12), `motion_blur_log` (runs, still
  and cut frames, GPU time every 300 frames), `motion_blur_split` (1 = only the right half is
  smeared: an A/B check inside one moving frame).
- Checking it: a still screenshot shows nothing by design. `py -3 mb_shot.py --free OUT.png 1.5 0 8`
  turns the free camera 1.5 degrees every frame for 8 frames and shoots during the turn (with
  `motion_blur_split = 1` the left half is the "off" picture); `mb_shot.py OUT.png DX DY` turns
  the game camera with the mouse instead (only when the human player's camera listens).
- Measured (480x270, Apple Core): 0.017 ms GPU per blurred frame; with the camera held, 0 of 299
  frames ran the pass. Captures (stream copy): `captures/mb_free_split.png` (1.5 deg/frame
  turn, right half blurred), `mb_free_after.png` (same view held), `mb_fast_split.png`
  (4 deg/frame), `mb_move_split.png` (sideways move), `mb_turn_crop_compare.png`.
- Not verified: in play with the game's own camera moves and shakes, 1080p, and how the uneven
  frame pacing (50 Hz camera ticks vs the frame rate) makes the smear length vary from frame to
  frame.

## Textures

The game's textures are tiny (mostly 128 x 128). `wumrt_textures/` in the sandbox replaces
them by hash (the same key as `wumrt_materials.ini`): `HASH_c.wtx` is the colour image,
`HASH_n.wtx` a normal map whose alpha holds the relief height (`relief.py`). A replacement is
loaded when the game first uses that texture.

**Sets.** `redo_textures.py` made the installed 1024 set (SDXL + ControlNet tile, realistic
detail). `restyle_textures.py` was a loose animated-film restyle that went too far (grass
became leaves). `film_textures.py` is the middle: painted, clean, slightly simplified, layouts
held. How hard each texture is pushed depends on its class in `wumrt_texstyle.ini`, labelled
by eye for all 451 dumped textures:

| class | what | strength / tile guidance (variant C) |
|---|---|---|
| F | featureless fills: plain grass, sand, dirt, water, ice, gradients | 0.72 / 0.50, guide not smoothed |
| S | structured repeating surfaces: bricks, planks, roof tiles, plates, bark | 0.82 / 0.38, guide smoothed (bilateral + edge-preserving) |
| A | atlases fitted to a model | 0.60 / 0.50, smoothed |
| W | worm skin and face | 0.44 / 0.70 |
| G | signs, posters, glows, cutouts | 0.32 / 0.85 (no new shapes) |

Wood planks and the hazard barrel get 0.12 less (the user preferred the gentle version of
those). Repeating textures use circular padding so they still tile. Variants A (`mid`), B, C
are kept in the script; the pick sheet compares them.

Status 2026-11-14 17:15: main's sandbox has the variant C set (`film_textures.py`, all 451,
1024) with normal maps made from the new colours (PBRify, `--flip-green`), heights from
`relief.py model` in their alpha, and packed `.wtc` (`texset_finish.sh`, then
`texinstall_main.sh`). The previous `redo_textures.py` set is kept beside it as
`rt-sandbox/wumrt_textures_redo` (to go back: `rm -r wumrt_textures; mv wumrt_textures_redo
wumrt_textures`). A 2048 pass was made for the 82 terrain textures (`out/tex_mid/2k`) but is
not installed: at strength 0.30 it adds no visible detail over the 1024 maps
(`out/judge_2k_vs_1k.png`) for four times the memory.

```
D:/AI-Upscale/venv-restyle/Scripts/python.exe -I film_textures.py D:/AI-Upscale/models/restyle --root MAIN \
    --variant C --out out/tex_mid [--size2k out/tex_terrain.txt] [--fast]
python film_textures.py install out/tex_mid          # -> HASH_c.wtx in this copy's sandbox (2K where made)
D:/AI-Upscale/venv/Scripts/python.exe -I ai_maps.py models/pbrify_normal/1x-PBRify_NormalV3.pth n --gpu --flip-green \
    --src out/tex_mid --out out/tex_mid_maps --size 512
python textures.py import out/tex_mid_maps           # normal maps
python relief.py heights ../../rt-sandbox/wumrt_textures
```

Resumable: textures already written are skipped. Never run two generation pipelines at once
or beside several sandbox games: on 2026-11-14 that, together with the user's own game, ended
in a GPU reset (nvlddmkm 153). `--fast` uses the SDXL Lightning 8-step LoRA (no guidance
pass, about 6x less GPU time; not yet judged).

**32-bit memory.** The game is a 32-bit exe without the large-address-aware flag: 2 GB of
address space. The harness frees its own copy of each replacement right after uploading it,
but the OpenGL driver keeps copies inside the game's address space, so the address space
grows with the textures' size (a level's worth of 1024 RGBA8 maps came to ~1.3 GB virtual).
Settings in `wumrt.ini` (read on start and on every edit; they apply to textures loaded after
the change, so change level or restart to see them on a level already loaded):

```
tex_compress = 1   # 0 = RGBA8 (default); 1 = colour maps stored S3TC-compressed by the driver
                   # (DXT1 when opaque: 1/8 the size, DXT5 with alpha: 1/4); 2 = normal maps too (DXT5)
tex_max = 0        # halve replacements bigger than this on load (0 = no limit), e.g. 1024 to cap 2K maps
```

The log line `rt: replacement textures loaded so far` gives the MB they take on the GPU and
the time spent uploading. `texmem.py mem LABEL` prints the sandbox game's virtual / private
memory into `out/texmem.log`; `texmem.py laa on|off` sets the large-address-aware flag on this
copy's sandbox exe only (never the real game's); `texmem_test.sh` is the one-batch measurement.
`python texpack.py [FOLDER] [--normals]` packs `.wtc` files (run it again after changing a
`.wtx`: a `.wtc` older than its `.wtx` is ignored, so heights `relief.py` writes into the
normal maps' alpha are never lost to a stale pack).

Measured 2026-11-14 on Apple Core Island at 480x270, 15-30 s after the match started (32
colour + 32 normal replacements loaded; installed set: 1024 colour maps, normal maps at their
original size):

| run | virtual | private | replacements on the GPU | upload time (32 + 32) |
|---|---|---|---|---|
| 1024 RGBA8, no large-address flag | 1406 MB | 556 MB | 174 MB | 0.25 s |
| 1024, `tex_compress = 1` (driver compresses) | 1344 MB | 568 MB | 25 MB | 1.9 s |
| terrain at 2048 (plain resizes, only to measure), `tex_compress = 2`, large-address flag on | 1385 MB | 577 MB | 89 MB | 8.1 s |
| 1024, `tex_compress = 1` with `texpack.py` files, large-address flag on | 1381 MB | 564 MB | 25 MB | 0.13 s |

So the driver does not keep much of the texture data in the game's address space: compressing
saves ~60 MB of virtual memory and 85% of the GPU memory, and 2048 terrain fits with room to
spare (the large-address flag lifts the limit from 2 GB to 4 GB; the game ran normally with it,
`selftest.sh` passed with it, 0 errors). What compression on load costs is time: ~60 ms per
1024 map and ~250 ms per 2048 map, paid the first time a texture is drawn (a hitch in play).
`texpack.py` removes that: it packs every `.wtc` once on the CPU (20 s for all 902 colour and
normal maps; 36-42 dB PSNR against the original), and with `tex_compress >= 1` the game
uploads those as they are: 4 ms per map, faster than uncompressed RGBA8. Not yet measured: the
2048 terrain from `.wtc` files in a match (two starts of that run stuck in the Deathmatch menu:
the menu automation added only one team; the driver-compressed 2048 run reached the match).

The flag is only set on a sandbox's exe copy (`texmem.py laa on`); `install.py` does not set
it on the real game, and nothing here touches the real exe.

## Stylised sea

`water_style = 1` (0 = off, the default) turns the game's sea into animated-film water. It needs
`reflections = 1`: the tracer already meets every camera ray with the sea plane, and now also
stores in the reflection texture's alpha how far that ray runs under the water before it hits
the terrain (`rtRefl.a = 1 + distance`, 8192 where it hits nothing; `wumrt_trace.comp`). The
patched `water.cg` (`RtWaterStyle`, from `patch_shaders.py`) uses it for:

- **depth colour**: turquoise in the shallows, a saturated mid blue, then deep blue
  (1/e at 28 world units along the view ray; a worm is about 15 tall). The traced mirror image
  still goes on top with the same Fresnel blend as before.
- **shore foam**: a soft band of chunky blobs (two slow, coarse samples of the game's ripple map)
  where the water is less than about 6 units deep straight down, with rings rolling in towards
  the shore and a solid lip in the last unit. Nothing at all where the ray met no seabed.
- **sun glints**: a tight sun highlight on the ripple normals, up to 6x the sun's brightness
  so the film post's bloom picks it up (`hdr = 1`); none when the sun is low or the game paints
  its sea dark.

Everything is scaled by how bright the game itself paints its sea at that pixel, so night
levels stay dark. The value of `water_style` (0..1) blends the body colour between the game's
and the stylised one; foam and glints scale with it too. The engine sees the new uniform
`rtWater` (= strength, direction towards the sun) hidden like the other rt uniforms; the C side
is `water_style` in `wumrt.c` and `rt_water_sun()` in `rt_core.c`. Not done: caustics on the
shallow seabed (would need the sea height in `Landscape.cg`).
## Cut-face edges (sculpted soil lips)

Explosions cut the landscape into hard walls. The game draws those cut faces with the same
Landscape program and the same material texture as the ground around them, and the re-meshed
terrain keeps smooth (shared) vertex normals, so a cut's lip is a tight convex bend of the
interpolated normal. `RtEdge` in the patched `Landscape.cg` (its own `edges` block in
`patch_shaders.py`) finds it per pixel from screen derivatives of the mesh normal (curvature,
about 1 / radius; > 0 convex) and the world's up direction (`rtParams3.yzw`):

- lip band (convex bend while the normal turns from up-facing towards the wall): the soil gets
  lighter there, a soft bevel highlight strongest where it faces the sun;
- wall (normal far from up): a darker, more saturated soil colour;
- concave crease (crater floor meeting the wall): darker;
- lips and walls: a rim light from the traced sky light at grazing view angles.

No extra geometry or passes; a few ALU per terrain pixel. Settings (`wumrt.ini`, live):

| setting | default | meaning |
|---|---|---|
| `edges` | 0 (off) | strength; 1 = as tuned, up to 2. Negative = debug colours (red lip, green wall, blue crease) |
| `edges_radius` | 8 | world units: a convex bend tighter than this counts fully as a lip |

The values reach the shader in the spare `rtRelief.zw` (set with the relief uniform, so only
draws with traced light on). Natural cliffs get the same treatment as cuts (the shader cannot
tell them apart); with strength 1 that reads as richer cliff soil, which fits the look.

Seen so far (Apple Core, 480x270, `captures/ledge_cmp.png`: off / on / debug): ledges whose wall
is a hard crease (split vertices, the normal jumps from up to sideways) get the darker wall
and the rim but no bevel band, because there is no bend to measure; the band shows on smooth
convex bends (rock shoulders, rubble). A fresh explosion crater was not caught in the test
run, so how the band looks on real cuts is not verified yet.

## Known limits

- Scattered objects: only on levels whose terrain textures are labelled (Apple Core Island so
  far); see "Scattered stones and tufts" for what is not right yet.
- Sky light: the sea and anything else that casts no shadow is not in the traced scene; the
  sea is approximated as a mirror of the sky at 35%. Surface colours are the game's textures
  as painted (bright, saturated), so colour bleeding is stronger than in nature; `gi_bounce`
  scales it. Hit colours use texture coordinates of meshes whose coordinates live in a
  vertex buffer; others use the texture's average colour. Up to 512 material textures.
- Materials: the game's own data is uniformly dull (specular powers 5 to 30), so glossy,
  metallic and glowing surfaces only exist where `wumrt_materials.ini` lists the texture;
  96 textures from eight levels are labelled so far (`materials.py collect` in each match,
  then `materials.py todo` for sheets of what is left). The labels have not yet been seen
  on a metal or glass object up close. Metals keep their diffuse shading.
- Replacement textures: the normal maps in use come from PBRify_Remix's normal model
  (`ai_maps.py ... n --flip-green`, its green channel points the other way), which is
  crisper than `textures.py normals` (brightness as height) and stays flat on painted
  highlights. Normal maps also scale the traced indirect light by how far the mapped
  normal tilts towards the sky. Direction was judged on one carved door at 1080p (raised
  mouldings lit on their upper edges) and the shader's sign set from that; it has not been
  checked on terrain, or left to right. The roughness model is not used: on these painted textures it follows
  brightness (it calls white ice and polished steel rough), so roughness stays a label. Only the diffuse sun light and highlights use
  them; the traced light does not. Textures are found by hash, so every level has to be
  visited once to dump its textures; two levels are done. Not part of `install.py` yet.
- The denoiser has no motion vectors: light on moving objects is re-accumulated as they
  move, and indirect light near them lags a few frames. It does not yet detect lighting
  changes to shorten history (the adaptive part of A-SVGF).
- Reflections: only the sea, as a flat mirror; only shadow casters and the sky appear in
  it (no particles, no other water effects); surface colours come from 128-pixel copies of
  the textures and the sky from 256-pixel views, so mirrored detail is soft.
- Each camera holds 13 screen-sized 16-bit float textures; at 4 megapixels that is about
  420 MB of video memory. Not yet reduced.
- Only run in the sandbox so far. Play mode (no input isolation) and `install.py` are untested in the real game.
- Not run at a real 4K window, with supersampling, or on a Steam Deck.
- After something is destroyed or terrain is blown up, its old shadow lingers for about a
  tenth of a second until the merged BVH is rebuilt. Not checked visually.
- The traversal stack holds 24 entries (the deepest seen in two levels was 14). If a ray
  ever needs more, the harness notices within a few frames and switches to larger stacks
  for the rest of the session, which are several times slower on the GPU.
- A few alpha-textured meshes that live in plain memory cast no shadow.
- Sky occlusion uses the flat triangle normals, so low-polygon round shapes can look
  faceted. Its cost has not been measured on an idle GPU.
- Accumulated sky and bounce light lags a few frames behind moving objects and lights.
- Bounced light is one bounce of sunlight within `sky_reach`. Surface colour is one average
  per texture, known one frame after a mesh is first drawn. Its cost is unmeasured.
  Point lights and the HUD's 3D models are untouched.
