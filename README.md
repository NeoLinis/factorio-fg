# Factorio frame generation (DLSS-G and FSR)

A ReShade add-on that adds frame generation to Factorio 2.x: generated frames between Factorio's own 60, up to your
display's refresh rate. It works with either of two technologies:

| | NVIDIA DLSS Frame Generation | AMD FSR Frame Generation |
|---|---|---|
| GPUs | NVIDIA RTX 40/50; RTX 20/30 through dlssg_for_sm86 | Any DirectX 12 GPU: AMD Radeon RX 5000 and newer, NVIDIA GTX 10 and newer, Intel Arc |
| Multiplier | 2x, 3x, 4x | 2x |
| Extra files | NVIDIA Streamline (download), dlssg_for_sm86 on RTX 20/30 | Included in the release zip |

The GUI stays sharp, and the motion between frames comes from the add-on's own analysis of the picture, since
Factorio does not provide motion vectors or depth. On an NVIDIA RTX card you can switch between the two in the
settings.

**Download:** the latest `factorio-fg-x.y.z.zip` from this repository's *Releases* page; then follow *Install* below.
Version history: `CHANGELOG.md`. How it was built, step by step: `DEVLOG.md`.

Not affiliated with or endorsed by Wube Software (Factorio), NVIDIA or AMD.

## Requirements

| What | Notes |
|---|---|
| A DirectX 12 GPU | For DLSS-G: NVIDIA RTX. For FSR: any (Shader Model 6.2). Tested on an RTX 3080. |
| Windows 10/11, hardware-accelerated GPU scheduling **on** | Settings > System > Display > Graphics. Reboot after changing it. Required by DLSS-G, recommended for FSR. |
| Factorio 2.x on DirectX 11 | The default on Windows. Do not use `--force-opengl`. |
| ReShade 6.x **with full add-on support** | Installed for `bin\x64\factorio.exe`, API DirectX 10/11/12. No effects needed. |
| Driver-level frame generation **off** for Factorio | NVIDIA Smooth Motion or AMD Fluid Motion Frames would add their own frames on top. |

## Install

Everything goes into Factorio's `bin\x64` folder, next to `factorio.exe`. For Steam: right-click Factorio >
*Manage* > *Browse local files*, then open `bin\x64`. Close Factorio before you start.

**1. ReShade.** Download ReShade from reshade.me, the version **with full add-on support** (the plain version cannot
load add-ons). Run the installer, pick `bin\x64\factorio.exe`, choose **DirectX 10/11/12**, and skip the effect
packages: none are needed. This puts `dxgi.dll` and `ReShade.ini` into `bin\x64`.

**2. This add-on.** From the release zip, copy into `bin\x64`:
- `factorio_fg.addon64`
- `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` (AMD FSR Frame Generation)

If an older developer build (`hudless_probe.addon64`) is there, delete it: only one of the two may be present.

That is all FSR needs. For DLSS-G on an NVIDIA RTX card, also:

**3. NVIDIA Streamline (DLSS-G).** Download the Streamline SDK **2.14.16** release zip from
github.com/NVIDIA-RTX/Streamline/releases. In the zip, open `bin\x64` (not `bin\x64\development`). Create a folder `fg`
inside Factorio's `bin\x64` and copy these six files into it:
`sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll`.

**4. RTX 20/30 only: dlssg_for_sm86.** From its release, copy `version.dll`, `dlssg_sm86.ini` and the folder
`dlssg_sm86` into `bin\x64`. RTX 40/50 cards skip this step.

**5. Windows and driver settings.**
- Hardware-accelerated GPU scheduling **on**: Settings > System > Display > Graphics > *Change default graphics
  settings*. Reboot after changing it. DLSS-G does not run without it.
- Driver frame generation **off** for Factorio: NVIDIA Smooth Motion (NVIDIA app > Graphics > Factorio) or AMD Fluid
  Motion Frames (AMD Software > Gaming > Factorio).

When you are done, `bin\x64` contains:

```
factorio.exe
dxgi.dll                                 ReShade
ReShade.ini
factorio_fg.addon64                      this add-on
amd_fidelityfx_loader_dx12.dll           FSR (from the release zip)
amd_fidelityfx_framegeneration_dx12.dll  FSR
fg\                                      DLSS-G only: the six Streamline DLLs (step 3)
version.dll                              DLSS-G on RTX 20/30 only (step 4)
dlssg_sm86.ini                           DLSS-G on RTX 20/30 only
dlssg_sm86\                              DLSS-G on RTX 20/30 only
```

**Updating** the add-on: replace `factorio_fg.addon64` and the two AMD DLLs. The first start afterwards recompiles the
motion shaders (see below). Settings are kept.

**Removing** it: delete `factorio_fg.addon64`, the two `amd_fidelityfx_*.dll` and `factorio_fg_shaders.cache`. To
remove everything, also delete the `fg` folder, the dlssg_for_sm86 files, and ReShade's `dxgi.dll`, `ReShade.ini`,
`ReShade2.ini` and `ReShade.log`.

Factorio updates do not need a new add-on as long as the game draws its frame the same way. If an update changes
the shaders the add-on looks for, it finds the snapshot points from the structure of the frame instead (see
*Motion vectors* below).

### Building it yourself

Needs "Build Tools for Visual Studio 2022" with **Desktop development with C++** (includes CMake and Ninja). All
other headers are in `third_party\`.

| Script | Makes |
|---|---|
| `build_release.bat` | `factorio_fg.addon64`: developer tools hidden |
| `build.bat` | `hudless_probe.addon64`: developer build (frame trace, capture rule editor, snapshot preview) |
| `package_release.bat` | the release build, zipped with this README, the changelog and the licences into `dist\` |

GitHub Actions (`.github/workflows/build.yml`) builds both on every push; pushing a tag `vX.Y.Z` that matches
`src\version.h` publishes a release with the zip and `RELEASE_NOTES.md`.

## First start

1. Start Factorio and load a save. Press **Home** to open ReShade, then go to the add-on's tab (*Factorio Frame
   Generation*).
2. Tab *Frame generation*: open **Setup check**. Everything should be green; anything else comes with the fix.
3. Tick **Frame generation on**. *Technology* defaults to **Automatic**: DLSS-G on an NVIDIA RTX card when it is
   installed, FSR otherwise. The multiplier defaults to **Auto**, which picks the largest multiplier that does not go
   past your display's refresh rate (FSR: 2x).
4. Close the overlay to see the result: the generated picture is hidden while the ReShade overlay is open.

The first start after installing or updating compiles the motion shaders. An orange note in the top left says so
and counts up; it takes up to a minute, and frame generation runs without motion vectors until then. The result is
cached in `bin\x64\factorio_fg_shaders.cache`, so later starts load at once.

**Ctrl+F11** (by default) shows a small readout in the top left: multiplier and frame rates, camera motion, and
timings.

## Settings

Saved in `bin\x64\ReShade.ini`, section `[FACTORIO_HUDLESS_PROBE]`.

### Frame generation tab

| Setting | What it does |
|---|---|
| Frame generation on | Presents the game through the add-on's D3D12 window with frame generation. Off: Factorio as usual. Can be switched at any time. |
| Technology | Automatic, NVIDIA DLSS-G or AMD FSR. *In use* shows which one runs. A change applies at the next start of Factorio. If the chosen one cannot start, the other is used. |
| Multiplier | Off (bridge only), 2x, 3x, 4x, or Auto. *Auto target fps* sets the aim for Auto (0 = the display's refresh rate). FSR generates one frame per Factorio frame, so 2x at most. |
| Keep the character sharp | DLSS-G and FSR. On by default. While you move, generated frames show the character from the newest Factorio frame (like the GUI) instead of a blend of two animation poses. Settings the running technology cannot use (3x/4x with FSR, FSR options with DLSS-G) are greyed out. |
| ...and remove it from what frame generation blends | Off by default (an option to compare). Frame generation gets the ground under the character (from the floor snapshot) instead of the character, so it cannot drag a copy of it into the ground in front; the sharp character goes on top as before. |
| Scene-cut detection | Tells frame generation to start over when the picture changes completely (map view, surface change, teleport). Without it those moments smear for a few frames. Costs a short wait each frame (see *cut check wait* on the readout). |
| Tearing / VRR present | Present with sync interval 0. Leave on with G-Sync / FreeSync. |
| On-screen readout | The readout, and its hotkey. |
| Setup check | Checks HAGS, DLSS-G (Streamline, dlssg_for_sm86), FSR (AMD's DLLs), the shader compiler and duplicate add-ons. One of DLSS-G and FSR is enough. |

### Motion vectors tab

| Setting | What it does |
|---|---|
| Motion vectors | Off: frame generation works from its own optical flow only (for comparison). |
| World layers | Uses snapshots of Factorio's world picture at fixed points of its draw order (see below). Strongly recommended. The status next to it says whether all six were taken this frame, and the line below it which snapshot points are in use: the built-in ones, or - if a Factorio update changed those shaders - the ones found from the structure of the frame. |
| Air split | Separates alt-mode icons (camera motion) from flying robots (motion search) within the air layer. The default was set on Nauvis; the section explains how to check and change it. |
| Mover search | Searches blocks that the camera motion does not explain for their own motion: trains, cars, bots, the character. |
| Wide search for fast movers | Extends that search from 48 to 192 px per frame where nothing was found: trains with speed upgrades. |
| Fine tuning | The block decision thresholds. *Reset to defaults* restores them. |
| Debug views | Replace the game picture: *Motion kinds* (tint = motion relative to the camera), *World layers / depth* (dark ground, blue floor, white objects, cyan alt-mode icons, yellow flying), and the raw snapshots. With FSR: *FSR debug view*, AMD's own (only generated frames, with its internal pictures). |

## How it works

Factorio renders with DirectX 11; DLSS-G and FSR need DirectX 12. The add-on copies each finished Factorio frame into
a D3D12 swap chain of its own, a window laid over the game window, and presents it there: through NVIDIA Streamline
for DLSS-G, or through AMD's frame generation swap chain for FSR, which inserts the generated frame and paces
presentation itself.

| Input frame generation needs | Where it comes from |
|---|---|
| The frame | Factorio's back buffer, copied at present. |
| The frame without GUI ("HUD-less") | A copy taken right before Factorio draws its GUI, found by the pixel shader of the post pass that precedes it. |
| Motion vectors | Estimated from the HUD-less picture (below). |
| Depth | From the world layers: ground far, floor, objects, alt-mode icons, flying things nearest. |

**Motion estimation**, on the GPU each frame (about 1-1.5 ms on an RTX 3080 at 1440p):

1. *Camera*: a coarse-to-fine search for pan and zoom over the whole picture, down to a sub-pixel fit.
2. *World layers*: six snapshots of Factorio's world target as the frame is drawn: after the tiles (ground), after
   the floor stage (belts, pipes, inserters, shadows), before the cloud pass (all objects), after it, before the
   first flying-robot draw, and the finished world. Where a pixel first appears says what it is. Bare ground gets
   exactly the camera motion, with no search.
3. *Blocks* of 16 x 16 px: the camera motion where it genuinely matches; otherwise a mover search (unique matches
   only, so repeating patterns are not mistaken for motion), "still" near the screen centre for the character the
   camera follows, and the previous frame's motion to follow movers steadily.
4. *Per pixel*: at block borders each pixel takes its own or a neighbouring block's motion, whichever matches;
   bare ground and alt-mode icons get the camera motion.

## Surfaces

| Where | World layers |
|---|---|
| Nauvis, Fulgora | All six snapshots. |
| Vulcanus | No cloud pass there: objects and flying things share one layer. |
| Gleba, Aquilo | All six snapshots (checked). |
| Map view, remote view | Remote view as on the surface. Map view has no world draw order: plain motion search. |
| Space platforms | Not used: the starfield moves on its own and must not be locked to the camera. Plain motion search. |

## Troubleshooting

| Symptom | Look at |
|---|---|
| Setup check shows problems | The fix is listed with each item. |
| Orange "motion shaders failed to compile" note | `bin\x64\ReShade.log`, lines `motion: compiling ... failed`. Please report them. |
| "Snapshots incomplete" in the Motion vectors tab | It names the missing snapshot. After a Factorio update the add-on switches to points found from the frame structure within a few seconds; if it stays incomplete, a frame trace (developer build) shows the new draw order. |
| Frame generation does not start | `ReShade.log`, lines starting with `[Factorio Frame Generation] fg:`, `fsr:` or `streamline:`; Streamline's log files in `bin\x64\fg\`; for RTX 20/30 `bin\x64\dlssg_sm86\logs\`. |
| FSR artifacts you want to report | *Motion vectors* tab, *Debug views*: *FSR debug view* shows AMD's own internal pictures; a screenshot of it helps. |
| FSR picture smears in the wrong direction when panning | *Motion vectors* tab: *Flip sign*, and tell us (the sign convention is checked on an RTX 3080 only). |
| Steam overlay fps counter missing | It draws on Factorio's own window, which the bridge window covers. Use the NVIDIA overlay (Alt+R), PresentMon, or `ShowFPS=1` under `[OVERLAY]` in `bin\x64\ReShade2.ini` (the bridge window's ReShade). |

## Known limits

- Module icons in machines (and similar rows of small identical icons) look soft when zoomed out on a pan. That is
  Factorio's own rendering - it happens without the add-on too - not frame generation.
- Very small movers far zoomed out (a single bot a few pixels wide) may not be found and get the camera motion.
- Generated frames add about one Factorio frame of latency (DLSS-G with Reflex; FSR similar), plus the scene-cut check.
- FSR: 2x only. On Radeon RX 9000 the FSR API picks its ML frame generation (FSR 4), on all other GPUs the analytical
  one (FSR 3.1); the ML version has not been tested with this add-on yet. The camera values FSR asks for are fixed
  (Factorio is a flat top-down view).
- The GUI, and the character while you move, are shown from the newest Factorio frame in generated frames (both
  technologies), so they stay sharp; their animation runs at Factorio's frame rate. The ground just uncovered behind
  a moving character can look noisy in generated frames: that is frame generation guessing what was hidden.
- Changing *Technology* takes effect at the next start of Factorio.

## Third-party

`third_party\` holds the ReShade add-on API headers, Dear ImGui headers, the Streamline headers and AMD's FSR API
headers, with their own licences. AMD's two FSR frame generation DLLs (MIT, see `AMD_FidelityFX_license.md`) are in
the release zip; the build downloads them from AMD's FSR SDK 2.3.0 release and checks their hashes. Streamline's
runtime DLLs and dlssg_for_sm86 are not included and come from their own releases.

## Licence

MIT, see `LICENSE`. The third-party files keep their own licences (BSD 3-Clause for ReShade, MIT for Dear ImGui,
Streamline and AMD's FSR headers and DLLs); see `THIRD_PARTY_NOTICES.md`.
