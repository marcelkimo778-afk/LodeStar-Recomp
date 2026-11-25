#!/usr/bin/env python3
"""Build a LodeStar release: the portable zip and the installer.

    python System/Engine/Tools/installer/make_release.py [--version 0.1.0] [--out Developer/Release]

1. stage: the committed project files (git archive HEAD - nothing local, nothing generated) + LodeStar.exe +
   empty Game/ and Mods/ folders;
2. check: no file from the game may be in it (exefs / NSO / NCA / NSP / romfs data, generated C, module images,
   PGO profiles, saves) - the release stops if one is found;
3. LodeStar-<version>-portable.zip and LodeStar-Setup-<version>.exe (Inno Setup 6, LodeStar.iss) in --out;
4. update.json: what the launcher's update needs (launcher / rebuild / recompile, launcher/updater.py), from the files
   that changed since the previous release tag (v*) of this repository;
--publish: also tag this repository, refresh the public copy (--public, default ../LodeStar-Recomp), commit and push
   it, and create the GitHub release with the three files (needs the GitHub CLI, logged in).  The version comes
   from System/VERSION."""
from __future__ import annotations

import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SYSTEM = HERE.parents[2]                 # System/
PROJECT = SYSTEM.parent                  # the project folder (git repository root)
ISCC = [Path(os.environ.get("LOCALAPPDATA", "")) / "Programs/Inno Setup 6/ISCC.exe",
        Path(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"), Path(r"C:\Program Files\Inno Setup 6\ISCC.exe")]
FORBIDDEN_NAMES = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1", "prod.keys", "title.keys"}
FORBIDDEN_EXT = {".nso", ".nca", ".nsp", ".xci", ".profdata", ".profraw", ".bfres", ".gfpak", ".trpak"}
FORBIDDEN_PARTS = ("build_work/gen/", "build_work/images/", "build_work/out/", "build_work/exefs", "/romfs/",
                   "/exefs/", "bench_routes/", "Engine/pgo/", "save_backups/", "save_slots/", "/save/")


def stage(dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    dirty = subprocess.run(["git", "-C", str(PROJECT), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    if dirty:
        print("note: uncommitted changes are NOT in the release (only HEAD is):\n" + dirty)
    tar = subprocess.run(["git", "-C", str(PROJECT), "archive", "--format=tar", "HEAD"], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(dst, filter="data")
    exe = PROJECT / "LodeStar.exe"
    if not exe.is_file():
        sys.exit("LodeStar.exe is missing: python System/Engine/Tools/build_lodestar.py")
    shutil.copy2(exe, dst / "LodeStar.exe")
    for d in ("Game", "Mods"):
        (dst / d).mkdir(exist_ok=True)
    (dst / "Game" / "PUT YOUR DUMP HERE.txt").write_text(
        "Use 'Import game files' in LodeStar: pick the folder with your own extracted exefs and romfs\n"
        "(Pokemon Legends: Arceus 1.0.0, dumped from your Switch). No game files come with LodeStar.\n", encoding="utf-8")
    (dst / "Mods" / "README.txt").write_text(
        "Your mods go here (or use the mod browser in LodeStar): Mods/01001F5010DFA000/<mod name>/romfs|exefs|cheats\n",
        encoding="utf-8")


def check(dst: Path) -> None:
    bad = []
    for p in dst.rglob("*"):
        rel = p.relative_to(dst).as_posix()
        fixture = rel.startswith(("System/Engine/Tests/mods_fixture/", "System/Engine/Tests/modbrowser_fixture/"))
        if p.is_file() and (p.suffix.lower() in FORBIDDEN_EXT or (not fixture and (
                p.name in FORBIDDEN_NAMES or any(f in "/" + rel for f in FORBIDDEN_PARTS)))):
            bad.append(rel)
    if bad:
        sys.exit("STOP: files that must never ship are in the release:\n  " + "\n  ".join(bad[:40]))
    size = sum(p.stat().st_size for p in dst.rglob("*") if p.is_file())
    print(f"staged {sum(1 for p in dst.rglob('*') if p.is_file())} files, {size / 2**20:.1f} MB - no game files")


GH = Path(r"C:\Program Files\GitHub CLI\gh.exe")
REPO = "marcelkimo778-afk/LodeStar-Recomp"


def git(*args, cwd=PROJECT, check=True) -> str:
    return subprocess.run(["git", "-C", str(cwd), *args], capture_output=True, text=True, check=check).stdout.strip()


def update_kind(version: str) -> tuple[str, int, str]:
    """(kind, files changed, previous tag) since the newest release tag before this version."""
    sys.path.insert(0, str(SYSTEM / "launcher"))
    import updater
    tags = [t for t in git("tag", "--list", "v*").split() if updater.newer(version, t)]
    if not tags:
        return "recompile", 0, ""                          # the first release: everything is new
    prev = max(tags, key=updater.vtuple)
    changed = [p for p in git("diff", "--name-only", prev, "HEAD").splitlines() if p]
    return updater.kind_of_change(changed), len(changed), prev


def publish(version: str, out: Path, files: list[Path], notes: str, public: Path) -> int:
    if not GH.is_file():
        sys.exit("the GitHub CLI is missing (winget install GitHub.cli, then gh auth login)")
    tag = f"v{version}"
    if tag not in git("tag", "--list", tag).split():
        git("tag", "-a", tag, "-m", f"LodeStar {version}")
    # the public copy: exactly the committed files of this release (no history from here)
    for p in public.iterdir():
        if p.name != ".git":
            shutil.rmtree(p) if p.is_dir() else p.unlink()
    tar = subprocess.run(["git", "-C", str(PROJECT), "archive", "--format=tar", tag], capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(public, filter="data")
    git("add", "-A", cwd=public)
    if git("status", "--porcelain", cwd=public):
        git("commit", "-q", "-m", f"LodeStar {version}", cwd=public)
    has_release = subprocess.run([str(GH), "release", "view", tag, "--repo", REPO], capture_output=True).returncode == 0
    if not has_release:   # no release on it yet: the tag follows this commit (moved if an older test tag exists)
        git("tag", "-f", "-a", tag, "-m", f"LodeStar {version}", cwd=public)
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    for ref in ("main", tag):
        force = ["--force"] if ref == tag and not has_release else []
        r = subprocess.run(["git", "-C", str(public), "push", *force, "origin", ref], capture_output=True, text=True, env=env)
        if r.returncode != 0:
            sys.exit(f"push of {ref} failed: {r.stderr.strip()[:400]}")
    r = subprocess.run([str(GH), "release", "create", tag, *map(str, files), "--repo", REPO, "--title",
                        f"LodeStar {version}", "--notes", notes], capture_output=True, text=True)
    if r.returncode != 0 and "already exists" in r.stderr:
        r = subprocess.run([str(GH), "release", "upload", tag, *map(str, files), "--repo", REPO, "--clobber"],
                           capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"gh release failed: {r.stderr.strip()[:400]}")
    print(f"published {tag}: {r.stdout.strip()}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=(SYSTEM / "VERSION").read_text(encoding="utf-8").strip())
    ap.add_argument("--out", default=str(PROJECT / "Developer" / "Release"))
    ap.add_argument("--notes", default="", help="release notes (shown on GitHub and in the launcher's update dialog)")
    ap.add_argument("--publish", action="store_true")
    ap.add_argument("--public", default=str(PROJECT.parent / "LodeStar-Recomp"))
    a = ap.parse_args()
    if a.version != (SYSTEM / "VERSION").read_text(encoding="utf-8").strip():
        sys.exit("--version differs from System/VERSION: change System/VERSION and commit it first")
    out = Path(a.out)
    st = out / "stage" / "LodeStar"
    stage(st)
    check(st)
    zpath = out / f"LodeStar-{a.version}-portable.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(st.rglob("*")):
            z.write(p, Path("LodeStar") / p.relative_to(st))
    print(f"wrote {zpath} ({zpath.stat().st_size / 2**20:.1f} MB)")
    images = out / "build"
    subprocess.run([sys.executable, str(HERE / "make_images.py"), str(images)], check=True)
    iscc = next((p for p in ISCC if p.is_file()), None)
    if not iscc:
        sys.exit("Inno Setup 6 is missing: winget install JRSoftware.InnoSetup")
    r = subprocess.run([str(iscc), "/Q", f"/DStage={st}", f"/DImages={images}", f"/DAppVer={a.version}", f"/O{out}",
                        str(HERE / "LodeStar.iss")], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:], r.stderr[-3000:])
        return 1
    setup = out / f"LodeStar-Setup-{a.version}.exe"
    print(f"wrote {setup} ({setup.stat().st_size / 2**20:.1f} MB)")
    import hashlib
    import json
    kind, nchanged, prev = update_kind(a.version)
    notes = a.notes or (f"Changes since {prev}: {nchanged} files." if prev else "First release.")
    manifest = {"version": a.version, "kind": kind, "notes": notes, "zip": zpath.name,
                "sha256": hashlib.sha256(zpath.read_bytes()).hexdigest(), "previous": prev, "files_changed": nchanged}
    upath = out / "update.json"
    upath.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {upath}: kind {kind}" + (f" ({nchanged} files changed since {prev})" if prev else " (first release)"))
    if a.publish:
        return publish(a.version, out, [setup, zpath, upath], notes, Path(a.public))
    return 0


if __name__ == "__main__":
    sys.exit(main())
