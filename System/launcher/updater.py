"""LodeStar updates from GitHub releases (no tkinter here, so it is tested: tests/test_updater.py).

Each release has an update.json asset (written by Engine/Tools/installer/make_release.py from the files that changed
since the previous release) next to the portable zip:
    {"version": "0.2.0", "kind": "launcher" | "rebuild" | "recompile", "notes": "...",
     "zip": "LodeStar-0.2.0-portable.zip", "sha256": "<hex of the zip>"}
kind: launcher  - only the launcher / documents changed: swap the files, restart, nothing to build;
      rebuild   - the runtime, the graphics layer, the build or the shader translator changed: rebuild the game;
      recompile - the recompiler changed: translate the game's code again, then rebuild.
Applying one: the zip is downloaded and checked, then a small helper (a copy of this file run with --apply) waits
for the launcher to close, copies the new files over the install folder - never the player's own data (PROTECTED) -
writes Settings/pending_update.json and starts LodeStar.exe again; the launcher then runs the build the kind needs."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = "marcelkimo778-afk/LodeStar-Recomp"
UA = "LodeStar-Launcher/1.0"
KINDS = ("launcher", "rebuild", "recompile")            # least to most work
BUTTON = {"launcher": "Update launcher", "rebuild": "Update & rebuild", "recompile": "Update & full rebuild"}
# never overwritten by an update: the player's game files, mods, settings, saves, the built game and its caches
PROTECTED = ("Game/", "Mods/", "Settings/", "System/build_work/gen/", "System/build_work/images/",
             "System/build_work/out/", "System/build_work/objcache/", "System/build_work/xlate/sw_xlate",
             "System/Build/", "System/Engine/pgo/", "unins")


def project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def current_version(root: Path | None = None) -> str:
    try:
        return (Path(root or project_root()) / "System" / "VERSION").read_text(encoding="utf-8").strip()
    except OSError:
        return "0.0.0"


def vtuple(v: str) -> tuple:
    out = []
    for part in str(v).lstrip("vV").split("."):
        num = "".join(ch for ch in part if ch.isdigit())
        out.append(int(num) if num else 0)
    return tuple(out + [0] * (3 - len(out)))


def newer(a: str, b: str) -> bool:
    """a newer than b."""
    return vtuple(a) > vtuple(b)


def kind_of_change(paths) -> str:
    """The work an update needs, from the files that changed (make_release.py; repo-relative paths)."""
    kind = "launcher"
    for p in paths:
        p = p.replace("\\", "/")
        if p.startswith("System/Engine/Source/") or p == "System/Engine/swrecomp.py":
            return "recompile"
        if p.startswith(("System/Engine/Runtime/", "System/build_work/")) or p in ("System/build_work/build.py",):
            kind = "rebuild"
    return kind


def protected(rel: str) -> bool:
    rel = rel.replace("\\", "/")
    return any(rel.startswith(p) for p in PROTECTED)


def _get(url: str, timeout: float = 15, accept: str = "application/vnd.github+json") -> bytes:
    rq = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": accept})
    with urllib.request.urlopen(rq, timeout=timeout) as r:
        return r.read()


def check(repo: str = REPO, root: Path | None = None, timeout: float = 15) -> dict:
    """{"available": bool, "version", "kind", "notes", "zip_url", "sha256", "error"} for the newest release."""
    cur = current_version(root)
    out = {"available": False, "current": cur, "version": cur, "kind": "launcher", "notes": "", "zip_url": "",
           "sha256": "", "error": ""}
    try:
        rel = json.loads(_get(f"https://api.github.com/repos/{repo}/releases/latest", timeout))
    except urllib.error.HTTPError as e:
        out["error"] = "no public release yet" if e.code == 404 else f"GitHub HTTP {e.code}"
        return out
    except (OSError, ValueError) as e:
        out["error"] = f"no connection ({e})"
        return out
    tag = rel.get("tag_name", "")
    if not tag or not newer(tag, cur):
        return out
    assets = {a.get("name"): a.get("browser_download_url") for a in rel.get("assets", [])}
    manifest = {}
    if assets.get("update.json"):
        try:
            manifest = json.loads(_get(assets["update.json"], timeout, "application/octet-stream"))
        except (OSError, ValueError) as e:
            out["error"] = f"update.json unreadable ({e})"
            return out
    zname = manifest.get("zip") or next((n for n in assets if n and n.endswith("-portable.zip")), "")
    if not zname or not assets.get(zname):
        out["error"] = f"release {tag} has no portable zip"
        return out
    kind = manifest.get("kind") if manifest.get("kind") in KINDS else "recompile"   # unknown: the safe, full path
    out.update(available=True, version=tag.lstrip("vV"), kind=kind, notes=manifest.get("notes") or rel.get("body", ""),
               zip_url=assets[zname], sha256=(manifest.get("sha256") or "").lower())
    return out


def download(info: dict, progress=None) -> Path:
    """The release zip into a temporary file, its SHA-256 checked (raises on a mismatch)."""
    rq = urllib.request.Request(info["zip_url"], headers={"User-Agent": UA, "Accept": "application/octet-stream"})
    fd, tmp = tempfile.mkstemp(prefix="lodestar_update_", suffix=".zip")
    h = hashlib.sha256()
    with urllib.request.urlopen(rq, timeout=60) as r, os.fdopen(fd, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    if info.get("sha256") and h.hexdigest() != info["sha256"]:
        os.remove(tmp)
        raise ValueError("the download is damaged (SHA-256 does not match the release)")
    return Path(tmp)


def apply_files(zip_path: Path, root: Path) -> int:
    """Copy the release's files over the install folder, skipping PROTECTED paths; returns the number written.
    The zip holds one top folder (LodeStar/)."""
    n = 0
    root = Path(root).resolve()
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            name = info.filename.replace("\\", "/")
            rel = name.split("/", 1)[1] if "/" in name else ""
            if not rel or info.is_dir() or protected(rel):
                continue
            dst = (root / rel).resolve()
            if root not in dst.parents:
                continue                                    # never outside the install folder
            dst.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(dst, "wb") as out:
                shutil.copyfileobj(src, out)
            n += 1
    return n


def start_apply(zip_path: Path, info: dict, root: Path | None = None) -> None:
    """Hand over to a helper process that waits for this launcher to close, then applies the update and restarts
    LodeStar.  The helper is a copy of this file in the temp folder (the original is replaced by the update)."""
    root = Path(root or project_root())
    helper = Path(tempfile.gettempdir()) / "lodestar_updater.py"
    shutil.copy2(__file__, helper)
    py = sys.executable.replace("pythonw.exe", "python.exe")
    pyw = py.replace("python.exe", "pythonw.exe")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "DETACHED_PROCESS", 0)
    subprocess.Popen([pyw if Path(pyw).is_file() else py, str(helper), "--apply", str(zip_path), str(root),
                      str(os.getpid()), info["kind"], info["version"]], creationflags=flags, close_fds=True)


def _wait_pid(pid: int, timeout: float = 60) -> None:
    if sys.platform != "win32":
        return
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
    if h:
        ctypes.windll.kernel32.WaitForSingleObject(h, int(timeout * 1000))
        ctypes.windll.kernel32.CloseHandle(h)


def _helper(zip_path: str, root: str, pid: str, kind: str, version: str) -> int:
    _wait_pid(int(pid))
    time.sleep(1.0)
    r = Path(root)
    log = r / "Settings" / "update.log"
    try:
        n = apply_files(Path(zip_path), r)
        (r / "Settings").mkdir(exist_ok=True)
        (r / "Settings" / "pending_update.json").write_text(
            json.dumps({"version": version, "kind": kind, "files": n, "at": time.time()}), encoding="utf-8")
        log.write_text(f"updated to {version} ({kind}), {n} files\n", encoding="utf-8")
    except Exception as e:   # the old install stays usable: nothing protected was touched
        log.write_text(f"update to {version} failed: {e}\n", encoding="utf-8")
    try:
        os.remove(zip_path)
    except OSError:
        pass
    exe = r / "LodeStar.exe"
    if exe.is_file():
        subprocess.Popen([str(exe)], cwd=str(r))
    return 0


if __name__ == "__main__" and len(sys.argv) >= 7 and sys.argv[1] == "--apply":
    sys.exit(_helper(*sys.argv[2:7]))
