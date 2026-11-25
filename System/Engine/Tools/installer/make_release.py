#!/usr/bin/env python3
"""Build a LodeStar release: the portable zip and the installer.

    python System/Engine/Tools/installer/make_release.py [--version 0.1.0] [--out Developer/Release]

1. stage: the committed project files (git archive HEAD - nothing local, nothing generated) + LodeStar.exe +
   empty Game/ and Mods/ folders;
2. check: no file from the game may be in it (exefs / NSO / NCA / NSP / romfs data, generated C, module images,
   PGO profiles, saves) - the release stops if one is found;
3. LodeStar-<version>-portable.zip and LodeStar-Setup-<version>.exe (Inno Setup 6, LodeStar.iss) in --out."""
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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="0.1.0")
    ap.add_argument("--out", default=str(PROJECT / "Developer" / "Release"))
    a = ap.parse_args()
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
