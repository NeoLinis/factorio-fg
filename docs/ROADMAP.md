# Roadmap

What comes next, and why in this order. Plans, not promises: each step is tested in betas first and can change with
what the tests show. Released versions: [`CHANGELOG.md`](../CHANGELOG.md).

| Version | Theme | In short |
|---|---|---|
| **1.3** | **Better information for frame generation** | Exact motion, depth and UI for everything on screen, not just the character - and a way to measure how good it is. |
| **1.4** | **Pixel-exact frame generation** | A frame generator of our own that moves Factorio's world by whole pixels: crisp in every frame, any graphics card, 2x-4x. |
| 1.5 | Mods and modpacks | Adapts to the scene by itself: modded characters and vehicles, space, weather, very dense bases. |
| later | AMD's multi-frame generation | 3x / 4x with AMD's own FSR as soon as AMD ships it. |
| later | Linux / Steam Deck | Depends on what 1.4-1.5 settle. |

## 1.3 - Better information for frame generation

DLSS-G and FSR are only as good as what they are told. Today the add-on knows the GUI and the character exactly; the
rest of the world's motion is still estimated from the picture. 1.3 makes the rest exact as well.

### 1. Measure first

Before changing the inputs, the add-on should be able to say how good they are:

- **Motion fit:** each frame, the previous frame moved by our motion vectors is compared with the new real frame, per
  layer (ground, floor, objects, flying, icons) and per screen area. A wrong vector shows up as a difference. Shown in
  the readout as a single number per layer, and as a heat map in the debug views.
- **Recordings (developer tool):** a few seconds of what frame generation is given (picture without GUI, motion,
  depth, UI layer, masks), saved to disk, so a problem can be looked at frame by frame and before/after changes can be
  compared on the same scene.

Every later step is judged by these numbers, not only by eye.

*Status: the motion fit line is in 1.3 beta 2; recordings to follow.*

### 2. Motion from sprites, for everything

The character method, generalised. Factorio's sprite draws (positions straight from its vertex data) are matched
between frames:

- **Moved with the camera:** buildings, belts, rails - the camera's motion.
- **Same place, new animation frame:** assemblers, inserters' bases, water, trees - the camera's motion; their
  animation is not mistaken for movement (the source of most module, assembler-grid and Py-style smear).
- **Moved on its own:** belt items, inserter hands, trains, cars, biters, bots, projectiles - their exact motion.

The camera itself comes from the sprites too: static buildings give the pan and zoom exactly (Factorio's own float
positions), instead of the sub-pixel fit on the picture. That also answers whether the alt-mode module rows that
DLSS-G blurs together on diagonal moves (FSR keeps them apart) come from our camera or from DLSS-G's own optical
flow.

*Status: the camera from the sprites is in 1.3 beta 2.*

The motion vectors are then drawn from the sprites themselves, pixel-exact. The picture-based estimate stays for
whatever is not a sprite (lights, full-screen effects) and as the fallback. Fixes the known limit of 1.2: sparse items
on fast belts when running against them.

The open question is cost: a dense base zoomed out is tens of thousands of sprites per frame, and the work must not
land on Factorio's render thread. A probe across zoom levels decides whether matching runs on the CPU (with a budget)
or on the GPU.

### 3. Depth from draw order

Factorio draws objects back to front. The order a sprite is drawn in becomes its depth within its layer, so frame
generation knows which of two overlapping things is in front: an inserter hand over a belt, a train over its rails, a
bot over a building, a tree over a wall. Today all objects share roughly one depth.

### 4. Shadows of everything that moves

1.2 handles the character's shadow. Bots, trains, cars, biters and tanks cast shadows that move with them but are
drawn on the ground; from the sprites, each shadow gets its owner's motion (or, for things the camera follows, is laid
over like the character's).

### 5. The UI layer, finer

- **Transparency:** the UI layer is on/off per pixel today, so the world under semi-transparent GUI (window edges,
  tooltips, chat) is held still from the real frame. It gets real alpha, so the world under it keeps moving.
- **Things that follow the mouse, not the world:** the building preview in hand, selection boxes, drag rectangles.
  They move with the cursor, so they get the cursor's motion (or are laid over).
- **Things that follow a player:** another player's name in multiplayer, the character's own light (below).

### 6. Night and lights

Factorio applies its lighting after the world is drawn. At night the character's light moves with the character while
the world slides under it - the same problem as the shadow, the other way round. The light map is read from
Factorio's lighting pass: the character's light stays with the character, lamps move with the world, and day/night
changes and flicker are not taken for motion.

### 7. Camera information

Both technologies are told "no camera" today (identity matrices); the camera's pan and zoom are only in the motion
vectors. They get the real (orthographic) camera instead - kept only if the measurements of step 1 show it helps.

### 8. Ready for FSR's multi-frame generation

AMD's interface already has room for several generated frames per real one, and the add-on already passes AMD's
requests straight through. When AMD's multi-frame generation ships (expected on Radeon RX 9000 first), the multiplier
goes above 2x for FSR as soon as it works. Nothing to build until then.

### Order

The steps are released as betas roughly in the order above: 1 first (everything else is judged by it), then 2-4 (the
sprite work, the biggest part), then 5-7, which are independent of each other. 8 whenever AMD ships.

## 1.4 - Pixel-exact frame generation

Found in 1.3: Factorio's frames are pixel-exact - the world moves by whole pixels each frame (running: 7, 7, ..., 7, 6
px). DLSS-G and FSR show the in-between frames at fixed fractions of that step (4x: 1/4, 1/2, 3/4), which lands
between pixels unless the step happens to be a multiple of 4. There they resample the picture, and sharp one-pixel
detail - alt-mode icons above all - goes soft in the generated frames and sharp again in the real ones. As the step
alternates (7, 6) and changes with zoom, that shows as blur, jitter and pulsing; most along the direction of motion,
where rows of icons sit side by side. The beta 9 readout confirmed it: the artifacts appear exactly when the in-between
positions switch between whole pixels and fractions. DLSS-G and FSR cannot avoid this (one motion field per frame,
fixed fractions), and nothing the add-on hands them changes it.

A generator of our own can: every in-between frame moves the world by a whole number of pixels (7 px at 4x: 2, 4, 5),
so it is a pixel copy of a real frame - exactly as sharp. Factorio suits this unusually well, because 1.3 already knows
how everything moved:

- **Most of the picture moves with the camera** (exact from the sprites): an integer shift of the previous or the next
  real frame, whichever shows that spot (what is uncovered at the screen edge or behind moving things comes from the
  other frame).
- **Things that move on their own** (belt items, trains, bots, biters): their own motion, also rounded to whole pixels;
  where they cover or uncover something, the world layers and depth decide what is in front.
- **The GUI, the character, its shadow:** laid over from the newest real frame, as now.
- **Pacing:** the add-on presents the in-between frames itself, evenly spaced between Factorio's frames, on its own
  swap chain (VRR / tearing as now). The latency stays as with DLSS-G: one Factorio frame.
- **Any multiplier, any graphics card:** 2x, 3x, 4x (and more for 360 Hz monitors) on anything with DirectX 12. DLSS-G
  and FSR stay available; the technology setting picks.

Steps: a prototype at 2x with the camera shift only (most of the screen), compared with DLSS-G on the same scene; then
moving things and uncovered areas; then 3x / 4x and pacing; only pursued past the prototype if it clearly beats DLSS-G
on sharpness without new artifacts.

## 1.5 - Mods and modpacks

Mods cannot change how Factorio draws - everything they add goes through the same engine stages as vanilla. So the
more the add-on works from Factorio's own drawing (1.3), the less it cares which mods are loaded. 1.4 adds what is
still specific:

- **Whatever the camera follows**, found generically (what stays put on screen while the world slides under it)
  instead of by the character's player-colour layer: modded characters, jetpacks, cars, tanks, spidertrons,
  spaceships.
- **Scene awareness:** a short description of the current scene from the frame's structure (surface, space, clouds,
  fog, zoom, sprite count, lighting) picks the strategy: space backgrounds with their own motion and far depth; a work
  budget for very dense scenes; full-screen weather and lighting not tied to the camera. The motion fit of 1.3 makes
  parts that keep failing more careful by themselves, instead of tuning per mod. The readout shows the scene and why.
- **Scene report** button: everything needed for a bug report in one file.
- **Tested with:** Space Age, Stellar Horizons (incl. its enhanced graphics), Space Exploration, Krastorio 2 (Spaced
  Out), Pyanodons - and a compatibility list in the README.

## Later

### AMD's multi-frame generation

Step 8 of 1.3: as soon as AMD ships it. As of October 2026 it is announced in AMD's SDK and hidden in its drivers (up
to 8x, Radeon RX 9000), but not working yet. (3x / 4x on any card comes with 1.4.)

### Linux / Steam Deck

The add-on depends on Windows pieces today: Factorio's DirectX 11 renderer, ReShade's DirectX hooks, NVIDIA's and
AMD's DirectX 12 frame generation. Under Proton some of these may work; the native Linux build would need its own
path. The frame generator of 1.4 is the most portable way there.
