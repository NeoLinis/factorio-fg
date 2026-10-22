Smooth Factorio on high-refresh monitors: NVIDIA DLSS-G (2x-4x) or AMD FSR (2x) frame generation between Factorio's
own 60 frames, without touching the game's 60 UPS logic.

**New in 1.2.0**
- **Your character is tracked exactly.** The add-on now reads the character from the sprites Factorio draws, instead of
  guessing it from the picture. Walking, running and flying in mech armour look clean in generated frames, and other
  players in multiplayer are not mistaken for you. Choose *laid over* (sharpest) or *generated* (smoothest).
- **Its shadow too:** the shadow stays with the character while the ground moves under it - no more dragging or
  noise around it at 4x.
- **Belts:** empty belts no longer stick or blur, belts running side by side in opposite directions no longer mix, and
  items slide where the belt's motion is clear.
- **Alt-mode icons** move with the world, so rows of module icons no longer smear while you run along them.
- **Fewer stutters:** the add-on no longer makes Factorio wait for the graphics card every frame, and false scene
  cuts (zooming, explosions) no longer reset frame generation.
- **Steam overlay works:** frame generation steps aside while it is open.
- **Readout (Ctrl+F11):** now also shows the character tracking, the shadow and a stutter counter; each stutter is
  logged with what caused it.
- New README: what the add-on is for and how to install it, in short; the technical details moved to
  `docs/HOW_IT_WORKS.md`.

**Install / update:** download `factorio-fg-1.2.0.zip` below and follow *Install* in the README. Updating from 1.1:
replace `factorio_fg.addon64` and the two `amd_fidelityfx_*.dll`. The first start recompiles the shaders (up to a
minute, shown in the top left).

**Known limits:** FSR is 2x only; ground uncovered right behind a fast-moving character, and items on fast belts while
you run against them, can show small artifacts; FSR 4 (RX 9000) is untested.

Full history: `CHANGELOG.md`.
