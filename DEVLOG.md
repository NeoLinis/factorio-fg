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

## FSR: the GUI kept out of frame generation

First test on the RTX 3080: FSR 3.1 ran, module blur on pans looked better than with DLSS-G in places, but the GUI
ghosted on movement and the character showed artifacts. FSR was given the final frame plus `HUDLessColor`, from
which it detects the UI itself; that detection let the moving world show through the GUI in generated frames.

FSR's swap chain can instead compose a separate UI surface over every presented frame. The add-on has both
pictures, so a small compute pass writes the UI surface (the final frame where it differs from the HUD-less copy,
alpha 1; elsewhere transparent), the back buffer gets the HUD-less frame, and the surface is registered with the
swap chain each frame, with its internal double buffering so the texture can be rewritten right after Present.
Real frames come out identical to Factorio's; generated frames show the interpolated world with the newest GUI on
top. With frame generation off the surface is unregistered and the final frame is presented as before. FSR's own
detection stays as the fallback (no HUD-less frame, motion debug view shown, shader not available). AMD's debug view
(`FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW`) is available under Debug views for the character artifacts.

The real cause: the GUI capture rule was **Off**. `load_config` started from `int mode = 0` (Off) and kept it when
the `Mode` key was missing, i.e. on every fresh install; the next settings change saved that Off. Since 1.0.0 a
clean install therefore never captured a HUD-less frame: the "HUD-less" copy was the final frame with the GUI,
DLSS-G coped (its optical flow sees the GUI standing still), FSR moved the GUI with the world, and the still zone
for the character (which needs the rule) was off. The default is now the built-in rule, and a saved Off is
switched once (`CaptureModeChecked`). The readout has a *GUI capture* line so this cannot go unnoticed again.

Switching the technology in place (bridge shutdown and restart) hung DXGI in both directions on the RTX 3080; the
choice now applies at the next start of Factorio.

## Character held still per pixel

FSR's debug view showed the running character with the camera's motion in our vectors (FSR's own optical flow saw
it differently but trusted ours), and the readout showed 0 % still blocks. The world layers had the character as an
object, so the block search was what missed it: a 16 x 16 block on the character is mostly ground moving with the
camera, and the running animation never matches "no motion" well enough for the block to turn still. DLSS-G hid
this with its own optical flow. Now `cs_expand` checks object pixels in the still zone (when the camera moves): if
"no motion" matches the 3 x 3 neighbourhood clearly better (x0.7) than both the camera and the block's motion, the
pixel is still. Buildings passing the centre match the camera and are unaffected.

## The character as part of the UI layer (FSR)

With the character held still (beta 7) the remaining blur on it is the blend itself: Factorio animates at the
game's tick rate, so a generated frame between two ticks can only mix two poses (FSR blends them softer than
DLSS-G). The character stays at the same place on screen while the camera follows it, so it does not need to be
interpolated at all: `cs_expand` writes a `hold` mask (object pixels in the still zone that ended up still while
the camera moves), and FSR's UI extraction adds those pixels (grown by one for the outline) to the UI layer. The
swap chain then lays the newest real frame's character over each generated frame, exactly as it does the GUI.
Setting: *Keep the character sharp (FSR)*, on by default. The shadow is left out: the ground under it moves, so
taking it from the real frame would show a seam.

## The character removed from what FSR blends (experimental toggle)

Holding the character and laying it over each generated frame left an "after image in front": FSR still blends a
picture with the character in it, and around a static object on scrolling ground it pulls character pixels into
the ground half a camera step away. Tools that add frame generation to existing games treat the character, its
shadow and the moving background separately and fall back to real-frame pixels at uncovered background. Here the
background under the character is known: the floor snapshot (world after the floor stage: ground, belts,
shadows, no objects). With *...and remove it from what FSR blends*, the UI pass also writes a clean frame: the
HUD-less frame, with the held character (grown by 2 px) replaced by the floor snapshot times the lighting ratio
(finished frame / snapshot, summed over 8 points 48 px around that are not held, per channel). That clean frame
goes into the back buffer, so FSR interpolates ground only; the sharp character from the real frame still goes
on top through the UI layer. Off by default to compare.

## The UI layer for both technologies

The GUI and held-character layer and the clean frame were FSR-only code. They now live in `ui_layer` (one compute
pass, recorded by the bridge before the backend), and each backend uses them its own way: FSR copies the clean (or
HUD-less) frame into the back buffer and registers the layer with its swap chain; DLSS-G gets the layer as UI color
and alpha with `enableUserInterfaceRecomposition` on, and the clean frame as its HUD-less input. If this DLSS-G
refuses the option, it continues without the layer (and without the clean frame, which would otherwise make the
character look like UI). The character still-hold in the motion vectors was already shared.

## 1.1.0 wrap-up

Betas 25-26: a pass closing the hold over pixels near the character (the vote missed its trailing edge) gave a halo
in front while flying and no gain behind, and was removed; a held shadow is no longer painted over (the floor
snapshot contains it, and moved with the ground it was dragged behind the player). With the paint-over on or off
the noise behind a moving character looked the same: it is frame generation guessing uncovered ground, which the
add-on cannot know better from outside the game. Paint-over stays as an option, off by default. Released as 1.1.0.

## 1.2.0: exact character mask

Goal: keep the character in frame generation but give it exact inputs (mask, motion vectors, depth) so it gets smoother;
if a technology still shows copies or noise behind him, take him out of frame generation for that technology (world
without the character for frame generation, the character laid over like the GUI).

The vote ("stays put while the world moves") works per pixel and misses limbs and the trailing edge; the noise behind the
player sits exactly there. Factorio positions sprites on the CPU and sends screen-pixel rectangles (step 3a), so the same
rule can be applied per sprite: a sprite at the same screen rectangle as last frame while the camera moved follows the
camera (character, driven vehicle, ridden wagon, their shadows). A sprite is all or nothing, and its shape comes from
its own alpha: exact to the pixel.

### Beta 1: sprite probe (step 0)

`src/sprite_probe.cpp`. *Motion vectors* tab, *Sprite probe*, *Record 6 s*, keep running. It follows input layouts,
vertex / index buffers (size, heap, initial data), their writes (Map discard / no-overwrite, UpdateSubresource, GPU
copies) and the world target's draws per stage. On frame pairs 30/31, 60/61, ... it reads the mapped vertex data at
Unmap (no-overwrite maps only from the end of what was drawn so far), decodes every sprite into a screen rectangle and
texture region, finds the camera shift from sprites whose texture region appears once in both frames, and classes
every sprite: world (moved with the camera), HELD (same place, same texture region), HELD animated (same place and size,
other texture region), own motion, unmatched. It also times reading mapped memory with memcpy and with streaming loads.
Decides: CPU path (read the vertex data, split Factorio's draws) or GPU path (copy and classify on the GPU).

### Beta 2: probe results 1, decoding fixed

Run 1 (running) and 2 (flying), 2560x1440, RTX 3080:
- World target: 42-85 draws and 17k-40k indices per frame; no instancing. Main sprite pipeline VS 26F61E61 with PS 7C158CCA
  (all stages) and D82AC825; a few others in the air stage.
- Vertex layout (all sprite pipelines): TEXCOORD0 float2 +0, TEXCOORD1 float2 +8, TEXCOORD2 RGBA8 +16, TEXCOORD3 uint +20
  (stride 24). No POSITION semantic, which is why beta 1 decoded nothing.
- One dynamic vertex buffer of 1 MB: Map discard once per frame, then 40-72 Map no-overwrite per frame (a ring); one
  static index buffer (quad pattern, 131076 indices). No UpdateSubresource, no GPU copies.
- Reading mapped (write-combined) memory: memcpy 300-320 MB/s, SSE4.1 streaming loads 3.2-5.1 GB/s. Factorio writes
  well under 1 MB per frame, so reading it all costs ~0.2-0.3 ms with streaming loads: the CPU path is affordable.

### Beta 3: probe results 2

- Decoding works: 5500-6300 sprites per frame (most of them ground tiles, drawn in one draw of ~32k indices), positions in
  screen pixels (top-left origin). Draws use first-index offsets into the static quad index buffer (vertex offset 0).
  Camera shift found exactly from sprites: integer steps of 10-14 px per frame while running.
- The objects stage alternates PS 7C158CCA and PS CE9376D6 (VS AC1FB602; TEXCOORD3 = 0xFFFFFFFF there).
- The only "HELD" sprites were the full-screen passes (float3 positions); the character's sprites are not found as
  staying put: they come out as own motion / unmatched. The centre list in beta 2 was filled by ground tiles, so beta 3
  lists only non-tile sprites near the centre, with their closest match last frame, to see how they actually change.
- The world's vertex data arrives in the first few Maps of the frame (one of ~520 KB with the tiles); afterwards the GUI
  keeps mapping the same buffer. Reading stops once the world is drawn.

### Beta 4: probe results 3, candidate rule

Running (run 1): the character is two sprites drawn one after the other in the objects stage with PS 7C158CCA: the body
(colour FFFFFFFF) and the player-colour mask (colour 7F0000FF, the player's red at half alpha), both covering the
screen centre (top-left about -30..-60, -66..-77 px from it, 56-107 x 47-110 px). Animation frames are trimmed to their
visible pixels, so size and corner change every frame and frames come from different atlases (texture changes too): no
exact match. Last frame's closest similar sprite is 0-5 px away (camera 15-21 px). Flying (run 2) over a forest: the list
filled up with trees (PS CE9376D6, VS AC1FB602: trunk + tinted leaves) before reaching the character.
Candidate rule tested in beta 4: same shader and colour, size within 35%, centre within max(6 px, 10% of the size) of
last frame's and at least 4 px closer than the camera-shifted position.

### Beta 5: probe results 4

Running: the rule finds the body and the player-colour mask in 9 of 10 moving pairs (2.2-4.8 px from staying put,
9.7-15 px from the camera shift); once the body's trimmed size changed by more than 35% (missed), once a 6x7 px belt
item matched by chance. No shadow on the world target near the centre: shadows are likely drawn into their own target
and blended in by the full-screen pass BCD79626. Flying: no character sprite decoded at all, and 2-3 draws per frame
without data: reading stopped at the first draw to another target after the air stage, before the flying character is
drawn. Beta 5 reads until the composite, also decodes other screen-sized targets during the world, and allows 50% size
change (12% position tolerance).

### Beta 6: probe results 5

The rule works for the character: running, body + player-colour mask in every moving pair but one (no character sprite
near the centre in that frame at all: a camera jump), 0-4 px from staying put against 15-23 px camera shifts, nothing
else picked up. Flying: drawn in the air stage as body, mask and a glow layer (colour 00FFFFFF, extra 0x10: additive),
all caught in every moving pair; once a 2 px glow speck as well. No shadow: nothing was decoded on other targets, because
beta 5 only took targets in the back buffer's format. Beta 6 takes any screen-sized target.

### Beta 7: probe results 6

Running again: body + mask found in all 10 moving pairs, nothing else. Other screen-sized targets (all RGBA8) are only
drawn to before the world's tile draw: T1 with ~5000 sprites per frame (a shadow layer? BCD79626 blends something in
during the floor stage) and four small ones. Those sprites count as stage "tiles" and were skipped by the rule; beta 7
includes them. On the world target, PS D82AC825 draws black at half alpha (shadows of some kind) in the floor stage,
but nothing of it near the character.

### Beta 8: the mask (step 1)

Probe results 7 (running, flying): the shadow is on T3, a screen-sized target drawn before the world's tiles (PS 1C022511,
black sprites ~100-150 x 50-70 px below right of the character, further right while flying), found by the rule in every
moving pair. False hits only on T4/T5: long thin strips at the screen edges (redrawn bands, 2000 x 17 px): excluded by
size. Identification done.

`src/sprite_mask.cpp`: records the sprite draws (sprite layout, the dynamic vertex buffer) of the world from the floor
stage to the air stage and of the other screen-sized targets before the tiles; at present maps the vertex buffer
no-overwrite (the frame's data is still there: Factorio discards only at the next frame's start) and reads just those
draws' vertices (streaming loads); keeps sprites whose centre is within 30% of the screen height of the centre and that
are at most 45% of it in size; camera shift from sprites whose texture region and size appear once in both frames;
picks: same shader, colour and target as a sprite last frame, size within 50%, centre within max(6 px, 12%) of staying
put and 4 px closer than the camera-shifted position, or continuing a picked sprite (not moved with the camera).
Last frame's sprites are bucketed by shader, colour, target and 96 px cell. The picked sprites are drawn with their own
atlas views (kept alive with AddRef from the draw to present) into an RGBA8 mask: R the character (alpha; the additive
glow by brightness), G the shadow (MAX blend). Optional overlay onto the frame. Native D3D11 on the immediate context,
state saved and restored.

### Beta 9: mask results 1

Beta 8 in game: the running character mostly correct, sometimes dropped after moving; the shadow missing or a blue
square; flying mostly a red square or dropped; zoomed out found less often, with random objects or lines. Changes:
- a track of the character's parts (shader, colour, target, rectangle relative to the screen centre, smoothed, forgotten
  after 45 frames unseen): a sprite overlapping a part (40% of the smaller rectangle, size ratio 0.4-2.5) is kept, unless
  it clearly moved with the camera. Covers stops, idle / take-off animations (sizes change by more than 50%).
- zoom-aware: moving from 2 px camera shift; tolerance max(3 px, 12% of the size); follows needs b0 < half the camera
  shift and bc >= b0 + max(1.5, 30% of the shift); a sprite that moved with the camera (bc + max(1, 25%) < b0) is never
  picked. New parts (not in the track) must overlap a box of 12% of the screen height around the centre.
- the additive glow (colour alpha 0) is no longer drawn into the mask: it covered its whole quad (the red square).
- shadow coverage: textures whose view has no alpha channel (R8, BC4, BC5...) use red (the blue square was alpha = 1).
  ReShade.log lists the view formats seen.

### Beta 10: mask results 2

Beta 9: "basically perfect" at normal zoom; zoomed far out trees, grass and other objects mostly red, a blue square in
movement. Status zoomed out: 6575 sprites near the centre, 1102 picked, all by "continue", camera +0 +0, 16 ms; at
normal zoom standing still 34 picked (continue) - also too many. Cause: continuation could spread from one picked sprite
to every look-alike within 3 px (grass, decoratives), and a wrong camera shift (few distinct sprites zoomed out) made
everything "follow" once. Now: continuation and track matches are one-to-one; more than 12 follows in a frame are all
dropped; more than 24 picks reset; the camera shift needs best >= 3 and a third of all distinct matches; shadow-target
sprites only within 1.5 x the character's size of its rectangle and at most 2.5 x its size; reach 20% of the screen
height; once the shadow target is known only it is read (another target has ~5000 sprites a frame).

### Beta 11: cost

Zoomed far out, moving cost a lot of frame time in beta 9 (16 ms reported). Beta 10 already capped the picks and the
reach; beta 11 also skips the floor stage (the character is drawn in the objects or air stage), detects Factorio's 16-bit
quad index pattern once (0 1 2 2 1 3, +4 per quad) and then reads quads directly instead of index by index, and drops a
sprite early when its first corner is far from the centre. MSVC warnings C4244 (std::fill with an int) and C4127 fixed.

### Beta 12: mask results 3

Beta 11: cost better; fully zoomed out the character is found most of the time; the shadow is missing or a square over
the character; now and then trees or grass in red. Changes: new parts start only within the character's known parts
expanded by half their size (the centre box only while nothing is known); world when an exact look-alike lies at the
camera-shifted place (bc <= 0.75 px, b0 > 1 px), so a neighbouring look-alike can no longer keep grass picked; shadow-
target sprites must be black (colour RGB 0, as the shadow pass draws them).

### Beta 13: mask results 4

Beta 12 screenshots (zoomed out, forest): tree trunks across the screen in red; closer in, trees around the character in
red, the shadow now a correct blue ellipse. The beta 12 region (known parts expanded by half their size) grew with every
picked tree. Now a world sprite qualifies only if the distance from the screen centre to its rectangle is at most its own
size (min 8 px) - the camera centres on the character, flying the body is about half its height above the centre - and
at most the 8 world sprites nearest the centre are kept.

### Beta 14: the character by identity

Beta 13: fewer trees, but the shadow and sometimes the character dropped. The motion rule needs the camera to move and a
reliable camera shift; zoomed out in a forest neither holds. Instead the character is now found by what it is: Factorio
draws its player-colour mask as a separate sprite tinted with the player's colour at half alpha (7F0000FF for red), right
after the body over the same place, and flying the additive glow right after that. So: the mask = the sprite near the
centre (within its own size) with alpha 0x40-0xC0 and a colour that is neither black nor white, nearest the centre; the
body = the sprite up to 3 places before it in draw order covering at least half of it; the glow = additive sprites up
to 3 places after it covering 30%. No camera needed, no memory, nothing can spread. Vehicles the player drives carry the
player colour too (wanted: the camera follows them). The shadow still needs the camera rule (black sprite on the shadow
target within 1.5 x the body's size of it, at most 2.5 x its size), then: carried on (one sprite, last frame's shadow
within tolerance) and remembered by texture region + size once it followed the camera (up to 8192 regions).

### Beta 15: mask results 5

Beta 14: the character red most of the time, except where a tree is drawn over it; the shadow missing fully zoomed out
and mostly dropped even fully zoomed in. Trees: their leaves are tinted sprites too (alpha 0x43-0xCC, e.g. 658AEDF2), and
the nearest tinted sprite won. The colour layer now needs alpha 0x70-0x90 (the player colour at half alpha), and once the
same colour won 20 frames in a row only that colour counts (unlocked after 300 frames without it). Shadow: it needs the
camera shift, which came from the sprites near the centre only - too few zoomed in. Now every world sprite of the objects
and air stages anywhere on screen (corners 0 and 3: texture region + size key, top-left) feeds the camera shift (up to
8000). Remembered shadow regions are keyed by texture region only (no screen size), so they hold across zoom levels.

### Beta 16: mask results 6

Beta 15: "huge improvement", no shadow anywhere. Fully zoomed out the character is sometimes NOT FOUND (colour locked,
camera "?"). Likely: the near-centre list (20% of the screen height) filled its 20000 sprites with forest before the
character's draw; the 8000 camera sprites were the first 8000 in draw order, a different set each frame (camera unknown,
so no shadow); swaying trees spread the deltas. Now: reach = 5 x the character's size (min 64 px) once known (reset after
30 frames without it); camera sprites only from the central 60% (up to 16000); camera shift = mean delta within the best
3x3 block of 1 px bins, trusted with >= 3 and >= 30% of the matches; sprites >= 1 px.

### Beta 17: mask results 7

Beta 16 screenshots: the character red, with a thin red bar beside it; status "10 sprites near the centre; character found
(locked, 2 sprites), shadow -, 0 shadow regions known, camera ? +0 +0". The camera shift was unknown in an open field:
only the objects and air stages fed it, and there are few objects there. The floor stage is recorded again, for the
camera only (not for picking); camera sprites are thinned by key (1 in 2^k, adjusted between 1500 and 12000 per frame,
the same set in consecutive frames). The body: the largest sprite up to 3 places before the colour layer that lies at
least half inside it and has at least 30% of its area (the bar was a thin sprite taken first).

### Beta 18

Beta 17: the overlay's red is wrong in places (bars, blocks) - not the character's own red highlights. The colour layer's
texture is a tint mask (which pixels take the player colour), sampled as coverage it gave rectangles. It is still the key
to finding the character but is no longer drawn; the body's alpha is the mask.

### Beta 19: remembered body frames, dumps

Beta 18 fully zoomed out: the character sometimes NOT FOUND for long stretches (colour no longer locked: missing for 300+
frames), "camera ?", the overlay blocky (at ~15 px the atlas is heavily minified). The colour layer seems missing or
different there. Now every body animation frame found by its colour layer is remembered (view + texture region, up to
16384); without a colour layer, the nearest remembered body frame near the centre is the body. Also a dump when the
character is lost (or on request): status, lock, camera matches, and every sprite within 160 px of the centre in draw
order (target, stage, shader, colour, rectangle, texture region) plus last frame's picks - to see what Factorio draws
there instead of guessing.

### Beta 20: the vertex buffer is reused within a frame

Beta 19's dumps (12 of them, max zoom out and normal) explained everything at once: the same vertex bytes read back for
three different draws - shadow-target draws at stage 0, world draws at stage 2, another shader later - all showing GUI
bars (400x5 px, colour 6403070F). Factorio's 1 MB dynamic vertex buffer is a ring: no-overwrite maps while it has room,
a discard when it is full. The probe (a light scene) saw one discard per frame; with ~40000 sprites a frame (dump order
numbers up to #77197, 96 bytes each) it is discarded several times a frame, and reading at present gives every earlier
generation's draws the last generation's data. Hence random blocks in the overlay, the character "NOT FOUND" with only a
handful of unrelated sprites near the centre, the shadow (drawn first, at stage 0) never found and the camera shift
always "?" (no world sprites where they should be). Bodies remembered by beta 19 from such frames were garbage too (one
with the tree shader).

Fix: each recorded draw keeps its vertex range (from the index buffer at draw time) and is pending until copied:
- at Factorio's next no-overwrite map of the buffer (ReShade's map event comes after the map, with Factorio's pointer:
  no-overwrite keeps what was drawn, so the pending draws' bytes are read from it, no map of our own);
- at the draw itself when the ring is nearly full (free space < 1.25 x the largest batch between two maps, decaying,
  + 16 KB): the next map may be a discard, so the add-on maps no-overwrite itself on the immediate context and copies;
- at present for the rest (the last generation is intact).
A pending draw met by a discard is counted as lost (status "N draws, M lost"; the dump shows draws, copied early, lost,
KB). The copies go to one growing buffer reused every frame. How far the ring is used comes from every draw on it (quad
index pattern or vertex counts), also those not recorded. Shadow-target draws on other targets are skipped before any
copying once the shadow target is known.

Colour layer: dumps 2-8 (start of the session, before the colour lock) took grey half-alpha tints (7F3C3C3C, 70353535,
smoke / leaves) for it. A candidate now needs a body right before it in draw order (as the real one always has).

### Beta 21: drops - the quad index buffer not recognised

Beta 20 removed the artifacts (the user: "big improvement, no more artifacts, only drops"). Its 12 dumps, all at a drop:
"~700 draws, ~520 lost, 0 copied early", and still "0 / 0 camera sprites". In those frames the shadow target's draws
(start of the frame) and the air stage (end) were read, the objects stage in between lost. Two things fit:
- camera sprites are only collected on the fast path (Factorio's quad index pattern), and there were none: the index
  buffer was not recognised as the quad pattern. It is 262152 bytes, 21846 quads - more than 65536 vertices, so the
  pattern cannot hold to its end in 16 bits; beta 20 required all of it;
- without the pattern, how full the ring is (gen_fill) was never updated, so the copy before a discard never happened
  and the pending draws of a whole generation met the discard.
Fixes: the pattern is accepted for the quads from the start that follow it (quad_count; at least 256), and the fast path
needs the draw within them; gen_fill also from every recorded draw's own vertex range (whatever the index buffer); a
safety net - after any lost draw, every recorded draw is copied right at the draw (own no-overwrite map) for 600 frames
(status "copying at every draw"); camera sprites also from draws off the quad pattern. ReShade.log lists index buffers
(size, pattern length, first indices); the dump shows how many draws were on the pattern.

The dumps also show what is probably the character's shadow: on T3 (stage 0), black, ~40x16 px, from the body's feet
to the right (#926, #866, #885, #897, #712). It was never picked because the shadow rules need the camera shift.

### Beta 22: 32-bit indices, the shadow on any target

Beta 21: "only the shadow sometimes flashes at max zoom, the player is stable." Both dumps were benign: one at load (the
main menu's character, colour 7F4C657F, still locked), one mid-zoom (all sprite sizes change, no camera match). The log
showed why the quad pattern was never recognised: every index buffer "starts 0 0 1 0 2 0 2 0" read as 16-bit - it is
0 1 2 2 1 3 in 32-bit indices (the 2400000-byte buffer: 100000 quads). The probe had it wrong. Detection now tries 32
then 16 bits and the fast path needs the draw's index size to match.

Shadow flicker: beta 21 picked it on T4, earlier dumps showed it on T3; once a shadow target was known, draws on other
targets were no longer recorded (a cost saving from when reading was slow). If Factorio alternates or switches shadow
targets, the shadow is missed on the other one. All screen-sized targets are recorded again; g_shadow_target is now
only reported. A dump is written when the character is found but its shadow was just lost (at most 6).

The colour lock is released after 120 frames without it instead of 300.

### Beta 23: the shadow by its place

Beta 22: "the shadow blinks (fast direction switches) and sometimes drops standing still (picked up again in motion)".
Six "shadow was lost" dumps, all moving (camera +13 px): the real shadow is right there every time - T5, black, from
the body's feet to the right, ~1.3x the body's width - but this frame's shadow sprite (a new animation frame after a
turn) sits ~12 px from last frame's, which is about the camera shift. The "moves with the camera" test threw it out
before anything else was looked at, even a remembered region. Standing still nothing follows the camera, so only
"continues" was left, and once that chain broke the shadow stayed gone until the next movement.

Now the place relative to the body is evidence of its own: (left edge - body left) / size, (bottom - body bottom) / size,
width / size, height / size, tolerances 0.3 / 0.25 / 0.35 / 0.25. Running starts from what the dumps show (0.3, +0.1,
1.3 x 0.55; the three dumps fit with errors <= 0.19), flying starts unknown (the shadow is lower by the flying height)
and is used after 10 learned picks. Learned (EMA) only from picks with independent evidence (remembered region or
follows the camera), so it cannot drift on its own picks. "Moves with the camera" now only rules out candidates with no
other evidence. Ranks: remembered 3; follows the camera or by its place 2; continues 1; ties by the closest fit to the
place.

### Beta 24: the shadow's place per body frame

Beta 23: "rare drops on shadows, usually transitions under elevated rails while flying. Could be good enough." Six dumps:
- two takeoff / landing frames: the body already on the air stage (flying place expected: left edge 0.7, bottom +0.48)
  while the shadow was still at the running place (0.12 / +0.03, 1.47 x 0.47). Now a candidate fits if it matches
  either place (flying once learned); both dumps' shadows fit the running place and the elevated rails' shadows next
  to them do not (too tall);
- a turn zoomed in: the body sprite's trimmed box jumped 30 px, the left-edge offset went from 0.12 to 0.56;
- three in flight: the learned flying place drifts between dumps (left edge 0.77 -> 1.15) - it depends on the facing.
Exact fix: every body animation frame (texture region, remembered anyway) gets its own shadow place, in body sprite
heights from its left / bottom edge, learned (EMA 0.3) from every pick with rank >= 2. A candidate within 0.08 / 0.08
/ 0.12 / 0.12 of it ranks like a remembered region (3). Up to 16384 frames.

### Beta 25: hugging a tree

Beta 24: "only zoomed in and hugging a tree, otherwise near perfect" - screenshots: the whole tree trunk red, the tree's
shadow blue. Dumps 4 and 5: last frame's body was a tree (PS CE9376D6, 291 x 498 px) for a colour layer of 81 x 105.
Factorio sorts by position, so a tree the character stands in front of is drawn right before it - within the 3 places
the body is looked for - and it covers the colour layer completely; "the largest overlapping sprite" picked it, and
the shadow by its place then followed the tree's body box. The real body is 1.3-2.2x the colour layer's area in every
dump (zoomed in, out, flying); the tree was 17x. Body candidates are now limited to 4x the area and 2x the larger side.
The remembered-frame fallback also needs about the character's size (0.5-2x), since this bug may have taught it tree
regions. The other dumps: map view / scene changes (7 sprites near the centre, a 257 px T4 sprite) and the main menu.

### Beta 26: beta 25's limit too tight; the shadow between ground and air

Beta 25 dumps 7-18 ("character lost"): body and colour layer both present, but facing up or down the colour layer covers
only the torso and the body is up to 2.2x its height (44.6 x 89.2 over 37.1 x 43.1; 75.4 x 172.3 over 67.3 x 78.1) -
beta 25's "at most 2x the size" rejected the body, so the colour layer had no body and was not accepted. Trees in the
same dumps: 4.2-4.7x the size, 14-17x the area. Now 3x the size, 4x the area.

Shadow-lost dumps 1-6 (mostly flying): taking off and landing the shadow sits between its running and flying place
(bottom edge +0.3 body sizes: neither +0.02 nor +0.48 / +0.66), and sideways it was 0.32-0.36 off with a 0.3 tolerance.
The expected place is now interpolated along the line from the running to the flying place, the position along it
read from the candidate's bottom edge (t clamped to 0..1); sideways tolerance 0.4. Checked against the six dumps with
their own learned places: five fit (one flying frame is 0.31 off in height), the elevated rails' shadows beside them
do not.

### Beta 27: blue tree shadows - the learned place ran away

Beta 26 screenshots: big tree shadows in blue next to the character (hugging a tree, flying over a forest). The dumps
show the learned place gone wrong: running -0.84 / -0.62 (left of and above the body), flying -0.57 / -1.01 - from the
expected ~+0.2 / +0.05 and ~+0.7 / +0.5. A feedback loop: a tree shadow got in (remembered region or "follows the
camera" with a tree next to the body), was learned from, the place moved towards trees, the next tree shadow fit better.
The same drift caused drops (dump 4: the real shadow, 1.28 x 0.43 body sizes at +0.09 / +0.07, no longer fit).
Fixes, none of which learning can move:
- every candidate, whatever ranks it (remembered region, per-frame place, camera, continuation), must be 0.6-2.0 body
  sizes wide and 0.25-1.1 high (all real ones so far: 1.1-1.7 x 0.43-0.92) and within 0.8 sideways / 0.5 vertically of
  the expected place;
- the learned places are clamped (running left edge -0.1..0.7, bottom -0.15..0.35; flying -0.1..1.4, 0.15..1.2; width
  0.9-1.7, height 0.35-0.9);
- texture regions are remembered, and per-frame places learned, only from picks that also fit the place.

### Beta 28: shadow drops - hold it, absolute window

Beta 27: "sometimes the blue shadow disappears, otherwise overall good tracking." The shadow-lost dumps (mostly flying):
the real shadow is in the frame - dump 7: same texture region and place as last frame's pick - but beta 27's "within
0.8 / 0.5 of the expected place" gate rejected it (vertical 0.51: the learned flying place said +0.26, the shadow sat
at +0.77; dump 1: flying place not learned yet (6 picks), so the running place was expected, 0.54 off). The flying
height varies too much for a window around a learned place. Now the gate is absolute: left edge -0.4..1.6, bottom
-0.3..1.4 body sizes (running to high flight), plus the size gate from beta 27 (which is what keeps tree shadows out).

And a hold: the last picked shadow (its sprite, texture view AddRef'd) is drawn for up to 3 frames when none is found,
moved by how far the body box moved. Released when the character is lost (it must not reappear later at a stale
offset), replaced by every new pick, released at shutdown.

### Beta 29: step 2 - the mask goes to frame generation

Beta 28: "looked really good" (both dumps: no world drawn at all - menu / scene change). Step 1 done.

1.1 already had the hook: the motion pass guessed a "hold" mask (pixels in the still zone near the centre that
match "no motion" better than the camera, voted over frames), and everything downstream uses it: zero motion there,
the UI layer takes held pixels from the newest real frame (FSR overlay, DLSS-G UI recomposition), "paint over"
replaces them by the world snapshots. Values: 1 character on the ground, 0.5 flying (painted from the objects
snapshot), 0.8 shadow (not painted). Step 2 replaces the guess with the exact mask:
- sprite_mask: B = flying (the body's / glow's air stage), so the hold values can be given exactly;
- bridge: one more shared texture (always R8G8B8A8_UNORM, optional), the mask copied in at present next to the
  HUD-less copy (sprite_mask::frame_end runs before bridge_frame); motion::layer_set::character;
- motion: t7 char_mask (TABLE_SIZE 32, CHAR_SLOT 31, a fourth descriptor range; a null descriptor when unused);
  flag 65536. With it the still-zone guessing is skipped entirely. Per full-res pixel: character -> zero motion,
  depth near (flying 0.1 / FSR 0.2, ground objects DEPTH_NEAR / FSR 0.5), hold 1 or 0.5; painted over (flag 32768
  with layers) -> the camera and the ground's depth as before; shadow -> zero motion, hold 0.8, depth unchanged;
  anything else that the blocks set to "no motion" while the camera moves -> the camera (only the character stays put,
  blocks mostly covering it must not freeze the ground next to it).
- settings: "Character in generated frames: laid over / generated" (the old "Keep the character sharp", same key
  FsrHoldCharacter), "Use it for frame generation" (SpriteMaskForFrameGeneration, on); status line in the mask
  section says whether the last frame used it.
Without the mask (option off, mask disabled, not valid) everything is as in 1.1. The shader source changed, so the
cache recompiles once. Verified: all 17 entry points compile with dxc (cs_5_1, the fxc grammar lint passes).


### Beta 30: the shadow as a darkening

Beta 29: "Super impressive improvement, 4x on the player is near perfect only rare slight halo. But on 4x player shadow
gets noisy (pixelation and dragging), 2x not that noticeable." The four dumps are benign: one flying frame without the
colour layer (body and glow there), three with no world target drawn (scene changes, logged as scene cuts).

Why the shadow: a held pixel (0.8) is laid over from the newest real frame - shadow *and the ground under it*. While
running, that ground stands still for all generated frames (three at 4x) and then jumps; around the hold's hard edge
the generated ground next to it is already elsewhere. A shadow is not an object but a darkening of the ground, so no
single motion vector fits it: the ground under it moves with the camera, the darkening stays with the character.

Fix: split it. Factorio blends its shadow target into the world in the floor stage, so a shadow pixel is ground x
(1 - k x coverage), with the coverage our mask's G (the shadow sprite's alpha) and k a constant.
- motion, measuring k: in cs_expand, for pixels where the camera moved the ground across the shadow's edge (q now
  clear of the shadow and q + v_cam inside it last frame, or the other way; the shadow stays put on screen, so this
  frame's mask stands for last frame's): ratio of the same ground point's luma inside / outside. Only with flat
  coverage inside (3 x 3 at 2 px: range <= 0.08), none outside, no character near either end, ground or floor layer at
  both, no zoom, a good camera match, >= 1.5 px of motion. Each sample (1 - ratio) / coverage goes into a 32-bin
  histogram weighted by the coverage; the 1-thread coarse pick pass of the next frame takes the weighted median (with
  >= 150 samples), averages it into k (plain mean over the first 10 frames, then 10 % per frame) and clears the
  histogram. stats[] grew to 64 uints: [8] k, [9] frames measured, [10] samples last frame, [11] count, [16..47] bins.
  Valid after 3 frames; read back for the overlay.
- motion, writing (flag 131072, exact mask, UI layer usable): shadow pixels (G > 0.004, not the character) on ground
  or floor get hold = k x G / 4 (below 0.25, so nothing treats them as held), and the camera's motion where the blocks
  said "still" (own motion is kept: belt items). Shadow on objects (drawn over it): what the blocks decided. Before k
  is known: held as in beta 29.
- ui_layer: hold values below 0.25 are a darkening a = 4 x value: the UI layer gets black at alpha a (DLSS-G
  recomposes it, FSR's swap chain composes it - both blend, black is the same premultiplied or not), and the clean
  frame gets the HUD-less pixel / (1 - a) (written whenever the shadow may be laid over: the shadow taken out; after the
  paint-over if both). FSR's real frames are composed the same way, so they come out as Factorio drew them.
  The hold now always reaches the UI layer; "generated" mode ignores the character's values (>= 0.25) there, so the
  shadow is laid over in both modes. Painting over still needs the character laid over.
- Halo: the character hold from 2 % mask coverage (was 20 %), grown by 1 px instead of 2 with the exact mask.
- fg::ui_layer_usable(): the shadow goes to the camera only if DLSS-G did not refuse UI recomposition.
- Setting "Lay its shadow over as a darkening" (ShadowOverlay, on) and a status line with the measured darkness.

Beta 30 result: "DLSS-G 4x impressive. Some shadow artifacts still (very minor). FSR: works great, no issues, almost
like no FG." Steps 2 and 3 done: the character stays in frame generation with exact inputs on both technologies; the
step 4 fallback (splitting the draws) is not needed.

### Beta 31: shadow classes, held shadows, readout

Beta 30 follow-up: the remaining DLSS-G 4x shadow artifacts are "transition between flying and walking over some
entities", "slight flicker and quick recovery", barely visible at 2x. The darkness readout was not seen (it was only in
the ReShade window).

Two suspects:
- Held shadows. When the mask misses the shadow (most often taking off and landing), sprite_mask draws last frame's
  shadow moved with the body for up to 3 frames - but in those transitions the shadow moves relative to the body, so the
  held one is off. Laid over as a darkening, that brightens the clean frame where the shadow is not (and leaves the
  real one in it, moving with the ground) - a flash that recovers once the shadow is found again. Now the mask marks a
  held shadow (alpha = coverage / 4, so A < G; sel.y = 2 in the mask shader), and the motion pass holds it as in beta 29
  (0.8) for those frames instead of darkening, and never measures on it.
- Entities. The darkening was only laid over ground and floor, on the assumption that objects are drawn after the
  shadow blend. That holds for the walking shadow as far as we know, but whether the flying character's shadow
  darkens buildings under it is unknown (1.1's comment said it is not in the floor snapshot). So the measurement now has
  four classes: walking / flying (the mask's B is set on the shadow while the character flies) x ground+floor / objects
  (both ends of a sample in the same class; icons and flying things excluded). Each pixel uses its class's darkness:
  objects measured below 0.1 count as not darkened (what the blocks decided, as beta 30); objects not measured yet are
  held (0.8); ground falls back to the other ground class. stats[] has 160 uints now ([8..11] k, [12..15] frames,
  [16..19] samples, [20..23] counts, [32..159] four histograms); a class needs 60 samples in a frame.
- On-screen readout (Ctrl+F11): "Mask: character found (flying) | shadow found / HELD / MISSING | last 10 s: shadow
  held n, missing n, character lost n frames" and "Shadow: laid over | darkness on ground: walking 0.xx, flying 0.xx |
  on objects: walking 0.xx, flying ?(samples)".

### Beta 32: stutters, Steam overlay, multiplayer

Long play on beta 30 (multiplayer, early deathworld, a car): "yellow belts that go downwards sometimes stick and blur
and are hard to shake off, usually one lane and empty"; rare weird stutters "similar to the sprite mask at max zoom
out"; the Steam overlay "seems to open but is simply not visible". The 18 dumps: map / remote view frames, the car
(the character's colour unseen for 200-500 frames, the lock released), and another player's character (green
7FFF06FF, ours 7F0000DD) picked off-centre while the camera stood still - after the lock was released in the car, a
player standing near the centre could be learned (the colour layer only had to be within one body size of it).

- Multiplayer: the colour layer must lie over the screen centre sideways (0.35 body sizes), at most 0.35 above it,
  or up to 1.2 below it (raised in flight); while a colour is being learned (20 frames) it must stay at the same place
  relative to the centre (0.6 body sizes), or learning starts over. Our character does; a player walking past does not.
- Stutter monitor (stutter.cpp): per Factorio present the interval since the last one, and the add-on's work in it:
  our present handler at the last present (mask at present, captures, the bridge with its cut-check wait) and this
  frame's draw-time work (sprite_mask times its recorded draws and every vertex copy, counts its own maps). A frame
  over 2x the median of the last 120 (and 10 ms over it) is a stutter, attributed "mostly the add-on" (>= half of the
  extra time), "partly" (>= 15 %) or "outside the add-on". Logged with the breakdown (ReShade.log, at most 500) and to
  hudless_probe\stutters.txt (buffered, written about once a second between stutters; renamed to
  stutters_previous.txt at the next start, so a long session survives the restart that overwrites ReShade.log).
  Readout: stutters in the last 60 s / total, the last one, the add-on's time per frame (last, worst in 60 s).
  bridge::timings gained the last frame's own cut wait and present time (the others are smoothed).
- Steam overlay (steam_overlay.cpp): Steam draws into Factorio's frames, under the bridge window. GameOverlayActivated
  (callback 331) is registered through steam_api64.dll's SteamAPI_RegisterCallback with a hand-made CCallbackBase
  (both Run slots read only the parameter, so the MSVC overload order does not matter). If Steam never sends it (a game
  may dispatch callbacks manually), Shift+Tab is watched while Factorio is in front (Esc or Shift+Tab again closes).
  While open, the bridge window is hidden like for the ReShade overlay (one visibility function for both).
- The belts: not changed yet - waiting for a motion debug view picture of a stuck belt (whether it gets own motion,
  which way, and on which layer).

Beta 32: "Steam overlay, good." Plus the stuck belts in the motion debug view (and the world layers view): the empty
belts (chevron surface) are tinted green over long stretches with magenta patches - own motion, and in some blocks
the opposite direction; the full belts are untinted (camera). Layers view: belts are floor (as are grass and
decoratives). So the empty belt's scrolling surface is the cause: a repeating pattern, where shifts by one repeat
match as well as the real scroll; with the temporal candidate a block keeps whichever it found.

### Beta 33: ambiguous own motion on the floor

cs_block, after the camera / still decisions: a block with own motion, at least 24 of its 64 half-res pixels floor
and at most 8 object or higher, is tested along the motion's dominant axis: relative shifts -6..+6 half-res px from
the camera (skipping the chosen one and its neighbours); if any matches within 1.3 x its SAD (+0.05), the motion is
ambiguous and the block takes the camera. Repeating floor (belt surfaces, compressed lanes) moves with the world and
cross-fades; a real mover matches only at its own shift and keeps it. Cost: up to 12 block SADs, only for own-motion
floor blocks.

### Beta 34: the cut-check wait; floor follows the camera

Beta 33 sessions with the stutter log (two sessions, ~50 stutters, many while taking debug-view screenshots):
- "MOSTLY THE ADD-ON" stutters of 30-65 ms: all the scene-cut wait (cut wait 16-63 ms). The bridge submitted the first
  part of the motion passes and waited (up to 100 ms) for it to reach the cut metric; the GPU runs in order, so that
  wait includes whatever is still queued - frame generation's 3 frames and its pacing. On average 3.5-4 ms per frame
  (readout), with spikes. Also a hazard: on a timeout the shared event could be signalled later and release the
  allocator-fence wait early. Now: a dedicated event, reset before use, at most 2 ms, completion checked on the fence
  itself; not done -> judged before the next frame's motion pass (motion::current_difference still reads the pending
  frame then), dropped if even then not done. A cut found late still invalidates the motion history and resets
  frame generation before the next frame; only the frames generated across the cut are lost. Readout/timings count
  late checks and late cuts.
- 220-230 ms "MOSTLY THE ADD-ON" with a small cut wait: DLSS-G reconfiguring (on/off) - the debug view toggles for the
  screenshots (DLSS-G off / 4x in the log right before). Not in normal play.
- 381 ms: the motion shaders finished compiling (first start). 125-553 ms "outside the add-on" at scene cuts / map.
- The rest (35-50 ms with the add-on at 3-6 ms, of which the cut wait 2-4 ms): outside the add-on.
- Mask while drawing: up to ~1.3 ms, own maps = draws (the ring-full rule copies at every draw on these maps). Left
  for now (not what stutters).

Screenshots (motion debug view): belts still tinted - one solid magenta belt next to a solid green one (opposite
directions, possibly right), and green belts with magenta patches next to opposite-running belts (the per-pixel
stage picks a neighbouring block's vector where the repeating surface matches it locally). Beta 33's block test does
not see those, nor repeats longer than its +-6 half-res px. Layers view: grass and decoratives are "floor" (drawn after
the ground snapshot point) and flicker between floor and ground frame to frame.

Floor now follows the camera (flag 262144, "FloorFollowsCamera", on): in cs_expand floor pixels (layer 1/3) take
the camera like bare ground, and floor depth = ground depth (both DLSS-G and FSR scales). Floor things that move on
their own: only belt items (1-2 px per frame) - they cross-fade. Trains, cars, inserters, bots, biters are objects or
air; the character has its exact mask; its shadow overlay already gave its floor pixels the camera.

### Beta 35: certain belt motion, cut threshold, bridge breakdown

Beta 34: "slight belt jitter with items, usually from running diagonally". Stutter log: the cut wait now 1-2.4 ms;
the add-on stutters left are the bridge taking 30-58 ms with the cut wait at ~2 ms (so the time is elsewhere: the
command-allocator wait for the GPU, or Present inside DLSS-G with its pacing - right after a long Factorio frame),
and 222 ms once when DLSS-G was switched back on after the debug view. The log also shows ~25 scene cuts in 6
minutes at differences of 0.04-0.09 with averages near 0 (zooming, explosions on a deathworld, fast turns): each one
reset DLSS-G. Real cuts (map, remote view) are 0.2+. Dumps: map / remote view frames and flying at 4x zoom-in
(the shadow 225 px wide, held, then lost) - nothing new.

- Floor (flag 262144): instead of always the camera, a floor pixel keeps its own block's motion when the block is own
  motion (kind 2) within 6 half-res px of the camera and the pixel's 3x3 detail SAD at it is under 0.7x the camera's
  (-0.01). Never a neighbouring block's vector. Belt items and the belt surface move together, so where cs_block
  found the belt's motion unambiguous (beta 33's repeat test) both slide; where not, both take the camera.
- scene_cut_min_difference 0.04 -> 0.08.
- bridge timings: last_alloc_wait_ms (GetCurrentBackBufferIndex's allocator fence wait) and last_present_call_ms
  (fg::present_start .. present_end around Present); in the stutter line as "GPU queue wait" and "Present".

### Beta 36: icons, documentation

Beta 35: "seems fine except running against the belt flow diagonally at higher speeds makes blue belts with sparse
green circuits visibly artifact." Stutter log: apart from the shader compile at start (404 ms), only three add-on
stutters, all inside DLSS-G's Present (27-44 ms) right after a long Factorio frame (its pacing catching up) - the
cut wait and the allocator wait are gone from the picture. Fast belts against the run: sparse items in blocks that
are mostly belt surface and ground, so the block rarely has the items' motion - this needs per-item motion from the
sprites (planned for 1.3, like the character).

Alt-mode module blur while running sideways: cs_expand gave an icon pixel the camera only when the camera matched it
at least as well as its block's motion (and let it take a neighbouring block's vector otherwise); a row of identical
module icons matches a shift by one icon about as well. Icon pixels now always take the camera (icons sit on
buildings). Laying icons over from the real frame like the character would not work: they move with the world, so
they would step at Factorio's frame rate in generated frames.

README rewritten for players (why, what, install, first start, short troubleshooting); the technical part and the
full 1.2 settings reference in docs/HOW_IT_WORKS.md, packaged in the zip's docs\. RELEASE_NOTES.md drafted for 1.2.0.
ADDON_VERSION_FULL (file properties) follows the stage again (it said beta 1).

## 1.2.0 release

Beta 36 log: the add-on's only stutters are the start (loading, shader compile) and three inside DLSS-G's Present
(40-60 ms) right after a long Factorio frame (scene cut / map) - its pacing catching up; nothing of the add-on's own
waiting is left. README check by a reader: "a 280 Hz monitor" was wrong - 4x of 60 is 240 fps at most (DLSS-G), 2x
120 (FSR); fixed, with what Auto picks per refresh rate.

Release: version stage "release" (shown as plain 1.2.0); the mask section is called *Character tracking*; its dump
button, the automatic sprite_mask_N.txt dumps and the sprite probe only with *Show developer tools*; the stutter log
stays on (small, and the README points to it). CHANGELOG has a 1.2.0 release row above the betas; RELEASE_NOTES.md is
the GitHub release text. Built both variants (MinGW check; the user builds with MSVC).
