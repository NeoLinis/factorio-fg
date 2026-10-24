Smooth Factorio on high-refresh monitors: NVIDIA DLSS-G (2x-4x) or AMD FSR (2x) frame generation between Factorio's
own 60 frames, without touching the game's 60 UPS logic.

**New in 1.3.0**
- **No more "force field" around the character.** Zoomed out, a circle around the character made elevated rails, power
  lines and nearby buildings stand still in generated frames while the rest moved. Fixed.
- **The camera is read from Factorio's sprites.** The camera's movement now comes from the exact positions of the
  sprites Factorio draws, instead of being estimated from the picture.
- **Better character tracking.** Another player (or the main menu's demo character) walking past is no longer
  followed.
- **Clearer settings.** Both tabs are organised into sections; the main switches first, details folded away.
- **More in the readout (Ctrl+F11):** where the camera's movement came from, how well the motion explains each frame
  (per layer), and where the in-between frames land.
- **Known limit, explained:** rows of alt-mode icons can blur or pulse slightly at some zoom levels while moving
  sideways. Factorio moves its world in whole pixels, and DLSS-G / FSR draw the in-between frames between pixels. The
  next major version (1.4) is planned to bring a frame generator of our own that keeps every frame on whole pixels -
  see `docs/ROADMAP.md`.

**Install / update:** download `factorio-fg-1.3.0.zip` below and follow *Install* in the README. Updating: replace
`factorio_fg.addon64` and the two `amd_fidelityfx_*.dll`. The first start recompiles the shaders (up to a minute, shown
in the top left).

Full history: `CHANGELOG.md`. What's next: `docs/ROADMAP.md`.
