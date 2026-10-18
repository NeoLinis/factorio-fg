Frame generation for Factorio 2.x through ReShade: NVIDIA DLSS-G (2x-4x, RTX 40/50, RTX 20/30 with dlssg_for_sm86)
and now **AMD FSR Frame Generation** (2x, any DirectX 12 GPU).

**New in 1.1.0**
- **AMD FSR Frame Generation** as a second technology: works on AMD, Intel and NVIDIA GPUs. AMD's two DLLs are in the
  zip (MIT licence). *Technology*: Automatic (DLSS-G on NVIDIA RTX when installed, else FSR), DLSS-G or FSR; a change
  applies at the next start of Factorio.
- **GUI and character kept out of frame generation** (DLSS-G and FSR): the GUI, and the character while you run,
  drive or fly (mech armour included), are taken from the newest real frame and laid over every generated frame, so
  they no longer smear or leave copies.
- **Fix for 1.0.0 installs:** on a fresh install the GUI capture rule was saved as *Off*, so the "HUD-less" copy
  still had the GUI in it. 1.1.0 switches a saved *Off* back to the built-in rule once (ReShade.log says so). The
  readout now shows a *GUI capture* line.
- Readout shows the technology's status; settings a technology cannot use are greyed out; new diagnostics under
  *Motion vectors > Debug views*.

**Install / update:** download `factorio-fg-1.1.0.zip` below and follow *Install* in the README. Updating from 1.0.0:
replace `factorio_fg.addon64` and add `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll`
next to it (only needed for FSR). The first start recompiles the motion shaders (up to a minute, shown top left).

**Known limits:** FSR is 2x only; the ground just uncovered behind a moving character can look noisy in generated
frames (both technologies); FSR 4 (RX 9000) is untested.

Full history: `CHANGELOG.md`.
