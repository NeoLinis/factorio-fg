# Third-party notices

This add-on is built with the following third-party code. Their licences are compatible with the MIT licence of the
add-on itself (`LICENSE`) and require their notices to be kept; the full texts are in `third_party\`.

| Component | Used for | Licence | Copyright |
|---|---|---|---|
| ReShade add-on API headers (`third_party/reshade`) | Hooking Factorio's DirectX 11 rendering | BSD 3-Clause (`third_party/reshade/LICENSE.md`) | Copyright 2014 Patrick Mours |
| Dear ImGui headers (`third_party/imgui`) | The settings window and readout, drawn through ReShade | MIT (`third_party/imgui/LICENSE.txt`) | Copyright (c) 2014-2025 Omar Cornut |
| NVIDIA Streamline SDK headers (`third_party/streamline`) | Calling DLSS Frame Generation | MIT (`third_party/streamline/license.txt`) | Copyright (c) 2023 NVIDIA CORPORATION |

Not included, downloaded separately by the user, each under its own terms:

- **ReShade** itself (reshade.me).
- **NVIDIA Streamline runtime and DLSS-G** (`sl.*.dll`, `nvngx_dlssg.dll`), from NVIDIA's Streamline releases, under
  NVIDIA's licence for those binaries.
- **dlssg_for_sm86** (RTX 20/30 only), a third-party, unofficial project.

Factorio is a trademark of Wube Software. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. This project is
not affiliated with or endorsed by either.
