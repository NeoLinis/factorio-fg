# How it works

The technical side of Factorio Frame Generation: what the add-on does each frame, every setting, and how to build it.
For installing and using it, the [README](../README.md) is enough.

## The idea in one picture

```
Factorio (DirectX 11, 60 fps)                         the add-on                              your screen
 draws the world ......................> snapshots of the world layers  \
 draws the character's sprites ........> exact character + shadow mask   >  motion vectors,  ->  DLSS-G or FSR  ->  up to 4x fps
 draws the GUI (after a copy without it) > picture without GUI          /   depth, GUI layer      (DirectX 12)
```

Factorio renders with DirectX 11; DLSS-G and FSR need DirectX 12. The add-on (a ReShade add-on) watches Factorio
draw each frame, copies the finished frame into a DirectX 12 window of its own laid over the game window, and
presents it there through NVIDIA Streamline (DLSS-G) or AMD's frame generation swap chain (FSR). Factorio's game
loop is never touched.

## What frame generation is given

| Input | Where it comes from |
|---|---|
| The frame | Factorio's back buffer, copied at present. |
| The frame without GUI | A copy taken right before Factorio draws its GUI (found by the post-processing pass before it). |
| Motion vectors | Estimated on the GPU from consecutive GUI-less frames (below), with the character from its sprites. |
| Depth | From the world layers: bare ground far, objects nearer, flying things nearest. |
| A UI layer | The GUI and the character from the newest real frame, plus the character's shadow, laid over every generated frame. |

## Motion estimation

On the GPU every frame (about 1.5-2.5 ms on an RTX 3080 at 1440p):

1. **Camera**: a coarse-to-fine search for pan and zoom over the whole picture, down to a sub-pixel fit.
2. **World layers**: six snapshots of Factorio's world picture as it is drawn - after the tiles (ground), after the
   floor stage (belts, rails, pipes, shadows), before the cloud pass (all objects), after it, before the first
   flying-robot draw, and the finished world. Where a pixel first appears says what it is.
3. **Blocks** of 16 x 16 px: the camera's motion where it genuinely matches; otherwise a search for the block's own
   motion (trains, cars, bots, biters) that only accepts unique matches, so repeating patterns are not mistaken for
   motion; and last frame's motion to follow movers steadily.
4. **Per pixel**: at block borders each pixel takes its own or a neighbouring block's motion, whichever matches.
   Bare ground and alt-mode icons always move with the camera. Belts and other floor keep their own motion only where
   it is certain (an empty belt's scrolling surface repeats and would otherwise pick the wrong way).

## The character (1.2)

Picture-only frame generation struggles most with the character: it stays in the middle of the screen while the
world slides under it, and its running animation changes every frame. 1.2 reads it from Factorio's own sprites:

- The add-on records the sprite draws of each frame (positions come straight from Factorio's vertex data) and finds
  the character by its player-colour layer, with its body, its flying glow and its shadow.
- The result is a pixel-exact **mask**. Frame generation gets zero motion and near depth there, and the UI layer
  lays the character over each generated frame from the newest real frame (or lets frame generation blend it, if
  you choose *generated*).
- Its **shadow** is a darkening of whatever is under it. The add-on measures how dark it is (where the camera moves
  the ground across the shadow's edge), removes it from the picture frame generation blends, and lays it back over
  every frame as a darkening: the ground under it moves with the world, the shadow stays with the character.
- In multiplayer, other players are not taken for you: your character is the one where the camera is centred.

## Scene cuts, stutters, overlays

- **Scene cuts** (map view, remote view, teleport) tell frame generation to start over instead of blending two
  unrelated pictures. The check waits at most 2 ms for the GPU; when the GPU is busier it is answered a frame later.
- **Stutter monitor**: every frame that takes over twice the usual time is logged with what took the time - the
  add-on's parts versus everything outside it - in `ReShade.log` and `bin\x64\hudless_probe\stutters.txt` (the
  previous session's stays as `stutters_previous.txt`). The readout counts them.
- **Overlays** drawn into Factorio's own picture (ReShade's, Steam's) would be hidden under the add-on's window; the
  window steps aside while they are open.

## Settings

Saved in `bin\x64\ReShade.ini`, section `[FACTORIO_HUDLESS_PROBE]`. The defaults are the recommended values.

### Frame generation tab

| Setting | What it does |
|---|---|
| Frame generation on | Presents the game through the add-on's window with frame generation. Off: Factorio as usual. |
| Technology | Automatic (DLSS-G on NVIDIA RTX when installed, else FSR), DLSS-G or FSR. Applies at the next start. |
| Multiplier | Off, 2x, 3x, 4x or Auto (the largest that stays within the monitor's refresh rate; *Auto target fps* overrides it). FSR: 2x at most. |
| Character in generated frames | *Laid over* (default): from the newest real frame, sharp. *Generated*: blended by frame generation with its exact motion. |
| ...and remove it from what frame generation blends | Off by default, for comparison: frame generation gets the ground under the character instead. |
| Scene-cut detection | Starts frame generation over on map view, surface change, teleport. |
| Tearing / VRR present | Leave on with G-Sync / FreeSync. |
| On-screen readout | The readout (Ctrl+F11 by default) and its hotkey. |
| Hide frame generation while the Steam overlay is open | So the overlay can be seen; *go by Shift+Tab* if Steam does not tell the add-on. |
| Setup check | Checks GPU scheduling, DLSS-G files, FSR files, the shader compiler and duplicate add-ons. |

### Motion vectors tab

| Setting | What it does |
|---|---|
| Character tracking | Builds the mask from Factorio's sprites (*Show it* draws it in red/blue), uses it for frame generation, and lays the shadow over as a darkening. The status lines show what was found and the measured shadow darkness. |
| Motion vectors | Off: frame generation uses only its own optical flow (for comparison). |
| World layers | Uses the six snapshots (strongly recommended). After a Factorio update that changes its shaders, the snapshot points are found from the frame's structure instead. |
| Alt-mode icons are drawn before the split | Separates alt-mode icons from flying robots in the air layer. |
| Mover search / Wide search for fast movers | Finds things that move on their own; the wide search reaches fast trains (192 px per frame). |
| Belts and other floor: own motion only where it is certain | On by default. Off: belts are treated like everything else (as before 1.2). |
| Fine tuning | The block decision thresholds; *Reset to defaults* restores them. |
| Debug views | Replace the picture (frame generation pauses): *Motion kinds* (tint = own motion, hue = direction), *World layers / depth* (dark ground, blue floor, white objects, cyan icons, yellow flying), the raw snapshots, and FSR's own debug view. |

## Surfaces

| Where | World layers |
|---|---|
| Nauvis, Fulgora, Gleba, Aquilo | All six snapshots. |
| Vulcanus | No cloud pass: objects and flying things share one layer. |
| Remote view | As on the surface. |
| Map view | No world drawing: plain motion search. |
| Space platforms | The starfield moves on its own: plain motion search. |

## Known limits

- Generated frames add about one Factorio frame of latency (DLSS-G with Reflex; FSR similar).
- The ground just uncovered behind a fast-moving character is guessed by frame generation and can look noisy.
- Items on belts take the belt's motion where it is unambiguous; running fast against a busy belt can still show
  artifacts (tracking each item from its sprite is planned).
- Module icons can look soft zoomed out on a pan; check with frame generation off - Factorio's own rendering does
  this too.
- FSR is 2x only. FSR 4 (Radeon RX 9000) has not been tested.

## Troubleshooting details

| Symptom | Look at |
|---|---|
| Orange "motion shaders failed to compile" note | `ReShade.log`, lines `motion: compiling ... failed`. |
| "Snapshots incomplete" in the Motion vectors tab | Names the missing snapshot; after a Factorio update the add-on switches to points found from the frame structure. |
| Frame generation does not start | `ReShade.log` (`fg:`, `fsr:`, `streamline:` lines), Streamline's logs in `bin\x64\fg\`, for RTX 20/30 `bin\x64\dlssg_sm86\logs\`. |
| Character not tracked | The *Character tracking* status. With *Show developer tools* on, `hudless_probe\sprite_mask_N.txt` is written whenever it is lost - please attach those. |
| FSR smears the wrong way when panning | *Motion vectors* tab: *Flip sign*, and please report it. |

## Building it yourself

Needs "Build Tools for Visual Studio 2022" with **Desktop development with C++** (includes CMake and Ninja). All
other headers are in `third_party\`.

| Script | Makes |
|---|---|
| `build_release.bat` | `factorio_fg.addon64`, developer tools hidden |
| `build.bat` | `hudless_probe.addon64`, the developer build (frame trace, capture rule editor, snapshot preview, sprite probe) |
| `package_release.bat` | the release build, zipped with the docs and licences into `dist\` |

GitHub Actions (`.github/workflows/build.yml`) builds both on every push; pushing a tag `vX.Y.Z` that matches
`src\version.h` publishes a release with the zip and `RELEASE_NOTES.md`.

AMD's two FSR DLLs are downloaded by the build from AMD's FSR SDK 2.3.0 release and checked by hash. Streamline's DLLs
and dlssg_for_sm86 are not included and come from their own releases.
