#!/usr/bin/env python3
"""Build the mod browser's built-in GitHub index (launcher/assets/github_index.json).

    python Engine/Tools/github_index.py [--repos FILE] [--out FILE]

For every repository in the list it fetches the file list with git (a blob-less, depth-1 clone: no GitHub API
requests, so no 60-an-hour limit, and nothing but names and sizes is downloaded) and records this game's mods:
  - archives (.zip / .7z / .rar) whose path names the game (title id 01001F5010DFA000 or "arceus");
  - folder mods: a folder whose path names the game and that holds romfs/, exefs/ or cheats/ (the files are
    fetched one by one from raw.githubusercontent.com when a player installs it).
The launcher ships the result: the built-in repositories then list instantly and use no API requests; repositories
a player adds are looked up live.  Run it before a release (needs git on PATH and internet access)."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]          # System/
TID = "01001f5010dfa000"
ARCHIVES = (".zip", ".7z", ".rar")
MOD_DIRS = ("romfs", "exefs", "cheats")
MAX_FOLDER_FILES, MAX_FOLDER_BYTES = 400, 200 << 20

# Repositories checked by hand (2026-11-25); names are what the launcher shows.  Order = list order.
DEFAULT_REPOS = [
    ("theboy181/switch-ptchtxt-mods", "theboy181 - pchtxt mods"),
    ("Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods", "Fl4sh9174 - Ultrawide & FPS mods"),
    ("ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats", "ChanseyIsTheBest - 60 FPS / resolution / graphics"),
    ("Kenji-NX/switch-pchtxt-mods", "Kenji-NX - pchtxt mods"),
    ("yuzu-mirror/yuzu-mod-archive", "yuzu mod archive (mirror)"),
    ("ADEMOLA200/Switch-Emulator-Mod-Database", "ADEMOLA200 - Switch mod database"),
    ("wikid24/ryujinx-discord-mods-cheats", "Ryujinx Discord mods & cheats (mirror)"),
    ("OldManKain/Switch-Cheats-Mods-Saves", "OldManKain - cheats, mods, saves"),
    ("Koriebonx98/Game_OS.Mods.switch", "Game OS - Switch mods"),
    ("medinacristofer034-art/SwitchOptimizerMods", "Switch Optimizer Mods"),
    ("cdnutter/NX-Graphics-Patches", "cdnutter - graphics patches"),
    ("whytecloud/switch-ultrawide-mods-pchtxt", "whytecloud - ultrawide pchtxt"),
    ("StevensND/switch-port-mods", "StevensND - port mods"),
    ("TLin-Y/Switch-Ultrawide-Mods", "TLin-Y - Ultrawide mods"),
    ("CharredPlatinum/Switch-Ultrawide-Mods", "CharredPlatinum - Ultrawide mods"),
    ("ansongo/switch-ultrawide-mods", "ansongo - Ultrawide mods"),
    ("andercard0/Switch-Ultrawide-Mods", "andercard0 - Ultrawide mods"),
    ("New8ie/Switch-Ultrawide-Mods", "New8ie - Ultrawide mods"),
    ("Arch9SK7/MyNXCheats", "Arch9SK7 - cheats"),
    ("Lyrx997/Switch-Cheats", "Lyrx997 - cheats"),
    ("HamletDuFromage/switch-cheats-db", "switch-cheats-db (cheats)"),
    ("t0mtee/PCHTXT-Updates", "t0mtee - pchtxt updates"),
]


def is_game(path: str) -> bool:
    """The path names this game, and no other game's title id (a folder "[ Arceus]" under another game's id is
    that game's cheat, not ours)."""
    low = path.lower()
    if any(t != TID for t in re.findall(r"(?<![0-9a-f])01[0-9a-f]{11}000(?![0-9a-f])", low)):
        return False
    return TID in low or "arceus" in low


def version_in(path: str) -> str:
    m = re.search(r"(?<![\w.])(\d+\.\d+\.\d+)(?![\w])", path)
    return m.group(1) if m else ""


def list_repo(repo: str, work: Path) -> tuple[list[tuple[str, int, str]], str]:
    """(path, size 0, blob sha) of every file at HEAD, and the commit; [] if the clone fails.  No sizes: in a
    blob-less clone `ls-tree -l` downloads every file to measure it (95 MB for one repository); the sha is enough
    to tell files apart (forks carry the same blobs)."""
    d = work / repo.replace("/", "__")
    url = f"https://github.com/{repo}.git"
    r = subprocess.run(["git", "clone", "--quiet", "--filter=blob:none", "--no-checkout", "--depth", "1", url, str(d)],
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        print(f"  {repo}: clone failed: {r.stderr.strip()[:200]}", file=sys.stderr)
        return [], ""
    ls = subprocess.run(["git", "-C", str(d), "ls-tree", "-r", "-z", "HEAD"], capture_output=True, timeout=600)
    head = subprocess.run(["git", "-C", str(d), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    files = []
    for rec in ls.stdout.decode("utf-8", "replace").split("\0"):
        if not rec or "\t" not in rec:
            continue
        meta, path = rec.split("\t", 1)
        parts = meta.split()
        if len(parts) >= 3 and parts[1] == "blob":
            files.append((path, 0, parts[2]))
    return files, head


def mods_of(repo: str, files: list[tuple[str, int, str]]) -> list[dict]:
    out = []
    # archives
    for path, size, sha in files:
        if path.lower().endswith(ARCHIVES) and is_game(path):
            out.append({"repo": repo, "kind": "file", "path": path, "size": size, "sha": sha, "version": version_in(path)})
    # folder mods: the outermost folder (naming the game) directly holding romfs/ exefs/ cheats/
    roots = set()
    for path, _size, _sha in files:
        parts = path.split("/")
        for i in range(1, len(parts) - 1):
            if parts[i].lower() in MOD_DIRS:
                root = "/".join(parts[:i])
                if is_game(root + "/" + parts[i]) or is_game(path):
                    roots.add(root)
                break
    for root in sorted(roots):
        if any(root.startswith(o + "/") for o in roots if o != root):
            continue                                      # inside another mod folder
        inside = [(p, s, h) for p, s, h in files if p.startswith(root + "/")]
        if not inside or len(inside) > MAX_FOLDER_FILES:
            continue
        import hashlib
        key = hashlib.sha1("".join(sorted(p[len(root) + 1:] + h for p, _s, h in inside)).encode()).hexdigest()
        out.append({"repo": repo, "kind": "folder", "path": root, "size": 0, "sha": key, "version": version_in(root),
                    "files": [[p[len(root) + 1:], s] for p, s, _h in inside]})
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repos", help="a file with one owner/repo [name] per line (default: the list in this tool)")
    ap.add_argument("--out", default=str(ROOT / "launcher" / "assets" / "github_index.json"))
    a = ap.parse_args()
    repos = DEFAULT_REPOS
    if a.repos:
        repos = []
        for line in Path(a.repos).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                rp, _, nm = line.partition(" ")
                repos.append((rp, nm.strip() or rp))
    index = {"generated": time.strftime("%Y-%m-%d"), "title_id": TID.upper(), "repos": [], "mods": []}
    with tempfile.TemporaryDirectory(prefix="ghidx_") as tmp:
        for repo, name in repos:
            t0 = time.time()
            files, head = list_repo(repo, Path(tmp))
            mods = mods_of(repo, files) if files else []
            print(f"  {repo}: {len(files)} files, {len(mods)} mods for this game ({time.time() - t0:.0f} s)")
            index["repos"].append({"repo": repo, "name": name, "commit": head, "files": len(files), "mods": len(mods),
                                   "ok": bool(files)})
            index["mods"] += mods
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(index, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    useful = sum(1 for r in index["repos"] if r["mods"])
    print(f"wrote {a.out}: {len(index['mods'])} mods from {useful} of {len(repos)} repositories")
    return 0


if __name__ == "__main__":
    sys.exit(main())
