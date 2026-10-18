DLSS Frame Generation (2x-4x) for Factorio 2.x through ReShade, on RTX 40/50 and, with dlssg_for_sm86, RTX 20/30.

**First release.** Highlights:
- Generated frames with the GUI kept sharp (HUD-less capture right before Factorio draws its GUI).
- Motion vectors estimated from the picture: camera pan and zoom, plus trains, cars, bots and the character.
- World layers from Factorio's draw order (ground, floor, objects, alt-mode icons, flying robots) for exact camera
  motion on bare ground and depth for DLSS-G.
- Auto multiplier up to the display's refresh rate, scene-cut detection, setup check, on-screen readout (Ctrl+F11).
- About 1-1.5 ms of GPU time per Factorio frame on an RTX 3080 at 1440p.

**Install:** download `factorio-fg-1.0.0.zip` below, then follow *Install* in the README: ReShade with full add-on
support, the NVIDIA Streamline 2.14.16 DLLs in `bin\x64\fg\`, dlssg_for_sm86 on RTX 20/30, and `factorio_fg.addon64`
in `bin\x64`. The first start compiles the motion shaders (up to a minute, shown top left).

Full history: `CHANGELOG.md`.
