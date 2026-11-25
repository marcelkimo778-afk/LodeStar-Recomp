# Third-party notices

LodeStar contains or uses the following open-source work. Each copied file keeps its original copyright header.

| Component | Source | License | Used in |
|---|---|---|---|
| totk-pc-recomp: NVN front end, Vulkan back end, platform layer, FSR / colour passes, DLAA glue (ported with our edits by `System/Engine/Tools/port_gfx.py` and `gfx_patches/`) | https://github.com/valentderah/totk-pc-recomp @ `0f65b4ee07e934e99b0c937d059f6bb6ae72e1c3`, Copyright (c) 2026 Valiantsin Dzerakh | MIT (`System/Engine/Runtime/gfx/LICENSE.totk-pc-recomp`) | `System/Engine/Runtime/gfx/` |
| The projects totk-pc-recomp builds on (LAS Recomp, MacNativeGames, d2r-mobile, nxnative, h1k421's `nvn.h`, deko3d's block-linear layout (zlib), SavageWindRecomp, Ryujinx and others), as far as their code is in the ported files | listed with commits in `System/Engine/Runtime/gfx/THIRD_PARTY_NOTICES.totk-pc-recomp.md` | MIT / zlib (see that file) | `System/Engine/Runtime/gfx/` |
| Ryujinx shader translator (`Ryujinx.Graphics.Shader`, `Spv.Generator`, parts of `Ryujinx.Common`) with totk-pc-recomp's patches and front end | https://github.com/Ryujinx/Ryujinx, via totk-pc-recomp, Copyright (c) Ryujinx Team and Contributors | MIT (`System/build_work/xlate/src/LICENSE.Ryujinx.txt`) | `System/build_work/xlate/src/` (built into `sw_xlate.dll`) |
| AMD FidelityFX Super Resolution 1.0 (EASU / RCAS shaders, compiled) | https://github.com/GPUOpen-Effects/FidelityFX-FSR, via totk-pc-recomp, Copyright (c) 2021 Advanced Micro Devices, Inc. | MIT | `System/Engine/Runtime/gfx/vk/fsr_easu_spv.h`, `fsr_rcas_spv.h` |
| Dear ImGui | https://github.com/ocornut/imgui, Copyright (c) 2014-2025 Omar Cornut | MIT (`System/Engine/Runtime/third_party/imgui/LICENSE.txt`) | in-game menu and overlays |
| libopus (`libopus-0.dll`, unmodified) | https://opus-codec.org, Copyright (c) Xiph.Org and contributors | BSD-3-Clause (`System/Engine/Runtime/third_party/opus/COPYING`) | music and voices (nn::codec Opus decoder) |

Ideas and documentation (no code copied; LodeStar's code for these is its own):

| Source | License | What was used |
|---|---|---|
| nxaevora (`docs/arceus-timing.md` and notes) | GPL-3.0 | research notes on which game sites need a frame-rate fix and the physics-step guard; facts and addresses only, the hooks in `System/Engine/Runtime/src/sw_hooks.c` are written independently (checked: no shared code lines beyond generic one-liners) |
| switchbrew wiki | community documentation | Switch service and command layouts (e.g. the hardware Opus decoder in `sw_ipc.c`) |

Not included, used from your PC at run time or build time:

| Component | License | Purpose |
|---|---|---|
| NVIDIA DLSS (`nvngx_dlss.dll`) | NVIDIA's terms, comes with the games that ship it | DLAA; the launcher copies your own copy from another game's folder, it is never distributed here |
| Vulkan headers and loader (Vulkan SDK / your graphics driver) | Apache-2.0 / MIT | graphics |
| Visual Studio Build Tools, LLVM/clang, .NET runtime, Python | their own licences | building the game on your PC |
| Capstone, Unicorn (developer tools only) | BSD-3-Clause / GPL-2.0 | analysis and differential tests; never linked into the game |

No code or data of the game is part of this repository or of any release: you build the game from your own dump.
