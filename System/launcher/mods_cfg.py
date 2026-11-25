"""Mod config (Settings/mods.json) and the game's mod list (sw_app.exe --list-mods) - no tkinter, so it is tested
(tests/test_mods_cfg.py).  The runtime (Engine/Runtime/src/sw_mods.c) is the one place that finds mods; the
launcher only shows what it reports and writes the config it reads.  Format: MODS.md."""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

DEFAULTS = {"enabled": True, "search_default_dirs": True, "search_emulator_dirs": False, "mods_dirs": [], "mods": []}


def load(path: Path) -> dict:
    """Settings/mods.json (comments allowed, as the runtime allows them); defaults if missing or broken."""
    cfg = json.loads(json.dumps(DEFAULTS))
    try:
        text = Path(path).read_text(encoding="utf-8-sig")
    except OSError:
        return cfg
    lines = []
    for line in text.splitlines():                  # drop // and # comment lines (not inside strings: whole lines)
        s = line.strip()
        lines.append("" if s.startswith("//") or s.startswith("#") else line)
    try:
        data = json.loads("\n".join(lines))
    except ValueError:
        return cfg
    if isinstance(data, dict):
        cfg.update(data)
    return cfg


def save(path: Path, cfg: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def list_mods(exe: Path, config: Path, extra_env: dict | None = None, timeout: int = 60) -> dict:
    """What the game will load: runs `sw_app.exe --list-mods` with the same config the game gets."""
    env = dict(os.environ)
    env["SW_MODS_CONFIG"] = str(config)
    env.pop("SW_NO_MODS", None)
    env.update(extra_env or {})
    r = subprocess.run([str(exe), "--list-mods"], cwd=str(Path(exe).parent), env=env, capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=timeout,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    out = r.stdout.strip().splitlines()
    for line in reversed(out):                      # the JSON is the last stdout line; messages go to stderr
        if line.startswith("{"):
            data = json.loads(line)
            data["messages"] = [m[7:] for m in r.stderr.splitlines() if m.startswith("[mods] ")]
            return data
    raise RuntimeError((r.stderr or r.stdout or f"exit code {r.returncode}").strip()[-2000:])


def rows_from_listing(listing: dict) -> list[dict]:
    """Rows in priority order (the runtime already sorted them by the config, then by name)."""
    rows = []
    for m in sorted(listing.get("mods", []), key=lambda m: m.get("priority", 1 << 20)):
        rows.append({
            "name": m["name"],
            "enabled": bool(m.get("enabled", True)),
            "marker": m.get("why_off", "") == ".disabled file in the mod folder",
            "layout": m.get("layout", ""),
            "path": m.get("path", ""),
            "romfs": m.get("romfs_files", 0) if m.get("romfs") else None,
            "exefs": m.get("exefs_files", 0) if m.get("exefs") else None,
            "cheats": [dict(c) for c in m.get("cheats", [])],
            "code_patches": int(m.get("code_patches", 0) or 0),
            "fps_conflict": bool(m.get("fps_conflict", False)),
            "meta": read_meta(m.get("path", "")),
        })
    return rows


def read_meta(folder: str) -> dict:
    """mod_meta.json of a mod the browser installed ({} for others)."""
    try:
        d = json.loads((Path(folder) / "mod_meta.json").read_text(encoding="utf-8")) if folder else {}
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def notes(row: dict, target: str = "1.0.0") -> str:
    """The Installed list's notes column: compatibility (browser-installed mods), code patch / FPS warnings."""
    out = []
    meta = row.get("meta") or {}
    if meta:
        c = meta.get("compat", "unknown")
        out.append(f"v{target}" if c == "match" else f"wrong version ({meta.get('detected_version') or '?'})"
                   if c == "mismatch" else "unknown version")
    if row.get("fps_conflict"):
        out.append("! clashes with the built-in FPS modes (skipped)")
    elif row.get("code_patches"):
        out.append(f"! {row['code_patches']} code patch(es) skipped")
    if row.get("update"):
        out.append(f"update: {row['update']}")
    return ", ".join(out)


def describe(row: dict) -> str:
    parts = []
    if row["romfs"] is not None:
        parts.append(f"{row['romfs']} file(s)")
    if row["exefs"] is not None:
        parts.append(f"{row['exefs']} patch file(s)")
    if row["cheats"]:
        on = sum(1 for c in row["cheats"] if c.get("enabled"))
        parts.append(f"{len(row['cheats'])} cheat(s), {on} on")
    return ", ".join(parts) or "-"


def config_from_rows(cfg: dict, rows: list[dict]) -> dict:
    """The config with the mods in this order (first = highest priority), their on/off state and chosen cheats."""
    out = dict(cfg)
    mods = []
    for r in rows:
        # a .disabled marker file is the mod folder's own switch: do not copy it into the config as well
        e: dict = {"name": r["name"], "enabled": bool(r["enabled"] or r.get("marker"))}
        chosen = [c["name"] for c in r.get("cheats", []) if c.get("enabled")]
        if chosen:
            e["cheats"] = chosen
        mods.append(e)
    out["mods"] = mods
    out.pop("disabled", None)                       # folded into "mods"
    out.pop("cheats", None)
    return out


def move(rows: list[dict], i: int, delta: int) -> int:
    j = i + delta
    if 0 <= i < len(rows) and 0 <= j < len(rows):
        rows[i], rows[j] = rows[j], rows[i]
        return j
    return i


def game_env(config_dir: Path) -> dict[str, str]:
    """Environment for the game run: the mod config the launcher edits (none yet: the game's defaults)."""
    p = Path(config_dir) / "mods.json"
    return {"SW_MODS_CONFIG": str(p)} if p.is_file() else {}
