# Development log

How the add-on was built, step by step, with the reasoning behind each change. The user guide is `README.md`.
Some early sections describe settings that have since changed or gone.

---

# Factorio frame generation: HUD-less probe (step 1), D3D12 bridge (step 2a), DLSS-G (step 2b)

A ReShade add-on that answers one question before we build anything bigger:
**can we reliably grab Factorio's frame right before the GUI is drawn?**
If yes, that copy becomes the HUD-less buffer DLSS-G uses to keep the UI clean
(the thing Smooth Motion gets wrong).

## Install

1. Install the **latest ReShade with full add-on support** into Factorio
   (`bin/x64/factorio.exe`, API: DirectX 11). Effects are not needed.
2. Copy `hudless_probe.addon64` next to `factorio.exe`.
3. Make sure Factorio runs on DirectX 11 (default on Windows; don't use `--force-opengl`).
4. Turn **Smooth Motion off** while probing so it doesn't touch the frames.
5. Start Factorio, press **Home**, open the **Factorio HUD-less Probe** window.

From step 2b on there is no prebuilt add-on: build it with `build.bat` (MSVC). Older notes below
about the MinGW build apply to steps 1 and 2a only. ReShade is built with MSVC, and
the two compilers return structs from C++ member functions differently, so this build never calls
a ReShade method that returns a struct (back buffer, resource desc and view lookups go through
D3D11 COM instead). An MSVC build (below) is the reliable option and will be required from step 2.

## The Factorio rule (default)

Found with this probe on Factorio 2.x (Space Age), checked on planets and in space:

| Draws | Target | What |
|---|---|---|
| world pass | world texture | entities, belts, asteroids |
| composite | final texture (+ a half-res buffer) | background, space dust, world composite, post pass `575CE4FC` |
| GUI | final texture | sprites `165FFE29`, text `E4E0B92F`, icons, panel blur |
| last | back buffer | full-screen copy to the screen |

**Rule:** copy the final texture right after the post pass `575CE4FC` (mode *After anchor*,
occurrence 1). **In space** (a space background pass `383F8F67` / `80D6CA13` ran this frame) one
more draw is let through first: the space dust layer, which uses the same sprite shader as the GUI.
On planets that same slot is the minimap background, so it stays out. If that shader is ever missing, e.g. after a game update recompiles shaders, the
fallback copies after the frame's last full-screen pass into an offscreen target. In both traces
that is the same boundary, right before the first GUI draw.

The rule is the built-in default and is saved to `ReShade.ini` under `[FACTORIO_HUDLESS_PROBE]`.
**Use Factorio default rule** restores it after experimenting.

## Find the GUI boundary (for other versions or games)

1. Load a save, open the inventory so plenty of GUI is on screen.
2. Pick **Draw index (scrub)** and drag the slider. The preview shows the back buffer
   *before* that draw. Find the point where the world is complete but no GUI is visible.
3. Click **Capture frame trace**. Look near that index for:
   - a `> BACKBUFFER` row (render target switch from an offscreen target), and/or
   - a change in PS or Blend hash where the GUI starts.
4. Click the first GUI draw, then **Use as GUI start**. This switches to **Shader rule**
   (pixel shader hash + occurrence) and saves it to `ReShade.ini`.

## Validate it

Watch the green "Matched … streak" line while you:
pan, zoom in/out, open map view, open/close inventory, hover tooltips, open chat,
toggle Alt-mode, and have trains/bots on screen.

- **Streak never breaks, preview never shows GUI:** the boundary is stable. 
- **Misses or GUI leaks into the preview:** the occurrence count shifts with scene content.
  Capture a trace in the failing situation and send it; we'll switch to a structural
  rule (e.g. "first draw after the back buffer is bound").

Press **F10** to dump the next frame to `hudless_probe/<frame>/` next to the add-on:
`final.bmp`, `hudless.bmp`, `ui_mask.bmp` (white = pixels the GUI changed) and `trace.csv`.
Send me a couple of dumps (normal view, inventory open, map view) and we move on to step 2.

## Build with MSVC (recommended from step 2 on)

Needs "Build Tools for Visual Studio 2022" with **Desktop development with C++** (includes CMake and Ninja).
Everything else (ReShade and Dear ImGui headers) is in `third_party/`.

Double-click **`build.bat`**. It finds the Build Tools, builds, and puts `hudless_probe.addon64`
next to `build.bat`. Copy that next to `factorio.exe`, replacing the old one.

Manual equivalent, from an "x64 Native Tools Command Prompt for VS 2022":
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## How it works

- `init_pipeline`: FNV-1a hashes each vertex shader, pixel shader and blend state.
- `bind_pipeline` / `bind_render_targets`: tracks what's bound.
- `draw*`: before each draw, checks the rule; on a hit, copies the back buffer into the
  HUD-less texture. With no hit (no GUI), it copies at present, so HUD-less == final,
  which is what DLSS-G expects.
- `present`: closes the frame, optionally writes the dump.

## Step 2a: D3D12 present bridge

DLSS Frame Generation only runs on D3D12. The bridge proves the plumbing before DLSS-G is added:

- a D3D12 device on the same GPU as Factorio,
- a borderless child window over Factorio's drawing area (disabled and hit-test transparent, so
  every click and key still goes to Factorio),
- a D3D12 flip-model swap chain on that window,
- every frame: Factorio's final frame and the HUD-less copy are copied into textures shared
  between D3D11 and D3D12 (synchronised with a shared fence) and presented from D3D12.

Factorio keeps presenting its own swap chain underneath; you only see the bridge's output.

**Turn it on:** *Frame generation* tab, tick **Frame generation on**, multiplier **Off** for the bridge alone.
It takes effect on the next frame, no restart. The bridge window is hidden while the ReShade
overlay is open (the overlay is drawn on Factorio's own swap chain); close the overlay to see it.

**What to check**

| Check | Expected |
|---|---|
| Picture | identical to normal, no black frame, no flicker, correct colours |
| Input | mouse, clicks, drag, scroll, keyboard all work as before |
| Resize, maximise, fullscreen toggle | picture follows, no stretching |
| Alt-tab away and back | picture comes back |
| Smoothness / latency | no new stutter, no extra input lag |
| `ReShade.log` | one `bridge: D3D12 device on ...` line, no `bridge: ... failed` |

ReShade also attaches itself to the bridge's D3D12 swap chain (you will see a second swap chain
in `ReShade.log`). That is expected; the add-on ignores every device except Factorio's.

**Tearing / VRR present:** on by default (sync interval 0 with tearing allowed), which suits
G-Sync/FreeSync displays. Untick it to present with v-sync instead.

## Step 2b: DLSS Frame Generation through Streamline

The bridge now hands its D3D12 frames to NVIDIA Streamline with DLSS-G, using the HUD-less image
from step 1. Motion vectors are still zero and depth is flat: DLSS-G works from its own optical
flow for now, so expect smearing or wobble on fast pans and moving belts. Real camera motion
vectors are step 3.

### Files to install (all in `Factorio\bin\x64`)

| What | Where it comes from | Put it in |
|---|---|---|
| `hudless_probe.addon64` | `build.bat` (MSVC) | `bin\x64\` |
| `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` | Streamline SDK **2.14.16** release zip from github.com/NVIDIA-RTX/Streamline/releases, folder `bin\x64` (not `development`) | `bin\x64\fg\` (create the folder) |
| `version.dll`, `dlssg_sm86.ini`, `dlssg_sm86\` | dlssg_for_sm86 0.4.0 | `bin\x64\` |

### Windows / driver settings

- **Hardware-accelerated GPU scheduling: on** (Settings, System, Display, Graphics). DLSS-G refuses to run without it. Needs a reboot.
- **Smooth Motion off** for Factorio, and its mod files removed from the Factorio folder.
- For this test, set `[Logging] Level=2` in `dlssg_sm86.ini`.

### Turn it on

*Frame generation* tab: tick **Frame generation on**, pick **2x**, close the overlay. The `DLSS-G:` line shows the live state, the
multiplier limit this system reports (with dlssg_for_sm86, up to 4x), and how many frames the last
present showed.

### Where to look when it does not work

| Log | What it tells |
|---|---|
| `ReShade.log`, lines starting `[Factorio HUD-less Probe] fg:` / `streamline:` | which step failed (DLLs found, signature, slInit, DLSS-G support) |
| `bin\x64\fg\` Streamline log files | Streamline's own errors |
| `bin\x64\dlssg_sm86\logs\` | whether the sm86 proxy is active (`loader_*.jsonl`, `backend_*.jsonl` with `route active=true`) |

### Measuring performance

The Steam overlay draws on Factorio's own picture, which the bridge window covers, so it does not
show while the bridge is on. Ways to see the frame rate instead:

- **Probe window:** the line under the DLSS-G status shows Factorio's own frame rate and the output
  rate (Factorio fps x frames shown per Factorio frame).
- **ReShade's counter on the bridge picture:** ReShade attaches its own runtime to the bridge's
  swap chain with a separate config, `ReShade2.ini` next to `factorio.exe`. Add
  ```
  [OVERLAY]
  ShowFPS=1
  ShowFrameTime=1
  ```
  It counts every frame actually shown, generated ones included.
- **NVIDIA app overlay (Alt+R) or PresentMon** also see the generated frames.

## Step 3a: camera probe

Finds Factorio's camera transform (pan and zoom) in the vertex-shader constant buffers, so DLSS-G
can get exact camera motion.

1. *Diagnostics* tab, *Camera constant-buffer probe*: tick **Track vertex-shader constant buffers**. The rows
   below show live contents; values that change while you move are camera candidates.
2. Click **Record 10 s**, close the overlay (Home) and, during those 10 seconds:
   stand still ~2 s, walk right ~2 s, walk up ~2 s, zoom in with the mouse wheel ~2 s, zoom out ~2 s.
3. Send `bin\x64\hudless_probe\camera.csv`.

The bridge and frame generation can stay on while recording.

## Step 3b: image-based motion vectors

The camera probe showed Factorio has no camera matrix on the GPU: sprite positions are computed on
the CPU. The only vertex constants are a fixed pixel-to-screen matrix and the game tick. So motion
is estimated from the images instead, on the bridge's D3D12 device, every frame (`src/motion.cpp`):

1. luma of the HUD-less frame at 1/2 and 1/4 resolution
2. global shift (the camera): every shift within +-64 px tested at 1/4 resolution on a sparse grid
3. per 16x16 block: the best of zero motion, the camera shift, and last frame's motion of the block and
   its neighbours, refined by one half-res pixel and a sub-pixel fit. The character, which the camera
   follows, matches "zero"; the world matches the camera; belts and trains keep their own motion.
4. expanded to a full-resolution motion-vector texture for DLSS-G (pixels, current -> previous)

Shaders are compiled at start-up with Windows' `d3dcompiler_47.dll`; if anything fails, DLSS-G keeps
running with zero motion vectors and `ReShade.log` says why (`motion:` lines).

**Controls** (*Motion vectors* tab): *Motion vectors* on/off to compare, *Flip sign* as a
diagnostic, and a live readout `camera shift +x, +y px (match %)`. Walking right should show a
positive x shift, walking down a positive y shift.

### Step 3b-2: cleaner motion vectors, on-screen readout

- **On-screen readout** (top left of the game picture, drawn after frame generation): multiplier,
  Factorio and output fps, camera shift and match quality, motion vectors on/off. Toggle it in the
  probe window. It replaces the Steam overlay's counter while the bridge is on.
- **Motion search changes:** camera motion is now the default answer; "no motion" must match clearly
  better (the character). Candidates that would sample outside the previous frame are not allowed
  (screen edges). Blocks with no real match take the camera motion (ground revealed behind the
  character). A 3x3 median removes stray blocks, and near block borders each 2x2 pixel group picks
  its own or a neighbouring block's motion, so outlines are 2 px precise instead of 16.
- **Sliders:** *Zero-motion penalty* (higher: the character must match "still" more clearly) and
  *No-match threshold* (lower: more blocks fall back to camera motion). Changes apply immediately.

### Step 3b-3: fast pans, stability, tidier window

- **Window:** four tabs. *Frame generation* (on/off, multiplier, fps, status, tearing, on-screen readout),
  *Motion vectors* (on/off, status, sliders), *HUD-less capture* (the step 1 rule, preview, frame trace, dumps),
  *Diagnostics* (back buffer info, camera constant-buffer probe).
- **Fast pans:** the camera search was limited to +-64 px per frame. It is now two-level: at 1/8 resolution every
  shift within +-128 px around zero *and* around last frame's camera motion, then refined at 1/4 resolution.
  Pans up to several hundred px per frame are tracked, and stopping is found immediately. Flat areas (black space)
  now resolve to "no motion" instead of a corner of the search.
- **Stability slider** (new, default 0.05): a block pays extra to change kind (camera / still / own motion) from
  last frame. Blocks picked as "still" (the character) get exactly zero motion, no sub-pixel jitter.
- **Defaults** are now 0.10 / 1.00 / 0.05 (*Reset to defaults*). The no-match threshold now goes down to 0.1.

### Step 3b-4: diagonal shimmer

- **Camera to a fraction of a pixel:** a third search level at full resolution (+-3 px around the 1/4-res result,
  sub-pixel fit). On test images the camera error went from about 0.5 px per axis to under 0.1 px.
- **Block snapping:** a block whose estimate lies within 0.75 half-res px of the camera (or of zero) gets exactly
  the camera's (or zero) motion, so neighbouring blocks no longer land on slightly different vectors when the
  camera sits between pixels, which is what diagonal movement does on both axes at once.

### Step 3b-5 (0.5.4): version tracking, readout hotkey

- **Version:** shown at the top of the probe window and on the on-screen readout; `ReShade.log` logs it at start
  and notes `updated from version X to Y` after an update. History in `CHANGELOG.md`.
- **Readout hotkey:** *Frame generation* tab, next to *On-screen readout*. Default **Ctrl+F11**; *Set hotkey*, then
  press the new combination (Esc cancels, Backspace clears). Works with the overlay closed.

## Step 4a (0.5.5): scene cuts, timings

- **Scene-cut detection** (*Frame generation* tab, on by default). When the picture changes completely (map view
  open/close, switching surface, teleport, loading), DLSS-G would otherwise blend the old and new picture for the
  generated frames. Each frame, the motion passes run first; the bridge waits for them, compares this frame with the
  last one at the best camera shift, and if the difference is *Scene-cut factor* (default 4) times above its running
  average, tells DLSS-G to start over. Detected cuts are counted and logged (`bridge: scene cut ...`).
  DLSS-G also has its own automatic scene-change detection, which our camera constants allow; this is a backup.
- **Timings** (readout line 3 and the *Frame generation* tab):
  - *GPU motion*: the motion estimation passes on the GPU.
  - *cut check wait*: how long the CPU waits for that check. It includes Factorio's own GPU work still in flight,
    so it is the real cost of scene-cut detection. If it is large (several ms) and Factorio's fps drops, turn
    detection off.
  - *bridge CPU*: all the bridge's work per Factorio frame, including that wait.

## Step 4b (0.6.0): zoom

The camera model is now *pan + zoom*: a pixel p of this frame was at `Z + a * (p - Z) + t` in the previous frame.
- Coarse level (1/8 res): besides the pan search, 25 zoom factors (0.62x to 1.62x per frame, 4 % apart) about the
  screen centre and about the mouse, each with pans around last frame's. A zoom must match 3 % better than the best
  plain pan, so still or panning pictures never pick up a tiny zoom.
- 1/4 res and full res: pan +-3 px and 5 zoom variants (1 % and 0.25 % apart), then sub-pixel / sub-step fits.
  On test images, motion at the screen corners during a 10 % zoom step is within 0.4 px.
- Blocks that move with the camera get the camera motion per pixel (with zoom it differs across a block).
- The scene-cut check uses the zoom-aware match, so zoom steps no longer count as cuts.
- Readout line 2 shows `zoom`: 1.000 when not zooming, above 1 while zooming in.

## Step 4c (0.6.1): auto multiplier

*Frame generation* tab, multiplier **Auto**: the largest multiplier whose output stays at or below the target,
where the target is the refresh rate of the display Factorio is on (or *Auto target fps* if set). Examples with
Factorio at 60 fps: 144/165 Hz -> 2x, 180 Hz -> 3x, 240 Hz -> 4x, 60 Hz -> off. If Factorio slows down (big base,
under 60 UPS), the multiplier goes up to keep the output near the target. A new choice must hold for 2 seconds
before it is applied, so short dips do not make DLSS-G reconfigure. Changes are logged
(`auto frame generation 2x -> 3x ...`), and the readout shows `FG 2x auto`.

## Step 4d (0.6.2): shimmer, motion debug view

- **Own-motion confirmation** (*Own-motion strictness*, default 0.50): a block may take its own motion (not the
  camera, not still) only if it already had that motion last frame, or if it matches at least that much better than
  the camera (0.5 = half the difference). Animated sprites (water, trees, smoke, working machines) change shape
  without moving; a shifted patch sometimes matches an animation frame slightly better, and that bogus vector made
  DLSS-G warp them for a frame. Trains, cars, bots match far better and pass at once.
- **Jitter averaging:** blocks that keep the same own motion as last frame (belts, trains) average their sub-pixel
  estimate with last frame's.
- **Debug view** (*Motion vectors* tab, not saved): the game picture darkened and tinted by what each pixel got.
  No tint = camera, blue = still, other colours = own motion (colour = direction relative to the camera). Tints that
  flicker on things that do not move are the shimmer sources.
- **Block shares** (tab and readout line 4): share of blocks moving with the camera / still / own motion.

## Step 4e (0.6.3): repeating patterns

On repeating content (module icons, assembler grids, belt lanes) a block shifted by exactly one repeat matches about
as well as the right answer, and once picked it was "confirmed" by the next frame. DLSS-G then blended two copies
(ghost stripes when moving sideways). Own motion now pays **Repeating-pattern guard** (default 0.04) per half-res
pixel it differs from the camera, in the block search and in the per-pixel choice near block borders. Real movers
differ from the camera by a few pixels (belts) or match far better (trains), so they are barely affected.

Note on the debug view: own motion below about 1.5 px per frame (yellow and red belts at normal zoom) is below the
search grid and is given the camera motion, which is at most about a pixel off. That is why tints mostly show when
zoomed out or with fast things.

## Step 4f (0.6.4): cloud shadows, screen edges

- **Detail matching:** the block search and the per-pixel border choice compare luma minus its local 5x5 mean
  (half res), not raw luma. Smooth brightness changes (Nauvis's drifting cloud shadows, day/night, flickering
  light) no longer look like motion; the ground under a passing shadow keeps the camera motion. The camera search
  still uses plain luma.
- **Screen edges:** a block whose camera-shifted position lies outside the previous frame is new ground scrolling
  in; it now always gets the camera motion instead of whatever patch happened to match.

## Step 4g (0.6.5): still zone (wagon-wheel aliasing)

Identical, evenly spaced things (alt-mode module icons, belt items, machine grids) that move by exactly one spacing
per frame match "no motion" perfectly - and slightly better than the true, fractional camera motion. The debug view
showed module icons as "still" (blue) at some zoom levels. Only what the camera follows (the character, the vehicle
being driven, their shadows) is really still on screen, and that is near the centre. **Still zone** (default 0.25
screen heights radius, drawn as a circle in the debug view) is now the only place "still" is allowed.

DLSS-G's own optical flow is fooled by the same aliasing (it showed with motion vectors off too); correct vectors
there are the most we can give it.

## Step 4h (0.6.6)

- Repeating-pattern guard default 0.1 (tested best).
- Map view: nothing follows the camera there, so "still" is switched off entirely. Map view is recognised by the
  game view's post pass (the boundary anchor) being absent; the readout shows `map: no still` when that applies.

## Step 5 (0.7.0): setup check, update warning, release build

- **Setup check** (*Frame generation* tab, also logged at start): Factorio version, hardware-accelerated GPU
  scheduling, Streamline files and version, dlssg_for_sm86 files, the shader compiler, and both add-on builds present
  at once. Problems open the panel automatically and show on the readout.
- **Factorio updates:** the Factorio version is remembered; after an update a notice says so. If the GUI capture
  rule has not matched once in the first 30 s of a session (its shader gone after an update), the readout and the
  tab warn (map view alone does not trigger it).
- **Release build:** `build_release.bat` -> `factorio_fg.addon64`, overlay tab *Factorio Frame Generation*,
  developer tools (capture rule editor, frame trace, camera probe, Diagnostics tab) hidden behind *Show developer
  tools*. Settings are shared with the developer build. If both files are present, only the first one loaded runs.

## Step 6 (0.8.0): depth layers

DLSS-G used to get a flat depth buffer. Now the motion pass also writes a two-layer depth image with the motion
vectors: **near** (0.25) where a pixel is still (the character or vehicle the camera follows, its shadow) or has its
own motion (trains, cars, bots, belt items), **far** (0.75) where it moves with the camera (ground, buildings).
DLSS-G uses depth to decide whose motion wins at a foreground object's edge and what ground is revealed behind it,
which is where most of the remaining artifacts around the character and moving vehicles came from. The layers are
exactly the debug view's tints: untinted = far, tinted = near. Toggle: *Motion vectors* tab, *Depth layers for
DLSS-G* (off = flat depth as before).

Result (0.8.1): no visible difference in play. With Factorio's orthographic identity camera and motion vectors that
already include all motion, DLSS-G evidently gets little from depth; the remaining artifacts come from its own
optical flow. Depth layers are now **off by default** (saves the per-frame copy); the toggle stays for experiments.

### 0.8.2: is the depth used at all?

*Motion vectors* tab, with *Depth layers for DLSS-G* on:
- **Debug view -> Depth sent to DLSS-G**: near layer bright, far layer dark. Checks our side.
- **Depth test -> Swapped (deliberately wrong)**: the ground is put in front of the character and vehicles. If their
  edges look clearly worse than with *Normal*, DLSS-G uses the depth; if Normal, Swapped and off all look the same,
  it ignores it in this setup (orthographic identity camera).
- **Status line**: which depth was tagged this frame, whether Streamline accepted the tag, and for how many frames.

Result (0.8.3): with *Swapped*, belts flashed on fast pans, so DLSS-G does use the depth where motions meet. The
normal layers are the right way round and cost almost nothing, so they are **on by default** again.

## Step 7 (0.9.0): finding movers

The depth view showed that almost nothing was marked near: not the walking character, not trains or bots.
- **Bug - the walking character:** its animation never matches perfectly, so the best ("still") match exceeded the
  no-match threshold and the block fell back to the camera. The fallback now applies only when the best match is not
  clearly better than the camera (`best > threshold` *and* `best > 0.6 x camera`).
- **Mover search:** the block search only tried last frame's motion +-1 px, so nothing that started moving fast was
  ever found. Each block now also searches every shift within +-48 full-res px of the camera at 1/4 resolution (8x8
  window, 2 px steps, then +-1). A result is used only if it is **unique** (nothing at least 3 px away matches within
  1/0.6 of it - repeating patterns always have such a second match) and **clearly better than the camera**
  (Own-motion strictness). Once a block follows a mover, the next search is centred on its last motion, so fast trains
  stay found beyond the +-48 px reach. Found movers skip the repeating-pattern guard (the uniqueness test replaces it).
  Tested on synthetic frames: a train moving 40 px/frame relative to the ground is found exactly; ground, a
  repeating stripe and flat areas are rejected.
- **Median:** a block with its own motion survives the 3x3 median if a neighbour agrees, so 1-2 block movers (bots,
  biters) are no longer removed as noise.

### 0.9.1: steadier movers

Test of 0.9.0: structures and belts were marked near when zoomed, and the player and bots were detected only about
half of the time (flicker, which DLSS-G turns into artifacts).
- No new movers while zooming (the camera differs across the search window then).
- A **new** mover must be unique (0.6) and beat the camera by the *Own-motion strictness* margin at 1/4 res **and**
  at half-res detail. A mover **already being followed** only has to stay plausible (0.8 uniqueness, 10 % better
  than the camera), so it stays marked from frame to frame.
- The no-match fallback keeps a block's kind from last frame (still / own motion) unless the camera matches nearly as
  well (0.85 instead of 0.6): the walking character no longer alternates between still and camera.
- *Mover search* toggle (*Motion vectors* tab) to compare with the pre-0.9 behaviour.

### 0.9.2: cost, player

- The mover search made the motion passes 2.4 ms (from 0.5) and the scene-cut wait 3.5 ms. It now runs only on
  blocks the camera does not explain (camera SAD above `0.25 x no-match threshold + 0.1`) or that already follow a
  mover; still ground skips it.
- A block that was "still" stays still as long as "still" matches better than the camera at all.

### 0.9.3: revealed ground

The marking showed up *behind* the walking player, exactly where the artifacts were. That strip is ground that was
under the character a frame earlier: the camera candidate points at the character and matches terribly, so any
other candidate passed the "better than the camera" checks. Still and own motion now also need a **genuine match**:
the difference must be under 0.45 x the detail of the two patches (sum of |luma - local mean|). On synthetic detail,
unrelated textures score 0.55-0.83 (never pass), a patch with 30 % of its pixels animated about 0.21 (always passes).
Revealed ground falls back to the camera, which is what it is.

### 0.9.4: small movers

Zoomed out, bots and the player cover a few pixels of a 16 px block, so the block's decision follows the ground.
Each block now keeps its mover-search runner-up even when it goes to the camera; per half-res pixel (3x3
neighbourhood) the runner-up - or "still", inside the still zone while the camera moves - is taken when it matches
those pixels genuinely (detail test) and at least twice as well as the camera. The rest of the block stays with
the camera.

### 0.9.5: back to block level

0.9.4's per-pixel second opinions failed in play: on zoomed-out ground (fine, noisy texture) a 3x3 neighbourhood
cannot tell a genuine match from chance, so the screen filled with near speckle, the strip behind the walking player
was picked up again, and bots were still missed. Removed. The mover search now runs only where the camera motion is
not a genuine match (detail test) instead of above a fixed SAD, which let detailed ground through and cost 2.4 ms.

Limit reached: things a few pixels across (bots when zoomed out) cannot be separated from the ground reliably by
image matching at this cost; DLSS-G's own optical flow handles them.

## Step 8 (0.10.0): world layers

Suggested in testing: use Factorio's draw order instead of guessing from pixels. A trace of the game view
(Factorio 2.0, Nauvis) shows the world is drawn into one offscreen picture in this order:

| Draws | Content | Pixel shader |
|---|---|---|
| ground | tiles (the 56k-index draws), decals | 7C158CCA, D82AC825 |
| 1 | full-screen pass over the ground | **BCD79626** |
| entities | buildings, belts, shadows, the player, bots, trains | 7C158CCA, ... |
| 1 | full-screen pass over everything (clouds) | **703619E0** |
| icons | alt-mode icons | 2848D8D3 |
| post | lighting + composition into the final picture | 575CE4FC |

The add-on copies the world picture right after BCD79626 (**ground snapshot**) and right before 703619E0
(**entity snapshot**; no clouds, no alt-mode icons). Both are pre-lighting, so day/night does not matter. Where the
two are equal, the pixel is bare ground, which always moves exactly with the camera:
- such pixels get the camera motion and the far depth layer, whatever the block search said (exact edges around the
  player; the revealed ground behind a moving player or train is ground, so no trailing marks);
- a block with no placed pixel at all skips the search (cheaper).
Only pixels with something placed on them go through the motion search. *World layers* toggle and status in the
*Motion vectors* tab; debug view mode *World layers* shows the mask (white = placed on the ground). If the two
shaders are not found in a frame (map view, space, a Factorio update), that frame works as before.

### 0.10.1: layers replace the guessed depth, alt-mode icons

- The depth built from motion kinds (steps 6-7) is **removed**. Depth now comes only from the world layers: placed on
  the ground = near, bare ground = far; flat when the snapshots are not available (map view, space).
- Debug view modes: *World layers / depth* (white = placed on the ground; grey everywhere = no snapshots this frame),
  *Ground snapshot* and *Entity snapshot* (the copies themselves, before lighting; magenta = not available).
- **Alt-mode icons as UI** (off by default): icons are drawn into the world picture after the cloud pass, so DLSS-G
  blends them like the world, which blurs rows of identical module icons. With this on, the world is also copied
  before the first icon draw; at the composite pass (575CE4FC) the add-on runs that pass once more itself on the
  icon-free world, keeps that as the HUD-less image, and restores the world so Factorio's own pass composes the
  picture with icons as usual. DLSS-G then treats the icons like the GUI: sharp from the real frame, moving in 60 fps
  steps. Needs world layers; not in space (the dust draws after the composite).

### 0.10.2

- **Frame generation off/on:** unticking *Frame generation on* used to shut the bridge and Streamline down, and
  Streamline cannot be started twice in one process, so it could not be turned on again. It now pauses: the window
  is hidden, DLSS-G is off, nothing is presented; ticking it again resumes at once.
- **Alt-mode icons as UI removed:** it made the icons blur more. Tests also showed that without frame generation the
  icons blur fully at speed on a 280 Hz OLED (sample-and-hold), so frame generation already improves them; what is
  left is DLSS-G's optical flow on rows of identical small icons.
- **Ground snapshot point:** the default (after BCD79626) already contains belts, pipes, inserters, floor entities
  and all shadows - the character's shadow and belt items then wrongly got camera motion. World layers are **off by
  default** until the right point is found. Developer tools, *HUD-less capture*: *Capture frame trace*, select a
  world draw, **Ground snapshot after this draw**, and check the preview (*Preview: Ground snapshot*): the right
  point is the last one that shows only tiles.

## Step 8b (0.11.0): three world layers

Picked in game with the frame trace: after the **1st draw with 7C158CCA** the world picture holds only tiles (0.11.1;
the large draw after it adds decoratives - grass, rocks - and on Vulcanus in remote view also stack inserters, so
#2 was not safe; decoratives now fall in the floor layer, where the motion search finds the camera motion). So there
are three snapshots and three layers:

| Layer | Snapshot point | Content | Motion | Depth |
|---|---|---|---|---|
| ground | after 7C158CCA #1 | tiles | always exactly the camera, no search | far |
| floor | after BCD79626 | belts, pipes, inserters, floor entities, all shadows | motion search | middle |
| objects | before 703619E0 | buildings, poles, character, vehicles, bots | motion search | near |

Static versus moving cannot come from draw order (the character, bots and assemblers share the object stage and
batches); that stays the motion search's job. The layers decide where it looks and give DLSS-G the front-to-back
order where different motions meet. Debug view *World layers / depth*: dark = ground, blue = floor, white = objects;
*Ground / Floor / Entity snapshot* show the copies. A ground point saved by 0.10.x (BCD79626, which is the floor
stage) is moved to the new default automatically.

## Step 8c (0.12.0): air layer

Flying robots are not in the object stage: Factorio draws them (and the alt-mode icons) in a late **air stage**,
after the cloud pass, so the object snapshot only held their shadows (blue, floor layer). Two more snapshots of the
world picture: right **after the cloud pass** (703619E0) and the finished world right **before the composite pass**
(575CE4FC, which puts it on the final target with lighting). Wherever these two differ something was drawn in the
air stage:

| Layer | Content | Depth |
|---|---|---|
| ground | tiles | far (0.75) |
| floor | decoratives, belts, pipes, inserters, shadows | 0.5 |
| objects | buildings, character, vehicles | 0.25 |
| air | flying robots, alt-mode icons | nearest (0.1) |

Air pixels get the motion search like objects. Debug view *World layers / depth*: yellow = air; *World (with air)*
shows the last snapshot. All five snapshots must be taken in a frame for the layers to be used (green note in the
debug panel); in views without that pass order (map, some menus) the add-on falls back to the plain search.

## Step 8c (0.12.1): icons versus flying robots, fast trains

**Air split.** The air stage holds two different things: alt-mode icons, which sit on buildings and move with the
camera, and flying robots, which move on their own. A sixth snapshot right before the first draw of one shader
(default D82AC825) splits it; in the debug view *World layers / depth* alt-mode icons are **cyan**, flying things
**yellow**. Icon blocks take the camera motion whenever the camera genuinely matches (no search, no candidate that
could land on the neighbouring identical icon); single icon pixels take it too unless the camera clearly matches
worse (an icon on a moving wagon). If icons and robots come out the wrong colours, flip *Alt-mode icons are drawn
before it* in the Motion vectors tab; if both are one colour, pick another split shader from the list of air stage
draws shown there (last frame's shaders with draw counts) and check again.

**Wide mover search.** The normal mover search reaches +-48 full-res px per frame; a train with speed upgrades
moves further. Where a block holds objects or flying things, the camera does not match and the normal search found
nothing, a second search at 1/8 res reaches +-192 px (same uniqueness test, then refined). Once found, the block
follows the motion as before. Toggle: *Wide search for fast movers*; compare *GPU motion* on the readout with it on
and off. Found movers are now refined at half resolution (the search result could be 2 px off), and the per-pixel
repeating-pattern cost is capped, so fast movers keep their edges.

## 0.12.2: shader cache

The motion shaders are compiled by d3dcompiler_47 at run time. With 0.12.1 that took about two minutes on the
render thread, before the game's loading screen appeared. Now it runs on a worker thread (the readout says
*compiling shaders*; frame generation runs with zero motion vectors until it is done) and the result is stored in
`factorio_fg_shaders.cache` next to the add-on. Later starts load it in milliseconds; a new add-on version with
changed shaders compiles once again. ReShade.log shows `motion: shaders compiled in N s (slowest: ...)` or
`motion: shaders loaded from the cache`. Deleting the file is safe.

## 0.12.3: latency

*Cut check wait* on the readout is how long the add-on holds the frame on the CPU to decide whether the picture
changed completely (scene cut) before DLSS-G gets it - that wait is added latency. It used to cover Factorio's
frame on the GPU plus all motion passes. The scene-cut metric is ready after the camera's coarse search, so that
part is now submitted on its own and the CPU waits only for it; block search, smoothing and expansion run on the
GPU meanwhile. *GPU motion* is shown per stage: camera (luma, downsampling, camera solve), blocks (block search,
mover and wide search), rest (smoothing, per-pixel expansion, depth, debug). Turning *Scene cut detection* off
removes the wait completely (at the cost of smeared frames on map open / teleports).

## Step 9 (0.13.0): performance

0.12.3's readout showed where the GPU time goes: camera 0.7 ms, blocks 3.4 ms, rest 0.1 ms. The block pass ran one
thread per 16 x 16 px block (14,400 threads at 1440p - too few to keep the GPU busy), and the mover searches ran
inside it: a block that searched (169 shifts, or 625 for the wide search) made the other 31 blocks of its warp wait,
and each search held a 169-entry array per thread in slow local memory. Now:

| Pass | Work |
|---|---|
| cs_block_pre | per block: bare ground, new ground at the edges and alt-mode icons are settled here; the camera match; whether a search is needed |
| cs_mover | one group of 64 threads per block that needs a search: the search grid is shared out over the threads; the others leave at once |
| cs_block | candidates, refinement and the final decision for the remaining blocks |

The decisions are the same as in 0.12. The readout's new line *Searched blocks / wide / movers* shows how many blocks
needed the mover search, the wide search, and how many movers were found last frame. The shader compile (or cache
load) now starts when the add-on loads instead of when frame generation starts.

## 0.14.0: one copy per snapshot

Each world layer snapshot used to be copied twice per frame: from Factorio's world target into the add-on's texture
when the draw order reached it, and from there into the bridge's shared texture at present (six full-screen
copies each way). The bridge now hands out its shared textures and the snapshots are copied straight into them. Two
sets alternate frame by frame, so D3D11 never writes the set D3D12 may still be reading; the existing fence wait at
present orders the rest. When the textures are recreated (resize, frame generation restarted) a frame's snapshots
are dropped rather than used. With the developer preview showing a snapshot, the old two-copy path is used.
