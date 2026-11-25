#!/usr/bin/env python3
"""Build LodeStar.exe (the launcher's starter, Engine/Tools/lodestar) into the project folder.

    python Engine/Tools/build_lodestar.py

Only needed once (it starts launcher/launcher.py, so launcher changes need no rebuild), or after the icon changes.
Uses the same MSVC toolchain as build_work/build.py."""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Engine" / "Source"))
import swrecomp.pipeline.build as build_mod  # noqa: E402

SRC = ROOT / "Engine" / "Tools" / "lodestar"


def rc_exe(env: dict) -> str | None:
    found = shutil.which("rc", path=env.get("PATH")) if env else shutil.which("rc")
    if found:
        return found
    kits = sorted(Path(r"C:\Program Files (x86)\Windows Kits\10\bin").glob("10.*/x64/rc.exe"))
    return str(kits[-1]) if kits else None


def main() -> int:
    tool = build_mod.find_toolchain()
    if tool.name != "msvc":
        print("LodeStar.exe needs MSVC (Windows)", file=sys.stderr)
        return 1
    rc = rc_exe(tool.env)
    out = ROOT.parent / "LodeStar.exe"   # the project folder (ROOT = System/)
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        objs = [str(SRC / "lodestar.c")]
        if rc:
            r = subprocess.run([rc, "/nologo", "/fo", str(t / "lodestar.res"), str(SRC / "lodestar.rc")],
                               env=tool.env, capture_output=True, text=True, cwd=SRC)
            if r.returncode == 0:
                objs.append(str(t / "lodestar.res"))
            else:
                print("note: no icon (rc failed):", (r.stdout + r.stderr)[-400:])
        r = subprocess.run([tool.compiler, "/nologo", "/O1", "/W3", "/DUNICODE", "/D_UNICODE", "/Fo" + str(t) + "\\",
                            "/Fe:" + str(out), *objs, "user32.lib", "kernel32.lib",
                            "/link", "/SUBSYSTEM:WINDOWS", "/ENTRY:wWinMainCRTStartup"],
                           env=tool.env, capture_output=True, text=True)
        if r.returncode != 0 or not out.is_file():
            print((r.stdout + r.stderr)[-2000:], file=sys.stderr)
            return 1
    print(f"built {out} ({out.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
