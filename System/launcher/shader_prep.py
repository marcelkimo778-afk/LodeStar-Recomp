"""Shader preparation for the launcher: cache status (what is prepared, for which translator and GPU driver) and a
progress parser for the preparation run.

The work itself is done by Engine/Tools/shader_census.py (finds shaders in the game files) and
`sw_app.exe --prepare-shaders` / `--warm-shaders` / `--gpu-info` (Engine/Runtime/gfx/sw_gfx_glue.c). This module
only reads their files and output; it never deletes or edits a cache.
"""
from __future__ import annotations

import hashlib
import json
import re
import time
from dataclasses import dataclass, field
from pathlib import Path

MANIFEST_FORMAT = 2                      # prepared.json "format" written by the current exe


def translator_id(dll: Path) -> str:
    """The translator identity as the runtime computes it without a .id file: sha256 of the translator binary."""
    try:
        return hashlib.sha256(dll.read_bytes()).hexdigest()
    except OSError:
        return ""


def _json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def gpu_key_from_output(text: str) -> str:
    m = re.search(r"^\[gpu\] key (\S+)", text, re.M)
    return m.group(1) if m else ""



def cache_status(cache_dir: Path, xlate: str, gpu_key: str | None) -> dict:
    """state: none | ready | rebuild | refresh, with a label for the button and a sentence for the status line.
    rebuild: the translator, game files (census) or manifest format changed - translate what is missing.
    refresh: only the GPU driver changed - the translated shaders stay valid, the driver cache is warmed again.
    gpu_key None: not known yet (the check runs in the background) - the driver is not judged."""
    man = _json(cache_dir / "prepared.json")
    census = _json(cache_dir / "census.json")
    if man.get("status") != "complete":
        return {"state": "none", "button": "Build Shaders",
                "text": "Not prepared yet. Building translates the game's shaders ahead of play "
                        "(about a minute or more the first time)."}
    n, rec, cen = man.get("variants", 0), man.get("recorded", 0), man.get("census", 0)
    what = f"{n:,} shader variants ({rec:,} met in play, {cen:,} found in game files, default state)"
    why = []
    if man.get("format") != MANIFEST_FORMAT:
        why.append("made by an older version")
    if xlate and man.get("translator") and man["translator"] != xlate:
        why.append("the shader translator changed")
    if census and census.get("programs", 0) and census.get("programs") != man.get("census"):
        why.append("the game files changed")
    if why:
        return {"state": "rebuild", "button": "Rebuild Shaders",
                "text": f"Outdated ({', '.join(why)}): {what}. Rebuilding translates only what is missing."}
    if gpu_key:
        warmed = _json(cache_dir / "warmed.json").get("gpu", "")
        if gpu_key not in (man.get("gpu", ""), warmed):
            return {"state": "refresh", "button": "Refresh Shaders",
                    "text": f"GPU driver changed since the shaders were prepared: {what}. Refresh rebuilds the "
                            "driver cache only (the translated shaders stay valid)."}
    return {"state": "ready", "button": "Rebuild Shaders",
            "text": f"Ready: {what}, prepared {man.get('date', '?')}. Variants not met yet are compiled during "
                    "play in the background."}


@dataclass
class ShaderPrepProgress:
    """Follows the census, translation and driver-cache steps of Build Shaders (or warm only for Refresh)."""
    warm_only: bool = False
    phase: str = "starting"
    scan: tuple = (0, 0)
    translate: tuple = (0, 0)
    build: tuple = (0, 0)
    started: float = field(default_factory=time.time)
    summary: str = ""

    def feed(self, line: str) -> bool:
        s = line.strip()
        m = re.search(r"\[census\] scanning (\d+)/(\d+) files", s)
        if m:
            self.phase, self.scan = "scanning", (int(m.group(1)), int(m.group(2)))
            return True
        m = re.search(r"\[census\] done: (\d+) shader programs", s)
        if m:
            self.phase, self.scan = "scanned", (1, 1)
            return True
        m = re.search(r"\[prep\] translating: (\d+) variants", s)
        if m:
            self.phase, self.translate = "translating", (0, int(m.group(1)))
            return True
        m = re.search(r"gfx_shader: prepare: (\d+) of (\d+) pairs", s)
        if m:
            self.phase, self.translate = "translating", (int(m.group(1)), int(m.group(2)))
            return True
        m = re.search(r"gfx_shader: prepare: (\d+) seed variants", s)
        if m:
            n = int(m.group(1))
            self.phase, self.translate = "building", (n, n)
            return True
        m = re.search(r"stage library prebuild preparation (?:progress|done): (\d+) of (\d+) variants", s)
        if m:
            self.phase, self.build = ("warming" if self.warm_only else "building"), (int(m.group(1)), int(m.group(2)))
            return True
        if s.startswith("[prep] warming"):
            self.phase = "warming"
            return True
        m = re.search(r"\[prep\] (done|failed)[: ](.*)", s)
        if m:
            self.phase, self.summary = m.group(1), m.group(2).strip()
            return True
        return False

    @property
    def fraction(self) -> float:
        if self.phase == "done":
            return 1.0
        bd, bt = self.build
        if self.warm_only:
            return 0.05 + 0.95 * (bd / bt if bt else 0.0) if self.phase == "warming" else 0.02
        sd, st = self.scan
        td, tt = self.translate
        f = 0.05 * (sd / st if st else 0.0)
        if self.phase == "translating":
            f = 0.05 + 0.60 * (td / tt if tt else 0.0)
        if self.phase == "building":
            f = 0.65 + 0.35 * (bd / bt if bt else 0.0)
        return min(1.0, f)

    def describe(self) -> str:
        if self.phase == "scanning":
            return f"Scanning game files for shaders ({self.scan[0]:,}/{self.scan[1]:,} files)"
        if self.phase == "translating":
            d, t = self.translate
            return f"Translating shaders: {d:,} of {t:,} variants done, {max(0, t - d):,} left"
        bd, bt = self.build
        left = f": {bd:,} of {bt:,} done, {max(0, bt - bd):,} left" if bt else ""
        if self.phase == "building":
            return "Building shader stages into the GPU driver cache" + left
        if self.phase == "warming":
            return "Warming the GPU driver cache (translated shaders are reused)" + left
        if self.phase == "done":
            return f"Done: {self.summary}"
        if self.phase == "failed":
            return f"Failed: {self.summary}"
        return "Starting..."
