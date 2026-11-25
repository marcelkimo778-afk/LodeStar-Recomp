#!/usr/bin/env python3
"""Compile build_work/gen + Engine/Runtime into System/build_work/out/Pokemon Arceus.exe.

Called by the launcher.  Incremental: only sources newer than their object (or
whose headers changed) are recompiled.  Objects are compiled into a temp dir
and moved into place only after a successful compile, so a crashed build never
leaves a partial .obj that a later run would trust.

Module images are copied from build_work/images -- the images written by the
*same* recompile run that produced gen/.  Images from any other run have the
same sizes but different relocation patches, so they must never be mixed.

Environment:
  SW_OPT       optimisation flag for generated chunks (default /O1 on MSVC;
               /Od builds much faster but runs slower)
  SW_BUILD_ID  string baked into the exe and printed at startup
  SW_JOBS      parallel compile jobs (default: CPU count)
  SW_BUILD_PRIORITY  low (default: the PC stays usable, compilers inherit it) | normal
  SW_BIG_JOBS  giant (>1.5 MB) sources compiled at once (default 2; each can
               need ~7 GB of RAM in the optimiser)
  SW_OBJ_CACHE  object cache folder for generated chunks (default
               build_work/objcache; "off" disables): a chunk whose C text, flags
               and runtime headers are unchanged is copied from the cache
               instead of compiled - after a regen only changed chunks compile
  SW_HOT_CHUNKS  list of hot chunk files ("sw_chunk_NNNN" per line, from a
               profile; default build_work/hot_chunks.txt when it exists):
               compiled with SW_HOT_OPT (default /O2) - "good vs best": a hot
               file that is too slow at /O2 falls back to the normal /O1, never /Od
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:  # compiler output may contain non-cp1252 characters
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "Engine" / "Source"))

import swrecomp.pipeline.build as build_mod  # noqa: E402
from swrecomp.errors import SwRecompError  # noqa: E402

WORK = PROJECT_ROOT / "build_work"
GEN_DIR = WORK / "gen"
IMAGES_SRC = WORK / "images"
RUNTIME_DIR = PROJECT_ROOT / "Engine" / "Runtime"
OUT_DIR = WORK / "out"
OBJ_DIR = OUT_DIR / "obj"
MSVC_LIBS = ["user32.lib", "gdi32.lib", "opengl32.lib"]
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # never flash a console per compiler call
FILE_TIMEOUT = 300  # seconds per file before falling back to /Od


def run(cmd, env):
    return subprocess.run(cmd, env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", creationflags=NO_WINDOW)


def compile_failed(result) -> bool:
    # One source per cl.exe call: any non-zero exit is a failure.  A crashed
    # compiler (e.g. 0xC00000FD stack overflow) exits non-zero with no
    # "error Cnnnn" line and an empty object; that used to count as success.
    return result.returncode != 0


def compiler_crashed(result) -> bool:
    """The optimiser itself failed (crash, ICE, out of heap), not our code.
    Localised MSVC prints "Fehler" etc. but always keeps the Cnnnn codes."""
    text = result.stdout + result.stderr
    return result.returncode >= 0x80000000 or result.returncode < 0 or \
        re.search(r"\b(C1001|C1002|C1060|C1063)\b", text) is not None


def parse_module_table(sw_program_c: Path):
    table = sw_program_c.read_text()
    match = re.search(r"const SwModuleDesc sw_modules\[\]\s*=\s*\{(.*?)\n\};", table, re.DOTALL)
    if not match:
        return None
    modules = []
    for line in match.group(1).splitlines():
        m = re.match(r'\s*\{"([^"]+)",\s*(0x[0-9a-fA-F]+)ULL,\s*(0x[0-9a-fA-F]+)ULL,.*?"(images/[^"]+)"', line)
        if m:
            modules.append((m.group(1), int(m.group(3), 16), Path(m.group(4)).name))
    return modules


def copy_images() -> bool:
    modules = parse_module_table(GEN_DIR / "sw_program.c")
    if modules is None:
        print("ERROR: could not parse sw_modules table from sw_program.c", file=sys.stderr)
        return False
    dst_dir = OUT_DIR / "images"
    dst_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n=== Module table vs images ({IMAGES_SRC}) ===")
    ok = True
    for name, expected, bin_name in modules:
        src = IMAGES_SRC / bin_name
        if not src.is_file():
            print(f"  {name:10s}: MISSING {src}")
            ok = False
            continue
        size = src.stat().st_size
        if size != expected:
            print(f"  {name:10s}: SIZE MISMATCH table=0x{expected:X} file=0x{size:X}")
            ok = False
            continue
        shutil.copy2(src, dst_dir / bin_name)
        print(f"  {name:10s}: OK 0x{expected:X}")
    if not ok:
        print("ERROR: image verification failed; re-run the recompile step", file=sys.stderr)
    return ok


def _set_priority() -> None:
    """Run the build (and every cl.exe it starts) below normal priority so the PC stays usable."""
    if sys.platform != "win32" or os.environ.get("SW_BUILD_PRIORITY", "low").lower() == "normal":
        return
    try:
        import ctypes
        IDLE_PRIORITY_CLASS = 0x40
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), IDLE_PRIORITY_CLASS)
    except Exception:
        pass


def main() -> int:
    _set_priority()
    build_start = time.time()
    try:
        tool = build_mod.find_toolchain()
    except SwRecompError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    msvc = tool.name == "msvc"
    exe = OUT_DIR / ("Pokemon Arceus.exe" if msvc else "sw_app")   # the game: its own icon (res/game.ico)

    print(f"Toolchain: {tool.name} ({tool.compiler})")
    print(f"Gen dir:   {GEN_DIR}")
    print(f"Out dir:   {OUT_DIR}")

    if not (GEN_DIR / "sw_program.c").is_file():
        print(f"ERROR: {GEN_DIR / 'sw_program.c'} not found; run the recompile step first",
              file=sys.stderr)
        return 1

    # A failed build must not leave the previous exe behind looking current: it is moved to sw_app.prev.exe (not
    # deleted - a broken build then still leaves a working exe to copy back).
    if exe.exists():
        try:
            exe.replace(exe.with_name(exe.stem + ".prev" + exe.suffix))
        except OSError as e:
            print(f"ERROR: cannot delete old exe (still running?): {e}", file=sys.stderr)
            return 1

    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    if not copy_images():
        return 1

    chunk_sources = sorted(GEN_DIR.glob("*.c"))
    runtime_sources = sorted((RUNTIME_DIR / "src").glob("*.c"))
    # Graphics layer ported from totk-pc-recomp (Engine/Runtime/gfx, not ref/):
    # C23 + C11 atomics, the GCC-builtin shim and the Vulkan SDK headers.
    gfx_dir = RUNTIME_DIR / "gfx"
    vk_dir = gfx_dir / "vk"
    vk_sdk = Path(os.environ.get("VULKAN_SDK", r"C:\VulkanSDK\1.4.363.0"))
    # The Vulkan back end (gfx/vk) needs the Vulkan SDK; without it the NVN
    # front end runs on the null back end (no pixels).
    have_vulkan = (vk_sdk / "Include" / "vulkan" / "vulkan.h").is_file() and os.environ.get("SW_VULKAN", "1") != "0"
    gfx_sources = sorted(gfx_dir.glob("*.c")) + (sorted(vk_dir.glob("*.c")) if have_vulkan else [])
    runtime_sources += gfx_sources
    # In-game overlay (Engine/Runtime/ui, C++): Dear ImGui (third_party/imgui, MIT) on the Vulkan present path - the
    # mods menu and the mod browser.  Needs the Vulkan back end like gfx/vk.
    ui_dir = RUNTIME_DIR / "ui"
    imgui_dir = RUNTIME_DIR / "third_party" / "imgui"
    ui_sources = []
    if have_vulkan:
        ui_sources = ([q for q in sorted(ui_dir.glob("*.cpp")) if q.name != "mb_cli.cpp"]   # mb_cli: its own exe
                      + sorted(imgui_dir.glob("*.cpp")) + [imgui_dir / "backends" / "imgui_impl_vulkan.cpp"])
    runtime_sources += ui_sources
    ui_flags = ["/std:c++17", "/EHsc", "/wd4100", "/I" + str(imgui_dir), "/I" + str(imgui_dir / "backends"),
                "/I" + str(vk_dir), "/I" + str(gfx_dir), "/I" + str(vk_sdk / "Include"), "/I" + str(RUNTIME_DIR / "src")]
    ui_header_time = max((q.stat().st_mtime for q in list(ui_dir.glob("*.h")) + list(imgui_dir.rglob("*.h"))
                          + [vk_dir / "ui" / "menu.h"] if q.exists()), default=0)
    gfx_flags = ["/std:clatest", "/experimental:c11atomics", "/wd4100", "/wd4200", "/wd4201",
                 "/FI" + str(gfx_dir / "compat" / "gcc_compat.h"), "/I" + str(gfx_dir / "compat"),
                 "/I" + str(gfx_dir), "/I" + str(vk_sdk / "Include")]
    if have_vulkan:
        gfx_flags.append("/DSW_HAVE_VULKAN")
    gfx_header_time = max((q.stat().st_mtime for q in list(gfx_dir.glob("*.h")) + list(gfx_dir.glob("*.inc"))
                           + list((gfx_dir / "compat").rglob("*.h")) + list(vk_dir.rglob("*.h"))), default=0)
    includes = [RUNTIME_DIR / "include", GEN_DIR]
    def newest(paths) -> float:
        return max((p.stat().st_mtime for p in paths if p.exists()), default=0)

    # Generated chunks include only sw_rt.h; runtime files and sw_program.c may
    # include any runtime header.  Keeping these apart means editing e.g.
    # sw_window.h rebuilds a handful of runtime files, not all 8,800 chunks.
    chunk_header_time = newest([RUNTIME_DIR / "include" / "sw_rt.h", RUNTIME_DIR / "include" / "sw_simd6.h"])
    other_header_time = newest(list((RUNTIME_DIR / "include").glob("*.h")) + list((RUNTIME_DIR / "src").glob("*.h"))
                               + [GEN_DIR / "sw_program.h"])   # src/*.h: sw_menu.h, sw_mods.h (runtime files only)

    def needs_rebuild(src: Path, obj: Path) -> bool:
        try:
            st = obj.stat()
        except OSError:
            return True
        header_time = chunk_header_time if src.name.startswith("sw_chunk_") else other_header_time
        if src.parent in (gfx_dir, vk_dir):
            header_time = max(header_time, gfx_header_time)
        if src.suffix == ".cpp":
            header_time = max(header_time, ui_header_time)
        return st.st_size == 0 or src.stat().st_mtime > st.st_mtime or header_time > st.st_mtime

    chunk_opt = os.environ.get("SW_OPT", "/O1 /Oi" if msvc else "-O1")
    hot_opt = os.environ.get("SW_HOT_OPT", "/O2" if msvc else "-O2")
    hot_file = Path(os.environ.get("SW_HOT_CHUNKS", str(WORK / "hot_chunks.txt")))
    hot: set[str] = set()
    if hot_file.is_file():
        for line in hot_file.read_text(encoding="utf-8", errors="replace").splitlines():
            name = line.split()[0] if line.split() else ""
            if name.startswith("sw_chunk_"):
                hot.add(name.split(".")[0])
        print(f"Hot chunks: {len(hot)} from {hot_file} at {hot_opt}")

    def opt_for(src: Path) -> str:
        return hot_opt if src.stem in hot else chunk_opt

    # SW_CHUNK_CC=<clang-cl.exe>: the generated chunks (only they) are compiled with clang-cl instead of cl; the
    # runtime stays on MSVC, the objects link together. SW_CHUNK_CC_FLAGS adds flags (e.g. "-march=x86-64-v3").
    # The compiler is part of each object's stamp and cache key, so switching back and forth rebuilds / reuses
    # the right objects.
    # Default (2026-11-16): clang-cl when LLVM is installed (C:\Program Files\LLVM), -O2 for x86-64-v3 (AVX2 + FMA;
    # the runtime refuses to start without AVX2). On the user's village route at a 120 fps limit: 95 -> 117.5 fps
    # on screen, frames over 10 ms 1,100 -> 65. SW_CHUNK_CC=cl (or off) keeps MSVC for the chunks.
    chunk_cc = os.environ.get("SW_CHUNK_CC", "").strip() if msvc else ""
    default_clang = Path(r"C:\Program Files\LLVM\bin\clang-cl.exe")
    if msvc and not chunk_cc and default_clang.is_file():
        chunk_cc = str(default_clang)
    if chunk_cc.lower() in ("cl", "off", "msvc", "0"):
        chunk_cc = ""
    chunk_cc_flags = os.environ.get("SW_CHUNK_CC_FLAGS", "-march=x86-64-v3" if chunk_cc else "").split()
    if chunk_cc and "SW_OPT" not in os.environ:
        chunk_opt = "-O2"
    # PGO for the generated chunks (clang only).  SW_PGO=gen: instrumented chunks + clang_rt.profile; a route
    # replay (bench.py ROUTE=..., the route's end calls sw_pgo_write) writes LLVM_PROFILE_FILE; merge with
    # llvm-profdata into Engine/pgo/arceus.profdata.  SW_PGO=use (default when that file exists): the chunks are
    # optimised with it (counters and function names only, no game code).  SW_PGO=off: no profile.
    pgo_mode = os.environ.get("SW_PGO", "").strip().lower()
    pgo_file = Path(os.environ.get("SW_PGO_FILE", str(PROJECT_ROOT / "Engine" / "pgo" / "arceus.profdata")))
    pgo_lib = None
    if chunk_cc and pgo_mode == "gen":
        chunk_cc_flags.append("-fprofile-instr-generate")
        pgo_lib = Path(chunk_cc).parent.parent / "lib" / "clang"
        libs_found = sorted(pgo_lib.glob("*/lib/windows/clang_rt.profile-x86_64.lib"))
        pgo_lib = libs_found[-1] if libs_found else None
        if not pgo_lib:
            print("ERROR: SW_PGO=gen needs clang_rt.profile-x86_64.lib (LLVM install)", file=sys.stderr)
            return 1
        print(f"PGO: instrumented chunks ({pgo_lib})")
    elif chunk_cc and pgo_mode in ("", "use") and pgo_file.is_file():
        chunk_cc_flags.append("-fprofile-instr-use=" + str(pgo_file))
        # the profile's content is part of every chunk's stamp: a new profile rebuilds the chunks
        chunk_cc_flags.append("-DSW_PGO_ID=" + hashlib.sha256(pgo_file.read_bytes()).hexdigest()[:16])
        print(f"PGO: chunks optimised with {pgo_file}")
    # SW_LTO=thin (2026-11-25): ThinLTO over the chunks - small game functions in other chunks can be inlined into
    # their callers (each chunk alone never sees them).  The chunks become LLVM bitcode, so the exe is linked by
    # lld-link (MSVC's link cannot read it); the runtime stays MSVC.  The LTO cache (build_work/lto_cache) keeps
    # unchanged modules between links.
    lto_mode = os.environ.get("SW_LTO", "").strip().lower()
    lld_link = Path(chunk_cc).parent / "lld-link.exe" if chunk_cc else None
    if lto_mode == "thin" and lld_link and lld_link.is_file():
        chunk_cc_flags.append("-flto=thin")
        print(f"ThinLTO: chunks as bitcode, linked by {lld_link}")
    else:
        if lto_mode:
            print("note: SW_LTO needs clang chunks and lld-link.exe; building without LTO")
        lto_mode = ""
    # Engine/Runtime/src/sw_build_cfg.h tells the runtime (always MSVC) what the chunks need: with AVX2 chunks it
    # refuses to start on a CPU without AVX2 (a clear message instead of an illegal-instruction crash). Written only
    # when it changes (it is a runtime header: a change rebuilds the runtime files).
    need_avx2 = any("x86-64-v3" in f or "x86-64-v4" in f or "avx2" in f.lower() for f in chunk_cc_flags)
    cfg_h = RUNTIME_DIR / "src" / "sw_build_cfg.h"
    cfg_text = ("/* written by build_work/build.py: what the generated chunks were compiled for */\n"
                f"#define SW_CHUNKS_NEED_AVX2 {1 if need_avx2 else 0}\n")
    if not cfg_h.is_file() or cfg_h.read_text(encoding="utf-8") != cfg_text:
        cfg_h.write_text(cfg_text, encoding="utf-8")
    other_header_time = max(other_header_time, cfg_h.stat().st_mtime)
    chunk_tag = (" |cc=" + Path(chunk_cc).name + " " + " ".join(chunk_cc_flags)).rstrip() if chunk_cc else ""
    if chunk_cc:
        print(f"Chunks compiled with {chunk_cc} {' '.join(chunk_cc_flags)}")

    def stamp_for(src: Path) -> str:
        return opt_for(src) + (chunk_tag if src.name.startswith("sw_chunk_") else "")

    def opt_stamp(obj: Path) -> Path:
        return obj.with_suffix(obj.suffix + ".opt")

    objects, chunk_jobs, runtime_jobs = [], [], []
    for src in chunk_sources + runtime_sources:
        obj = OBJ_DIR / (src.stem + tool.obj_suffix)
        objects.append(obj)
        stale = needs_rebuild(src, obj)
        if not stale and src.name.startswith("sw_chunk_"):
            # an object built with other flags (hot list changed) is rebuilt too
            try:
                built = opt_stamp(obj).read_text().strip()
            except OSError:
                built = chunk_opt          # objects from before the stamps: the normal flags
            stale = built != stamp_for(src)
        if stale:
            (runtime_jobs if src in runtime_sources else chunk_jobs).append((src, obj))

    # Objects left over from chunks that no longer exist would link in stale code.
    wanted = {o.name for o in objects}
    for stale in OBJ_DIR.glob("*" + tool.obj_suffix):
        if stale.name not in wanted:
            stale.unlink()

    total_jobs = len(chunk_jobs) + len(runtime_jobs)
    print(f"Sources: {len(objects)} total, {total_jobs} to compile")
    print("Compiling...")

    # Fresh temp dir every run: a leftover object from an older build must
    # never be mistaken for the output of a compile that produced nothing.
    tmp_dir = OBJ_DIR / ".tmp_build"
    shutil.rmtree(tmp_dir, ignore_errors=True)
    tmp_dir.mkdir(parents=True, exist_ok=True)

    fallback_opt = "/Od" if msvc else "-O0"
    # Biggest files first: they take longest, and starting them late left the
    # build waiting on one or two giant chunks at the end.
    chunk_jobs.sort(key=lambda so: so[0].stat().st_size, reverse=True)
    jobs = [(s, o, "/O2" if msvc else "-O2") for s, o in runtime_jobs] + \
           [(s, o, opt_for(s)) for s, o in chunk_jobs]
    # The optimiser can use ~7 GB on a multi-MB chunk; a few of those at once
    # exhausted 32 GB.  Limit how many giant files compile concurrently.
    big_bytes = 1_500_000
    big_jobs = max(1, int(os.environ.get("SW_BIG_JOBS", "2")))
    big_sem = threading.BoundedSemaphore(big_jobs)
    lock = threading.Lock()
    state = {"done": 0, "warnings": 0, "slow": [], "error": None}

    # ---- object cache (chunks only): key = sha256(chunk C, flags, runtime headers, compiler)
    cache_env = os.environ.get("SW_OBJ_CACHE", str(WORK / "objcache"))
    cache_dir = None if cache_env.lower() == "off" else Path(cache_env)
    if cache_dir:
        cache_dir.mkdir(parents=True, exist_ok=True)
        hh = hashlib.sha256(str(tool.compiler).encode())
        for h in sorted((RUNTIME_DIR / "include").glob("*.h")) + [GEN_DIR / "sw_program.h"]:
            if h.is_file():
                hh.update(h.name.encode())
                hh.update(h.read_bytes())
        header_key = hh.hexdigest()
    cache_stats = {"hit": 0, "store": 0}

    def cache_key(src: Path, opt: str) -> str:
        # the chunk compiler and its flags (chunk_tag: e.g. PGO instrumentation) are part of the key
        k = hashlib.sha256(header_key.encode() + opt.encode() +
                           (chunk_tag.encode() if src.name.startswith("sw_chunk_") else b"") + b"\0")
        k.update(src.read_bytes())
        return k.hexdigest()

    def compile_one(src: Path, opt: str):
        tmp_obj = tmp_dir / (src.stem + tool.obj_suffix)
        if msvc:
            cmd = [a for a in tool.compile_cmd([src], tmp_dir, includes, opt=opt)
                   if not a.startswith("/MP")]
            if chunk_cc and src.name.startswith("sw_chunk_"):
                # clang-cl takes cl's flags; -Wno-everything: generated code (unused labels, casts) is noisy
                cmd = [chunk_cc] + [a for a in cmd[1:] if a not in ("/bigobj",)]
                # -ffp-contract=off: no fused multiply-add the guest code did not ask for (Arm rounds a separate
                # multiply and add twice; a fused one once - the float results must stay the guest's)
                # /Zc:tlsGuards-: C has no dynamically initialised thread-locals; without it every access to the
                # runtime's __declspec(thread) variables (direct calls, sw_rt.h) tests an MSVC on-demand-init guard
                cmd = cmd[:-1] + ["-Wno-everything", "/Zc:tlsGuards-", "/clang:-fno-strict-aliasing", "/clang:-ffp-contract=off",
                                  *chunk_cc_flags] + cmd[-1:]
            elif src.parent in (gfx_dir, vk_dir):
                extra = ["/I" + str(vk_dir)] if src.parent == vk_dir else []
                cmd = cmd[:-1] + gfx_flags + extra + cmd[-1:]
            elif src.suffix == ".cpp":
                cmd = cmd[:-1] + ui_flags + cmd[-1:]
        else:
            cmd = tool.compile_cmd([src], tmp_obj, includes, opt=opt)
        big = src.stat().st_size > big_bytes
        if big:
            big_sem.acquire()
        try:
            result = subprocess.run(cmd, env=tool.env, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", creationflags=NO_WINDOW,
                                    timeout=FILE_TIMEOUT if opt != fallback_opt else None)
        except subprocess.TimeoutExpired:
            return "timeout", None
        finally:
            if big:
                big_sem.release()
        return ("failed" if compile_failed(result) else "ok"), result

    times: list[tuple[float, str, str]] = []      # (seconds, file, opt) for the slowest-files report

    def worker(src: Path, obj: Path, opt: str):
        if state["error"]:
            return
        t0 = time.time()
        key = None
        if cache_dir and src.name.startswith("sw_chunk_"):
            key = cache_key(src, opt + chunk_tag)
            cached = cache_dir / (key + tool.obj_suffix)
            if cached.is_file() and cached.stat().st_size > 0:
                shutil.copyfile(cached, obj)
                opt_stamp(obj).write_text(stamp_for(src))
                with lock:
                    cache_stats["hit"] += 1
                    state["done"] += 1
                return
        status, result = compile_one(src, opt)
        if status == "timeout" and opt == hot_opt and hot_opt != chunk_opt:
            # a hot file too slow at /O2: the normal flags, never unoptimised
            with lock:
                print(f"  SLOW: {src.name} exceeded {FILE_TIMEOUT}s at {opt}; retrying with {chunk_opt}")
            opt = chunk_opt
            status, result = compile_one(src, opt)
        if status == "timeout":
            # Some generated code sends MSVC's optimiser into near-endless
            # work; build that file unoptimised rather than stall the build.
            with lock:
                state["slow"].append(src.name)
                print(f"  SLOW: {src.name} exceeded {FILE_TIMEOUT}s at {opt}; retrying with {fallback_opt}")
            status, result = compile_one(src, fallback_opt)
        elif status == "failed" and opt != fallback_opt and result is not None and \
                compiler_crashed(result):
            # MSVC internal compiler error / out of heap: a compiler problem
            # with optimising this file, not a bug in it.  Build it unoptimised.
            with lock:
                state["slow"].append(src.name)
                print(f"  SLOW: {src.name} crashed the optimiser at {opt}; retrying with {fallback_opt}")
            status, result = compile_one(src, fallback_opt)
        tmp_obj = tmp_dir / (src.stem + tool.obj_suffix)
        if status == "ok" and (not tmp_obj.exists() or tmp_obj.stat().st_size == 0):
            # cl.exe once produced a 0-byte object with exit code 0 (the link
            # then failed with LNK1136).  Never accept an empty object: retry.
            with lock:
                print(f"  RETRY: {src.name} produced an empty object")
            status, result = compile_one(src, opt)
        with lock:
            if status != "ok" or not tmp_obj.exists() or tmp_obj.stat().st_size == 0:
                out = (result.stdout + result.stderr) if result else ""
                errors = [l for l in out.splitlines() if re.search(r"\berror C\d+", l)]
                state["error"] = f"{src.name}:\n" + ("\n".join(errors[:40]) or out[-4000:])
                return
            for attempt in range(20):   # antivirus / indexers briefly lock new files
                try:
                    tmp_obj.replace(obj)
                    if src.name.startswith("sw_chunk_"):
                        opt_stamp(obj).write_text(stamp_for(src))   # wanted flags, even after a fallback
                        if key:
                            try:
                                shutil.copyfile(obj, cache_dir / (key + tool.obj_suffix))
                                cache_stats["store"] += 1
                            except OSError:
                                pass
                    break
                except PermissionError:
                    if attempt == 19:
                        raise
                    time.sleep(0.5)
            state["warnings"] += sum(1 for l in result.stdout.splitlines() if "warning" in l.lower())
            state["done"] += 1
            times.append((time.time() - t0, src.name, opt))
            print(src.name)  # launcher counts these lines for its progress bar
            if state["done"] % 64 == 0 or state["done"] == total_jobs:
                print(f"  progress {state['done']}/{total_jobs}  ({time.time() - build_start:.0f}s)")

    workers = int(os.environ.get("SW_JOBS", "0")) or (os.cpu_count() or 4)
    print(f"Workers: {workers} (giant files at once: {big_jobs})")
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for fut in [pool.submit(worker, *job) for job in jobs]:
            fut.result()
    if state["error"]:
        print("ERROR: compile failed in " + state["error"], file=sys.stderr)
        return 1
    print(f"Compiled {state['done']} files ({state['warnings']} warnings)")
    if cache_dir:
        print(f"Object cache: {cache_stats['hit']} reused, {cache_stats['store']} stored ({cache_dir})")
    if times:
        times.sort(reverse=True)
        total_cpu = sum(t for t, _n, _o in times)
        top = times[:15]
        print(f"Slowest files ({sum(t for t, _n, _o in top) / max(total_cpu, 1e-9) * 100:.0f}% of "
              f"{total_cpu / 60:.0f} CPU-minutes):")
        for t, name, opt in top:
            print(f"  {t:7.1f}s  {name}  {opt}")
    if state["slow"]:
        print(f"Built unoptimised (timeout or compiler crash) ({len(state['slow'])}): {', '.join(state['slow'])}")

    print("Linking...")
    if msvc:
        # the exe's icon (Engine/Runtime/res/sw_app.rc -> game.ico): the game window and Explorer show it
        link_objects = list(objects)
        rc_src = RUNTIME_DIR / "res" / "sw_app.rc"
        rc_exe = shutil.which("rc", path=tool.env.get("PATH")) if tool.env else shutil.which("rc")
        if not rc_exe:   # the Windows SDK's newest x64 rc.exe, else LLVM's llvm-rc
            kits = sorted(Path(r"C:\Program Files (x86)\Windows Kits\10\bin").glob("10.*/x64/rc.exe"))
            llvm = Path(r"C:\Program Files\LLVM\bin\llvm-rc.exe")
            rc_exe = str(kits[-1]) if kits else (str(llvm) if llvm.is_file() else None)
        if rc_src.is_file() and rc_exe:
            res = OBJ_DIR / "sw_app.res"
            try:
                r = run([rc_exe, "/nologo", "/fo", str(res), str(rc_src)], tool.env)
                ok = r.returncode == 0 and res.is_file()
                why = (r.stdout + r.stderr)[-500:]
            except OSError as e:
                ok, why = False, str(e)
            if ok:
                link_objects.append(res)
            else:
                print("note: the resource compiler failed, the exe has no icon:", why)
        elif rc_src.is_file():
            print("note: no rc.exe / llvm-rc.exe found, the exe has no icon")
        rsp = OUT_DIR / "objects.rsp"
        rsp.write_text("\n".join(f'"{o}"' for o in link_objects), encoding="utf-8")
        libs = list(MSVC_LIBS)
        if have_vulkan:
            libs += ["shell32.lib", "winmm.lib", "bcrypt.lib", "psapi.lib", "pdh.lib", "rstrtmgr.lib",
                     "dwmapi.lib", "shcore.lib", "imm32.lib", "ole32.lib", "advapi32.lib",
                     "/link", "/LIBPATH:" + str(vk_sdk / "Lib"), "vulkan-1.lib"]
        if "/link" not in libs:
            libs.append("/link")
        libs.append("/MAP:" + str(OUT_DIR / "sw_app.map"))
        if pgo_lib:
            libs.append(str(pgo_lib))
        cmd = [tool.compiler, "/nologo", "@" + str(rsp), "/Fe:" + str(exe), *libs]
        if lto_mode:
            (WORK / "lto_cache").mkdir(exist_ok=True)
            cmd = [str(lld_link), "/nologo", "@" + str(rsp), "/OUT:" + str(exe), *[a for a in libs if a != "/link"],
                   "/opt:lldltojobs=all", "/lldltocache:" + str(WORK / "lto_cache")]
    else:
        cmd = tool.link_cmd(objects, exe)
    result = run(cmd, tool.env)
    if result.returncode != 0 or not exe.exists():
        print("ERROR: link failed", file=sys.stderr)
        print((result.stdout + result.stderr)[-4000:], file=sys.stderr)
        return 1

    # mb_cli.exe: the launcher's mod browser helper (Engine/Runtime/ui/mb_cli.cpp + the mb_*.cpp it shares with the
    # game); small, built after the game so a failure here never blocks the game build
    if msvc:
        cli = OUT_DIR / "mb_cli.exe"
        cli_src = [ui_dir / n for n in ("mb_cli.cpp", "mb_util.cpp", "mb_http.cpp", "mb_sources.cpp", "mb_install.cpp",
                                        "mb_browser.cpp")]
        newest_src = max(q.stat().st_mtime for q in cli_src + list(ui_dir.glob("mb_*.h")))
        if not cli.exists() or cli.stat().st_mtime < newest_src:
            cli_obj = OBJ_DIR / "mb_cli"
            cli_obj.mkdir(exist_ok=True)
            r = run([tool.compiler, "/nologo", "/O2", "/EHsc", "/std:c++17", "/W3", "/D_CRT_SECURE_NO_WARNINGS",
                     "/Fo" + str(cli_obj) + os.sep, "/Fe:" + str(cli), *[str(q) for q in cli_src],
                     "winhttp.lib", "bcrypt.lib", "shell32.lib"], tool.env)
            print("mb_cli.exe:", "built" if r.returncode == 0 and cli.exists() else
                  "FAILED (the launcher's mod browser is unavailable)\n" + (r.stdout + r.stderr)[-2000:])

    # libopus (BSD, Engine/Runtime/third_party/opus): the game's music and voices go through hwopus (sw_ipc.c),
    # which loads it from next to the exe at run time; without it they stay silent
    opus_dll = PROJECT_ROOT / "Engine" / "Runtime" / "third_party" / "opus" / "libopus-0.dll"
    if opus_dll.exists():
        dst = OUT_DIR / opus_dll.name
        if not dst.exists() or dst.stat().st_size != opus_dll.stat().st_size:
            shutil.copy2(opus_dll, dst)

    digest = hashlib.sha256()
    with exe.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    print("BUILD SUCCESS")
    print(f"  Executable: {exe}")
    print(f"  Size: {exe.stat().st_size} bytes")
    print(f"  SHA-256: {digest.hexdigest()}")
    print(f"  Objects: {len(objects)}")
    print(f"  Time: {time.time() - build_start:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
