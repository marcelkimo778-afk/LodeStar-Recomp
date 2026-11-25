#!/usr/bin/env python3
"""Run the game, wait for presented frames, screenshot its window.

    python Engine/Tools/run_and_shoot.py [--wait-present N] [--delay S] [--timeout S] [--out shot.png]

Starts build_work/out/sw_app.exe --real-start, streams its output into
build_work/out/logs/<time>_shoot.log, and once the Nth "nvn: present #"
line (or any present when N=1) appeared, waits --delay seconds and saves a
screenshot of the game window (the top-level window of the process; the whole
screen if it cannot be found).  Stops the game afterwards.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import re
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build_work" / "out"


def window_rect_of(pid: int):
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            if r.right - r.left > 200 and r.bottom - r.top > 150:
                found.append((hwnd, (r.left, r.top, r.right, r.bottom)))
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else (None, None)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--wait-present", type=int, default=1)
    ap.add_argument("--delay", type=float, default=2.0)
    ap.add_argument("--timeout", type=float, default=240.0)
    ap.add_argument("--series", default="", help="comma-separated present numbers: one screenshot at each")
    ap.add_argument("--out", default=str(OUT / "screenshots" / f"{datetime.now():%Y%m%d_%H%M%S}.png"))
    args = ap.parse_args()

    log_path = OUT / "logs" / f"{datetime.now():%Y%m%d_%H%M%S}_shoot.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen([str(OUT / "Pokemon Arceus.exe"), "--real-start", "--max-dispatch", "0"], cwd=OUT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace")
    presents = [0]
    done = threading.Event()

    def pump():
        with log_path.open("w", encoding="utf-8") as f:
            for line in proc.stdout:
                f.write(line)
                m = re.search(r"nvn: present #(\d+)", line)
                if m:
                    presents[0] = max(presents[0], int(m.group(1)))
        done.set()

    threading.Thread(target=pump, daemon=True).start()
    t0 = time.time()
    shot = None
    if args.series:
        wanted = sorted(int(x) for x in args.series.split(",") if x.strip())
        shots = []
        base = Path(args.out).with_suffix("")
        from PIL import ImageGrab
        while wanted and time.time() - t0 < args.timeout and not done.is_set():
            if presents[0] >= wanted[0]:
                n = wanted.pop(0)
                hwnd, rect = window_rect_of(proc.pid)
                img = ImageGrab.grab(rect) if rect else ImageGrab.grab()
                path = f"{base}_f{n:05d}.png"
                Path(path).parent.mkdir(parents=True, exist_ok=True)
                img.save(path)
                shots.append(path)
                print(f"shot at present {presents[0]}: {path}", flush=True)
            time.sleep(0.05)
        shot = shots[-1] if shots else None
        wanted = []
    while not args.series and time.time() - t0 < args.timeout and not done.is_set():
        if presents[0] >= args.wait_present:
            time.sleep(args.delay)
            hwnd, rect = window_rect_of(proc.pid)
            try:
                from PIL import ImageGrab
            except ImportError:
                print("Pillow missing: no screenshot")
                break
            if hwnd:
                ctypes.windll.user32.SetForegroundWindow(hwnd)
                time.sleep(0.3)
            img = ImageGrab.grab(rect) if rect else ImageGrab.grab()
            Path(args.out).parent.mkdir(parents=True, exist_ok=True)
            img.save(args.out)
            shot = args.out
            break
        time.sleep(0.2)
    still_running = proc.poll() is None
    if still_running:
        proc.kill()
    proc.wait()
    print(f"presents seen: {presents[0]}; exit code {proc.returncode}{' (killed)' if still_running else ''}")
    print(f"log: {log_path}")
    print(f"screenshot: {shot or 'none'}")
    return 0 if shot else 1


if __name__ == "__main__":
    sys.exit(main())
