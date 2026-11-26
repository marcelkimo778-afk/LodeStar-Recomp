"""Recommended settings for this PC, and where the extra work goes (graphics card / balanced / processor).

No tkinter, so it is tested on its own (tests/test_pc_advice.py).  The launcher's Options > Game tab shows the
result ("Recommended for this PC") and fills the settings in on request.

What can and cannot move: the game's own logic (people, Pokemon, physics, its job threads) is processor work and
cannot be moved to the graphics card.  What we choose is where the EXTRA quality goes:
  - Graphics card: resolution scale, DLAA, shadows, tree / detail distance (measured: these cost the RTX 5070
    1 -> 3.8 ms at 2x, 7.5 ms at 3x; tree / detail distance cost nothing measurable), Simulation distance stays at
    the Switch's 100 m.
  - Processor: Simulation distance higher (people and Pokemon loaded further away; Max costs ~25% fps), the
    graphics card options stay light.
  - Balanced: some of both.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

WORKLOADS = ("Graphics card", "Balanced", "Processor")

# the launcher's choice lists (launcher.py), repeated so this module needs no tkinter import
RES = ("Original", "2x", "3x", "4x")
DIST = ("Original", "High (2x)", "Very high (3x)", "Ultra (5x)", "Max (10x)")
NPC = ("Original (100 m)", "150 m", "200 m", "300 m", "Max (400 m)")
SHADOW = ("Original", "2x", "3x", "4x")


# ---- hardware ------------------------------------------------------------------------------------------------------
def _reg(path: str, name: str):
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as k:
            return winreg.QueryValueEx(k, name)[0]
    except OSError:
        return None


def _ram_gb() -> float:
    if sys.platform != "win32":
        try:
            return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / 2 ** 30
        except (ValueError, OSError, AttributeError):
            return 0.0
    import ctypes

    class MS(ctypes.Structure):
        _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("avail", ctypes.c_ulonglong), ("tpage", ctypes.c_ulonglong), ("apage", ctypes.c_ulonglong),
                    ("tvirt", ctypes.c_ulonglong), ("avirt", ctypes.c_ulonglong), ("aext", ctypes.c_ulonglong)]
    m = MS()
    m.len = ctypes.sizeof(MS)
    return m.total / 2 ** 30 if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else 0.0


def _gpus() -> list[tuple[str, float]]:
    """(name, VRAM GB) of the display adapters, from the registry (qwMemorySize is 64-bit; WMI's AdapterRAM stops
    at 4 GB)."""
    out = []
    if sys.platform != "win32":
        return out
    try:
        import winreg
        base = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as k:
            for i in range(64):
                try:
                    sub = winreg.EnumKey(k, i)
                except OSError:
                    break
                if not sub.isdigit():
                    continue
                name = _reg(base + "\\" + sub, "DriverDesc")
                mem = _reg(base + "\\" + sub, "HardwareInformation.qwMemorySize")
                if mem is None:
                    raw = _reg(base + "\\" + sub, "HardwareInformation.MemorySize")
                    mem = int.from_bytes(raw[:4], "little") if isinstance(raw, bytes) else raw
                if name:
                    out.append((str(name), (int(mem) / 2 ** 30) if mem else 0.0))
    except OSError:
        pass
    return out


def detect(gpu_name: str | None = None) -> dict:
    """This PC: cpu (name), threads, ram_gb, gpu (name), vram_gb.  gpu_name: the Vulkan device the game uses
    (LodeStar Game.exe --gpu-info), else the adapter with the most video memory."""
    cpu = _reg(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString") or "unknown processor"
    gpus = _gpus()
    pick = None
    if gpu_name:
        for n, v in gpus:
            if n.lower() in gpu_name.lower() or gpu_name.lower() in n.lower():
                pick = (gpu_name, v)
        pick = pick or (gpu_name, 0.0)
    elif gpus:
        pick = max(gpus, key=lambda g: g[1])
    return {"cpu": " ".join(str(cpu).split()), "threads": os.cpu_count() or 1, "ram_gb": round(_ram_gb(), 1),
            "gpu": pick[0] if pick else "unknown graphics card", "vram_gb": round(pick[1], 1) if pick else 0.0}


# ---- tiers ---------------------------------------------------------------------------------------------------------
_IGPU = re.compile(r"\b(uhd|iris|hd graphics|radeon\(tm\) graphics|radeon graphics|vega \d+ graphics|adreno)\b", re.I)


def gpu_tier(hw: dict) -> int:
    """0 weak (integrated / < 4 GB), 1 entry (4-6 GB), 2 mid (8-11 GB), 3 high (12 GB and more)."""
    v, name = hw.get("vram_gb", 0.0), hw.get("gpu", "")
    if _IGPU.search(name) or v < 3.5:
        return 0
    return 1 if v < 7.5 else 2 if v < 11.5 else 3


def cpu_tier(hw: dict) -> int:
    """0: up to 4 threads, 1: up to 8, 2: up to 12, 3: 16 and more (the game runs 3+ busy threads, the
    recompiled main thread is the limit)."""
    t = hw.get("threads", 1)
    return 0 if t <= 4 else 1 if t <= 8 else 2 if t <= 12 else 3


def has_dlss(hw: dict) -> bool:
    return bool(re.search(r"\bRTX\b", hw.get("gpu", ""), re.I))


# ---- advice --------------------------------------------------------------------------------------------------------
def recommend_workload(hw: dict) -> str:
    g, c = gpu_tier(hw), cpu_tier(hw)
    # this game is processor-bound on nearly every PC (the recompiled main thread waits on the game's job
    # threads), so spare graphics-card time is the cheap place for extra quality
    if g >= c and g >= 1:
        return "Graphics card"
    if g == 0 or c >= g + 2:     # integrated / very small graphics card: keep the picture light
        return "Processor"
    return "Balanced"


def settings_for(hw: dict, workload: str | None = None) -> dict:
    """Launcher settings (launcher.json keys) for this PC with the extra work on `workload` (None: the
    recommended one).  Frame rate: what the processor holds steadily (steady beats high)."""
    g, c = gpu_tier(hw), cpu_tier(hw)
    w = workload if workload in WORKLOADS else recommend_workload(hw)
    if w == "Graphics card":
        res, sh, dist, npc = ("Original", "Original", "2x", "3x")[g], (0, 1, 3, 3)[g], (1, 2, 4, 4)[g], 0
        dlaa = has_dlss(hw) and g >= 1
    elif w == "Balanced":
        res, sh, dist, npc = ("Original", "Original", "Original", "2x")[g], (0, 1, 1, 3)[g], (1, 2, 2, 4)[g], \
            (0, 0, 1, 1)[c]
        dlaa = has_dlss(hw) and g >= 2
    else:   # Processor
        res, sh, dist, npc = "Original", (0, 0, 1, 1)[g], (0, 1, 2, 2)[g], (0, 1, 2, 3)[c]
        dlaa = False
    fps = ("30", "60", "60", "90")[c]
    if hw.get("ram_gb", 16) and hw.get("ram_gb", 16) < 12:   # big draw distances / textures want memory
        dist = min(dist, 1)
    return {"res_scale": res, "shadow_quality": SHADOW[sh], "draw_distance": DIST[dist], "detail_distance": DIST[dist],
            "npc_distance": NPC[npc], "anisotropy": "16x", "dlaa": "On" if dlaa else "Off", "fps": fps}


def summary(hw: dict) -> str:
    vram = f" ({hw['vram_gb']:g} GB)" if hw.get("vram_gb") else ""
    return (f"{hw['cpu']}, {hw['threads']} threads, {hw['ram_gb']:g} GB RAM\n{hw['gpu']}{vram}")


def advice_text(hw: dict) -> str:
    w = recommend_workload(hw)
    s = settings_for(hw, w)
    parts = [f"{s['fps']} fps", f"extra work on: {w.lower()}"]
    if s["res_scale"] != "Original":
        parts.append(f"resolution {s['res_scale']}")
    if s["dlaa"] == "On":
        parts.append("DLAA")
    parts.append(f"shadows {s['shadow_quality'].lower()}")
    parts.append(f"simulation {s['npc_distance'].lower()}")
    return "Recommended: " + ", ".join(parts)


if __name__ == "__main__":
    hw = detect()
    print(summary(hw))
    print("tiers: gpu", gpu_tier(hw), "cpu", cpu_tier(hw))
    print(advice_text(hw))
    for w in WORKLOADS:
        print(w, settings_for(hw, w))
