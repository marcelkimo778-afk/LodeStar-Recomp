<p align="center">
  <img src="System/launcher/assets/launcher.png" width="120" alt="LodeStar">
</p>

<h1 align="center">LodeStar</h1>

<p align="center">
  Pokémon Legends: Arceus (version 1.0.0) as a native Windows program, built from your own game dump.
</p>

---

> **No game files are included, and none can be downloaded here.**
> LodeStar only works with a copy of the game you own, dumped from your own Nintendo Switch.
> This project is not affiliated with, endorsed or sponsored by Nintendo, Game Freak, Creatures or The Pokémon Company.
> All trademarks belong to their owners. Do not open issues asking for game files or keys.

## Getting started

New here? The illustrated walkthrough is in **[docs/TUTORIAL.md](docs/TUTORIAL.md)**.

1. **Install LodeStar** - download the installer from [Releases](../../releases), or clone this repository.
2. **Dump your game** from your own Switch, as an already-extracted **ExeFS** and **RomFS** (for example with
   nxdumptool: "Dump ExeFS" and "Dump RomFS" of the base game, version 1.0.0).
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

## How it works

The game's code (main, sdk, subsdk0, subsdk1) is translated function by function into C, about 225,000 functions
in 3,340 files, then compiled with clang (with profile-guided optimisation) into one Windows program. A C runtime
stands in for the Switch's operating system (memory, threads, kernel calls, services, input, saves, audio), and the
game's NVN graphics calls are translated to Vulkan, its shaders with the Ryujinx shader compiler.

Details, build switches and tools: [docs/DEVELOPING.md](docs/DEVELOPING.md).
