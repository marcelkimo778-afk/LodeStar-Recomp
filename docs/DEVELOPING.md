# Developing LodeStar

Everything technical lives in `System/`. Paths below are relative to `System/`, and the commands are run from there.
Players do not need any of this: the launcher (`LodeStar.exe`) runs the same steps.

## Building by hand

    # 1. ARM64 -> C (a few minutes on 16 threads; writes build_work/gen + build_work/images)
    python -u Engine/swrecomp.py recompile ../Game/exefs --module main --module sdk --module subsdk0 --module subsdk1 --max-functions 10000000 --out build_work

    # 2. C -> exe (full build about 10 minutes with clang on 16 threads; runtime-only changes about 10 s)
    python build_work/build.py
    #   SW_JOBS=10 SW_BIG_JOBS=1   gentler on a PC in use (builds run at idle priority)
    #   SW_OPT=/Od                 faster build, slower exe
    #   SW_LTO=thin                ThinLTO over the chunks (experimental: crashed once in testing, off by default)

    # 3. Run (from build_work/out: the exe loads images/*.bin next to itself)
    cd build_work/out && "./LodeStar Game.exe" --real-start --max-dispatch 0

The game code must be version 1.0.0 (build id `7FCAD279539DE183...`): the frame-rate, distance and other hooks are
made for it. The launcher's **Import game files** checks the version.

### PGO (profile-guided optimisation)

With `Engine/pgo/arceus.profdata` present, every build uses it (about +14 % fps and far fewer slow frames). After a
recompile the old profile no longer matches; make a new one:

    SW_PGO=gen python build_work/build.py                         # instrumented game (slow)
    set LLVM_PROFILE_FILE=../pgo_raw/pgo_%p.profraw                # then replay routes: Engine/Tools/bench.py ROUTE=bench_routes/...
    llvm-profdata merge -o Engine/pgo/arceus.profdata build_work/pgo_raw/*.profraw
    python build_work/build.py                                    # optimised with the profile (SW_PGO=off: without)

Check the merged profile before using it (`llvm-profdata show`: hundreds of thousands of functions, a non-zero total
count). An empty profile builds fine but silently loses the gain. The profile is made from running the game, so it
is never committed or shipped.

### Build facts

- The build moves the old exe aside first and writes the new one only when it has finished.
- Object cache (`build_work/objcache`): a chunk is reused when its C, its flags and **every header in
  `Engine/Runtime/include`** are unchanged. Editing one of those headers recompiles all ~3,340 chunks. Headers for
  new runtime features belong next to their .c in `Engine/Runtime/src`.
- Changes to the recompiler (`Engine/Source/swrecomp`) need step 1 again. Changes to `Engine/Runtime` need step 2 only.
- The chunks are compiled for x86-64-v3 (AVX2 + FMA); the runtime refuses to start on CPUs without AVX2.

## Project layout (inside `System/`)

    Engine/swrecomp.py             recompiler entry point
    launcher/                      launcher GUI (launcher.py, ui_kit.py), controls, saves, save editor, mod browser
    Engine/Source/swrecomp/        recompiler
        arm64/decoder.py             AArch64 decoder
        arm64/lift.py, ir.py         instructions -> IR
        arm64/emit.py                IR -> C (SWRECOMP_ABI=1: calling-convention-aware spills, experimental)
        pipeline/analyze.py          function discovery, jump tables
        pipeline/recompile.py        import binding, facts pass, output
        formats/                     NSO / ExeFS / PFS0 / RomFS readers
    Engine/Runtime/include/        sw_rt.h (CPU state + helpers used by generated code)
    Engine/Runtime/src/            runtime: loader, guest threads, kernel calls, services, input, RomFS, saves,
                                   mods, hooks (frame rate, distances), in-game menu
    Engine/Runtime/gfx/            NVN front end + Vulkan back end (ported from totk-pc-recomp, see
                                   THIRD_PARTY_NOTICES.md; edit Engine/Tools/port_gfx.py + gfx_patches, not the files)
    Engine/Runtime/ui/             in-game overlay, performance overlay, mod browser (mb_*.cpp, also mb_cli.exe)
    Engine/Tools/                  dev tools (below)
    Engine/Tests/, tests/          unit tests
    build_work/build.py            the build
    build_work/xlate/src/          the shader translator's sources (Ryujinx, MIT; built by Engine/Tools/build_xlate.py)
    build_work/gen, images, out    generated C, module images, the built game (never committed: made from game code)

## How it works

- **Recompiler**: functions are found from the entry points, symbols, call targets, jump tables and runtime-recorded
  seeds. Each function becomes a C function `sw_f_<guest address - 0x100000>`, split into ~3,340 `sw_chunk_*.c`.
- **Memory**: guest addresses are host addresses. The runtime reserves the address space below 2^39 at start-up and
  maps the module images at their Switch bases.
- **Startup** (`--real-start`): relocations -> module DT_INIT -> nninitStartup -> nnMain.
- **Services**: imports are bound to native implementations; IPC services are answered by `sw_ipc.c`.
- **Graphics**: NVN calls go to the ported front end. Maxwell shaders are translated with the Ryujinx shader
  compiler (`sw_xlate.dll`); the Vulkan back end draws and presents.
- **Threads**: with `SW_MULTICORE=1` every guest thread runs on its own host thread, `SW_CORES` emulated cores
  (the launcher passes the number of logical CPUs).

## Debugging

Rules:
- Fix the recompiler, decoder or runtime, never the generated C.
- Check new instruction support against Unicorn / Capstone, not against our own decoder.
- Test on save copies (`SW_SAVE_DIR`), never on the player's save. Tests run in a hidden window at low priority.

Tools (`pip install capstone unicorn` for the analysis ones):

    Engine/Tools/bench.py <name> <secs> ROUTE=bench_routes/<route> [AFFINITY=<hex>] [ENV=VAL ...]
        replays a recorded route (F9 in the game records one) on a save copy; AFFINITY limits the CPUs (weak-PC test)
    Engine/Tools/ft_summary.py <SW_FRAMETIME_LOG files>   avg, p50-p99.9, 1%/0.1% lows, slow-frame counts
    Engine/Tools/prof_threads.py <SW_PROFILE_OUT file>    per-thread profile of a SW_PROFILE=all run
    Engine/Tools/difftest.py                              sampled instructions: Unicorn vs generated C
    Engine/Tools/guest_disasm.py, guest_sym.py, guest_xref.py   disassembly, symbols, cross references
    Engine/Tools/github_index.py                          rebuild the mod browser's built-in GitHub index (before a release)
    Engine/Tools/port_gfx.py, build_xlate.py              regenerate the gfx port / the shader translator
    Engine/Tools/make_logos.py, build_lodestar.py         icons / LodeStar.exe

Useful switches (environment variables):

| Variable | Effect |
|---|---|
| `SW_MULTICORE=1 SW_CORE_PIN=0 SW_CORES=<n or auto>` | Guest threads on host threads with n emulated cores. |
| `SW_FPS=30\|60\|90\|120\|240` | Frame rate at start (the in-game menu changes it live). |
| `SW_FRAMEGEN=2..10`, `SW_FG_REBASE=0`, `SW_FG_TRACE=<n>` | Frame generation / old fixed timeline / log 600 presents. |
| `SW_SPIN_US=<us>` | How long a blocked guest thread spins before it sleeps (default by CPU count: 50 / 200 / 1000). |
| `SW_FRAMETIME_LOG=<file>`, `SW_GPU_TIME=1` | Every frame time to a file; GPU time per frame. |
| `SW_FPS_OVERLAY=0\|1\|2` | Performance overlay at start. |
| `SW_RES_SCALE=1..4`, `SW_DLAA=1` | Resolution scale, DLAA. |
| `SW_PROFILE=all SW_PROFILE_OUT=<file>` | Sampling profiler of every thread. |
| `SW_THREAD_STATS=1`, `SW_HANG_DUMP=<s>` | Thread wait statistics / dump every thread when nothing is presented for s seconds. |
| `SW_SAVE_DIR=<dir>`, `SW_INPUT=script\|none` | Save folder / scripted input (tests). |
| `SW_MODS_CONFIG=<file>`, `SW_ROMFS=<dir>` | Another mod config / RomFS folder. |
| `SW_LOCKSTEP=auto,1500,4` + `SW_LOCKSTEP_DLL=<unicorn.dll>` | Random guest functions in Unicorn and natively, mismatches printed (slow). |

Reading failures:
- `unsupported instruction 0x...`: add decode (`decoder.py`), lifting (`lift.py`) and emission (`emit.py`), then
  run difftest.
- `call to non-recompiled address`: appended to `missing_seeds.txt`; the next recompile includes it.
- Crash handler: prints `exe rva`; look it up in `build_work/out/sw_app.map`.

Tests:

    python -m pytest Engine/Tests tests

## Open problems

- Wild Pokemon spawn only within ~40-50 m of the player whatever the Simulation distance (a separate spawner).
- Grass draw distance follows none of the settings yet.
- Talk prompt flicker (rare), one ~1.6 s frame when a save loads into the world.
- Nexus Mods single sign-on needs an application slug registered with Nexus (`launcher/modbrowser_cli.py`
  `NEXUS_APP_SLUG`); until then the log-in opens the API-key page.
