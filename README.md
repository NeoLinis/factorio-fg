# Factorio Frame Generation

Smooth Factorio on a high-refresh monitor. This add-on puts NVIDIA DLSS Frame Generation or AMD FSR Frame Generation
between Factorio's frames: up to 240 fps with DLSS-G (4x) or 120 fps with FSR (2x), instead of 60 - while the game
itself keeps running exactly as before at 60 updates per second.

Not affiliated with or endorsed by Wube Software (Factorio), NVIDIA or AMD.

## Why it exists

Factorio's simulation runs at 60 updates per second, and its picture is tied to that: 60 frames per second, whatever
your monitor can do. Raising that would mean speeding up the game itself.

Frame generation can fill the gap, but the usual tools - screen-capture apps like Lossless Scaling, or the driver's
own Smooth Motion / Fluid Motion Frames - only see the finished picture. They have to guess how everything moved:
the GUI wobbles, your character smears, and the guessing costs latency and GPU time.

This add-on sits inside Factorio's rendering instead. It gives the frame generation of NVIDIA or AMD what it needs to
do a clean job:

- **the picture without the GUI**, so the GUI is never distorted,
- **how everything moved**, measured from Factorio's own drawing,
- **exactly where your character and its shadow are**, read from the sprites Factorio draws,
- **what is ground, what is a building, what flies**, for depth.

The goal: better image quality, lower latency and less GPU cost than picture-only frame generation, without touching
the game's 60 UPS logic - saves, mods and multiplayer behave as always.

## What you get

| | NVIDIA DLSS-G | AMD FSR |
|---|---|---|
| Graphics cards | RTX 40/50 (RTX 20/30 with the community add-on dlssg_for_sm86) | Any DirectX 12 card: AMD, NVIDIA, Intel |
| Frames per Factorio frame | 2x, 3x or 4x: up to 120, 180 or 240 fps | 2x: up to 120 fps |
| Extra download | NVIDIA Streamline (free) | Nothing: included |

- **Auto mode** picks the largest multiplier your monitor can show: 144 Hz gets 2x (120 fps), 180 Hz 3x, 240 Hz
  4x (240 fps). Faster monitors also get 240 fps, the most DLSS-G can make from Factorio's 60.
- The **GUI stays sharp**; your **character** and its shadow move cleanly, walking or flying.
- Works in **multiplayer**, with the **Steam overlay**, on all planets.
- An optional **on-screen readout** (Ctrl+F11) shows frame rates and what the add-on is doing.

## Requirements

- Windows 10 or 11 with **hardware-accelerated GPU scheduling on** (Settings > System > Display > Graphics; reboot
  after changing it).
- Factorio 2.x with its default DirectX 11 renderer.
- **ReShade 6.x with full add-on support** (the plain ReShade cannot load add-ons).
- Driver frame generation **off** for Factorio (NVIDIA Smooth Motion, AMD Fluid Motion Frames): it would add its own
  frames on top.

## Install

Everything goes into Factorio's `bin\x64` folder, next to `factorio.exe`. On Steam: right-click Factorio > *Manage* >
*Browse local files*, then open `bin\x64`. Close Factorio first.

1. **ReShade**: download it from reshade.me (*with full add-on support*), run the installer, pick
   `bin\x64\factorio.exe`, choose *DirectX 10/11/12*, skip all effect packages.
2. **This add-on**: from the release zip, copy `factorio_fg.addon64` and the two `amd_fidelityfx_*.dll` files into
   `bin\x64`. That is all FSR needs.
3. **For DLSS-G** (NVIDIA RTX): download the NVIDIA Streamline SDK **2.14.16** zip from
   github.com/NVIDIA-RTX/Streamline/releases. Make a folder `fg` in `bin\x64` and copy these six files from the zip's
   `bin\x64` into it: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`,
   `nvngx_dlssg.dll`.
4. **RTX 20/30 only**: from the dlssg_for_sm86 release, copy `version.dll`, `dlssg_sm86.ini` and the folder
   `dlssg_sm86` into `bin\x64`.

**Updating**: replace `factorio_fg.addon64` and the two AMD DLLs; settings are kept. **Removing**: delete those files
and `factorio_fg_shaders.cache` (and ReShade's `dxgi.dll` if you want ReShade gone too).

## First start

1. Start Factorio, load a save, press **Home** to open ReShade and go to the *Factorio Frame Generation* tab.
2. Open **Setup check**: everything should be green - anything else comes with the fix.
3. Tick **Frame generation on** and close the overlay (the generated picture is hidden while it is open).

The very first start compiles the add-on's shaders once (up to a minute, shown in the top left). Until then frame
generation already runs, just less precisely.

The defaults are the recommended settings. On an NVIDIA RTX card you can switch between DLSS-G and FSR
(*Technology*; applies at the next start).

## Good to know

- Frame generation adds a little latency - roughly one game frame (DLSS-G uses NVIDIA Reflex to keep it low).
- Factorio itself still renders at 60 fps. Everything updates at that rate underneath; the extra frames make motion
  smooth.
- Uncovered ground right behind a fast-moving character, and items on fast belts while you run against them, can show
  small artifacts in generated frames.
- FSR is limited to 2x.

## Problems?

| Symptom | What to do |
|---|---|
| Setup check is not all green | Each item lists its fix. |
| Frame generation does not start | Look at `bin\x64\ReShade.log` (lines with `fg:`, `fsr:` or `streamline:`) and report them. |
| Stutters | The readout counts them; `bin\x64\hudless_probe\stutters.txt` says whether the add-on or the game took the time. |
| Anything else | Open an issue with a screenshot and `ReShade.log`. |

## More

- **How it works, every setting, building it yourself:** [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md)
- **What changed:** [`CHANGELOG.md`](CHANGELOG.md) - **how it was developed, step by step:** [`DEVLOG.md`](DEVLOG.md)

## Licence

MIT, see `LICENSE`. Third-party parts keep their own licences (ReShade API: BSD 3-Clause; Dear ImGui, NVIDIA
Streamline headers, AMD FSR headers and DLLs: MIT) - see `THIRD_PARTY_NOTICES.md`.
