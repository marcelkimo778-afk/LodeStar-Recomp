<p align="center">
  <img src="System/launcher/assets/launcher.png" width="120" alt="LodeStar">
</p>

<h1 align="center">LodeStar</h1>

<p align="center">
  A static recompiler that turns a Nintendo Switch game's ARM64 code into a native Windows program.<br>
  No emulator, no interpreter: the game's code is translated to C ahead of time and compiled on your PC.
</p>

<p align="center">
  <b>Supported game:</b> Pokémon Legends: Arceus, version 1.0.0 &nbsp;·&nbsp; <b>Platform:</b> Windows 10/11 x64
</p>

---

> **No game files are included, and none can be downloaded here.**
> LodeStar only works with a copy of the game you own, dumped from your own Nintendo Switch.
> This project is not affiliated with, endorsed or sponsored by Nintendo, Game Freak, Creatures or The Pokémon Company.
> All trademarks belong to their owners. Do not open issues asking for game files or keys.

## What you get

- **High, steady frame rates**: 30, 60, 90, 120 or 240 fps with the world running at real-time speed, plus built-in
  **frame generation** (2x to 10x, limited by your monitor). On a Ryzen 7 / RTX 5070 an open-field route runs at about
  190 fps; limited to 4 CPU threads it still holds about 160.
- **Sharper picture**: resolution scale up to 4x (4K and beyond), FSR 1 or NVIDIA DLAA, 16x texture filtering, larger
  sun shadows, far tree and detail distances, colour presets.
- **Recommended settings for your PC**: the launcher reads your processor, memory and graphics card and fills in
  settings that suit them, with the extra work on the graphics card, the processor or both.
- **Mods**: romfs/exefs/cheat mods in emulator and Atmosphere layouts, a mod browser (GameBanana, GitHub mod
  collections, Nexus Mods) with safe installs and version checks, and an in-game mods menu (F4).
- **Comfort**: controller and keyboard/mouse (rebindable, mouse camera), an in-game settings menu (F1), a performance
  overlay (F10), save import/export and a save library.

## Getting started

1. **Install LodeStar** - download the installer from [Releases](../../releases), or clone this repository.
2. **Dump your game** from your own Switch, as an already-extracted **ExeFS** and **RomFS** (for example with
   nxdumptool: "Dump ExeFS" and "Dump RomFS" of the base game, version 1.0.0). LodeStar never decrypts game packages.
3. **Start LodeStar**, click **Import game files** and pick the folder with your `exefs` and `romfs`.
   The launcher checks the files and the game version.
4. Click **Build** once (it translates and compiles the game on your PC: 10-30 minutes depending on the CPU), then
   **Play**.

### Requirements

- Windows 10/11 x64, a graphics card with Vulkan 1.3, a CPU with AVX2 (Intel Haswell / AMD Ryzen or newer).
- For the one-time build: Visual Studio Build Tools (C++), LLVM/clang, the .NET runtime, Python 3. The installer
  checks these and helps you install what is missing.
- About 20 GB free disk space (your dump + the build).

## Folders

    LodeStar.exe     start here (the launcher)
    Game/            your own game dump: exefs/ and romfs/
    Mods/            your mods
    Settings/        launcher, controls and in-game settings (stay on your PC)
    System/          everything the launcher needs - recompiler, runtime, tools; the game you build ends up here

## Controls

Default keyboard: A = Enter/Space/K, B = Backspace/L, X = I, Y = J, L = Q, R = E, ZL = 1, ZR = 3, + = Tab/P,
− = M, left stick = WASD, d-pad = arrows, right stick = numpad 8/4/6/2. Everything can be rebound in the launcher.

| Key | In the game window |
|---|---|
| F1, or Minus + Plus together | settings menu (Display, World, Sound, Controls, Game, Debug) |
| F2 | mouse camera on / off |
| F3 / F10 | FPS counter / performance overlay |
| F4 | mods menu |
| F9 | record an input route (for benchmarks) |
| F11 | fullscreen |

## Known problems

- Wild Pokémon still appear only within about 40-50 m of the player, whatever the Simulation distance.
- Simulation distance is processor work: high values lower the frame rate on most PCs.
- A rare start failure ("guest page ... is occupied by host memory"): start again.

## How it works

The game's code (main, sdk, subsdk0, subsdk1) is translated function by function into C, about 225,000 functions
in 3,340 files, then compiled with clang (with profile-guided optimisation) into one Windows program. A C runtime
stands in for the Switch's operating system (memory, threads, kernel calls, services, input, saves, audio), and the
game's NVN graphics calls are translated to Vulkan, its shaders with the Ryujinx shader compiler.

Details, build switches and tools: [docs/DEVELOPING.md](docs/DEVELOPING.md).

## Credits

LodeStar builds on open-source work, listed with their licences in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):
the graphics layer of [totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp) and the projects it builds on, the Ryujinx shader
translator, Dear ImGui, Opus and AMD FidelityFX FSR.
