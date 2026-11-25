"""Save manager for the Arceus recomp (no tkinter here, so it is testable).

The runtime keeps the game's save data as plain files in build_work/out/save
(sw_ipc.c handle_fs), the same layout as a Switch save dump (JKSV) or an
emulator's save folder.  So saves can be copied in and out as folders.

Editing values inside the save (money, items, ...) needs the game's save
encryption (SwishCrypto); see CHEATS below - planned, not implemented yet.
"""
from __future__ import annotations

import os
import shutil
import time
from pathlib import Path

TITLE_ID = "01001F5010DFA000"   # Pokémon Legends: Arceus

# What players ask for most in PLA save editors / cheat lists.  "status" is
# shown in the launcher; nothing here is wired up until the save decoder works.
CHEATS = [
    ("Money (Pokédollars)", "set to any amount, e.g. 9,999,999"),
    ("Merit points", "for the Ancient Retreat outfits / items"),
    ("Items & crafting materials", "999 of everything (Poké Balls, Apricorns, ores)"),
    ("Satchel space", "max bag slots"),
    ("Pokédex research", "complete research tasks / research level 10"),
    ("Pokémon editor", "level, nature, shiny, alpha, effort levels"),
    ("Unlock outfits & hairstyles", "all clothing in the wardrobe"),
    ("Time of day / weather", "pick morning/day/evening/night"),
]
CHEATS_STATUS = ("Planned: needs the save decoder (SwishCrypto). It will be "
                 "built and checked against the first real save the game writes.")


def save_dir(work_dir: Path) -> Path:
    return work_dir / "out" / "save"


def backup_dir(work_dir: Path) -> Path:
    return work_dir / "out" / "save_backups"


def list_files(folder: Path) -> list[tuple[str, int, float]]:
    """(relative path, size, mtime) for every file below folder."""
    if not folder.is_dir():
        return []
    out = []
    for p in sorted(folder.rglob("*")):
        if p.is_file():
            st = p.stat()
            out.append((p.relative_to(folder).as_posix(), st.st_size, st.st_mtime))
    return out


def has_save(folder: Path) -> bool:
    return bool(list_files(folder))


def list_backups(work_dir: Path) -> list[Path]:
    b = backup_dir(work_dir)
    if not b.is_dir():
        return []
    return sorted((p for p in b.iterdir() if p.is_dir()), key=lambda p: p.name, reverse=True)


def _stamp() -> str:
    return time.strftime("%Y%m%d_%H%M%S")


def backup(work_dir: Path, label: str = "") -> Path | None:
    """Copy the current save into save_backups/<time>[_label]; None if no save."""
    src = save_dir(work_dir)
    if not has_save(src):
        return None
    safe = "".join(ch if ch.isalnum() or ch in "-_" else "_" for ch in label.strip())[:40]
    dst = backup_dir(work_dir) / (_stamp() + (f"_{safe}" if safe else ""))
    n = 1
    while dst.exists():
        dst = dst.with_name(f"{dst.name}_{n}")
        n += 1
    shutil.copytree(src, dst)
    return dst


def _replace_save(work_dir: Path, src: Path) -> Path | None:
    """Make src the current save.  The old save is backed up first (never deleted)."""
    if not src.is_dir() or not has_save(src):
        raise ValueError(f"{src} contains no save files")
    old = backup(work_dir, "before_restore")
    dst = save_dir(work_dir)
    if dst.exists():
        # keep the old folder (moves are allowed, deletes are not): it is
        # already in save_backups, this just clears the live folder
        trash = work_dir / "out" / f"_trash_save_{_stamp()}"
        n = 1
        while trash.exists():
            trash = trash.with_name(f"_trash_save_{_stamp()}_{n}")
            n += 1
        os.replace(dst, trash)
    shutil.copytree(src, dst)
    return old


def restore(work_dir: Path, backup_path: Path) -> Path | None:
    return _replace_save(work_dir, backup_path)


def find_save_root(folder: Path) -> Path:
    """Accept a save folder or a parent of it: the folder holding the files.

    JKSV / emulator dumps sometimes nest one level ("0/", the title id, a user
    id).  Descend while there is exactly one sub-folder and no files.
    """
    cur = folder
    for _ in range(4):
        entries = list(cur.iterdir()) if cur.is_dir() else []
        files = [e for e in entries if e.is_file()]
        dirs = [e for e in entries if e.is_dir()]
        if files or len(dirs) != 1:
            break
        cur = dirs[0]
    return cur


def import_source(path: Path) -> Path:
    """The folder to import for a chosen folder or file (picking the 'main' file means its folder)."""
    return path.parent if path.is_file() else find_save_root(path)


def check_save(path: Path, keys=None) -> tuple[bool, str]:
    """(ok, text) for a chosen save folder/file: does it hold a Legends: Arceus 'main'?"""
    folder = import_source(path)
    main = folder / "main"
    if not main.is_file():
        return False, f"no 'main' file in {folder}"
    if keys is None:
        return True, f"'main' found ({main.stat().st_size / 1024:,.0f} KB) - not verified"
    try:
        import pla_save
        n = len(pla_save.decrypt(main.read_bytes(), keys))
    except Exception as e:           # noqa: BLE001
        return False, f"'main' is not a Legends: Arceus save ({e})"
    return True, f"Legends: Arceus save, {n} blocks"


def import_save(work_dir: Path, folder: Path) -> Path | None:
    """Import a save folder or its 'main' file (JKSV dump, emulator save, a backup).
    Returns the backup of the old save."""
    return _replace_save(work_dir, import_source(folder))


def export_save(work_dir: Path, dest_parent: Path) -> Path:
    src = save_dir(work_dir)
    if not has_save(src):
        raise ValueError("no save yet - play until the game autosaves")
    dst = dest_parent / f"Arceus_save_{_stamp()}"
    shutil.copytree(src, dst)
    return dst


def imports_dir(work_dir: Path) -> Path:
    """Folder where users can drop save folders/files to import."""
    return work_dir / "out" / "save_imports"


def find_newest_import(work_dir: Path) -> Path | None:
    """Find the newest valid save folder in the imports directory."""
    imp = imports_dir(work_dir)
    if not imp.is_dir():
        return None
    candidates = []
    for p in imp.iterdir():
        src = import_source(p)
        if has_save(src):
            main = src / "main"
            if main.is_file():
                candidates.append((main.stat().st_mtime, src))
    if not candidates:
        return None
    candidates.sort(reverse=True)
    return candidates[0][1]


def emulator_save_candidates() -> list[Path]:
    """Save folders of this game in common emulator locations (only ones that exist)."""
    out: list[Path] = []
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return out
    for emu in ("Ryujinx", "yuzu", "suyu", "sudachi", "citron", "eden"):
        base = Path(appdata) / emu
        if not base.is_dir():
            continue
        # yuzu-likes: nand/user/save/0000000000000000/<user>/<TITLE_ID>
        for p in base.glob(f"nand/user/save/*/*/{TITLE_ID}"):
            if p.is_dir():
                out.append(p)
        # Ryujinx: bis/user/save/<index>/0 with an ExtraData naming the title;
        # cheap check: the game's save holds a file called "main"
        for p in base.glob("bis/user/save/*/0"):
            if (p / "main").is_file():
                out.append(p)
    return out


# ---- save library (named saves the player switches between) -----------------------------------
# build_work/out/save_slots/slot_<n>/slot.json  {"name", "created", "updated"}
#                                  /data/       the save files (same layout as build_work/out/save)
# build_work/out/save_slots/state.json          {"active": <n>}: the slot the game's save came from
# Nothing is ever deleted outright: overwritten and deleted slots go to save_backups first.
import json  # noqa: E402
import re  # noqa: E402

_SLOT_RE = re.compile(r"slot_(\d+)$")


def slots_dir(work_dir: Path) -> Path:
    return work_dir / "out" / "save_slots"


def _slot_path(work_dir: Path, slot_id: int) -> Path:
    return slots_dir(work_dir) / f"slot_{int(slot_id)}"


def _read_json(p: Path) -> dict:
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _write_json(p: Path, data: dict) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=1), encoding="utf-8")
    os.replace(tmp, p)


def _folder_size(folder: Path) -> int:
    return sum(size for _n, size, _t in list_files(folder))


def list_slots(work_dir: Path) -> list[dict]:
    """Every save in the library, oldest first: id, name, path, created, updated, size, active."""
    root = slots_dir(work_dir)
    if not root.is_dir():
        return []
    active = active_slot(work_dir)
    out = []
    for p in root.iterdir():
        m = _SLOT_RE.match(p.name)
        if not (p.is_dir() and m and (p / "slot.json").is_file()):
            continue
        meta = _read_json(p / "slot.json")
        sid = int(m.group(1))
        out.append({"id": sid, "name": meta.get("name") or f"Save {sid}", "path": p,
                    "created": meta.get("created", 0), "updated": meta.get("updated", 0),
                    "size": _folder_size(p / "data"), "active": sid == active})
    out.sort(key=lambda s: s["id"])
    return out


def active_slot(work_dir: Path) -> int | None:
    v = _read_json(slots_dir(work_dir) / "state.json").get("active")
    return int(v) if isinstance(v, int) and _slot_path(work_dir, v).is_dir() else None


def _set_active(work_dir: Path, slot_id: int | None) -> None:
    _write_json(slots_dir(work_dir) / "state.json", {"active": slot_id})


def set_active(work_dir: Path, slot_id: int | None) -> None:
    """Which library save the game's save came from (None after an import)."""
    _set_active(work_dir, slot_id)


def _next_name(work_dir: Path) -> str:
    used = {s["name"] for s in list_slots(work_dir)}
    n = 1
    while f"Save {n}" in used:
        n += 1
    return f"Save {n}"


def new_slot(work_dir: Path, name: str | None = None, src: Path | None = None, activate: bool | None = None) -> dict:
    """New Save: a new library folder holding a copy of the game's current save ("Save N"), or of src (an
    emulator save or a backup; the game's save is not changed then)."""
    from_game = src is None
    src = save_dir(work_dir) if from_game else import_source(Path(src))
    if not has_save(src):
        raise ValueError("There is no game save yet - play until the game saves, or import one." if from_game
                         else f"{src} contains no save files.")
    root = slots_dir(work_dir)
    root.mkdir(parents=True, exist_ok=True)
    ids = [int(m.group(1)) for p in root.iterdir() if (m := _SLOT_RE.match(p.name))]
    sid = max(ids, default=0) + 1
    dst = _slot_path(work_dir, sid)
    shutil.copytree(src, dst / "data")
    now = time.time()
    _write_json(dst / "slot.json", {"name": (name or "").strip() or _next_name(work_dir), "created": now,
                                     "updated": now})
    if activate if activate is not None else from_game:
        _set_active(work_dir, sid)
    return next(s for s in list_slots(work_dir) if s["id"] == sid)


def save_info(folder: Path) -> tuple[float, int]:
    """(time the save was last written, size in bytes) of a save folder; (0, 0) if empty."""
    files = list_files(folder)
    return (max((t for _n, _s, t in files), default=0.0), sum(sz for _n, sz, _t in files))


def use_folder(work_dir: Path, folder: Path) -> Path | None:
    """Use an emulator save or a backup as the game's save (the game's save is backed up first)."""
    old = _replace_save(work_dir, import_source(Path(folder)))
    _set_active(work_dir, None)
    return old


def _checked_slot(work_dir: Path, slot_id: int) -> Path:
    """The slot folder, only if it really is one of ours (never the game's save or any other folder)."""
    p = _slot_path(work_dir, slot_id)
    root = slots_dir(work_dir).resolve()
    if not (p.is_dir() and (p / "slot.json").is_file() and p.resolve().parent == root
            and _SLOT_RE.match(p.name)):
        raise ValueError(f"Save {slot_id} does not exist.")
    if p.resolve() == save_dir(work_dir).resolve():
        raise ValueError("Refusing to touch the game's active save folder.")
    return p


def _backup_slot(work_dir: Path, p: Path, why: str) -> Path:
    """Move a slot's data (or the whole slot) into save_backups/<time>_<why>_<slot>."""
    dst = backup_dir(work_dir) / f"{_stamp()}_{why}_{p.name}"
    n = 1
    while dst.exists():
        dst = dst.with_name(f"{dst.name}_{n}")
        n += 1
    dst.parent.mkdir(parents=True, exist_ok=True)
    return dst


def save_current(work_dir: Path, slot_id: int) -> Path | None:
    """Save Current: the selected save becomes a copy of the game's latest save.  The previous copy is
    moved to save_backups first (returned); the slot keeps its name, id and creation date."""
    src = save_dir(work_dir)
    if not has_save(src):
        raise ValueError("There is no game save to copy.")
    p = _checked_slot(work_dir, slot_id)
    old = None
    tmp = p / "data_new"
    if tmp.exists():
        os.replace(tmp, _backup_slot(work_dir, p, "leftover"))
    shutil.copytree(src, tmp)                   # copy first: a failed copy leaves the slot as it was
    if (p / "data").exists():
        old = _backup_slot(work_dir, p, "before_save_current")
        os.replace(p / "data", old)
    os.replace(tmp, p / "data")
    meta = _read_json(p / "slot.json")
    meta["updated"] = time.time()
    _write_json(p / "slot.json", meta)
    _set_active(work_dir, int(slot_id))
    return old


def load_slot(work_dir: Path, slot_id: int) -> Path | None:
    """Use this save: copy the slot into the game's save folder (the game's save is backed up first)."""
    p = _checked_slot(work_dir, slot_id)
    old = _replace_save(work_dir, p / "data")
    _set_active(work_dir, int(slot_id))
    return old


def rename_slot(work_dir: Path, slot_id: int, name: str) -> None:
    name = name.strip()
    if not name:
        raise ValueError("The name cannot be empty.")
    p = _checked_slot(work_dir, slot_id)
    meta = _read_json(p / "slot.json")
    meta["name"] = name[:60]
    _write_json(p / "slot.json", meta)


def delete_slot(work_dir: Path, slot_id: int) -> Path:
    """Delete Selected Save: the slot leaves the library (moved to save_backups/<time>_deleted_slot_<n>,
    so a mistake can still be undone).  The game's active save is never touched."""
    p = _checked_slot(work_dir, slot_id)
    dst = _backup_slot(work_dir, p, "deleted")
    os.replace(p, dst)
    if active_slot(work_dir) is None:
        _set_active(work_dir, None)
    return dst
