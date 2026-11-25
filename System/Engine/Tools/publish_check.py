#!/usr/bin/env python3
"""
Publish check: compute SHA-256 of every file in configured exefs,
scan the rest of the project (skipping ignored folders) for any file
with a matching hash or size, and list every file larger than 20 MB.
"""
import hashlib
import json
import sys
from pathlib import Path


def _project_root() -> Path:
    return Path(__file__).resolve().parents[3]   # the project folder (this file is System/Engine/Tools/)


CONFIG_FILE = "Settings/config.json"


def _load_config() -> dict:
    config_path = _project_root() / CONFIG_FILE
    if config_path.is_file():
        try:
            return json.loads(config_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return {}


def _resolve_exefs() -> Path:
    cfg = _load_config()
    if "exefs_dir" in cfg:
        return (_project_root() / cfg["exefs_dir"]).resolve()
    return (_project_root() / "Game/exefs").resolve()


IGNORED_DIRS = {
    "roms", "System Resources", "Build", ".build", ".freebuff", "Build/Cache",
    "_to_delete", "backups", "launcher", "Engine", "Docs", "Config"
}

IGNORED_FILES = {
    "Config/config.json", "cleanup_list.txt", "hardcoded_paths_report.json",
    "NOTES.md", "README.txt", "HANDOFF.txt", "IMPORTANTTTTTTTT.txt",
    "seeds.txt", "batch_seeds.txt", "build_runtime.bat", "Engine/swrecomp.py",
    "LodeStar.exe", "file_tree.txt"
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    root = _project_root()
    exefs = _resolve_exefs()

    print(f"Project root: {root}")
    print(f"ExeFS: {exefs}")
    print()

    # Compute hashes of all files in exefs
    print("Hashing ExeFS files...")
    exefs_hashes = {}
    exefs_sizes = {}
    for p in exefs.rglob("*"):
        if p.is_file():
            try:
                h = sha256_file(p)
                sz = p.stat().st_size
                exefs_hashes[h] = p.relative_to(exefs)
                exefs_sizes[sz] = p.relative_to(exefs)
            except Exception as e:
                print(f"  Warning: failed to hash {p}: {e}")

    print(f"  Hashed {len(exefs_hashes)} files from ExeFS")
    print()

    # Scan project for matching hashes/sizes (excluding ignored dirs)
    print("Scanning project for duplicates...")
    matches_hash = []
    matches_size = []
    large_files = []

    for p in root.rglob("*"):
        if not p.is_file():
            continue

        # Skip ignored directories
        rel = p.relative_to(root)
        if any(part in IGNORED_DIRS for part in rel.parts):
            continue
        if rel.name in IGNORED_FILES:
            continue

        try:
            sz = p.stat().st_size
            if sz > 20 * 1024 * 1024:  # 20 MB
                large_files.append((rel, sz))

            if sz in exefs_sizes:
                matches_size.append((rel, sz, exefs_sizes[sz]))

            h = sha256_file(p)
            if h in exefs_hashes:
                matches_hash.append((rel, h, exefs_hashes[h]))
        except Exception as e:
            print(f"  Warning: failed to check {p}: {e}")

    # Report
    print()
    print("=" * 60)
    print("RESULTS")
    print("=" * 60)

    if matches_hash:
        print(f"\nFILES WITH MATCHING SHA-256 (exact duplicates): {len(matches_hash)}")
        for rel, h, exefs_rel in matches_hash:
            print(f"  {rel}  ->  exefs/{exefs_rel}  (hash: {h[:16]}...)")
    else:
        print("\nNo files with matching SHA-256 found outside ExeFS.")

    if matches_size:
        print(f"\nFILES WITH MATCHING SIZE (possible duplicates): {len(matches_size)}")
        for rel, sz, exefs_rel in matches_size:
            print(f"  {rel}  ({sz} bytes)  ->  exefs/{exefs_rel}")
    else:
        print("\nNo files with matching size found outside ExeFS.")

    if large_files:
        print(f"\nFILES LARGER THAN 20 MB: {len(large_files)}")
        for rel, sz in sorted(large_files, key=lambda x: -x[1]):
            print(f"  {rel}  ({sz / (1024*1024):.2f} MB)")
    else:
        print("\nNo files larger than 20 MB found outside ignored directories.")

    print()
    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())