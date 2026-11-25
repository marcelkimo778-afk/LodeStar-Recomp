#!/usr/bin/env python3
"""LodeStar launcher.

One window that walks through the pipeline in order and says what to do next:

    1. Game files   Game/exefs (+ romfs) present?
    2. Recompile    ARM64 -> C           (System/build_work/gen, ~10 min)
    3. Build        C -> Pokemon Arceus.exe (System/build_work/out, incremental)
    4. Play         run the game         (sw_app.exe --real-start)

Every task streams into the log panel and is also saved under
build_work/out/logs/, so a run can be read back after the window is closed.
When the game stops, the reason is pulled out of the output and shown in
plain words, together with the NVN (graphics) summary.  While it runs, the
"Boot progress" chips light up stage by stage (OS init, services, SDK init,
constructors, nnMain, graphics, first frame).  "Update & Play" chains
recompile (only if the translator changed) -> build -> play.
"""
from __future__ import annotations

import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import save_tools  # noqa: E402
import controls  # noqa: E402
import controls_ui  # noqa: E402
import mods_cfg  # noqa: E402
import pc_advice  # noqa: E402
import updater  # noqa: E402
import mods_ui  # noqa: E402
import shader_prep  # noqa: E402
import ui_kit as ui  # noqa: E402


# ---------------------------------------------------------------------------
# Paths and config (kept compatible with Config/config.json used by swrecomp)
# ---------------------------------------------------------------------------

def _project_root() -> Path:
    """Return the project root: the folder with LodeStar.exe, Game/, Mods/, Settings/ and System/ (this file is
    System/launcher/launcher.py, 2026-11-25 layout)."""
    return Path(__file__).resolve().parents[2]


SYSTEM_DIR = "System"            # everything technical: Engine/, launcher/, build_work/, Build/, tests/
GAME_EXE = "Pokemon Arceus.exe"  # the game (build_work/build.py writes it; its own icon, not the launcher's)


CONFIG_FILE = "Settings/config.json"
SETTINGS_FILE = "Settings/launcher.json"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)   # child processes never open a console window
MODULES = ("main", "sdk", "subsdk0", "subsdk1")


def _load_config() -> dict:
    path = _project_root() / CONFIG_FILE
    if path.is_file():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return {}


def _save_config(cfg: dict) -> None:
    path = _project_root() / CONFIG_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def _resolve_exefs() -> Path:
    cfg = _load_config()
    return (_project_root() / cfg.get("exefs_dir", "Game/exefs")).resolve()


def _resolve_romfs() -> Path | None:
    cfg = _load_config()
    if "romfs_dir" in cfg:
        return (_project_root() / cfg["romfs_dir"]).resolve()
    default = _project_root() / "Game/romfs"
    return default.resolve() if default.is_dir() else None


ROMFS_MARKERS = ("field", "pokemon", "archive")   # folders of bin/ (a dump with only the bin/ contents)


def romfs_check(romfs: Path | None, root: Path) -> tuple[bool, str, Path | None]:
    """Which RomFS dump this is and whether the game can start from it (same rules as sw_romfs.c).
    Accepted: the romfs folder (bin/ + system_resource/), the folder above it (<dir>/romfs), or a dump with only
    the contents of bin/ (some emulator dumps). system_resource/ (74 shader files) is required: the game crashes
    without it. Returns (ok, text, where the missing system_resource/ should go or None)."""
    if not romfs or not romfs.is_dir():
        return False, "missing - use 'Import game files' (Game files card)", None
    if not (romfs / "bin").is_dir() and (romfs / "romfs").is_dir():
        romfs = romfs / "romfs"
    if (romfs / "bin").is_dir():
        if (romfs / "system_resource").is_dir():
            return True, "complete", None
        return False, "system_resource/ (74 shader files) missing", romfs / "system_resource"
    if any((romfs / d).is_dir() for d in ROMFS_MARKERS):
        for p in (romfs / "system_resource", romfs.parent / "system_resource",
                  root / "roms_full" / "romfs" / "system_resource"):
            if p.is_dir():
                return True, f"bin/ contents only; shaders from {p}", None
        return False, ("bin/ contents only, system_resource/ (74 shader files) missing - dump the full RomFS "
                       "again (it holds romfs/system_resource)"), romfs.parent / "system_resource"
    return False, "not a RomFS dump (no bin/, field/, pokemon/ ...)", None


def find_dump(folder: Path) -> dict:
    """exefs/ and romfs/ in an extracted dump (any capitalisation, up to two folders down - nxdumptool and emulators
    nest them differently): {"exefs": Path | None, "romfs": Path | None, "why": text}."""
    out = {"exefs": None, "romfs": None, "why": ""}
    cands = [folder] + [d for d in folder.iterdir() if d.is_dir()] if folder.is_dir() else []
    cands += [e for d in cands[1:] for e in d.iterdir() if e.is_dir()]
    for d in cands:
        n = d.name.lower()
        if out["exefs"] is None and (n == "exefs" or (d / "main.npdm").is_file()) and (d / "main").is_file():
            out["exefs"] = d
        if out["romfs"] is None and n == "romfs" and d.is_dir():
            out["romfs"] = d
    if not out["exefs"]:
        out["why"] = "No ExeFS found (a folder with the files main and main.npdm)."
    elif not out["romfs"]:
        out["why"] = "No RomFS found (a folder named romfs)."
    return out


def exefs_version(exefs: Path) -> str:
    """The game version of a dump's main (its build id), or 'unknown (<build id>)'."""
    known = {"7FCAD279539DE183": "1.0.0", "AEE8F150DDA1B5A8": "1.1.1"}
    try:
        with open(exefs / "main", "rb") as f:
            head = f.read(0x60)
        bid = head[0x40:0x48].hex().upper() if head[:4] == b"NSO0" else ""
    except OSError:
        bid = ""
    return known.get(bid, f"unknown ({bid or 'not an NSO'})")


def game_files_check(root: Path) -> tuple[bool, str]:
    """ExeFS (all modules) and RomFS usable?  The Mods tab needs both, as Play does."""
    exefs = _resolve_exefs()
    missing = [m for m in MODULES if not (exefs / m).is_file()]
    problems = []
    if not exefs.is_dir() or missing:
        problems.append(f"ExeFS ({exefs}): " + ("folder missing" if not exefs.is_dir()
                                                   else "missing " + ", ".join(missing)))
    ok, text, _dest = romfs_check(_resolve_romfs(), root)
    if not ok:
        problems.append("RomFS: " + text)
    return (not problems, "\n".join(problems) or "ExeFS and RomFS complete")


def _resolve_work_dir() -> Path:
    cfg = _load_config()
    return (_project_root() / cfg.get("work_dir", "System/Build/Cache")).resolve()


CPU_CORE_CHOICES = ("auto", "3", "4", "6", "8", "12", "16")


FPS_CHOICES = ("30", "60", "90", "120", "240")

# Render resolution = the game's own output size: the Switch's handheld mode draws the picture at 1280x720, docked
# (TV) mode at 1920x1080.  The runtime reports the mode the game asks for (sw_ipc.c SW_OPERATION_MODE /
# SW_DISPLAY_RES); above 1080p the game keeps 1920x1080 (tested 2026-11-12).
RENDER_RES_CHOICES = ("1920x1080 (docked)", "1280x720 (handheld)")


# Render distance (built into the runtime, sw_romfs.c): how far away Pokemon, people and trees are drawn - the game's
# own field config values times the level's factor.  The in-game menu changes it too.
DRAW_DISTANCE_CHOICES = ("Original", "High (2x)", "Very high (3x)", "Ultra (5x)", "Max (10x)")


FRAMEGEN_CHOICES = ("Off",) + tuple(f"{n}x" for n in range(2, 11))


def framegen_env(choice) -> dict:
    """Environment for the frame generation setting (SW_FRAMEGEN 1 = off .. 10)."""
    v = str(choice or "")
    n = FRAMEGEN_CHOICES.index(v) + 1 if v in FRAMEGEN_CHOICES else 1
    return {"SW_FRAMEGEN": str(n)}


def draw_distance_env(choice) -> dict:
    """Environment for the render distance setting (SW_DRAW_DISTANCE level 0..4)."""
    v = str(choice or "")
    level = DRAW_DISTANCE_CHOICES.index(v) if v in DRAW_DISTANCE_CHOICES else 0
    return {"SW_DRAW_DISTANCE": str(level)}


SHADOW_CHOICES = ("Original", "2x", "3x", "4x")
PERF_OVERLAY_CHOICES = ("Off", "FPS", "Detailed")
ANISO_CHOICES = ("Original", "2x", "4x", "8x", "16x")
VSYNC_CHOICES = ("On", "Fast", "Off")
RES_SCALE_CHOICES = ("Original", "2x", "3x", "4x")
UPSCALE_CHOICES = ("Bilinear", "FSR 1")
SHARP_CHOICES = tuple(str(10 * i) for i in range(11))
COLORS_CHOICES = ("Original", "Vivid", "Rich")
# Presets (the in-game menu's Display > Preset): resolution scale, shadows, tree / detail / simulation distance, filtering.
# The higher ones load the graphics card and keep Simulation distance (processor work) low.
PRESETS = {
    "Performance": ("Original", 1, 0, 0, 0, "16x"),
    "Balanced": ("Original", 2, 2, 2, 1, "16x"),
    "Quality": ("2x", 4, 4, 4, 1, "16x"),
    "Ultra (GPU)": ("3x", 4, 4, 4, 2, "16x"),
}


def find_dlss_dll(roots=None):
    """nvngx_dlss.dll the player already has (another game's folder), newest first by modification time; None if none.
    Never shipped with this project: NVIDIA's DLSS model is licensed with the games that bundle it."""
    import os as _os
    from pathlib import Path as _P
    if roots is None:
        pf, pf86 = _os.environ.get("ProgramFiles", r"C:\Program Files"), _os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
        roots = [_P(pf86) / "Steam" / "steamapps" / "common", _P(pf) / "Epic Games", _P("C:/XboxGames"),
                 _P(pf) / "Steam" / "steamapps" / "common", _P("C:/Games"), _P("D:/SteamLibrary/steamapps/common"),
                 _P("D:/Games")]
    found = []
    for root in roots:
        root = _P(root)
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in _os.walk(root):
            if _os.path.relpath(dirpath, root).count(_os.sep) >= 5:
                dirnames[:] = []
            for f in filenames:
                if f.lower() == "nvngx_dlss.dll":
                    p = _P(dirpath) / f
                    found.append((_dll_version(p), p))
    return max(found)[1] if found else None


def _dll_version(path) -> tuple:
    """A DLL's file version (major, minor, build, private) on Windows; (0, 0, 0, mtime) elsewhere or without one."""
    try:
        import ctypes
        from ctypes import wintypes
        v = ctypes.windll.version
        n = v.GetFileVersionInfoSizeW(str(path), None)
        buf = ctypes.create_string_buffer(n)
        if n and v.GetFileVersionInfoW(str(path), 0, n, buf):
            ptr, ln = ctypes.c_void_p(), wintypes.UINT()
            if v.VerQueryValueW(buf, "\\", ctypes.byref(ptr), ctypes.byref(ln)) and ln.value >= 52:
                ms, ls = ctypes.cast(ptr.value + 8, ctypes.POINTER(wintypes.DWORD * 2)).contents
                return (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)
    except (OSError, AttributeError, ValueError):
        pass
    try:
        return (0, 0, 0, int(path.stat().st_mtime))
    except OSError:
        return (0, 0, 0, 0)


def preset_values(name: str) -> dict:
    """The launcher settings a preset sets."""
    res, sh, tree, det, sim, an = PRESETS[name]
    return {"res_scale": res, "shadow_quality": SHADOW_CHOICES[sh - 1], "draw_distance": DRAW_DISTANCE_CHOICES[tree],
            "detail_distance": DRAW_DISTANCE_CHOICES[det], "npc_distance": NPC_DISTANCE_CHOICES[sim], "anisotropy": an}


def picture_env(st: dict) -> dict:
    """Upscaling, sharpness and colours of the final picture (SW_UPSCALE 0/1, SW_SHARP 0..10, SW_COLORS 0..2)."""
    up, sh, co = st.get("upscaler", "FSR 1"), str(st.get("sharpness", "50")), st.get("colors", "Original")
    return {"SW_UPSCALE": "1" if up == "FSR 1" else "0",
            "SW_SHARP": str(SHARP_CHOICES.index(sh) if sh in SHARP_CHOICES else 5),
            "SW_COLORS": str(COLORS_CHOICES.index(co) if co in COLORS_CHOICES else 0)}


def res_scale_env(choice) -> dict:
    """Resolution scale: SW_RES_SCALE 1..4 (the game's screen-sized render targets that many times larger)."""
    v = str(choice or "Original")
    return {"SW_RES_SCALE": str(RES_SCALE_CHOICES.index(v) + 1 if v in RES_SCALE_CHOICES else 1)}


def aniso_env(choice) -> dict:
    """Texture filtering (anisotropic): SW_ANISO game|2|4|8|16 (default 16x)."""
    v = str(choice or "16x")
    return {"SW_ANISO": "game" if v == "Original" else v.rstrip("x") if v in ANISO_CHOICES else "16"}


def vsync_env(choice) -> dict:
    """V-Sync: On = fifo, Fast = mailbox (no tearing, no waiting), Off = immediate (may tear)."""
    v = str(choice or "On")
    return {"SW_VK_PRESENT": {"Fast": "mailbox", "Off": "immediate"}.get(v, "fifo")}


def perf_overlay_level(v) -> int:
    """The performance overlay setting as SW_FPS_OVERLAY's 0 / 1 / 2 (old settings kept a bool: True = FPS)."""
    if isinstance(v, bool):
        return 1 if v else 0
    v = str(v or "")
    return PERF_OVERLAY_CHOICES.index(v) if v in PERF_OVERLAY_CHOICES else 1


def perf_overlay_choice(v) -> str:
    return PERF_OVERLAY_CHOICES[perf_overlay_level(v)]
NPC_DISTANCE_CHOICES = ("Original (100 m)", "150 m", "200 m", "300 m", "Max (400 m)")


def npc_distance_env(choice) -> dict:
    """Environment for the people & pokemon distance setting (SW_NPC_DISTANCE level 0..4)."""
    v = str(choice or "")
    return {"SW_NPC_DISTANCE": str(NPC_DISTANCE_CHOICES.index(v) if v in NPC_DISTANCE_CHOICES else 0)}


def shadow_env(choice) -> dict:
    """Environment for the shadow quality setting (SW_SHADOW_SCALE 1..4: the sun shadow map size)."""
    v = str(choice or "")
    return {"SW_SHADOW_SCALE": str(SHADOW_CHOICES.index(v) + 1 if v in SHADOW_CHOICES else 1)}


def detail_distance_env(choice) -> dict:
    """Environment for the detail distance setting (SW_DETAIL_DISTANCE level 0..4): rocks, props and other objects."""
    v = str(choice or "")
    level = DRAW_DISTANCE_CHOICES.index(v) if v in DRAW_DISTANCE_CHOICES else 0
    return {"SW_DETAIL_DISTANCE": str(level)}


def adopt_ingame_settings(settings: dict, config_dir) -> bool:
    """A render distance chosen in the in-game menu (the runtime writes Config/ingame_settings.json): taken over into
    the launcher's settings when it is newer than the last one taken.  True when the settings changed."""
    import json
    from pathlib import Path as _P
    try:
        d = json.loads((_P(config_dir) / "ingame_settings.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    at, lv, fg, dl = d.get("changed_at", 0), d.get("draw_distance"), d.get("frame_gen"), d.get("detail_distance")
    sq = d.get("shadow_quality")
    nd = d.get("npc_distance")
    po = d.get("perf_overlay")
    an, pm, rs = d.get("anisotropy"), d.get("present_mode"), d.get("render_scale")
    up, shp, co = d.get("upscaler"), d.get("sharpness"), d.get("colors")
    dl = d.get("dlaa")
    if isinstance(dl, int) and dl in (0, 1):
        settings["dlaa"] = "On" if dl else "Off"
    if isinstance(up, int) and 0 <= up < len(UPSCALE_CHOICES):
        settings["upscaler"] = UPSCALE_CHOICES[up]
    if isinstance(shp, int) and 0 <= shp < len(SHARP_CHOICES):
        settings["sharpness"] = SHARP_CHOICES[shp]
    if isinstance(co, int) and 0 <= co < len(COLORS_CHOICES):
        settings["colors"] = COLORS_CHOICES[co]
    if at <= settings.get("ingame_seen", 0):
        return False
    if isinstance(lv, int) and 0 <= lv < len(DRAW_DISTANCE_CHOICES):
        settings["draw_distance"] = DRAW_DISTANCE_CHOICES[lv]
    if isinstance(fg, int) and 1 <= fg <= len(FRAMEGEN_CHOICES):
        settings["frame_gen"] = FRAMEGEN_CHOICES[fg - 1]
    if isinstance(dl, int) and 0 <= dl < len(DRAW_DISTANCE_CHOICES):
        settings["detail_distance"] = DRAW_DISTANCE_CHOICES[dl]
    if isinstance(nd, int) and 0 <= nd < len(NPC_DISTANCE_CHOICES):
        settings["npc_distance"] = NPC_DISTANCE_CHOICES[nd]
    if isinstance(sq, int) and 1 <= sq <= len(SHADOW_CHOICES):
        settings["shadow_quality"] = SHADOW_CHOICES[sq - 1]
    if isinstance(rs, int) and 1 <= rs <= len(RES_SCALE_CHOICES):
        settings["res_scale"] = RES_SCALE_CHOICES[rs - 1]
    if isinstance(an, int) and 0 <= an < len(ANISO_CHOICES):
        settings["anisotropy"] = ANISO_CHOICES[an]
    if isinstance(pm, int) and 0 <= pm < len(VSYNC_CHOICES):
        settings["vsync"] = VSYNC_CHOICES[pm]
    if isinstance(po, int) and 0 <= po < len(PERF_OVERLAY_CHOICES):
        settings["fps_overlay"] = PERF_OVERLAY_CHOICES[po]
    settings["ingame_seen"] = at
    return True


def render_env(choice) -> dict:
    """Environment for the render resolution setting."""
    v = str(choice or "")
    if v.startswith("1280"):
        return {"SW_OPERATION_MODE": "handheld", "SW_DISPLAY_RES": "1280x720"}
    return {"SW_OPERATION_MODE": "docked", "SW_DISPLAY_RES": "1920x1080"}


def fps_env(fps) -> dict:
    """SW_FPS for the frame-rate setting (30 = the game's own pacing, no hook)."""
    v = str(fps).strip()
    return {"SW_FPS": v} if v in FPS_CHOICES and v != "30" else {}


def cpu_env(mode: str, cores: str, host_cpus: int | None) -> dict:
    """Environment for the game's CPU mode.  multi: every guest thread on its own host
    thread (SW_MULTICORE) with N emulated cores, any core (SW_CORE_PIN=0); single: all
    guest threads take turns on one host core (fallback if multi-core misbehaves)."""
    if mode == "single":
        return {"SW_MULTICORE": "0"}
    n = host_cpus or 4
    if str(cores).isdigit() and int(cores) > 0:
        k = int(cores)
    else:                                     # auto: every logical CPU of the host, 3..16
        k = max(3, min(16, n))
    k = max(1, min(16, k))
    return {"SW_MULTICORE": "1", "SW_CORE_PIN": "0", "SW_CORES": str(k)}


def audio_env(enabled, volume) -> dict:
    """Sound settings -> the runtime's switches: SW_AUDIO_OUT=0 mutes (the silent service stub),
    SW_AUDIO_VOLUME=0..100 (sw_ipc.c, waveOut)."""
    if not enabled:
        return {"SW_AUDIO_OUT": "0"}
    try:
        v = int(float(volume))
    except (TypeError, ValueError):
        v = 100
    return {"SW_AUDIO_VOLUME": str(max(0, min(100, v)))}


# main module build ids (first 4 bytes of the NSO module id at 0x40) of the game versions; the runtime's fps /
# camera / water hooks are per build (src/sw_hooks.c)
GAME_BUILDS = {"7fcad279": "1.0.0", "aee8f150": "1.1.1"}


def game_version(main_nso: Path) -> tuple[str, str]:
    """(version, build id) of an ExeFS 'main' (NSO): '1.0.0' / '1.1.1', 'unknown' for another build, ''
    when the file is missing or not an NSO."""
    try:
        with open(main_nso, "rb") as f:
            head = f.read(0x60)
    except OSError:
        return "", ""
    if len(head) < 0x48 or head[:4] != b"NSO0":
        return "", ""
    bid = head[0x40:0x44].hex()
    return GAME_BUILDS.get(bid, "unknown"), bid


def built_game_version(gen_dir: Path) -> tuple[str, str]:
    """(version, build id) the generated code (gen/program.json, module 'main') was made from."""
    try:
        prog = json.loads((gen_dir / "program.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "", ""
    for m in prog.get("modules", []):
        if m.get("name") == "main":
            bid = str(m.get("module_id", ""))[:8]
            return GAME_BUILDS.get(bid, "unknown"), bid
    return "", ""


THEMES = ("Hisui paper", "Dark")


def _load_settings() -> dict:
    defaults = {"max_dispatch": "0", "watchdog": "0", "debug": False, "trace": False, "profile": False,
                "fast_build": False, "log_filter": "Important", "jobs": "0", "big_jobs": "2",
                # game window
                "window_size": "1280x720", "fullscreen": False, "gpu": "vulkan", "fps_overlay": "FPS",
                "cpu_mode": "multi", "cpu_cores": "auto", "pipe_prewarm": False, "fps": "30",
                "render_res": RENDER_RES_CHOICES[0], "draw_distance": DRAW_DISTANCE_CHOICES[0],
                "detail_distance": DRAW_DISTANCE_CHOICES[0], "shadow_quality": SHADOW_CHOICES[0],
                "npc_distance": NPC_DISTANCE_CHOICES[0], "frame_gen": FRAMEGEN_CHOICES[0],
                # sound
                "audio_on": True, "audio_volume": 100,
                # look
                "theme": THEMES[0],
                # extra diagnostics
                "ipc_log": False, "romfs_log": False, "oq_force": False, "peek": "", "watch": "",
                # launcher behaviour
                "animations": True, "autoscroll": True, "confirm_recompile": True, "check_updates": True,
                "max_log_lines": "20000", "keep_logs": "60"}
    path = _project_root() / SETTINGS_FILE
    try:
        defaults.update(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, json.JSONDecodeError):
        pass
    return defaults


def _save_settings(settings: dict) -> None:
    path = _project_root() / SETTINGS_FILE
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(settings, indent=2), encoding="utf-8")
    except OSError:
        pass


# ---------------------------------------------------------------------------
# Pure helpers (unit-tested in tests/test_launcher_logic.py)
# ---------------------------------------------------------------------------

def classify_line(line: str) -> str:
    """Return a log tag for one output line: error, warn, gfx, good, info."""
    low = line.lower()
    if low.startswith(("nvn:", "=== nvn")) or "nvn summary" in low:
        return "gfx"
    if re.search(r"\berror c\d+|^error|traceback|halted|=== execution stopped|fatal|\btrap\b", low):
        return "error"
    if "build success" in low or "nnmain: returned" in low or low.startswith("emitted "):
        return "good"
    if "warning" in low or low.startswith("  slow:") or "unimplemented" in low:
        return "warn"
    return "info"


def is_important(line: str, tag: str) -> bool:
    """Lines shown with the 'Important' filter: everything but per-file noise."""
    if tag != "info":
        return True
    s = line.strip()
    if re.fullmatch(r"sw_\w+\.c", s) or s.startswith(("init[", "  init[", "  RELOC", "DISPATCH:")):
        return False
    return True


@dataclass
class BuildProgress:
    total: int = 0
    done: int = 0
    slow: int = 0
    started: float = field(default_factory=time.time)
    phase: str = "starting"

    def feed(self, line: str) -> bool:
        """Update from one build.py output line; True if anything changed."""
        s = line.strip()
        m = re.match(r"Sources: (\d+) total, (\d+) to compile", s)
        if m:
            self.total, self.done, self.phase = int(m.group(2)), 0, "compiling"
            self.started = time.time()
            return True
        if self.phase == "compiling" and re.fullmatch(r"sw_\w+\.c", s):
            self.done += 1
            return True
        if s.startswith("SLOW:"):
            self.slow += 1
            return True
        if s.startswith("Linking"):
            self.phase = "linking"
            return True
        if s.startswith("BUILD SUCCESS"):
            self.phase = "done"
            return True
        return False

    @property
    def fraction(self) -> float:
        if self.phase in ("linking", "done"):
            return 1.0
        return self.done / self.total if self.total else 0.0

    def eta_seconds(self) -> float | None:
        if self.phase != "compiling" or self.done < 32 or not self.total:
            return None
        rate = self.done / max(1e-6, time.time() - self.started)
        return (self.total - self.done) / rate

    def describe(self) -> str:
        if self.phase == "starting":
            return "Preparing..."
        if self.phase == "linking":
            return "Linking Pokemon Arceus.exe (a few minutes)..."
        if self.phase == "done":
            return "Build finished"
        text = f"Compiling {self.done:,} / {self.total:,} files"
        eta = self.eta_seconds()
        if eta is not None:
            text += f"  -  about {format_duration(eta)} left"
        if self.slow:
            text += f"  -  {self.slow} slow file(s) built unoptimised"
        return text


def format_duration(seconds: float) -> str:
    seconds = int(seconds)
    if seconds < 60:
        return f"{seconds}s"
    if seconds < 3600:
        return f"{seconds // 60} min"
    return f"{seconds // 3600} h {seconds % 3600 // 60:02d} min"


def summarize_run(lines: list[str], returncode: int | None) -> str:
    """Turn the game's output into a short, readable result."""
    text = "\n".join(lines)
    out = []
    reached = [stage for stage, marker in (
        ("startup", "=== REAL STARTUP ==="),
        ("OS init", "nnosInitialize: returned"),
        ("constructors", "init_array:"),
        ("nninitStartup", "nninitStartup: returned"),
        ("SDK init", "nninitInitializeSdkModule: returned"),
        ("nnMain", "nnMain: calling"),
    ) if marker in text]
    ctors = len(re.findall(r"^  init\[", text, re.M))
    if ctors:
        out.append(f"Constructors run: {ctors}")
    abort = re.search(r"^SDK ABORT: (.+)$", text, re.M)
    if abort:
        out.append("SDK abort: " + abort.group(1).strip())
    if reached:
        out.append("Reached: " + " -> ".join(reached))
    reason = re.search(r"^reason\s*:\s*(.+)$", text, re.M) or \
        re.search(r"HALTED at 0x[0-9a-f]+: (.+)$", text, re.M)
    if reason:
        out.append("Stopped: " + reason.group(1).strip())
    func = re.search(r"^function\s*:\s*(.+)$", text, re.M)
    if func:
        out.append("In function: " + func.group(1).strip())
    nvn = re.search(r"=== NVN summary: (\d+) procs requested, (\d+) called ===", text)
    if nvn:
        out.append(f"Graphics (NVN): {nvn.group(1)} functions requested, {nvn.group(2)} called")
    elif "nvn: nvnBootstrapLoader(" in text:
        out.append("Graphics (NVN): bootstrap reached")
    else:
        out.append("Graphics (NVN): not reached yet")
    tracker = BootTracker()
    for line in lines:
        tracker.feed(line)
    if tracker.services:
        shown = ", ".join(tracker.services[:14]) + (" ..." if len(tracker.services) > 14 else "")
        out.append(f"System services opened ({len(tracker.services)}): {shown}")
    if tracker.calls:
        out.append(f"Function calls before stop: {tracker.calls:,}")
    if returncode is not None:
        out.append(f"Exit code: {returncode}" + ("  (normal exit)" if returncode == 0 else ""))
    return "\n".join(out) if out else "No output."


# Boot stages shown in the launcher, in order, with the runtime output line that
# proves the stage was reached.
BOOT_STAGES = (
    ("Loaded", "=== REAL STARTUP ==="),
    ("OS init", "nnosInitialize: returned"),
    ("Startup", "nninitStartup: returned"),
    ("Services", "[ipc] ConnectToNamedPort"),
    ("SDK init", "nninitInitializeSdkModule: returned"),
    ("Constructors", "  init["),
    ("nnMain", "nnMain: calling"),
    ("Assets", "[romfs] virtual image"),
    ("Graphics", "nvn: nvnBootstrapLoader("),
    ("GPU", "[gfx] vk: using gpu"),
    ("Drawing", "nvn: first ClearColor"),
    ("First frame", "nvn: present #1"),
)

WINDOW_SIZES = ("1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160")


@dataclass
class RecompileProgress:
    """Follows swrecomp output: four modules are analysed, then C is emitted."""
    # rough share of the total time each step takes (main dominates)
    WEIGHTS = (("main", 0.62), ("sdk", 0.08), ("subsdk0", 0.06), ("subsdk1", 0.09), ("emit", 0.15))
    done: list = field(default_factory=list)
    started: float = field(default_factory=time.time)

    def feed(self, line: str) -> bool:
        s = line.strip()
        m = re.match(r"module (\w+): (\d+) functions discovered", s)
        if m and m.group(1) not in self.done:
            self.done.append(m.group(1))
            return True
        if s.startswith("emitted ") and "emit" not in self.done:
            self.done.append("emit")
            return True
        return False

    @property
    def fraction(self) -> float:
        return min(1.0, sum(w for name, w in self.WEIGHTS if name in self.done))

    def describe(self) -> str:
        nxt = next((n for n, _ in self.WEIGHTS if n not in self.done), None)
        if nxt is None:
            return "Recompile finished"
        what = "Writing C files" if nxt == "emit" else f"Analysing module '{nxt}'"
        frac = self.fraction
        text = f"{what}  ({len(self.done)}/{len(self.WEIGHTS)} steps)"
        elapsed = time.time() - self.started
        if frac > 0.05:
            text += f"  -  about {format_duration(elapsed / frac - elapsed)} left"
        return text


def runtime_newer_than(exe: Path, runtime_dir: Path) -> bool:
    """True if a runtime source (.c/.h) changed after the exe was linked."""
    if not exe.is_file():
        return True
    built = exe.stat().st_mtime
    return any(p.stat().st_mtime > built for pat in ("*.c", "*.h") for p in runtime_dir.rglob(pat))


HISTORY_FILE = "Settings/run_history.json"


def make_history_entry(lines: list[str], returncode: int | None, started: float) -> dict:
    """One row of the run history: when, how far, why it stopped."""
    tracker = BootTracker()
    for line in lines:
        tracker.feed(line)
    text = "\n".join(lines[-400:])
    reason = re.search(r"^reason\s*:\s*(.+)$", text, re.M)
    abort = re.search(r"^SDK ABORT: (.+)$", "\n".join(lines), re.M)
    stop = reason.group(1).strip() if reason else (("SDK abort: " + abort.group(1).strip()) if abort else "")
    return {"when": datetime.fromtimestamp(started).strftime("%Y-%m-%d %H:%M"),
            "seconds": int(time.time() - started), "furthest": tracker.furthest,
            "stages": sum(tracker.reached), "calls": tracker.calls,
            "stop": stop, "exit": returncode}


def load_history(root: Path) -> list[dict]:
    try:
        data = json.loads((root / HISTORY_FILE).read_text(encoding="utf-8"))
        return data if isinstance(data, list) else []
    except (OSError, json.JSONDecodeError):
        return []


def save_history(root: Path, entries: list[dict], keep: int = 50) -> None:
    path = root / HISTORY_FILE
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(entries[-keep:], indent=1), encoding="utf-8")
    except OSError:
        pass


def prune_logs(log_dir: Path, keep: int) -> int:
    """Move all but the newest `keep` launcher logs into logs/old/ (never deletes)."""
    if keep <= 0 or not log_dir.is_dir():
        return 0
    logs = sorted((p for p in log_dir.glob("*.log") if p.is_file()), key=lambda p: p.stat().st_mtime)
    old = logs[:-keep] if len(logs) > keep else []
    if old:
        (log_dir / "old").mkdir(exist_ok=True)
    moved = 0
    for p in old:
        try:
            p.replace(log_dir / "old" / p.name)
            moved += 1
        except OSError:
            pass
    return moved


def git_info(root: Path) -> str:
    """'branch @ abc1234' (+ '*' if there are uncommitted changes), '' without git."""
    try:
        br = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=root, capture_output=True,
                            text=True, timeout=3, creationflags=NO_WINDOW).stdout.strip()
        sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=root, capture_output=True,
                             text=True, timeout=3, creationflags=NO_WINDOW).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "-uno"], cwd=root, capture_output=True,
                               text=True, timeout=5, creationflags=NO_WINDOW).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""
    return f"{br} @ {sha}{'*' if dirty else ''}" if sha else ""


class BootTracker:
    """Follows a running game's output and remembers how far it got."""

    def __init__(self) -> None:
        self.reached = [False] * len(BOOT_STAGES)
        self.services: list[str] = []
        self.calls = 0

    def feed(self, line: str) -> bool:
        changed = False
        for i, (_name, marker) in enumerate(BOOT_STAGES):
            if not self.reached[i] and marker in line:
                self.reached[i] = True
                changed = True
        m = re.search(r'sm:GetService\("([^"]*)"\)', line)
        if m and m.group(1) not in self.services:
            self.services.append(m.group(1))
            changed = True
        m = re.match(r"dispatch calls\s*:\s*(\d+)", line.strip())
        if m:
            self.calls = int(m.group(1))
            changed = True
        return changed

    @property
    def furthest(self) -> str:
        names = [n for (n, _), r in zip(BOOT_STAGES, self.reached) if r]
        return names[-1] if names else "not started"


def translator_newer_than(gen_marker: Path, source_dir: Path) -> bool:
    """True if any recompiler source changed after the last recompile."""
    if not gen_marker.is_file():
        return True
    built = gen_marker.stat().st_mtime
    return any(p.stat().st_mtime > built for p in source_dir.rglob("*.py"))


def free_memory_gb() -> tuple[float, float] | None:
    """(free, total) physical memory in GB, Windows only."""
    if sys.platform != "win32":
        return None
    import ctypes

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    st = MEMORYSTATUSEX()
    st.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
        return None
    return st.ullAvailPhys / 2**30, st.ullTotalPhys / 2**30


# ---------------------------------------------------------------------------
# Background task runner
# ---------------------------------------------------------------------------

def console_python(arg: str) -> str:
    """pythonw.exe -> python.exe. A pythonw child has no console, so every console tool it runs
    (cl.exe, link.exe, git...) pops up its own window. A console python started with
    CREATE_NO_WINDOW gets one hidden console that all of its children share."""
    p = Path(arg)
    if p.name.lower() == "pythonw.exe":
        alt = p.with_name("python.exe")
        if alt.exists():
            return str(alt)
    return arg


class Task:
    """A subprocess whose stdout/stderr lines are pushed onto a queue."""

    def __init__(self, name: str, cmd: list[str], cwd: Path, env: dict | None,
                 out_queue: "queue.Queue[tuple[str, str]]", log_path: Path):
        self.name, self.cmd, self.cwd, self.env = name, cmd, cwd, env
        self.queue = out_queue
        self.lines: list[str] = []
        self.returncode: int | None = None
        self.stopped = False
        self.started = time.time()
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_file = log_path.open("w", encoding="utf-8", errors="replace")
        self.log_path = log_path
        # no console window for any child (the launcher runs under pythonw; the game opens its own window)
        flags = (subprocess.CREATE_NEW_PROCESS_GROUP | NO_WINDOW) if sys.platform == "win32" else 0
        cmd = [console_python(c) for c in cmd]
        self.proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                     errors="replace", bufsize=1, creationflags=flags)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self) -> None:
        for raw in iter(self.proc.stdout.readline, ""):
            line = raw.rstrip("\r\n")
            self.lines.append(line)
            self.log_file.write(line + "\n")
            self.queue.put(("line", line))
        self.returncode = self.proc.wait()
        self.log_file.close()
        self.queue.put(("done", self.name))

    def stop(self) -> None:
        self.stopped = True
        if self.proc.poll() is None:
            if sys.platform == "win32":
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.proc.pid)],
                               capture_output=True, creationflags=NO_WINDOW)
            else:
                self.proc.kill()


# ---------------------------------------------------------------------------
# UI
# ---------------------------------------------------------------------------

def _colors() -> dict:
    """The theme's tokens (ui_kit.T) under the names the launcher and its dialogs use."""
    return {"bg": ui.T["page"], "card": ui.T["canvas"], "border": ui.T["hairline"], "text": ui.T["ink"],
            "muted": ui.T["inkMuted48"], "accent": ui.T["primary"], "good": ui.T["successText"],
            "warn": ui.T["warningText"], "error": ui.T["dangerText"], "gfx": ui.T["primary"]}


COLORS = _colors()   # refreshed in place when the theme is chosen (LauncherApp.__init__)


class LauncherApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root_dir = _project_root()
        self.work = self.root_dir / SYSTEM_DIR / "build_work"
        self.out_dir = self.work / "out"
        self.exe = self.out_dir / (GAME_EXE if sys.platform == "win32" else "sw_app")
        self.settings = _load_settings()
        self.queue: queue.Queue[tuple[str, str]] = queue.Queue()
        self.task: Task | None = None
        self.progress: BuildProgress | None = None
        self.all_lines: list[tuple[str, str]] = []
        self.pipeline: list = []            # remaining steps of a chain (after an update, shader preparation)
        self.shader_progress = None         # Build Shaders progress (shader_prep.ShaderPrepProgress)
        self.gpu_key = None                 # sw_app.exe --gpu-info, read once in the background
        self._gpu_probe_started = False
        self._last_mem = 0.0
        self.recomp_progress: RecompileProgress | None = None
        self.history = load_history(self.root_dir)
        # animation state
        self._tick = 0
        self._bar_target = 0.0              # 0..1000, the bar eases towards it
        self._bar_value = 0.0
        self._flash: tuple[float, str] | None = None   # (start time, colour) of the result-card glow
        self._git = git_info(self.root_dir)
        moved = prune_logs(self.out_dir / "logs", int(self.settings.get("keep_logs", "60") or 0))

        self._apply_theme()
        self._logo_img = None
        self._set_icon()
        root.title("LodeStar")
        root.geometry("1120x860")
        root.minsize(960, 760)
        ui.ANIM["on"] = bool(self.settings.get("animations", True))
        root.configure(bg=COLORS["bg"])
        root.report_callback_exception = self._on_tk_error
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self._styles()
        self._build_ui()
        self.refresh_status()
        if moved:
            self.log(f"[launcher] moved {moved} old log file(s) to logs/old", "system")
        self._bind_keys()
        tab = int(self.settings.get("tab", 0) or 0)
        if tab in (1, 2, 3):
            self.seg.select(tab)
        self._show_window()
        self.root.after(100, self.poll_output)
        self.root.after(33, self._animate)
        self.root.after(800, self._after_update)                 # an update just applied: its rebuild
        if self.settings.get("check_updates", True):
            self.root.after(1500, self._check_update)             # every start, in the background

    # -- animation ---------------------------------------------------------
    @staticmethod
    def _mix(a: str, b: str, t: float) -> str:
        """Blend two #rrggbb colours (t = 0 -> a, 1 -> b)."""
        return ui.mix(a, b, t)

    # -- look: theme, logo, the swipe between themes -------------------------------------------
    def _apply_theme(self) -> None:
        """ui_kit tokens + COLORS for the chosen theme, and the Windows title bar to match."""
        ui.set_theme("dark" if self.settings.get("theme") == "Dark" else "paper")
        COLORS.update(_colors())
        self._title_bar()

    def _hwnd(self) -> int:
        """The launcher's top-level Windows window (Tk's wrapper around its client window)."""
        import ctypes
        return ctypes.windll.user32.GetParent(self.root.winfo_id()) or self.root.winfo_id()

    def _title_bar(self) -> None:
        if sys.platform != "win32":
            return
        try:   # DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 10 20H1+): dark or light title bar
            import ctypes
            on = ctypes.c_int(1 if self.settings.get("theme") == "Dark" else 0)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(self._hwnd(), 20, ctypes.byref(on), ctypes.sizeof(on))
        except (OSError, AttributeError, tk.TclError):
            pass

    def _show_window(self) -> None:
        """First appearance: laid out at its real size while hidden, then shown (fading in with animations on)."""
        anim = bool(self.settings.get("animations", True))
        try:
            self.root.update_idletasks()
            if anim:
                self.root.attributes("-alpha", 0.0)
            self.root.deiconify()
            ui.win_icon(self.root)
            self._title_bar()
        except tk.TclError:
            return
        if not anim:
            return
        t0 = time.perf_counter()

        def step():
            t = (time.perf_counter() - t0) / 0.22
            try:
                self.root.attributes("-alpha", min(1.0, ui.ease_out(t)))
            except tk.TclError:
                return
            if t < 1:
                self.root.after(ui.FRAME_MS, step)
        self.root.after(ui.FRAME_MS, step)

    def _set_icon(self) -> None:
        """The launcher's emblem (launcher/assets, drawn by Engine/Tools/make_logos.py) on the window and taskbar."""
        assets = Path(__file__).resolve().parent / "assets"
        try:
            if (assets / "launcher.ico").is_file() and sys.platform == "win32":
                self.root.iconbitmap(default=str(assets / "launcher.ico"))
                ui.set_app_icon(str(assets / "launcher.ico"))   # every window: title bar, taskbar, Alt+Tab
            if (assets / "launcher.png").is_file():
                self._logo_img = tk.PhotoImage(file=str(assets / "launcher.png"))
                self.root.iconphoto(True, self._logo_img)
        except tk.TclError:
            self._logo_img = None

    def _logo(self, parent, size: int, bg: str) -> tk.Label | None:
        """The emblem at about size px (the PNG is 512 px: integer subsampling)."""
        if not self._logo_img:
            return None
        k = max(1, round(512 / size))
        img = self._logo_img.subsample(k, k)
        lb = tk.Label(parent, image=img, bg=bg, bd=0)
        lb.image = img
        return lb

    def toggle_theme(self) -> None:
        self.switch_theme("Dark" if self.settings.get("theme") != "Dark" else THEMES[0])

    def switch_theme(self, name: str) -> None:
        """Change the theme live with a clean wipe from left to right.  A picture of the window as it was sits in a
        borderless window exactly on top of it; the launcher is rebuilt in the new theme underneath; then the
        picture window's visible area (its window region) is cut from the left, uncovering the real new launcher
        behind one thin gold line.  The picture never moves (moving the window and its picture each frame made the
        old side jitter), and nothing in the launcher repaints."""
        if name not in THEMES or name == self.settings.get("theme") or getattr(self, "_swiping", False):
            return
        self._swiping = True
        self.settings["theme"] = name
        self._store_settings()
        anim = bool(self.settings.get("animations", True))
        shot = ui.snapshot(self.root) if anim else None
        if shot is None:                                   # no picture (or animations off): just switch
            self._rebuild_ui()
            self._swiping = False
            return
        W, H = shot.width(), shot.height()
        rx, ry = self.root.winfo_rootx(), self.root.winfo_rooty()
        ov = tk.Toplevel(self.root)
        ov.withdraw()
        ov.overrideredirect(True)
        ov.transient(self.root)
        try:
            ov.attributes("-topmost", bool(self.root.attributes("-topmost")))   # above the launcher in any case
        except tk.TclError:
            pass
        ov.geometry(f"{W}x{H}+{rx}+{ry}")
        cv = tk.Canvas(ov, width=W, height=H, highlightthickness=0, bd=0, bg=ui.T["page"])
        cv.place(x=0, y=0, width=W, height=H)
        img = cv.create_image(0, 0, image=shot, anchor="nw")
        cv.image = shot
        new = ui.DARK if name == "Dark" else ui.PAPER
        line = cv.create_rectangle(0, 0, 2, H, fill=new["gold"], width=0)
        ov.deiconify()
        ov.lift()
        ov.update()                                        # the picture is on screen before anything changes
        self._rebuild_ui()
        try:                                               # the new look painted completely under the picture
            self.root.update_idletasks()
            self.root.update()
        except tk.TclError:
            pass
        ov.lift()
        ov_hwnd = 0
        if sys.platform == "win32":
            try:
                import ctypes
                ov_hwnd = ctypes.windll.user32.GetParent(ov.winfo_id()) or ov.winfo_id()
            except (OSError, AttributeError, tk.TclError):
                ov_hwnd = 0
        ms = 480
        t0 = [0.0]
        hires = sys.platform == "win32"
        if hires:   # 1 ms timers while it runs (Tk's after() otherwise waits in 15.6 ms steps: ~32 fps)
            try:
                import ctypes
                ctypes.windll.winmm.timeBeginPeriod(1)
            except (OSError, AttributeError):
                hires = False

        def finish():
            self._swiping = False
            if hires:
                try:
                    import ctypes
                    ctypes.windll.winmm.timeEndPeriod(1)
                except (OSError, AttributeError):
                    pass

        def ease(t: float) -> float:                       # smooth in and out: a calm, even swipe
            t = max(0.0, min(1.0, t))
            return t * t * (3 - 2 * t)

        def step():
            if not t0[0]:
                t0[0] = time.perf_counter()
            t = (time.perf_counter() - t0[0]) * 1000 / ms
            x = round(W * ease(t))
            try:
                if t >= 1 or x >= W - 1:
                    ov.destroy()
                    finish()
                    return
                if ov_hwnd:   # cut the visible part from the left: the picture itself never moves (no jitter)
                    import ctypes
                    rgn = ctypes.windll.gdi32.CreateRectRgn(x, 0, W, H)
                    ctypes.windll.user32.SetWindowRgn(ov_hwnd, rgn, True)   # the system owns rgn from here
                    cv.coords(line, x, 0, x + 2, H)
                else:
                    ov.geometry(f"{W - x}x{H}+{rx + x}+{ry}")
                    cv.coords(img, -x, 0)
                    cv.coords(line, 0, 0, 2, H)
                ov.update_idletasks()
            except tk.TclError:
                finish()
                return
            # the next frame on a fixed 60 Hz clock (not "16 ms after this one": no drift, no uneven steps)
            el = (time.perf_counter() - t0[0]) * 1000
            nxt = (int(el / 16.667) + 1) * 16.667
            self.root.after(max(1, int(nxt - el)), step)

        self.root.after(30, step)

    def _rebuild_ui(self) -> None:
        """Destroy and rebuild the main window's widgets in the current theme; every Tk variable (status, hints,
        options, progress text) and the log come back as they were.  Dialogs stay as they are."""
        self._store_settings()
        keep = {k: v.get() for k, v in vars(self).items() if isinstance(v, tk.Variable)}
        tab = self.seg.index
        bar_mode = str(self.bar.cget("mode"))
        status = (self.status_lbl._text, getattr(self, "_status_colour", "good"))
        for w in self.root.winfo_children():
            if not isinstance(w, tk.Toplevel) and not getattr(w, "_curtain", False):
                w.destroy()
        self._apply_theme()
        self.root.configure(bg=COLORS["bg"])
        self._styles()
        self._build_ui()
        for k, val in keep.items():
            v = getattr(self, k, None)
            if isinstance(v, tk.Variable):
                try:
                    v.set(val)
                except tk.TclError:
                    pass
        self.refresh_status()
        self._refilter()
        self._paint_stages()
        self._describe_options()
        self._set_buttons()
        self.status_lbl.set(status[0], status[1], busy=self.task is not None and status[1] == "accent")
        self.bar.configure(mode=bar_mode)
        if bar_mode == "indeterminate" and self.task is not None:
            self.bar.start()
        self._set_bar(self._bar_target, instant=True)
        if tab:
            self.seg.select(tab, notify=False)
            self._show_panel(tab)

    def _animate(self) -> None:
        """33 ms UI tick: eased progress bar and the breathing 'now' boot chip (the widgets in ui_kit
        animate their own hover / press / slide)."""
        import math
        self._tick += 1
        anim = bool(self.settings.get("animations", True))
        ui.ANIM["on"] = anim
        if str(self.bar.cget("mode")) == "determinate":
            if anim:
                self._bar_value += (self._bar_target - self._bar_value) * 0.18
                if abs(self._bar_target - self._bar_value) < 0.5:
                    self._bar_value = self._bar_target
            else:
                self._bar_value = self._bar_target
            self.bar.configure(value=self._bar_value)
        if self.task is not None and self.task.name == "play" and "now" in self.chips.states:
            self.chips.pulse = (math.sin(self._tick / 5.0) + 1) / 2 if anim else 0.0
            self.chips.draw()
        if self._flash:
            _start, colour = self._flash
            self._flash = None
            self.hero.flash(colour)
            self.boot_card.flash(colour)
        self.root.after(33, self._animate)

    def _set_bar(self, value: float, instant: bool = False) -> None:
        self._bar_target = max(0.0, min(1000.0, value))
        if instant:
            self._bar_value = self._bar_target
            self.bar.configure(value=self._bar_value)

    def _bind_keys(self) -> None:
        self.root.bind("<F5>", lambda _e: self.task is None and self.update_info and self.on_update())
        self.root.bind("<F6>", lambda _e: self.task is None and self.exe.is_file() and self.on_play())
        self.root.bind("<Control-b>", lambda _e: self.task is None and self.on_build())
        self.root.bind("<Escape>", self._on_escape)
        self.root.bind("<F11>", lambda _e: self.toggle_fullscreen())
        self.root.bind("<Control-comma>", lambda _e: self.on_options())
        self.root.bind("<Control-l>", lambda _e: self.on_clear_log())
        for i, key in enumerate(("<Control-Key-1>", "<Control-Key-2>", "<Control-Key-3>", "<Control-Key-4>")):
            self.root.bind(key, lambda _e, i=i: self.seg.select(i))

    def toggle_fullscreen(self) -> None:
        """F11: the launcher fills its monitor (again: back to the window).  On Windows the frame is removed and the
        window stretched over the monitor in place - it is never hidden, so no close / open animation plays (Tk's
        own -fullscreen hides and re-shows the window)."""
        fs = getattr(self, "_fs", None)
        if sys.platform != "win32":
            try:
                self.root.attributes("-fullscreen", fs is None)
                self._fs = None if fs is not None else (0, (0, 0, 0, 0))
            except tk.TclError:
                pass
            return
        import ctypes
        import ctypes.wintypes as wt
        u = ctypes.windll.user32
        u.GetWindowLongW.restype = ctypes.c_long
        GWL_STYLE, WS_CAPTION, WS_THICKFRAME = -16, 0x00C00000, 0x00040000
        SWP = 0x0004 | 0x0200 | 0x0020                     # NOZORDER | NOOWNERZORDER | FRAMECHANGED
        try:
            hwnd = self._hwnd()
            if fs is None:                                  # into fullscreen
                style = u.GetWindowLongW(hwnd, GWL_STYLE)
                r = wt.RECT()
                u.GetWindowRect(hwnd, ctypes.byref(r))
                self._fs = (style, (r.left, r.top, r.right - r.left, r.bottom - r.top))

                class MI(ctypes.Structure):
                    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT),
                                ("dwFlags", wt.DWORD)]
                mi = MI(ctypes.sizeof(MI))
                u.GetMonitorInfoW(u.MonitorFromWindow(hwnd, 2), ctypes.byref(mi))
                m = mi.rcMonitor
                u.SetWindowLongW(hwnd, GWL_STYLE, ctypes.c_long(style & ~(WS_CAPTION | WS_THICKFRAME)))
                u.SetWindowPos(hwnd, 0, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP)
            else:                                           # back to the window it was
                style, (x, y, w, h) = fs
                self._fs = None
                u.SetWindowLongW(hwnd, GWL_STYLE, ctypes.c_long(style))
                u.SetWindowPos(hwnd, 0, x, y, w, h, SWP)
        except (OSError, AttributeError, tk.TclError):
            pass

    def _on_escape(self, _e=None) -> None:
        """Esc leaves fullscreen first; otherwise it stops the running task (as before)."""
        if getattr(self, "_fs", None) is not None:
            self.toggle_fullscreen()
            return
        if self.task is not None:
            self.on_stop()

    # -- styling (ttk parts: dialogs, entries, combo boxes, check boxes, tables) -------------
    def _styles(self) -> None:
        s = ttk.Style()
        s.theme_use("clam")
        c, t = COLORS, ui.T
        base = ui.F(10)
        s.configure(".", background=c["bg"], foreground=c["text"], font=base)
        s.configure("Card.TFrame", background=c["card"])
        s.configure("TFrame", background=c["bg"])
        s.configure("Card.TLabel", background=c["card"], foreground=c["text"], font=base)
        s.configure("Muted.TLabel", background=c["card"], foreground=c["muted"], font=ui.F(9))
        s.configure("Head.TLabel", background=c["card"], foreground=c["text"], font=ui.F(13, 600))
        s.configure("Title.TLabel", background=c["bg"], foreground=c["text"], font=ui.F(22, 700))
        s.configure("Hint.TLabel", background=c["card"], foreground=c["text"], font=ui.F(18, 700))
        # buttons in dialogs: secondary pill-like (flat parchment, blue when pressed / default)
        s.configure("TButton", background=t["parchment"], foreground=c["text"], padding=(14, 6), borderwidth=0,
                    focuscolor=t["parchment"], font=ui.F(10, 600), relief="flat")
        s.map("TButton", background=[("disabled", t["parchment"]), ("pressed", t["track"]), ("active", t["track"])],
              foreground=[("disabled", c["muted"])])
        for name in ("Big.TButton", "Play.TButton"):
            s.configure(name, background=t["primary"], foreground="white", font=ui.F(11, 600), padding=(18, 8),
                        borderwidth=0, focuscolor=t["primary"])
            s.map(name, background=[("disabled", t["parchment"]), ("active", t["primaryHover"])],
                  foreground=[("disabled", c["muted"])])
        s.configure("Stop.TButton", background=t["dangerSoft"], foreground=t["dangerText"], font=ui.F(11, 600),
                    padding=(18, 8), borderwidth=0)
        s.map("Stop.TButton", background=[("disabled", t["parchment"]), ("active", t["dangerHover"])],
              foreground=[("disabled", c["muted"])])
        s.configure("TCheckbutton", background=c["card"], foreground=c["text"], font=base,
                    indicatorbackground=c["card"], indicatorforeground=t["primary"])
        s.map("TCheckbutton", background=[("active", c["card"])],
              indicatorbackground=[("selected", t["primary"]), ("active", t["parchment"])],
              indicatorforeground=[("selected", "white")])
        s.configure("Horizontal.TProgressbar", troughcolor=t["track"], background=t["primary"],
                    borderwidth=0, thickness=6)
        s.configure("TCombobox", fieldbackground=t["parchment"], background=t["parchment"], foreground=c["text"],
                    arrowcolor=t["inkMuted80"], selectbackground=t["parchment"], selectforeground=c["text"],
                    bordercolor=t["hairline"], lightcolor=t["parchment"], darkcolor=t["parchment"], padding=4)
        s.map("TCombobox", fieldbackground=[("readonly", t["parchment"])], foreground=[("readonly", c["text"])],
              background=[("readonly", t["parchment"]), ("active", t["track"])],
              bordercolor=[("focus", t["primary"])])
        self.root.option_add("*TCombobox*Listbox.background", c["card"])
        self.root.option_add("*TCombobox*Listbox.foreground", c["text"])
        self.root.option_add("*TCombobox*Listbox.selectBackground", t["primary"])
        self.root.option_add("*TCombobox*Listbox.font", base)
        for sb in ("TScrollbar", "Vertical.TScrollbar", "Horizontal.TScrollbar"):
            s.configure(sb, background=t["track"], troughcolor=c["card"], arrowcolor=t["inkMuted48"],
                        bordercolor=c["card"], lightcolor=t["track"], darkcolor=t["track"], gripcount=0,
                        borderwidth=0, arrowsize=10)
            s.map(sb, background=[("active", t["hairline"])])
        s.configure("TEntry", fieldbackground=t["parchment"], foreground=c["text"], insertcolor=c["text"],
                    bordercolor=t["hairline"], lightcolor=t["parchment"], darkcolor=t["parchment"], padding=4)
        s.map("TEntry", bordercolor=[("focus", t["primary"])])
        s.configure("TNotebook", background=c["card"], borderwidth=0, tabmargins=(0, 0, 0, 8),
                    bordercolor=t["hairline"], lightcolor=c["card"], darkcolor=c["card"])
        s.configure("TNotebook.Tab", background=c["card"], foreground=c["muted"], padding=(14, 6), borderwidth=0,
                    font=ui.F(10, 600), bordercolor=c["card"], lightcolor=c["card"], darkcolor=c["card"],
                    focuscolor=c["card"])
        s.map("TNotebook.Tab", background=[("selected", t["parchment"]), ("active", t["pearl"])],
              foreground=[("selected", t["primary"]), ("active", c["text"])])
        s.configure("Treeview", background=c["card"], fieldbackground=c["card"], foreground=c["text"],
                    rowheight=26, borderwidth=0, font=base)
        s.configure("Treeview.Heading", background=t["parchment"], foreground=t["inkMuted80"], font=ui.F(9, 600),
                    borderwidth=0, relief="flat")
        s.map("Treeview", background=[("selected", t["primarySoft"])], foreground=[("selected", c["text"])])
        s.map("Treeview.Heading", background=[("active", t["track"])])
        s.configure("Treeview", bordercolor=t["hairline"], lightcolor=c["card"], darkcolor=c["card"])
        s.map("TNotebook.Tab", bordercolor=[("selected", t["hairline"]), ("!selected", c["card"])],
              lightcolor=[("selected", t["parchment"]), ("!selected", c["card"])])
        s.configure("Horizontal.TScale", troughcolor=t["track"], background=t["primary"], bordercolor=c["card"],
                    lightcolor=t["primary"], darkcolor=t["primary"], sliderthickness=14, borderwidth=0)
        s.map("Horizontal.TScale", background=[("active", t["primaryHover"])])

    def _card(self, parent, title: str, subtitle: str = "") -> tk.Frame:
        """A white rounded card in a panel; returns its body frame (ttk children use Card.* styles)."""
        card = ui.Card(parent, bg=COLORS["bg"])
        b = card.body
        tk.Label(b, text=title, font=ui.F(13, 600), bg=COLORS["card"], fg=COLORS["text"]).pack(anchor="w")
        if subtitle:
            tk.Label(b, text=subtitle, font=ui.F(9), bg=COLORS["card"], fg=COLORS["muted"], wraplength=420,
                     justify="left").pack(anchor="w", pady=(2, 8))
        b.card = card
        return b

    def _label(self, parent, var=None, text="", size=10, weight=400, muted=False, wrap=0, bg=None):
        lb = tk.Label(parent, textvariable=var, text=text, font=ui.F(size, weight), bg=bg or COLORS["card"],
                      fg=COLORS["muted"] if muted else COLORS["text"], justify="left", anchor="w",
                      wraplength=wrap)
        return lb

    # -- layout --------------------------------------------------------------
    def _build_ui(self) -> None:
        c, t, P = COLORS, ui.T, ui.SPACE
        page = tk.Frame(self.root, bg=c["bg"])
        page.pack(fill="both", expand=True, padx=P["xxl"], pady=(P["xl"], P["base"]))

        # header: large title, quiet subtitle, status pill
        top = tk.Frame(page, bg=c["bg"])
        top.pack(fill="x")
        mark = self._logo(top, 64, c["bg"])
        if mark:
            mark.pack(side="left", padx=(0, P["md"]))
        titles = tk.Frame(top, bg=c["bg"])
        titles.pack(side="left")
        tk.Label(titles, text="LodeStar", font=ui.F(24, 700), bg=c["bg"], fg=c["text"]).pack(anchor="w")
        tk.Label(titles, text="Native PC build of Pokémon Legends: Arceus  ·  by Kimo", font=ui.F(10), bg=c["bg"],
                 fg=c["muted"]).pack(anchor="w")
        self.status_var = tk.StringVar(value="Ready")
        status_col = tk.Frame(top, bg=c["bg"])                 # the status pill, and Uninstall (red) right under it
        status_col.pack(side="right", anchor="n", pady=(6, 0))
        self.status_lbl = ui.StatusPill(status_col, bg=c["bg"])
        self.status_lbl.pack(anchor="e")
        ui.PillButton(status_col, "Uninstall", lambda: self.on_uninstall(self.root), kind="dangerFill", size="sm",
                      bg=c["bg"]).pack(anchor="e", pady=(P["xs"], 0))
        self.status_lbl.set("Ready", "good")
        dark = self.settings.get("theme") == "Dark"
        # right to left: status pill, settings (gear), light / dark switch
        ui.PillButton(top, "⚙", self.on_options, kind="secondary", size="sm", width=40,
                      bg=c["bg"]).pack(side="right", anchor="n", pady=(7, 0), padx=(0, P["sm"]))
        ui.PillButton(top, "☀  Light" if dark else "☾  Dark", self.toggle_theme, kind="secondary", size="sm",
                      bg=c["bg"]).pack(side="right", anchor="n", pady=(7, 0), padx=(0, P["xs"]))

        # hero: what to do next, the big actions, progress
        self.hero = ui.Card(page, bg=c["bg"], pad=P["xl"], radius=ui.RADIUS["xl"])
        self.hero.pack(fill="x", pady=(P["lg"], 0))
        h = self.hero.body
        row = tk.Frame(h, bg=c["card"])
        row.pack(fill="x")
        left = tk.Frame(row, bg=c["card"])
        left.pack(side="left", fill="x", expand=True)
        self.hint_var = tk.StringVar()
        self._label(left, self.hint_var, size=18, weight=700).pack(anchor="w")
        self.task_var = tk.StringVar(value="Nothing running")
        self._label(left, self.task_var, size=10, muted=True).pack(anchor="w", pady=(2, 0))
        btns = tk.Frame(row, bg=c["card"])
        btns.pack(side="right")
        self.btn_play = ui.PillButton(btns, "Play", self.on_play, kind="primary", size="lg")
        self.btn_play.pack(side="left")
        # shown only when GitHub has a newer release (updater.py): "Update launcher" / "Update & rebuild" / ...
        self.btn_update = ui.PillButton(btns, "Update", self.on_update, kind="secondary", size="lg")
        self.update_info: dict | None = None
        self.btn_stop = ui.PillButton(btns, "Stop", self.on_stop, kind="danger", size="lg")
        self.btn_stop.pack(side="left", padx=(P["sm"], 0))
        self.btn_stop.configure(state="disabled")
        self.bar = ui.PillBar(h, maximum=1000, bg=c["card"])
        self.bar.pack(fill="x", pady=(P["lg"], P["sm"]))
        foot = tk.Frame(h, bg=c["card"])
        foot.pack(fill="x")
        self.detail_var = tk.StringVar()
        self._label(foot, self.detail_var, size=9, muted=True).pack(side="left")
        self._label(foot, text="F5 Update (when there is one)  ·  F6 Play  ·  Esc Stop  ·  F11 Fullscreen  ·  Ctrl+1…4 tabs", size=9,
                    muted=True).pack(side="right")

        # tabs
        self.seg = ui.Segmented(page, ["Overview", "Build", "Log", "Mods"], self._show_panel, bg=c["bg"], width=480)
        self.seg.pack(pady=(P["lg"], P["md"]))
        holder = tk.Frame(page, bg=c["bg"])
        holder.pack(fill="both", expand=True)
        self.panels = [tk.Frame(holder, bg=c["bg"]) for _ in range(4)]
        self._build_overview(self.panels[0])
        self._build_build(self.panels[1])
        self._build_log(self.panels[2])
        # Mods tab (launcher/mods_ui.py): built when first shown, only with working ExeFS + RomFS
        self.mods_panel = mods_ui.ModsPanel(self.panels[3], self.root_dir, self.exe, COLORS,
                                            lambda: game_files_check(self.root_dir), exefs=_resolve_exefs)
        self.panels[0].place(x=0, y=0, relwidth=1, relheight=1)

        self.footer_var = tk.StringVar()
        tk.Label(page, textvariable=self.footer_var, font=ui.F(9), bg=c["bg"], fg=c["muted"],
                 anchor="w").pack(fill="x", pady=(P["sm"], 0))

    def _show_panel(self, i: int) -> None:
        for k, p in enumerate(self.panels):
            if k != i:
                p.place_forget()
        self.panels[i].place(x=0, y=14, relwidth=1, relheight=1)
        ui.slide_in(self.panels[i])
        self.settings["tab"] = i
        if i == 3:
            self.mods_panel.show()

    def _build_overview(self, panel: tk.Frame) -> None:
        c, P = COLORS, ui.SPACE
        # quick actions
        tiles = tk.Frame(panel, bg=c["bg"])
        tiles.pack(fill="x")
        for i, (icon, title, sub, cmd) in enumerate((
                ("💾", "Saves & cheats", "Import, export, edit your save", self.on_saves),
                ("🎮", "Controls", "Keyboard, mouse and controller", self.on_controls),
                ("⚙", "Options", "Frame rate, window, CPU, debugging", self.on_options),
                ("🕘", "History", "Earlier runs and how far they got", self.on_history))):
            tile = ui.Tile(tiles, icon, title, sub, cmd)
            tile.grid(row=0, column=i, sticky="nsew", padx=(0 if i == 0 else P["md"], 0))
            tiles.columnconfigure(i, weight=1, uniform="tile")
        # boot progress + last result
        self.boot_card = ui.Card(panel, bg=c["bg"])
        self.boot_card.pack(fill="x", pady=(P["md"], 0))
        b = self.boot_card.body
        head = tk.Frame(b, bg=c["card"])
        head.pack(fill="x")
        tk.Label(head, text="Boot progress", font=ui.F(13, 600), bg=c["card"], fg=c["text"]).pack(side="left")
        self.boot_var = tk.StringVar(value="")
        self._label(head, self.boot_var, size=9, muted=True).pack(side="right")
        self.chips = ui.ChipRow(b, [name for name, _m in BOOT_STAGES], bg=c["card"])
        self.chips.pack(fill="x", pady=(P["md"], P["sm"]))
        self.stage_chips = self.chips.names            # (kept for older callers)
        self.tracker = BootTracker()
        self.result_var = tk.StringVar(value="No run yet.")
        self._label(b, self.result_var, size=10, wrap=900).pack(anchor="w")
        # options summary
        self.var_dispatch = tk.StringVar(value=self.settings["max_dispatch"])
        self.var_watchdog = tk.StringVar(value=self.settings["watchdog"])
        self.var_debug = tk.BooleanVar(value=self.settings["debug"])
        self.var_trace = tk.BooleanVar(value=self.settings["trace"])
        self.var_profile = tk.BooleanVar(value=self.settings["profile"])
        self.var_jobs = tk.StringVar(value=self.settings["jobs"])
        self.var_big_jobs = tk.StringVar(value=self.settings["big_jobs"])
        self.opts_var = tk.StringVar()
        self._label(panel, self.opts_var, size=9, muted=True, bg=c["bg"]).pack(anchor="w", pady=(P["md"], 0))
        self._paint_stages()
        self._describe_options()

    def _build_build(self, panel: tk.Frame) -> None:
        c, P = COLORS, ui.SPACE
        grid = tk.Frame(panel, bg=c["bg"])
        grid.pack(fill="both", expand=True)
        for k in (0, 1):
            grid.columnconfigure(k, weight=1, uniform="col")

        def place(body, r, col):
            body.card.grid(row=r, column=col, sticky="new", padx=(0 if col == 0 else P["md"], 0),
                           pady=(0 if r == 0 else P["md"], 0))

        def btn_row(body):
            f = tk.Frame(body, bg=c["card"])
            f.pack(anchor="w", pady=(P["md"], 0))
            return f

        b = self._card(grid, "Game files", "Your extracted ExeFS (code) and RomFS (assets).")
        place(b, 0, 0)
        self.files_var = tk.StringVar()
        self._label(b, self.files_var, size=10).pack(anchor="w")
        f = btn_row(b)
        ui.PillButton(f, "Change ExeFS…", self.on_select_exefs, kind="secondary", size="sm").pack(side="left")
        ui.PillButton(f, "Change RomFS…", self.on_select_romfs, kind="secondary", size="sm").pack(side="left", padx=6)
        ui.PillButton(f, "Import game files…", self.on_import_dump, kind="primary", size="sm").pack(side="left", padx=6)

        b = self._card(grid, "Recompile", "Translate the game's ARM code to C. Only needed after recompiler "
                       "changes (about 10 minutes).")
        place(b, 0, 1)
        self.gen_var = tk.StringVar()
        self._label(b, self.gen_var, size=10).pack(anchor="w")
        f = btn_row(b)
        self.btn_recompile = ui.PillButton(f, "Recompile", self.on_recompile, kind="secondary", size="sm")
        self.btn_recompile.pack(side="left")

        b = self._card(grid, "Build", "Compile the C code into the game (Pokemon Arceus.exe). Only changed files are rebuilt.")
        place(b, 1, 0)
        self.exe_var = tk.StringVar()
        self._label(b, self.exe_var, size=10).pack(anchor="w")
        self.xlate_var = tk.StringVar()
        self._label(b, self.xlate_var, size=9, muted=True, wrap=420).pack(anchor="w", pady=(4, 0))
        f = btn_row(b)
        self.btn_build = ui.PillButton(f, "Build", self.on_build, kind="primary", size="sm")
        self.btn_build.pack(side="left")
        self.btn_clean = ui.PillButton(f, "Clean rebuild", self.on_clean_build, kind="secondary", size="sm")
        self.btn_clean.pack(side="left", padx=6)
        self.btn_xlate = ui.PillButton(f, "Shader translator", self.on_build_xlate, kind="ghost", size="sm")
        self.btn_xlate.pack(side="left")
        self.var_fast = tk.BooleanVar(value=self.settings["fast_build"])
        ttk.Checkbutton(b, text="Fast build (unoptimised, the game runs slower)", variable=self.var_fast,
                        command=self._store_settings).pack(anchor="w", pady=(P["sm"], 0))

        b = self._card(grid, "Shaders", "Prepare the game's shaders before playing, so they do not stutter the "
                       "first time they appear. Stop cancels safely.")
        place(b, 1, 1)
        self.shader_var = tk.StringVar()
        self._label(b, self.shader_var, size=10, wrap=420).pack(anchor="w")
        f = btn_row(b)
        self.btn_shaders = ui.PillButton(f, "Build Shaders", self.on_shaders, kind="secondary", size="sm",
                                         width=150)
        self.btn_shaders.pack(side="left")
        self.var_prewarm = tk.BooleanVar(value=bool(self.settings.get("pipe_prewarm", False)))
        ttk.Checkbutton(b, text="Prewarm recorded pipelines at game start (experimental)",
                        variable=self.var_prewarm, command=self._store_prewarm).pack(anchor="w", pady=(P["sm"], 0))

        dev = tk.Frame(panel, bg=c["bg"])
        dev.pack(fill="x", pady=(P["md"], 0))
        tk.Label(dev, text="Developer", font=ui.F(9, 600), bg=c["bg"], fg=c["muted"]).pack(side="left")
        ui.PillButton(dev, "Run tests", self.on_tests, kind="ghost", size="sm", bg=c["bg"]).pack(side="left", padx=6)
        ui.PillButton(dev, "Test frame", self.on_test_frame, kind="ghost", size="sm", bg=c["bg"]).pack(side="left")

    def _build_log(self, panel: tk.Frame) -> None:
        c, t, P = COLORS, ui.T, ui.SPACE
        card = ui.Card(panel, bg=c["bg"], pad=P["base"])
        card.pack(fill="both", expand=True)
        card.unbind("<Configure>")
        card.body.unbind("<Configure>")
        card._auto = False                          # this one fills the panel instead

        def relayout(_e=None):                      # fill the panel (not the body's natural height)
            w, h = card.winfo_width(), card.winfo_height()
            card.itemconfigure(card._win, width=max(1, w - 2 * card._pad), height=max(1, h - 2 * card._pad))
            card._layout()
        card.bind("<Configure>", relayout)
        b = card.body
        head = tk.Frame(b, bg=c["card"])
        head.pack(fill="x")
        tk.Label(head, text="Log", font=ui.F(13, 600), bg=c["card"], fg=c["text"]).pack(side="left")
        self.var_filter = tk.StringVar(value=self.settings["log_filter"])
        flt = ttk.Combobox(head, textvariable=self.var_filter, state="readonly", width=12,
                           values=("Important", "Errors", "Graphics", "Everything"))
        flt.pack(side="left", padx=P["md"])
        flt.bind("<<ComboboxSelected>>", lambda _e: self._refilter())
        for text, cmd in (("Open build folder", self.on_open_build), ("Open log file", self.on_open_log),
                          ("Copy errors", self.on_copy_errors), ("Clear", self.on_clear_log)):
            ui.PillButton(head, text, cmd, kind="ghost", size="sm").pack(side="right", padx=(4, 0))
        holder = tk.Frame(b, bg=c["card"])
        holder.pack(fill="both", expand=True, pady=(P["sm"], 0))
        self.log_box = tk.Text(holder, bg=t["pearl"], fg=c["text"], insertbackground=c["text"],
                               font=("Consolas", 9), relief="flat", wrap="none", state="disabled",
                               borderwidth=0, padx=10, pady=8, highlightthickness=1,
                               highlightbackground=t["dividerSoft"], selectbackground=t["primarySoft"],
                               selectforeground=c["text"])
        ysb = ttk.Scrollbar(holder, orient="vertical", command=self.log_box.yview)
        xsb = ttk.Scrollbar(holder, orient="horizontal", command=self.log_box.xview)
        self.log_box.configure(yscrollcommand=ysb.set, xscrollcommand=xsb.set)
        ysb.pack(side="right", fill="y")
        xsb.pack(side="bottom", fill="x")
        self.log_box.pack(side="left", fill="both", expand=True)
        for tag in ("error", "warn", "gfx", "good"):
            self.log_box.tag_config(tag, foreground=c[tag])
        self.log_box.tag_config("info", foreground=c["text"])
        self.log_box.tag_config("system", foreground=c["muted"])

    def _paint_stages(self) -> None:
        running = self.task is not None and self.task.name == "play"
        nxt = next((i for i, r in enumerate(self.tracker.reached) if not r), None)
        last = max((i for i, r in enumerate(self.tracker.reached) if r), default=-1)
        states = []
        for i in range(len(self.chips.names)):
            if self.tracker.reached[i]:
                states.append("done")
            elif running and i == last + 1:
                states.append("now")
            elif not running and nxt is not None and i == last + 1 and last >= 0:
                states.append("stopped")
            else:
                states.append("idle")
        if states != self.chips.states:
            self.chips.states = states
            self.chips.draw()
        extra = []
        if self.tracker.services:
            extra.append(f"{len(self.tracker.services)} services")
        if self.tracker.calls:
            extra.append(f"{self.tracker.calls:,} calls")
        self.boot_var.set(("Furthest: " + self.tracker.furthest) +
                          ("  ·  " + ", ".join(extra) if extra else ""))

    def _describe_options(self) -> None:
        parts = []
        d = self.var_dispatch.get().strip()
        if d not in ("", "0"):
            parts.append(f"stops after {d} calls")
        w = self.var_watchdog.get().strip()
        if w not in ("", "0"):
            parts.append(f"watchdog {w}s")
        for flag, name in ((self.var_debug, "debug log"), (self.var_trace, "call trace"),
                           (self.var_profile, "profiling")):
            if flag.get():
                parts.append(name)
        st = self.settings
        win = "fullscreen" if st.get("fullscreen") else st.get("window_size", "1280x720")
        for key, name in (("ipc_log", "IPC log"), ("romfs_log", "file log")):
            if st.get(key):
                parts.append(name)
        if st.get("peek"):
            parts.append("peek")
        if st.get("watch"):
            parts.append("watch")
        self.opts_var.set(f"Window: {win}   ·   Options: " +
                          (", ".join(parts) if parts else "default (runs until closed)"))

    def on_options(self) -> None:
        """Tabbed settings: Game, Debugging, Build, Launcher."""
        c = COLORS
        win = tk.Toplevel(self.root)
        win.title("Options")
        ui.window_icon(win, "options")
        win.configure(bg=c["card"])
        win.transient(self.root)
        win.resizable(False, False)
        outer = ttk.Frame(win, style="Card.TFrame", padding=14)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text="Options", style="Head.TLabel").pack(anchor="w", pady=(0, 8))
        nb = ttk.Notebook(outer)
        nb.pack(fill="both", expand=True)
        st = self.settings
        local = {
            "window_size": tk.StringVar(value=st.get("window_size", "1280x720")),
            "fullscreen": tk.BooleanVar(value=bool(st.get("fullscreen"))),
            "fps_overlay": tk.StringVar(value=perf_overlay_choice(st.get("fps_overlay", "FPS"))),
            "gpu": tk.StringVar(value=st.get("gpu", "vulkan")),
            "cpu_mode": tk.StringVar(value=st.get("cpu_mode", "multi")),
            "fps": tk.StringVar(value=str(st.get("fps", "30"))),
            "render_res": tk.StringVar(value=str(st.get("render_res", RENDER_RES_CHOICES[0]))),
            "draw_distance": tk.StringVar(value=str(st.get("draw_distance", DRAW_DISTANCE_CHOICES[0]))),
            "detail_distance": tk.StringVar(value=str(st.get("detail_distance", DRAW_DISTANCE_CHOICES[0]))),
            "shadow_quality": tk.StringVar(value=str(st.get("shadow_quality", SHADOW_CHOICES[0]))),
            "anisotropy": tk.StringVar(value=str(st.get("anisotropy", "16x"))),
            "res_scale": tk.StringVar(value=str(st.get("res_scale", "Original"))),
            "upscaler": tk.StringVar(value=str(st.get("upscaler", "FSR 1"))),
            "dlaa": tk.StringVar(value=str(st.get("dlaa", "Off"))),
            "sharpness": tk.StringVar(value=str(st.get("sharpness", "50"))),
            "colors": tk.StringVar(value=str(st.get("colors", "Original"))),
            "vsync": tk.StringVar(value=str(st.get("vsync", "On"))),
            "npc_distance": tk.StringVar(value=str(st.get("npc_distance", NPC_DISTANCE_CHOICES[0]))),
            "frame_gen": tk.StringVar(value=str(st.get("frame_gen", FRAMEGEN_CHOICES[0]))),
            "cpu_cores": tk.StringVar(value=str(st.get("cpu_cores", "auto"))),
            "ipc_log": tk.BooleanVar(value=bool(st.get("ipc_log"))),
            "romfs_log": tk.BooleanVar(value=bool(st.get("romfs_log"))),
            "oq_force": tk.BooleanVar(value=bool(st.get("oq_force"))),
            "peek": tk.StringVar(value=st.get("peek", "")),
            "watch": tk.StringVar(value=st.get("watch", "")),
            "audio_on": tk.BooleanVar(value=bool(st.get("audio_on", True))),
            "audio_volume": tk.IntVar(value=int(float(st.get("audio_volume", 100) or 0))),
            "theme": tk.StringVar(value=str(st.get("theme", THEMES[0]))),
            "animations": tk.BooleanVar(value=bool(st.get("animations", True))),
            "autoscroll": tk.BooleanVar(value=bool(st.get("autoscroll", True))),
            "confirm_recompile": tk.BooleanVar(value=bool(st.get("confirm_recompile", True))),
            "check_updates": tk.BooleanVar(value=bool(st.get("check_updates", True))),
            "max_log_lines": tk.StringVar(value=str(st.get("max_log_lines", "20000"))),
            "keep_logs": tk.StringVar(value=str(st.get("keep_logs", "60"))),
        }

        old_theme = st.get("theme", THEMES[0])

        def tab(title: str) -> ttk.Frame:
            """A tab whose content scrolls (scrollbar and mouse wheel) once it is taller than MAX_H."""
            MAX_H = 560
            holder = ttk.Frame(nb, style="Card.TFrame")
            nb.add(holder, text=f"  {title}  ")
            cv = tk.Canvas(holder, highlightthickness=0, bd=0, bg=c["card"])
            sb = ttk.Scrollbar(holder, orient="vertical", command=cv.yview)
            f = ttk.Frame(cv, style="Card.TFrame", padding=14)
            cv.create_window((0, 0), window=f, anchor="nw")
            cv.configure(yscrollcommand=sb.set)
            cv.pack(side="left", fill="both", expand=True)

            def fit(_e=None) -> None:
                cv.configure(scrollregion=cv.bbox("all"), width=f.winfo_reqwidth(),
                             height=min(f.winfo_reqheight(), MAX_H))
                if f.winfo_reqheight() > MAX_H:
                    if not sb.winfo_ismapped():
                        sb.pack(side="right", fill="y")
                elif sb.winfo_ismapped():
                    sb.pack_forget()

            f.bind("<Configure>", fit)

            def wheel(e) -> None:
                if f.winfo_reqheight() > MAX_H:
                    cv.yview_scroll(int(-e.delta / 120) or (-1 if e.delta > 0 else 1), "units")

            holder.bind("<Enter>", lambda _e: cv.bind_all("<MouseWheel>", wheel))
            holder.bind("<Leave>", lambda _e: cv.unbind_all("<MouseWheel>"))
            return f

        def row(parent, label: str, widget_factory, help_text: str = "") -> None:
            r = ttk.Frame(parent, style="Card.TFrame")
            r.pack(fill="x", pady=(8, 0))
            ttk.Label(r, text=label, style="Card.TLabel").pack(side="left")
            widget_factory(r).pack(side="left", padx=6)
            if help_text:
                ttk.Label(parent, text=help_text, style="Muted.TLabel", wraplength=420,
                          justify="left").pack(anchor="w")

        def check(parent, text: str, var, help_text: str = "") -> None:
            ttk.Checkbutton(parent, text=text, variable=var).pack(anchor="w", pady=(8, 0))
            if help_text:
                ttk.Label(parent, text=help_text, style="Muted.TLabel", wraplength=420,
                          justify="left").pack(anchor="w", padx=(22, 0))

        # --- Game -----------------------------------------------------------
        g = tab("Game")
        row(g, "Window size:", lambda p: ttk.Combobox(p, textvariable=local["window_size"],
                                                       values=WINDOW_SIZES, width=12),
            "Size of the game window (any WIDTHxHEIGHT works). The game's picture (Render resolution "
            "below) is scaled to it.")
        preset_var = tk.StringVar(value="Custom")

        def apply_preset(*_):
            name = preset_var.get()
            if name in PRESETS:
                for k, v in preset_values(name).items():
                    if k in local:
                        local[k].set(v)
        preset_var.trace_add("write", apply_preset)
        row(g, "Preset:", lambda p: ttk.Combobox(p, textvariable=preset_var, values=tuple(PRESETS),
                                                  state="readonly", width=14),
            "Fills in Resolution scale, Shadow quality, Tree / Detail / Simulation distance and Texture filtering. "
            "The higher presets put the extra work on the graphics card and keep Simulation distance (processor "
            "work) low. Change single options below afterwards if you like.")
        # --- recommended for this PC + where the extra work goes (pc_advice.py) ---
        try:
            hw = pc_advice.detect()
        except Exception:   # never let a hardware query break the Options window
            hw = None
        work_var = tk.StringVar(value="Custom")

        def apply_workload(*_):
            if hw and work_var.get() in pc_advice.WORKLOADS:
                for k, v in pc_advice.settings_for(hw, work_var.get()).items():
                    if k in local:
                        local[k].set(v)
        work_var.trace_add("write", apply_workload)
        if hw:
            box = ttk.Frame(g, style="Card.TFrame")
            box.pack(fill="x", pady=(12, 0))
            ttk.Label(box, text="Your PC", style="Card.TLabel").pack(anchor="w")
            ttk.Label(box, text=pc_advice.summary(hw), style="Muted.TLabel", justify="left").pack(anchor="w")
            ttk.Label(box, text=pc_advice.advice_text(hw), style="Muted.TLabel", wraplength=420,
                      justify="left").pack(anchor="w", pady=(4, 0))
            ttk.Button(box, text="Use recommended",
                       command=lambda: work_var.set(pc_advice.recommend_workload(hw))).pack(anchor="w", pady=(6, 0))
            row(g, "Extra work on:", lambda p: ttk.Combobox(p, textvariable=work_var, values=pc_advice.WORKLOADS,
                                                            state="readonly", width=14),
                "Where the extra quality goes, sized for this PC. The game's own work (people, Pokemon, its "
                "threads) always runs on the processor and cannot move. Graphics card: sharper picture "
                "(resolution scale, DLAA, shadows, far detail), Simulation distance stays at the Switch's 100 m - "
                "best when the processor is the limit, as on most PCs. Processor: people and Pokemon loaded "
                "further away, light graphics options - for a weak graphics card. Balanced: some of both. Also "
                "sets a frame rate this processor holds steadily.")
        row(g, "Render resolution:", lambda p: ttk.Combobox(p, textvariable=local["render_res"],
                                                             values=RENDER_RES_CHOICES, state="readonly", width=20),
            "The resolution the game draws its picture at. 1920x1080 is the Switch's TV (docked) mode, "
            "1280x720 its handheld mode (lighter on the GPU). The game does not go above 1920x1080 by itself. "
            "Applies at the next start.")
        row(g, "Resolution scale:", lambda p: ttk.Combobox(p, textvariable=local["res_scale"],
                                                            values=RES_SCALE_CHOICES, state="readonly", width=10),
            "Draws the 3D world and the interface at 2x to 4x the render resolution above (2x of 1920x1080 = "
            "3840x2160), shown at the window's size: much sharper edges and distant detail, like 4K. Costs GPU time "
            "(2x: about 4 times the pixels). The in-game menu (Graphics) changes it while you play.")
        row(g, "Anti-aliasing (DLAA):", lambda p: ttk.Combobox(p, textvariable=local["dlaa"], values=("Off", "On"),
                                                                state="readonly", width=6),
            "NVIDIA DLAA (DLSS at the game's own resolution): smooth edges without shimmer, using an RTX card's AI "
            "cores. Needs nvngx_dlss.dll next to the game: the launcher copies one you already have from another "
            "game's folder (Steam, Xbox, Epic) when you play. It is never shipped with this project.")
        row(g, "Upscaling:", lambda p: ttk.Combobox(p, textvariable=local["upscaler"], values=UPSCALE_CHOICES,
                                                     state="readonly", width=10),
            "How the picture is scaled up when the window is larger than it: FSR 1 (AMD FidelityFX Super "
            "Resolution) keeps edges sharp, Bilinear is softer.")
        row(g, "Sharpness:", lambda p: ttk.Combobox(p, textvariable=local["sharpness"], values=SHARP_CHOICES,
                                                     state="readonly", width=6),
            "Sharpening after FSR upscaling, 0 = none.")
        row(g, "Colors:", lambda p: ttk.Combobox(p, textvariable=local["colors"], values=COLORS_CHOICES,
                                                  state="readonly", width=10),
            "Original, or a more vivid / rich picture (saturation and contrast of the final image).")
        row(g, "Texture filtering:", lambda p: ttk.Combobox(p, textvariable=local["anisotropy"],
                                                             values=ANISO_CHOICES, state="readonly", width=10),
            "Anisotropic filtering: ground and walls seen at a shallow angle stay sharp into the distance. 16x "
            "costs almost nothing on a PC GPU. The in-game menu (Graphics) changes it while you play.")
        row(g, "V-Sync:", lambda p: ttk.Combobox(p, textvariable=local["vsync"],
                                                  values=VSYNC_CHOICES, state="readonly", width=8),
            "On: no tearing, the picture waits for the screen. Fast: no tearing and no waiting (the newest "
            "picture is shown). Off: the lowest input delay, the picture may tear. Changes in the game at once.")
        row(g, "Frame generation:", lambda p: ttk.Combobox(p, textvariable=local["frame_gen"],
                                                            values=FRAMEGEN_CHOICES, state="readonly", width=8),
            "Shows 2x to 10x the game's frame rate on screen: pictures made in between the game's own from their "
            "motion. Limited by the monitor's refresh rate (60 fps on 144 Hz: 2x; 30 fps on 144 Hz: 4x). Adds a "
            "little input delay. The in-game menu (Graphics) changes it while you play.")
        row(g, "Tree distance:", lambda p: ttk.Combobox(p, textvariable=local["draw_distance"],
                                                         values=DRAW_DISTANCE_CHOICES, state="readonly", width=16),
            "How far away trees are drawn in full. Original is "
            "the Switch's distance; Max is 10x (like the draw distance mod). Higher costs performance. The "
            "in-game menu (Graphics) changes it while you play.")
        row(g, "Simulation distance:", lambda p: ttk.Combobox(p, textvariable=local["npc_distance"],
                                                               values=NPC_DISTANCE_CHOICES, state="readonly", width=16),
            "How far away people, Pokemon and the world around them are loaded and kept running: the Switch loads "
            "100 m around you, so a guard or camp further away is simply not there. Also keeps them during events "
            "(like the 'De-Spawn Radius' mod) and widens Jubilife Village's crowd. This is CPU work: higher lowers "
            "the frame rate on most PCs. The in-game menu (Graphics) changes it while you play.")
        row(g, "Shadow quality:", lambda p: ttk.Combobox(p, textvariable=local["shadow_quality"],
                                                          values=SHADOW_CHOICES, state="readonly", width=10),
            "Sharper sun shadows: the shadow map 2x to 4x larger on each side (Original is the Switch's 1024x1024). "
            "Costs some GPU memory, little speed. Applies at the next start.")
        row(g, "Detail distance:", lambda p: ttk.Combobox(p, textvariable=local["detail_distance"],
                                                           values=DRAW_DISTANCE_CHOICES, state="readonly", width=16),
            "How far away rocks, props and other objects keep their full detail, and thin far objects stay "
            "visible (the Switch drops them to their simplest look past 200 m). Higher costs performance. The "
            "in-game menu (Graphics) changes it while you play.")
        row(g, "Graphics:", lambda p: ttk.Combobox(p, textvariable=local["gpu"], values=("vulkan", "null"),
                                                    state="readonly", width=10),
            "vulkan: real rendering on your GPU (Vulkan, own window). null: no GPU work, the old "
            "debug window only shows the clear colour.")
        check(g, "Fullscreen (borderless)", local["fullscreen"],
              "Covers the whole primary monitor. Close it with Alt+F4 or Stop.")
        row(g, "Performance overlay:", lambda p: ttk.Combobox(p, textvariable=local["fps_overlay"],
                                                              values=PERF_OVERLAY_CHOICES, state="readonly", width=10),
            "FPS: frame rate and frame time in the top-left corner (F3 in the game). Detailed: 1% / 0.1% lows, a "
            "frame-time graph, CPU and GPU load, the busiest thread, memory, new shaders and what limits the frame "
            "rate (F10 in the game cycles Off, FPS, Detailed).")
        row(g, "Frame rate:", lambda p: ttk.Combobox(p, textvariable=local["fps"], values=FPS_CHOICES,
                                                      state="readonly", width=6),
            "Frame rate at start (the in-game menu, Minus+Plus or F1, changes it live). 30 is the original. "
            "60/90/120/240: the game speed follows the real clock. Above 60 needs a display faster than 60 Hz.")
        row(g, "CPU mode:", lambda p: ttk.Combobox(p, textvariable=local["cpu_mode"], values=("multi", "single"),
                                                    state="readonly", width=10),
            "multi: the game's threads run in parallel on your CPU cores (much faster). "
            "single: all of them take turns on one core (slower; use it if multi-core crashes).")
        row(g, "Emulated CPU cores:", lambda p: ttk.Combobox(p, textvariable=local["cpu_cores"],
                                                             values=CPU_CORE_CHOICES, state="readonly", width=6),
            "How many guest threads may run at the same time (multi mode). The Switch has 3. "
            "auto: all of your CPU's threads (3 to 16).")

        dump, _bid = game_version(_resolve_exefs() / "main")
        built, _bbid = built_game_version(self.work / "gen")
        ver = (f"Game version: your dump {dump or 'not found'}, current build {built or 'none'}" +
               ("  -  recompile to switch" if dump and built and dump != built else ""))
        ttk.Label(g, text=ver, style="Muted.TLabel").pack(anchor="w", pady=(12, 0))

        def smooth_motion():
            """NVIDIA Smooth Motion (driver frame generation, RTX 50, Vulkan): 60 fps as the base, the game's path
            on the clipboard, the NVIDIA App opened, and the steps to switch it on for this game."""
            local["fps"].set("60")
            exe = str(self.exe.resolve())
            win.clipboard_clear()
            win.clipboard_append(exe)
            app = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "NVIDIA Corporation" / "NVIDIA App" / \
                "CEF" / "NVIDIA App.exe"
            opened = False
            if app.is_file():
                try:
                    subprocess.Popen([str(app)], creationflags=NO_WINDOW)
                    opened = True
                except OSError:
                    pass
            messagebox.showinfo("Frame generation (NVIDIA Smooth Motion)",
                                ("The NVIDIA App is opening.\n\n" if opened else
                                 "Open the NVIDIA App (not found in its usual place).\n\n") +
                                "1. Graphics > Program settings > Add a program\n"
                                "2. Paste the game's path (it is on the clipboard):\n   " + exe + "\n"
                                "3. Turn on Smooth Motion and apply.\n\n"
                                "The frame rate is set to 60 here: Smooth Motion doubles it to about 120 on a "
                                "120 Hz or faster display. Needs an RTX 50 graphics card and a current driver.",
                                parent=win)
        r = ttk.Frame(g, style="Card.TFrame")
        r.pack(fill="x", pady=(12, 0))
        ttk.Button(r, text="Set up Smooth Motion...", command=smooth_motion).pack(side="left")
        ttk.Label(g, text="Frame generation by the NVIDIA driver (RTX 50 cards): about twice the frames on screen "
                  "from a 60 fps game, a little more input lag.", style="Muted.TLabel", wraplength=420,
                  justify="left").pack(anchor="w")

        # --- Audio ------------------------------------------------------------
        a = tab("Audio")
        check(a, "Sound", local["audio_on"], "Off: the game runs silent.")
        vol_txt = tk.StringVar(value=f"{local['audio_volume'].get()}%")
        row(a, "Volume:", lambda p: ttk.Scale(p, from_=0, to=100, orient="horizontal", length=260,
                                              variable=local["audio_volume"],
                                              command=lambda v: (local["audio_volume"].set(int(float(v))),
                                                                 vol_txt.set(f"{int(float(v))}%"))))
        ttk.Label(a, textvariable=vol_txt, style="Card.TLabel").pack(anchor="w")
        opus = (self.out_dir / "libopus-0.dll").is_file() or (self.out_dir / "opus.dll").is_file()
        ttk.Label(a, text="Music and voices decoder (libopus-0.dll): " + ("found" if opus else
                  "missing - a build copies it next to the game"), style="Muted.TLabel").pack(anchor="w",
                                                                                         pady=(12, 0))

        # --- Debugging --------------------------------------------------------
        d = tab("Debugging")
        row(d, "Stop after N calls (0 = never):", lambda p: ttk.Entry(p, textvariable=self.var_dispatch, width=12),
            "Useful to stop a game that hangs in a loop.")
        row(d, "Stop if stuck for N seconds (0 = never):",
            lambda p: ttk.Entry(p, textvariable=self.var_watchdog, width=8),
            "Reports where the game is spinning (SW_WATCHDOG).")
        check(d, "Debug messages", self.var_debug, "Extra runtime logging (SW_DEBUG=1).")
        check(d, "Trace every call", self.var_trace, "Logs every function call (SW_TRACE_DISPATCH). Very slow.")
        check(d, "Profiling", self.var_profile, "Timing statistics (SW_PROFILE=1).")
        check(d, "Log every service request", local["ipc_log"], "Each IPC message the game sends (SW_IPC_LOG=1).")
        check(d, "Log game file reads", local["romfs_log"], "Every RomFS read (SW_ROMFS_LOG=1).")
        check(d, "Test: everything counts as visible", local["oq_force"],
              "Occlusion tests always report 'visible' (SW_OQ_FORCE=1). For testing the NPC Talk prompt flicker.")
        row(d, "Peek addresses:", lambda p: ttk.Entry(p, textvariable=local["peek"], width=28),
            "Comma-separated guest addresses printed at load and at the stop (SW_PEEK).")
        row(d, "Watch range:", lambda p: ttk.Entry(p, textvariable=local["watch"], width=28),
            "ADDR[:LEN] - report every store into it (--watch).")
        ttk.Button(d, text="Relocation audit",
                   command=lambda: (close(), self.on_reloc_audit())).pack(anchor="w", pady=(12, 0))

        # --- Build ------------------------------------------------------------
        b = tab("Build")
        row(b, "Parallel compile jobs (0 = all CPU cores):",
            lambda p: ttk.Entry(p, textvariable=self.var_jobs, width=6))
        row(b, "Giant files compiled at once:", lambda p: ttk.Entry(p, textvariable=self.var_big_jobs, width=6),
            "Each giant C file can need ~7 GB of RAM while optimising. Lower this if the PC "
            "runs out of memory during a build.")
        check(b, "Fast build (unoptimised, game runs slower)", self.var_fast)

        # --- Launcher ---------------------------------------------------------
        l = tab("Launcher")
        row(l, "Theme:", lambda p: ttk.Combobox(p, textvariable=local["theme"], values=THEMES, state="readonly",
                                                 width=14),
            "Hisui paper (light) or Dark.  Also the button next to the status in the header.")
        check(l, "Animations", local["animations"], "Eased progress bar, pulsing boot stage, header shimmer.")
        check(l, "Auto-scroll the log", local["autoscroll"])
        check(l, "Ask before a full recompile", local["confirm_recompile"])
        check(l, "Check for updates at start", local["check_updates"],
              "Asks GitHub for a newer LodeStar release each time the launcher starts (nothing is sent but the request).")
        row(l, "Log lines kept on screen:", lambda p: ttk.Entry(p, textvariable=local["max_log_lines"], width=8))
        row(l, "Log files kept (older ones move to logs/old):",
            lambda p: ttk.Entry(p, textvariable=local["keep_logs"], width=6))
        r = ttk.Frame(l, style="Card.TFrame")
        r.pack(fill="x", pady=(12, 0))
        ttk.Button(r, text="Open logs folder",
                   command=lambda: self._open((self.out_dir / "logs").resolve())).pack(side="left")
        ttk.Button(r, text="Reset to defaults", command=lambda: reset()).pack(side="left", padx=6)
        ttk.Label(l, text="Uninstall removes LodeStar's whole folder: the launcher, the game you built, your game files, "
                          "mods and settings. It asks first whether to keep a copy of your saves (Documents\\LodeStar "
                          "saves).", style="Muted.TLabel", wraplength=420, justify="left").pack(anchor="w", pady=(18, 0))
        ttk.Button(l, text="Uninstall LodeStar...", command=lambda: self.on_uninstall(win)).pack(anchor="w", pady=(6, 0))

        def reset():
            if not messagebox.askyesno("Reset", "Reset every option to its default?", parent=win):
                return
            path = self.root_dir / SETTINGS_FILE        # kept as launcher.json.bak
            try:
                path.replace(path.with_suffix(".json.bak"))
            except OSError:
                pass
            self.settings = _load_settings()
            for key, var in local.items():
                var.set(self.settings[key])
            for var, key in ((self.var_dispatch, "max_dispatch"), (self.var_watchdog, "watchdog"),
                             (self.var_debug, "debug"), (self.var_trace, "trace"),
                             (self.var_profile, "profile"), (self.var_jobs, "jobs"),
                             (self.var_big_jobs, "big_jobs"), (self.var_fast, "fast_build")):
                var.set(self.settings[key])

        def close():
            for key, var in local.items():
                val = var.get()
                self.settings[key] = val.strip() if isinstance(val, str) else val
            theme = self.settings.get("theme")
            self.settings["theme"] = old_theme      # switch_theme compares with the current one
            self._store_settings()
            self._describe_options()
            win.destroy()
            if theme != old_theme:
                self.root.after(80, lambda: self.switch_theme(theme))

        foot = ttk.Frame(outer, style="Card.TFrame")           # always visible, whatever tab is open
        foot.pack(fill="x", pady=(12, 0))
        ttk.Button(foot, text="Done", style="Big.TButton", command=close).pack(side="right")
        ttk.Button(foot, text="Uninstall LodeStar...", command=lambda: self.on_uninstall(win)).pack(side="left")
        win.protocol("WM_DELETE_WINDOW", close)
        win.bind("<Escape>", lambda _e: close())
        win.update_idletasks()
        x = self.root.winfo_rootx() + (self.root.winfo_width() - win.winfo_reqwidth()) // 2
        y = self.root.winfo_rooty() + (self.root.winfo_height() - win.winfo_reqheight()) // 3
        win.geometry(f"+{max(0, x)}+{max(0, y)}")
        win.grab_set()

    def on_history(self) -> None:
        """Recent runs: how far each got and why it stopped."""
        c = COLORS
        win = tk.Toplevel(self.root)
        win.title("Run history")
        ui.window_icon(win, "history")
        win.configure(bg=c["card"])
        win.transient(self.root)
        win.geometry("900x420")
        frame = ttk.Frame(win, style="Card.TFrame", padding=12)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="Run history (newest first)", style="Head.TLabel").pack(anchor="w")
        cols = ("when", "furthest", "calls", "secs", "stop")
        tree = ttk.Treeview(frame, columns=cols, show="headings", height=14)
        for col, title, width in (("when", "When", 130), ("furthest", "Furthest stage", 120),
                                  ("calls", "Calls", 90), ("secs", "Time", 60), ("stop", "Stopped because", 480)):
            tree.heading(col, text=title)
            tree.column(col, width=width, anchor="w")
        for e in reversed(self.history):
            tree.insert("", "end", values=(e.get("when", ""), e.get("furthest", ""),
                                           f"{e.get('calls', 0):,}", format_duration(e.get("seconds", 0)),
                                           e.get("stop", "") or f"exit {e.get('exit')}"))
        tree.pack(fill="both", expand=True, pady=(8, 0))
        best = max(self.history, key=lambda e: (e.get("stages", 0), e.get("calls", 0)), default=None)
        if best:
            ttk.Label(frame, text=f"Best so far: {best['furthest']} ({best.get('calls', 0):,} calls) on {best['when']}",
                      style="Muted.TLabel").pack(anchor="w", pady=(6, 0))
        ttk.Button(frame, text="Close", command=win.destroy).pack(anchor="e", pady=(8, 0))

    @property
    def controls_path(self) -> Path:
        return self.root_dir / "Settings" / "controls.json"

    def on_controls(self) -> None:
        """Controls window: Keyboard / Mouse and Controller tabs (launcher/controls_ui.py)."""
        controls_ui.ControlsWindow(self.root, self.root_dir / "Settings", COLORS)

    def on_saves(self) -> None:
        """Save manager: a library of named saves (select one like a Pokémon in the editor), plus import /
        export of the game's save and the cheat editor."""
        c, P = COLORS, ui.SPACE
        win = tk.Toplevel(self.root)
        win.title("Saves")
        ui.window_icon(win, "saves")
        win.configure(bg=c["card"])
        win.transient(self.root)
        win.geometry("820x780")
        win.minsize(680, 660)
        frame = tk.Frame(win, bg=c["card"], padx=P["xl"], pady=P["lg"])
        frame.pack(fill="both", expand=True)

        def label(parent, text="", var=None, size=10, weight=400, muted=False, wrap=0):
            return tk.Label(parent, text=text, textvariable=var, font=ui.F(size, weight), bg=c["card"],
                            fg=c["muted"] if muted else c["text"], justify="left", anchor="w", wraplength=wrap)

        label(frame, "Saves", size=18, weight=700).pack(anchor="w")
        cur_var = tk.StringVar()
        label(frame, var=cur_var, size=9, muted=True, wrap=700).pack(anchor="w", pady=(2, P["md"]))

        # every save on this PC, in groups: the game's save, the library, emulator saves, backups / test saves.
        # ● = the save the game is using now.  rows[iid] = {"kind": game|lib|emu|bak, "name", "path", "id"?}
        cols = (("name", "Save", 330, "w"), ("updated", "Last saved", 150, "w"), ("size", "Size", 80, "e"))
        holder = tk.Frame(frame, bg=c["card"])
        holder.pack(fill="both", expand=True)
        tv = ttk.Treeview(holder, columns=[x[0] for x in cols[1:]], show="tree headings", height=11,
                          selectmode="browse")
        tv.heading("#0", text="Save", anchor="w")
        tv.column("#0", width=cols[0][2], stretch=True)
        for col, title, width, anchor in cols[1:]:
            tv.heading(col, text=title, anchor=anchor)
            tv.column(col, width=width, anchor=anchor, stretch=False)
        sb = ttk.Scrollbar(holder, orient="vertical", command=tv.yview)
        tv.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        tv.pack(side="left", fill="both", expand=True)
        tv.tag_configure("active", foreground=ui.T["primary"])
        tv.tag_configure("group", foreground=c["muted"], font=ui.F(9, 600))
        rows: dict[str, dict] = {}
        open_groups = {"game": True, "lib": True, "emu": True, "bak": False}

        def busy() -> bool:
            if self.task and self.task.name == "play":
                messagebox.showinfo("Game running", "Stop the game first - it may be writing its save.", parent=win)
                return True
            return False

        def selected() -> dict | None:
            sel = tv.selection()
            return rows.get(sel[0]) if sel else None

        def need_selection() -> dict | None:
            s = selected()
            if s is None:
                messagebox.showinfo("Saves", "Select a save in the list first.", parent=win)
            return s

        def fmt(t: float) -> str:
            return datetime.fromtimestamp(t).strftime("%d %b %Y  %H:%M") if t else "-"

        def refresh(select: str | None = None) -> None:
            game_dir = save_tools.save_dir(self.work)
            act = save_tools.active_slot(self.work)
            slots = save_tools.list_slots(self.work)
            act_name = next((s["name"] for s in slots if s["id"] == act), None)
            g_t, g_size = save_tools.save_info(game_dir)
            if g_size:
                cur_var.set(f"The game's save: last written {fmt(g_t).replace('  ', ' ')}"
                            + (f"  ·  loaded from “{act_name}”" if act_name else
                               "  ·  not in My saves yet (select it and press New Save)"))
            else:
                cur_var.set("The game has no save yet. Pick one below and press Use this save, or play until "
                            "the first autosave.")
            keep = select if select is not None else (tv.selection()[0] if tv.selection() else None)
            for g in tv.get_children():
                if g in ("g_game", "g_lib", "g_emu", "g_bak"):
                    open_groups[g[2:]] = bool(tv.item(g, "open"))
            tv.delete(*tv.get_children())
            rows.clear()

            def group(key: str, title: str, n: int) -> str:
                gid = f"g_{key}"
                tv.insert("", "end", iid=gid, text=f"{title}  ({n})", open=open_groups[key], tags=("group",))
                return gid

            def add(gid: str, iid: str, info: dict, t: float, size: int, active: bool = False) -> None:
                rows[iid] = info
                tv.insert(gid, "end", iid=iid, text=("●  " if active else "    ") + info["name"],
                          values=(fmt(t), f"{size / 1024:,.0f} KB" if size else "-"),
                          tags=("active",) if active else ())

            gid = group("game", "Game save (in use)", 1 if g_size else 0)
            if g_size:
                add(gid, "game", {"kind": "game", "name": act_name or "Current game save", "path": game_dir},
                    g_t, g_size, active=True)
            gid = group("lib", "My saves", len(slots))
            for s in slots:
                add(gid, f"lib:{s['id']}", {"kind": "lib", "id": s["id"], "name": s["name"], "path": s["path"] / "data"},
                    s["updated"], s["size"], active=s["active"])
            emus = save_tools.emulator_save_candidates()
            gid = group("emu", "Emulator saves", len(emus))
            for i, p in enumerate(emus):
                t, size = save_tools.save_info(p)
                emu = next((part for part in p.parts if part.lower() in
                            ("ryujinx", "yuzu", "suyu", "sudachi", "citron", "eden")), "emulator")
                add(gid, f"emu:{i}", {"kind": "emu", "name": f"{emu}  ·  {p.parent.name}/{p.name}", "path": p},
                    t, size)
            baks = [(p, *save_tools.save_info(p)) for p in save_tools.list_backups(self.work)]
            baks.sort(key=lambda b: b[1], reverse=True)                 # newest save first
            gid = group("bak", "Backups & test saves", len(baks))
            for p, t, size in baks:
                add(gid, f"bak:{p.name}", {"kind": "bak", "name": p.name + ("" if size else "  (empty)"),
                                          "path": p, "empty": not size}, t, size)
            pick = keep if keep in rows else next((k for k, v in rows.items() if v["kind"] == "lib"
                                                   and str(v.get("id")) == str(act)), None) or \
                ("game" if "game" in rows else None)
            if pick:
                parent = tv.parent(pick)
                if parent:
                    tv.item(parent, open=True)
                tv.selection_set(pick)
                tv.see(pick)
            update_buttons()

        def update_buttons(_e=None) -> None:
            s = selected()
            kind = s["kind"] if s else None
            game = save_tools.has_save(save_tools.save_dir(self.work))
            empty = bool(s and s.get("empty"))
            b_use.configure(state="normal" if kind in ("lib", "emu", "bak") and not empty else "disabled")
            b_current.configure(state="normal" if kind == "lib" and game else "disabled")
            b_new.configure(state="normal" if ((kind in ("emu", "bak") and not empty) or
                                               (kind not in ("emu", "bak") and game)) else "disabled")
            b_rename.configure(state="normal" if kind == "lib" else "disabled")
            b_delete.configure(state="normal" if kind == "lib" else "disabled")
            hint_var.set({
                "game": "The save the game uses now. New Save keeps a copy of it in My saves.",
                "lib": "Use: load it into the game · Save Current: update it with the game's latest save · "
                       "Rename (double-click) · Delete.",
                "emu": "An emulator's save (read only here). Use: load it into the game · New Save: copy it "
                       "into My saves.",
                "bak": "A backup or test save (read only here). Use: load it into the game · New Save: copy it "
                       "into My saves.",
            }.get(kind, "Select a save."))

        def do_use() -> None:
            s = need_selection()
            if not s or s["kind"] == "game" or busy():
                return
            if not messagebox.askyesno("Use this save", f"Load “{s['name']}” into the game?\n\n"
                                       "The game's current save is backed up first.", parent=win):
                return
            try:
                if s["kind"] == "lib":
                    save_tools.load_slot(self.work, s["id"])
                else:
                    save_tools.use_folder(self.work, s["path"])
            except (ValueError, OSError) as e:
                messagebox.showerror("Use this save", str(e), parent=win)
                return
            self.log(f"[launcher] save “{s['name']}” loaded into the game", "good")
            refresh("game" if s["kind"] != "lib" else f"lib:{s['id']}")

        def do_save_current() -> None:
            s = need_selection()
            if not s or s["kind"] != "lib" or busy():
                return
            if not messagebox.askyesno("Save Current", f"Update “{s['name']}” with the game's latest save?\n\n"
                                       "Its previous copy is kept in save_backups.", parent=win):
                return
            try:
                old = save_tools.save_current(self.work, s["id"])
            except (ValueError, OSError) as e:
                messagebox.showerror("Save Current", str(e), parent=win)
                return
            self.log(f"[launcher] “{s['name']}” updated from the game's save"
                     + (f" (previous copy: {old.name})" if old else ""), "good")
            refresh(f"lib:{s['id']}")

        def do_new() -> None:
            s = selected()
            if busy():
                return
            src = s["path"] if s and s["kind"] in ("emu", "bak") else None
            try:
                new = save_tools.new_slot(self.work, src=src)
            except (ValueError, OSError) as e:
                messagebox.showerror("New Save", str(e), parent=win)
                return
            what = s["name"] if src else "the game's save"
            self.log(f"[launcher] new save “{new['name']}” created from {what}", "good")
            refresh(f"lib:{new['id']}")

        def do_rename() -> None:
            s = need_selection()
            if not s or s["kind"] != "lib":
                return
            from tkinter import simpledialog
            name = simpledialog.askstring("Rename", "New name:", initialvalue=s["name"], parent=win)
            if name is None:
                return
            try:
                save_tools.rename_slot(self.work, s["id"], name)
            except (ValueError, OSError) as e:
                messagebox.showerror("Rename", str(e), parent=win)
                return
            refresh(f"lib:{s['id']}")

        def do_delete() -> None:
            s = need_selection()
            if not s or s["kind"] != "lib":
                return
            if not messagebox.askyesno("Delete save", f"Delete “{s['name']}” from My saves?\n\n"
                                       "The game's current save is not touched. The deleted save is moved to "
                                       "save_backups (it can still be recovered from there).",
                                       icon="warning", parent=win):
                return
            try:
                dst = save_tools.delete_slot(self.work, s["id"])
            except (ValueError, OSError) as e:
                messagebox.showerror("Delete save", str(e), parent=win)
                return
            self.log(f"[launcher] save “{s['name']}” deleted (moved to {dst.name})", "system")
            refresh()

        row = tk.Frame(frame, bg=c["card"])
        row.pack(fill="x", pady=(P["md"], 0))
        b_use = ui.PillButton(row, "Use this save", do_use, kind="primary", size="md")
        b_use.pack(side="left")
        b_current = ui.PillButton(row, "Save Current", do_save_current, kind="secondary", size="md")
        b_current.pack(side="left", padx=(P["sm"], 0))
        b_new = ui.PillButton(row, "New Save", do_new, kind="secondary", size="md")
        b_new.pack(side="left", padx=(P["sm"], 0))
        b_rename = ui.PillButton(row, "Rename", do_rename, kind="ghost", size="md")
        b_rename.pack(side="left", padx=(P["sm"], 0))
        b_delete = ui.PillButton(row, "Delete", do_delete, kind="danger", size="md")
        b_delete.pack(side="right")
        hint_var = tk.StringVar()
        label(frame, var=hint_var, size=9, muted=True, wrap=700).pack(anchor="w", pady=(P["sm"], 0))
        tv.bind("<<TreeviewSelect>>", update_buttons)
        tv.bind("<Double-1>", lambda _e: (selected() or {}).get("kind") == "lib" and do_rename())
        tv.bind("<Delete>", lambda _e: do_delete())

        def do_import(folder: Path | None = None) -> None:
            if busy():
                return
            if folder is None:
                d = filedialog.askdirectory(parent=win, title="Folder with the save files (JKSV dump or emulator save folder)")
                if not d:
                    return
                folder = Path(d)
            try:
                save_tools.import_save(self.work, folder)
            except (ValueError, OSError) as e:
                messagebox.showerror("Import", str(e), parent=win)
                return
            save_tools.set_active(self.work, None)       # the game's save no longer comes from a library entry
            messagebox.showinfo("Import", "Save imported. Use New Save to keep it in the library.", parent=win)
            refresh()

        def do_import_emu() -> None:
            """Pick a save from emulator folders or browse manually."""
            if busy():
                return
            keys = None
            try:
                import pla_save
                keys = pla_save.keys_from_game(self.root_dir / "Game" / "exefs",
                                               self.root_dir / SYSTEM_DIR / "Build" / "Cache" / "modules",
                                               sample=save_tools.save_dir(self.work) / "main")
            except Exception:
                pass
            dlg = tk.Toplevel(win)
            dlg.title("Choose save to import")
            ui.window_icon(dlg, "saves")
            dlg.configure(bg=c["card"])
            dlg.transient(win)
            dlg.geometry("720x360")
            fr = ttk.Frame(dlg, style="Card.TFrame", padding=12)
            fr.pack(fill="both", expand=True)
            ttk.Label(fr, text="Pick the save to import (your current save is backed up first)", style="Head.TLabel").pack(anchor="w")
            ttk.Label(fr, text="Emulator saves found automatically - choose one, or use Browse.", style="Muted.TLabel", justify="left").pack(anchor="w", pady=(2, 8))
            cols = (("path", "Save folder", 500), ("date", "Modified", 130), ("check", "Check", 80))
            tv = ttk.Treeview(fr, columns=[x[0] for x in cols], show="headings", height=7)
            for col, title, width in cols:
                tv.heading(col, text=title)
                tv.column(col, width=width, anchor="w")
            tv.pack(fill="both", expand=True)
            ok_paths: dict[str, bool] = {}

            def add(path: Path, select: bool = False) -> None:
                src = save_tools.import_source(path)
                if str(src) in ok_paths:
                    tv.selection_set(str(src))
                    return
                ok, text = save_tools.check_save(src, keys)
                main = src / "main"
                when = (f"{datetime.fromtimestamp(main.stat().st_mtime):%Y-%m-%d %H:%M}"
                        if main.is_file() else "-")
                ok_paths[str(src)] = ok
                tv.insert("", "end", iid=str(src), values=(str(src), when, ("✓" if ok else "✗") + " " + text))
                if select:
                    tv.selection_set(str(src))
                    tv.see(str(src))

            for p in save_tools.emulator_save_candidates():
                add(p)

            def browse_folder() -> None:
                d = filedialog.askdirectory(parent=dlg, title="Save folder (holds the file 'main')")
                if d:
                    add(Path(d), select=True)

            def browse_file() -> None:
                f = filedialog.askopenfilename(parent=dlg, title="The save's 'main' file")
                if f:
                    add(Path(f), select=True)

            def do_it() -> None:
                sel = tv.selection()
                if not sel:
                    messagebox.showinfo("Import", "Select a save in the list first (or Browse).", parent=dlg)
                    return
                if not ok_paths.get(sel[0]) and not messagebox.askyesno(
                        "Import", "This does not look like a Legends: Arceus save. Import anyway?", parent=dlg):
                    return
                if not messagebox.askyesno("Import", f"Import this save?\n{sel[0]}\n\n(Current save is backed up first.)", parent=dlg):
                    return
                dlg.destroy()
                do_import(Path(sel[0]))

            row2 = ttk.Frame(fr, style="Card.TFrame")
            row2.pack(fill="x", pady=(8, 0))
            ttk.Button(row2, text="Browse folder...", command=browse_folder).pack(side="left")
            ttk.Button(row2, text="Browse 'main' file...", command=browse_file).pack(side="left", padx=6)
            ttk.Button(row2, text="Cancel", command=dlg.destroy).pack(side="right")
            ttk.Button(row2, text="Import selected", command=do_it).pack(side="right", padx=6)
            if not ok_paths:
                ttk.Label(fr, text="No emulator saves found automatically - use Browse.", style="Muted.TLabel").pack(anchor="w", pady=(6, 0))

        def do_export() -> None:
            d = filedialog.askdirectory(parent=win, title="Export the save to...")
            if not d:
                return
            try:
                p = save_tools.export_save(self.work, Path(d))
            except (ValueError, OSError) as e:
                messagebox.showerror("Export", str(e), parent=win)
                return
            messagebox.showinfo("Export", f"Saved to {p}", parent=win)

        tk.Frame(frame, bg=ui.T["hairline"], height=1).pack(fill="x", pady=(P["lg"], P["md"]))
        more = tk.Frame(frame, bg=c["card"])
        more.pack(fill="x")
        label(more, "Import / export", size=12, weight=600).pack(anchor="w")
        row2 = tk.Frame(more, bg=c["card"])
        row2.pack(anchor="w", pady=(P["sm"], 0))
        ui.PillButton(row2, "Import from folder…", do_import, kind="secondary", size="sm").pack(side="left")
        ui.PillButton(row2, "Import from emulator…", do_import_emu, kind="secondary", size="sm").pack(
            side="left", padx=6)
        ui.PillButton(row2, "Export…", do_export, kind="secondary", size="sm").pack(side="left")

        def open_editor() -> None:
            import save_editor
            save_editor.SaveEditor(win, COLORS, self.root_dir, busy)

        label(more, "Cheats", size=12, weight=600).pack(anchor="w", pady=(P["md"], 0))
        label(more, "Money, merit points, satchel, ranks, story/area/ride unlocks, team editor (level, shiny, "
              "alpha, nature, perfect IVs), items x999. Edits the game's save; Save Current keeps it in the "
              "library.", size=9, muted=True, wrap=700).pack(anchor="w", pady=(2, 4))
        bottom = tk.Frame(frame, bg=c["card"])
        bottom.pack(fill="x", pady=(P["sm"], 0))
        ui.PillButton(bottom, "Save editor & cheats…", open_editor, kind="ghost", size="sm").pack(side="left")
        ui.PillButton(bottom, "Close", win.destroy, kind="secondary", size="md").pack(side="right")
        save_tools.save_dir(self.work).mkdir(parents=True, exist_ok=True)
        win.bind("<FocusIn>", lambda e: e.widget is win and refresh())   # after the editor changed the save
        refresh()

    # -- status ----------------------------------------------------------------
    def refresh_status(self) -> None:
        exefs = _resolve_exefs()
        romfs = _resolve_romfs()
        missing = [m for m in MODULES if not (exefs / m).is_file()]
        files_ok = exefs.is_dir() and not missing
        lines = [("✔ " if files_ok else "✘ ") + f"ExeFS: {self._short(exefs)}"]
        if missing:
            lines.append("   missing: " + ", ".join(missing))
        ok, text, _ = romfs_check(romfs, self.root_dir)
        if romfs:
            text = self._short(romfs) + " - " + text
        lines.append(("✔ " if ok else "✘ ") + "RomFS (game assets): " + text)
        self.files_var.set("\n".join(lines))

        gen = self.work / "gen" / "program.json"
        gen_ok = gen.is_file()
        if gen_ok:
            chunks = len(list((self.work / "gen").glob("sw_chunk_*.c")))
            self.gen_var.set(f"✔ {chunks:,} C files  -  {self._age(gen)}")
        else:
            self.gen_var.set("✘ Not recompiled yet")

        exe_ok = self.exe.is_file()
        stale = exe_ok and gen_ok and gen.stat().st_mtime > self.exe.stat().st_mtime
        rt_stale = exe_ok and runtime_newer_than(self.exe, self.root_dir / SYSTEM_DIR / "Engine" / "Runtime")
        stale = stale or rt_stale
        if exe_ok:
            size = self.exe.stat().st_size / 1e6
            why = "runtime" if rt_stale else "C code"
            self.exe_var.set((f"⚠ Older than the {why} - rebuild\n" if stale else "✔ ")
                             + f"{GAME_EXE}  {size:,.0f} MB  -  {self._age(self.exe)}")
        else:
            self.exe_var.set("✘ Not built yet")

        if self.xlate_dll.is_file():
            self.xlate_var.set(f"✔ Shader translator ready ({self._age(self.xlate_dll)})")
        else:
            self.xlate_var.set("✘ Shader translator not built: the game draws nothing without it")
        if not files_ok:
            hint = "Choose your game files (Build tab)."
        elif not gen_ok:
            hint = "Recompile the game code (Build tab)."
        elif not exe_ok or stale:
            hint = "Build the game (Build tab)."
        else:
            hint = "Ready to play."
        self.hint_var.set(hint)
        self._refresh_shader_status()
        self._set_buttons()

    def _short(self, path: Path) -> str:
        try:
            return str(path.relative_to(self.root_dir)).replace("\\", "/")
        except ValueError:
            return str(path)

    @staticmethod
    def _age(path: Path) -> str:
        t = datetime.fromtimestamp(path.stat().st_mtime)
        return t.strftime("built %d %b %H:%M")

    def set_status(self, text: str, color: str | None = None) -> None:
        self._status_colour = color or ""
        self.status_var.set(text)
        self.status_lbl.set(text, color or "", busy=self.task is not None and color == "accent")

    def _set_buttons(self) -> None:
        busy = self.task is not None
        state = "disabled" if busy else "normal"
        for b in (self.btn_recompile, self.btn_build, self.btn_clean, self.btn_xlate):
            b.configure(state=state)
        self.btn_shaders.configure(state="disabled" if busy or not self.exe.is_file() else "normal")
        self.btn_play.configure(state="disabled" if busy or not self.exe.is_file() else "normal")
        self.btn_update.configure(state=state)
        self.btn_stop.configure(state="normal" if busy else "disabled")

    def _store_settings(self) -> None:
        self.settings.update(max_dispatch=self.var_dispatch.get().strip() or "0",
                             watchdog=self.var_watchdog.get().strip() or "0",
                             debug=self.var_debug.get(), trace=self.var_trace.get(),
                             profile=self.var_profile.get(), fast_build=self.var_fast.get(),
                             log_filter=self.var_filter.get(),
                             jobs=self.var_jobs.get().strip() or "0",
                             big_jobs=self.var_big_jobs.get().strip() or "2")
        _save_settings(self.settings)

    # -- logging -----------------------------------------------------------------
    def log(self, text: str, tag: str | None = None) -> None:
        tag = tag or classify_line(text)
        self.all_lines.append((text, tag))
        if self._visible(text, tag):
            self._append(text, tag)

    def _visible(self, text: str, tag: str) -> bool:
        f = self.var_filter.get()
        if tag == "system" or f == "Everything":
            return True
        if f == "Errors":
            return tag == "error"
        if f == "Graphics":
            return tag == "gfx"
        return is_important(text, tag)

    def _append(self, text: str, tag: str) -> None:
        box = self.log_box
        at_end = box.yview()[1] > 0.999 and bool(self.settings.get("autoscroll", True))
        box.configure(state="normal")
        box.insert("end", text + "\n", tag)
        try:
            cap = max(1000, int(self.settings.get("max_log_lines", 20000)))
        except (TypeError, ValueError):
            cap = 20000
        if int(box.index("end-1c").split(".")[0]) > cap:
            box.delete("1.0", f"{cap // 10}.0")
        box.configure(state="disabled")
        if at_end:
            box.see("end")

    def _refilter(self) -> None:
        self._store_settings()
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.configure(state="disabled")
        for text, tag in self.all_lines[-20000:]:
            if self._visible(text, tag):
                self._append(text, tag)

    # -- tasks ---------------------------------------------------------------------
    def _start(self, name: str, cmd: list[str], cwd: Path | None = None,
               env_extra: dict | None = None, title: str = "") -> None:
        if self.task:
            messagebox.showinfo("Busy", f"'{self.task.name}' is still running.")
            return
        env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8")
        env.update(env_extra or {})
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_path = self.out_dir / "logs" / f"{stamp}_{name}.log"
        try:
            self.task = Task(name, cmd, cwd or self.root_dir, env, self.queue, log_path)
        except OSError as e:
            self.log(f"[launcher] could not start {name}: {e}", "error")
            return
        self.progress = BuildProgress() if name == "build" else None
        self.recomp_progress = RecompileProgress() if name == "recompile" else None
        if not name.startswith("shaders"):
            self.shader_progress = None
        self.log(f"[launcher] {title or name} started  ({' '.join(cmd)})", "system")
        self.task_var.set(title or name)
        self.detail_var.set("Starting...")
        determinate = name in ("build", "recompile") or name.startswith("shaders")
        self.bar.configure(mode="determinate" if determinate else "indeterminate")
        self._set_bar(0, instant=True)
        if not determinate:
            self.bar.start(15)
        self.set_status(title or name, "accent")
        self._set_buttons()

    def _finished(self) -> None:
        task, self.task = self.task, None
        if task is None:
            return
        self.bar.stop()
        self.bar.configure(mode="determinate")
        self._set_bar(1000 if task.returncode == 0 else 0, instant=task.returncode != 0)
        self.recomp_progress = None
        took = format_duration(time.time() - task.started)
        if task.stopped:
            verdict, color = "stopped", "warn"
        elif task.returncode == 0:
            verdict, color = "finished", "good"
        else:
            verdict, color = f"failed (code {task.returncode})", "error"
        self.task_var.set(f"{task.name.capitalize()} {verdict}  -  {took}")
        self.detail_var.set(f"Full log: {task.log_path}")
        self.log(f"[launcher] {task.name} {verdict} after {took}", "system")
        self.set_status(f"{task.name.capitalize()} {verdict}", color)
        if task.stopped:
            glow = COLORS["warn"]
        else:
            glow = COLORS["good"] if task.returncode == 0 else COLORS["error"]
        self._flash = (time.time(), glow)
        if task.name.startswith("shaders") and self.shader_progress:
            sp = self.shader_progress
            if task.stopped:
                self.log("[launcher] shader preparation cancelled: the existing cache is unchanged and still "
                         "valid; Build Shaders continues where it stopped", "warn")
            elif task.returncode == 0 and task.name != "shaders-scan":
                self.log(f"[launcher] shaders: {sp.describe()}", "good")
                self.detail_var.set(sp.describe())
            elif task.returncode not in (0, None):
                why = sp.summary or next((l for l in reversed(task.lines) if "[prep]" in l or "[census]" in l), "")
                self.log(f"[launcher] shader preparation failed: {why or 'see the log'}; the existing cache was "
                         "not touched", "error")
        if task.name == "shoot":
            shot = next((l.split("screenshot: ", 1)[1].strip() for l in task.lines if l.startswith("screenshot: ")), "")
            if shot and shot != "none" and Path(shot).is_file():
                self.log(f"[launcher] frame captured: {shot}", "good")
                self._open(Path(shot))
            else:
                self.log("[launcher] no frame was presented before the game stopped", "warn")
        if task.name == "play":
            self.result_var.set(summarize_run(task.lines, task.returncode))
            self._paint_stages()
            entry = make_history_entry(task.lines, task.returncode, task.started)
            prev_best = max((e.get("stages", 0) for e in self.history), default=0)
            self.history.append(entry)
            save_history(self.root_dir, self.history)
            if entry["stages"] > prev_best:
                self.log(f"[launcher] new best: reached '{entry['furthest']}'", "good")
                self._flash = (time.time(), COLORS["gfx"])
        elif task.returncode not in (0, None) and not task.stopped:
            errs = [l for l in task.lines if classify_line(l) == "error"]
            if errs:
                self.detail_var.set("First error: " + errs[0][:160])
        self.refresh_status()
        # a chain of steps (after an update, shader preparation): the next one only after a clean finish
        if self.pipeline:
            if task.returncode == 0 and not task.stopped:
                step = self.pipeline.pop(0)
                self.root.after(300, step)
            else:
                self.log("[launcher] the remaining steps stopped (the previous one did not succeed)", "system")
                self.pipeline.clear()

    def poll_output(self) -> None:
        try:
            for _ in range(400):  # bounded so the UI never freezes
                kind, payload = self.queue.get_nowait()
                if kind == "done":
                    self._finished()
                    continue
                if self.progress and self.progress.feed(payload):
                    self._set_bar(self.progress.fraction * 1000)
                    self.detail_var.set(self.progress.describe())
                if self.shader_progress and self.shader_progress.feed(payload):
                    self._set_bar(self.shader_progress.fraction * 1000)
                    self.detail_var.set(self.shader_progress.describe())
                if self.recomp_progress and self.recomp_progress.feed(payload):
                    self._set_bar(self.recomp_progress.fraction * 1000)
                    self.detail_var.set(self.recomp_progress.describe())
                if self.task and self.task.name == "play" and self.tracker.feed(payload):
                    self._paint_stages()
                self.log(payload)
        except queue.Empty:
            pass
        if self.task and self.recomp_progress:
            if self._tick % 30 == 0:
                self.detail_var.set(self.recomp_progress.describe())
        elif self.task and not self.progress and not self.shader_progress:
            self.detail_var.set(f"Running for {format_duration(time.time() - self.task.started)}")
        now = time.time()
        if now - self._last_mem > 2:
            self._last_mem = now
            mem = free_memory_gb()
            ram = f"   ·   RAM free: {mem[0]:.1f} / {mem[1]:.0f} GB" if mem else ""
            git = f"   ·   {self._git}" if self._git else ""
            self.footer_var.set(f"Project: {self.root_dir}{git}{ram}")
        self.root.after(100, self.poll_output)

    # -- button handlers -------------------------------------------------------------
    def on_recompile(self) -> None:
        if self.settings.get("confirm_recompile", True) and not messagebox.askyesno(
                "Recompile", "Recompiling takes about 10 minutes and replaces System/build_work/gen. Continue?"):
            return
        self._recompile_now()

    def _recompile_now(self) -> None:
        cmd = [sys.executable, "-u", str(self.root_dir / SYSTEM_DIR / "Engine" / "swrecomp.py"), "recompile",
               str(_resolve_exefs())]
        for m in MODULES:
            cmd += ["--module", m]
        cmd += ["--max-functions", "10000000", "--out", str(self.work)]
        self._start("recompile", cmd, title="Recompiling (ARM -> C)")

    def on_build(self) -> None:
        env = {"SW_OPT": "/Od"} if self.var_fast.get() else {}
        jobs, big = self.var_jobs.get().strip(), self.var_big_jobs.get().strip()
        if jobs.isdigit() and int(jobs) > 0:
            env["SW_JOBS"] = jobs
        if big.isdigit() and int(big) > 0:
            env["SW_BIG_JOBS"] = big
        self._start("build", [sys.executable, "-u", str(self.work / "build.py")], env_extra=env,
                    title="Building the game")

    # -- updates (updater.py: GitHub releases of LodeStar) ---------------------------------------------------------
    def _check_update(self) -> None:
        def work():
            info = updater.check()
            self.root.after(0, lambda: self._show_update(info))
        threading.Thread(target=work, daemon=True).start()

    def _show_update(self, info: dict) -> None:
        if not info.get("available"):
            if info.get("error"):
                self.log(f"[update] no update check: {info['error']}", "system")
            return
        self.update_info = info
        self.btn_update.configure(text=updater.BUTTON[info["kind"]])
        self.btn_update.pack(side="left", padx=(ui.SPACE["sm"], 0), before=self.btn_stop)
        what = {"launcher": "only the launcher changed - no rebuild",
                "rebuild": "the game runtime changed - the game is rebuilt (a few minutes)",
                "recompile": "the recompiler changed - the game's code is translated again and rebuilt"}[info["kind"]]
        self.log(f"[update] LodeStar {info['version']} is available ({what})", "system")
        self.status_lbl.set(f"Update {info['version']}", "accent")

    def on_update(self) -> None:
        info = self.update_info
        if not info or self.task:
            return
        what = {"launcher": "Only the launcher changes: nothing is rebuilt.",
                "rebuild": "The game runtime changed: after the update the game is rebuilt (a few minutes).",
                "recompile": "The recompiler changed: after the update the game's code is translated again and "
                             "rebuilt (10-30 minutes)."}[info["kind"]]
        notes = (info.get("notes") or "").strip()
        if not messagebox.askyesno(f"Update to {info['version']}",
                                   f"LodeStar {info['current']} -> {info['version']}\n\n{what}\n\nYour game files, mods, "
                                   "settings, saves and built game are kept. The launcher closes and starts again."
                                   + (f"\n\nWhat's new:\n{notes[:600]}" if notes else "")):
            return
        self.btn_update.configure(state="disabled")
        self.task_var.set("Downloading the update...")

        def work():
            try:
                z = updater.download(info, lambda d, t: self.root.after(0, lambda: self.task_var.set(
                    f"Downloading the update... {d / 2**20:.1f} of {t / 2**20:.1f} MB" if t else "Downloading the update...")))
                self.root.after(0, lambda: self._apply_update(z, info))
            except (OSError, ValueError) as e:
                self.root.after(0, lambda: (messagebox.showerror("Update", f"The update could not be downloaded: {e}"),
                                            self.btn_update.configure(state="normal"), self.task_var.set("Nothing running")))
        threading.Thread(target=work, daemon=True).start()

    def _apply_update(self, zip_path, info: dict) -> None:
        self._store_settings()
        updater.start_apply(zip_path, info, self.root_dir)
        self.root.destroy()

    def _after_update(self) -> None:
        """Settings/pending_update.json (written by the update helper): the build the update needs, once."""
        p = self.root_dir / "Settings" / "pending_update.json"
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
            p.unlink()
        except (OSError, ValueError):
            return
        kind = d.get("kind", "launcher")
        self.log(f"[update] updated to {d.get('version')} ({d.get('files', '?')} files)", "system")
        if kind == "launcher" or not game_files_check(self.root_dir)[0]:
            return                                                # nothing to build, or no game files to build from
        steps = []
        if kind == "recompile":
            steps.append(self._recompile_now)
        steps.append(self.on_build_xlate)                         # the translator may have changed too
        steps.append(self.on_build)
        self.pipeline = steps[1:]
        self.log(f"[update] rebuilding the game after the update ({len(steps)} step(s))", "system")
        steps[0]()

    def on_clean_build(self) -> None:
        if not messagebox.askyesno("Clean rebuild", "Delete all compiled objects and rebuild "
                                   "everything? This takes much longer than a normal build."):
            return
        shutil.rmtree(self.out_dir / "obj", ignore_errors=True)
        self.on_build()

    def _run_args(self) -> tuple[list[str], dict]:
        try:
            dispatch = str(int(self.var_dispatch.get().strip() or "0", 0))
        except ValueError:
            dispatch = "0"
        env = {}
        wd = self.var_watchdog.get().strip()
        if wd.isdigit() and int(wd) > 0:
            env["SW_WATCHDOG"] = wd
        if self.var_debug.get():
            env["SW_DEBUG"] = "1"
        if self.var_trace.get():
            env["SW_TRACE_DISPATCH"] = "1"
        if self.var_profile.get():
            env["SW_PROFILE"] = "1"
        st = self.settings
        size = str(st.get("window_size", "")).strip().lower().replace(" ", "")
        if re.fullmatch(r"\d{3,4}x\d{3,4}", size):
            env["SW_WINDOW_SIZE"] = size
        if st.get("fullscreen"):
            env["SW_FULLSCREEN"] = "1"
        env.update(fps_env(st.get("fps", "30")))
        env.update(render_env(st.get("render_res", RENDER_RES_CHOICES[0])))
        if adopt_ingame_settings(st, self.root_dir / "Settings"):   # the in-game menu's render distance
            _save_settings(st)
        env["SW_FPS_OVERLAY"] = str(perf_overlay_level(st.get("fps_overlay", "FPS")))
        env.update(aniso_env(st.get("anisotropy", "16x")))
        env.update(res_scale_env(st.get("res_scale", "Original")))
        env.update(picture_env(st))
        env["SW_DLAA"] = "1" if st.get("dlaa") == "On" else "0"
        if st.get("dlaa") == "On" and not (self.out_dir / "nvngx_dlss.dll").is_file():
            src = find_dlss_dll()
            if src:
                try:
                    shutil.copy2(src, self.out_dir / "nvngx_dlss.dll")
                    self.log(f"DLAA: copied nvngx_dlss.dll from {src.parent}")
                except OSError as e:
                    self.log(f"DLAA: could not copy {src}: {e}")
            else:
                self.log("DLAA: no nvngx_dlss.dll found on this PC (put one next to Pokemon Arceus.exe); DLAA stays off")
        env.update(vsync_env(st.get("vsync", "On")))
        env.update(draw_distance_env(st.get("draw_distance", DRAW_DISTANCE_CHOICES[0])))
        env.update(detail_distance_env(st.get("detail_distance", DRAW_DISTANCE_CHOICES[0])))
        env.update(npc_distance_env(st.get("npc_distance", NPC_DISTANCE_CHOICES[0])))
        env.update(shadow_env(st.get("shadow_quality", SHADOW_CHOICES[0])))
        env.update(framegen_env(st.get("frame_gen", FRAMEGEN_CHOICES[0])))
        env.update(cpu_env(str(st.get("cpu_mode", "multi")), str(st.get("cpu_cores", "auto")), os.cpu_count()))
        env.update(audio_env(st.get("audio_on", True), st.get("audio_volume", 100)))
        try:
            env.update(controls_ui.game_env(self.root_dir / "Settings", self.out_dir))
        except OSError:
            pass                                    # default keys / controller layout
        if st.get("gpu") in ("vulkan", "null"):
            env["SW_GPU"] = st["gpu"]
        if st.get("pipe_prewarm"):
            env["TOTK_PIPE_PREBUILD"] = "1"          # experimental boot prewarm of recorded pipelines
        if st.get("ipc_log"):
            env["SW_IPC_LOG"] = "1"
        if st.get("romfs_log"):
            env["SW_ROMFS_LOG"] = "1"
        if st.get("oq_force"):
            env["SW_OQ_FORCE"] = "1"
        env.update(mods_cfg.game_env(self.root_dir / "Settings"))   # Config/mods.json (Mods window)
        romfs = _resolve_romfs()                    # the RomFS folder chosen in the launcher (Config/config.json)
        if romfs and romfs.is_dir():
            env["SW_ROMFS"] = str(romfs)
        if str(st.get("peek", "")).strip():
            env["SW_PEEK"] = str(st["peek"]).strip()
        args = ["--max-dispatch", dispatch]
        if str(st.get("watch", "")).strip():
            args += ["--watch", str(st["watch"]).strip()]
        return args, env

    def on_play(self) -> None:
        ok, text, dest = romfs_check(_resolve_romfs(), self.root_dir)
        if not ok:
            more = "\n\nImport a complete dump with 'Import game files' (Game files card)." if dest else ""
            messagebox.showerror("Game files incomplete", "RomFS: " + text + more)
            return
        self._store_settings()
        args, env = self._run_args()
        self.result_var.set("Running...")
        self.tracker = BootTracker()
        self._start("play", [str(self.exe), "--real-start", *args], cwd=self.out_dir, env_extra=env,
                    title="Game running")
        self._paint_stages()

    def on_reloc_audit(self) -> None:
        self._start("audit", [str(self.exe), "--reloc-audit", "--max-dispatch", "100000"],
                    cwd=self.out_dir, title="Relocation audit")

    @property
    def xlate_dll(self) -> Path:
        return self.work / "xlate" / "sw_xlate.dll"

    # -- shaders ---------------------------------------------------------------------
    def _shader_dir(self) -> Path:
        return self.out_dir / "shader-cache"

    def _refresh_shader_status(self) -> None:
        if not self.exe.is_file():
            self.shader_var.set("Build the game first (step 3).")
            return
        st = shader_prep.cache_status(self._shader_dir(), shader_prep.translator_id(self.xlate_dll), self.gpu_key)
        self.shader_var.set(st["text"])
        if not (self.task and self.task.name.startswith("shaders")):
            self.btn_shaders.configure(text=st["button"])
        if not self._gpu_probe_started:
            self._gpu_probe_started = True
            threading.Thread(target=self._gpu_probe, daemon=True).start()

    def _gpu_probe(self) -> None:
        """GPU/driver identity (a Vulkan device is opened for a moment), to notice driver updates."""
        try:
            r = subprocess.run([str(self.exe), "--gpu-info"], cwd=self.out_dir, capture_output=True, text=True,
                               timeout=60, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            key = shader_prep.gpu_key_from_output(r.stdout)
        except (OSError, subprocess.SubprocessError):
            key = ""
        if key:
            self.gpu_key = key
            try:
                self.root.after(0, self._refresh_shader_status)
            except (RuntimeError, tk.TclError):
                pass                                    # the launcher was closed meanwhile

    def _store_prewarm(self) -> None:
        self.settings["pipe_prewarm"] = bool(self.var_prewarm.get())
        self._store_settings()

    def _shader_env(self) -> dict:
        return {"SW_PREP_XLATE_ID": shader_prep.translator_id(self.xlate_dll)}

    def on_shaders(self) -> None:
        """Build / Rebuild: census of the game files, then --prepare-shaders. Refresh (driver changed only):
        --warm-shaders. Every step leaves the existing cache valid when it fails or is stopped."""
        if not self.exe.is_file():
            return
        st = shader_prep.cache_status(self._shader_dir(), shader_prep.translator_id(self.xlate_dll), self.gpu_key)
        if st["state"] == "refresh":
            self.shader_progress = shader_prep.ShaderPrepProgress(warm_only=True)
            self.btn_shaders.configure(text="Building Shaders...")
            self._start("shaders", [str(self.exe), "--warm-shaders"], cwd=self.out_dir,
                        env_extra=self._shader_env(), title="Refreshing shaders (driver cache)")
            return
        self.shader_progress = shader_prep.ShaderPrepProgress()
        self.btn_shaders.configure(text="Building Shaders...")
        self.pipeline = [self._shaders_prepare]
        self._start("shaders-scan", [sys.executable, "-u", str(self.root_dir / SYSTEM_DIR / "Engine" / "Tools" / "shader_census.py"),
                                    "--out", str(self._shader_dir())],   # RomFS: Config/config.json, as the census reads it
                    title="Building shaders: scanning game files")

    def _shaders_prepare(self) -> None:
        self._start("shaders", [str(self.exe), "--prepare-shaders"], cwd=self.out_dir, env_extra=self._shader_env(),
                    title="Building shaders: translating")

    def on_build_xlate(self) -> None:
        """Shader translator (Ryujinx's, MIT) built with Roslyn against the installed .NET runtime."""
        self._start("xlate", [sys.executable, "-u", str(self.root_dir / SYSTEM_DIR / "Engine" / "Tools" / "build_xlate.py")],
                    title="Building shader translator")

    def on_test_frame(self) -> None:
        """Run the game until it presents a frame, screenshot the window, open the picture."""
        self._start("shoot", [sys.executable, "-u", str(self.root_dir / SYSTEM_DIR / "Engine" / "Tools" / "run_and_shoot.py")],
                    title="Test frame (run + screenshot)")
        self._open_shot_after = True

    def on_tests(self) -> None:
        self._start("tests", [sys.executable, "-m", "pytest", "System/Engine/Tests", "System/tests", "-q"],
                    title="Running tests")

    def on_stop(self) -> None:
        self.pipeline.clear()
        if self.task:
            self.log(f"[launcher] stopping {self.task.name}...", "system")
            self.task.stop()

    def on_clear_log(self) -> None:
        self.all_lines.clear()
        self._refilter()

    def on_copy_errors(self) -> None:
        errs = [t for t, tag in self.all_lines if tag == "error"]
        self.root.clipboard_clear()
        self.root.clipboard_append("\n".join(errs) if errs else "(no errors)")
        self.log(f"[launcher] copied {len(errs)} error line(s) to the clipboard", "system")

    def _open(self, path: Path) -> None:
        if sys.platform == "win32":
            os.startfile(str(path))  # noqa: S606 - opening a local folder/file
        else:
            subprocess.Popen(["xdg-open", str(path)])

    def on_open_build(self) -> None:
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self._open(self.out_dir)

    def on_open_log(self) -> None:
        logs = sorted((self.out_dir / "logs").glob("*.log"))
        if logs:
            self._open(logs[-1])
        else:
            self.log("[launcher] no log files yet", "system")

    def _choose_dir(self, key: str, title: str) -> None:
        path = filedialog.askdirectory(title=title, initialdir=str(self.root_dir))
        if not path:
            return
        cfg = _load_config()
        p = Path(path).resolve()
        try:
            cfg[key] = str(p.relative_to(self.root_dir)).replace("\\", "/")
        except ValueError:
            cfg[key] = str(p)
        _save_config(cfg)
        self.refresh_status()

    def on_select_exefs(self) -> None:
        self._choose_dir("exefs_dir", "Select the extracted ExeFS folder")

    def on_import_dump(self) -> None:
        """Your own already-extracted dump (no decryption here, no keys): a folder holding exefs/ and romfs/ - from
        your console (nxdumptool: ExeFS / RomFS dump) or an emulator's 'Dump RomFS / ExeFS'.  Both are checked, then
        copied into Game/ (an older dump there is moved to Game/_old_<time>, never deleted)."""
        src = filedialog.askdirectory(title="Folder with your extracted game files (holds exefs and romfs)",
                                      initialdir=str(self.root_dir))
        if not src:
            return
        found = find_dump(Path(src))
        if not found["exefs"] or not found["romfs"]:
            messagebox.showerror("Import game files", found["why"] + "\n\nPick the folder that holds both an exefs "
                                 "folder (main, main.npdm, sdk, subsdk...) and a romfs folder (bin, system_resource).")
            return
        ok, text, _dest = romfs_check(found["romfs"], self.root_dir)
        if not ok:
            messagebox.showerror("Import game files", "RomFS: " + text)
            return
        ver = exefs_version(found["exefs"])
        if ver != "1.0.0" and not messagebox.askyesno(
                "Import game files", f"This ExeFS is game version {ver}, but this build supports 1.0.0 only "
                "(the frame-rate fixes and hooks are made for it). Import anyway?"):
            return
        game = self.root_dir / "Game"

        def work():
            import shutil
            stamp = time.strftime("%Y%m%d_%H%M%S")
            try:
                game.mkdir(exist_ok=True)
                for name in ("exefs", "romfs"):
                    dst, srcp = game / name, found[name]
                    if dst.exists() and dst.resolve() == srcp.resolve():
                        continue
                    if dst.exists():
                        old = game / f"_old_{stamp}"
                        old.mkdir(exist_ok=True)
                        shutil.move(str(dst), str(old / name))
                        self.root.after(0, lambda o=old, n=name: self.log(f"[import] old {n} moved to {o}", "system"))
                    self.root.after(0, lambda n=name: self.log(f"[import] copying {n}... (the RomFS is ~6 GB)", "system"))
                    shutil.copytree(srcp, dst)
                cfg = _load_config()
                cfg["exefs_dir"], cfg["romfs_dir"] = "Game/exefs", "Game/romfs"
                _save_config(cfg)
                self.root.after(0, lambda: (self.log(f"[import] done: game version {ver}", "system"), self.refresh_status()))
            except OSError as e:
                self.root.after(0, lambda: messagebox.showerror("Import game files", f"Copying failed: {e}"))
        self.log(f"[import] {found['exefs']} + {found['romfs']} -> {game}", "system")
        threading.Thread(target=work, daemon=True).start()

    def on_select_romfs(self) -> None:
        self._choose_dir("romfs_dir", "Select the extracted RomFS folder")

    def _on_tk_error(self, exc_type, exc, tb) -> None:
        import traceback
        for line in "".join(traceback.format_exception(exc_type, exc, tb)).splitlines():
            self.log(line, "error")
        self.set_status("Launcher error (see log)", "error")

    def on_uninstall(self, parent=None) -> None:
        """Start the installer's uninstaller (unins000.exe in the install folder; it removes the whole folder after
        asking about the saves) and close the launcher so its files are free.  A copy that was not installed with
        LodeStar-Setup.exe (a clone, the portable zip) has no uninstaller: deleting its folder removes it."""
        unins = sorted(self.root_dir.glob("unins*.exe"))
        if not unins:
            messagebox.showinfo("Uninstall", "This copy of LodeStar was not installed with the installer, so there is "
                                f"nothing to uninstall: delete the folder\n\n{self.root_dir}\n\nto remove it (keep a copy "
                                "of System\\build_work\\out\\save first if you want your saves).", parent=parent)
            return
        if self.task:
            messagebox.showerror("Uninstall", f"'{self.task.name}' is running. Stop it first.", parent=parent)
            return
        if not messagebox.askyesno("Uninstall LodeStar", "Uninstall LodeStar? The uninstaller asks about your saves "
                                   "next, then removes the whole folder.", parent=parent):
            return
        self._store_settings()
        try:
            subprocess.Popen([str(unins[-1])], cwd=str(self.root_dir.parent))
        except OSError as e:
            messagebox.showerror("Uninstall", f"The uninstaller could not start: {e}", parent=parent)
            return
        self.root.destroy()

    def on_close(self) -> None:
        if self.task and not messagebox.askyesno("Quit", f"'{self.task.name}' is running. Stop it and quit?"):
            return
        if self.task:
            self.task.stop()
        self._store_settings()
        self.root.destroy()


def main() -> None:
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("LodeStar.Launcher")
        except (OSError, AttributeError):
            pass
    root = tk.Tk()
    root.withdraw()            # hidden until it is built at its real size (no small empty window at start)
    LauncherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
