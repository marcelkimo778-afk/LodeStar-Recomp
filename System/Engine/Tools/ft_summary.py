#!/usr/bin/env python3
"""Frame-time summary of SW_FRAMETIME_LOG files ("present ms" per line).

    python Engine/Tools/ft_summary.py [--from S] [--to S] log1 [log2 ...]

The window is seconds since the first logged frame (default 70..160 s: the
village walk of the bench route, after loading).  Prints per log: average fps,
p50/p95/p99/p99.9 frame time, 1% and 0.1% low (fps of the mean of the slowest
1% / 0.1% frames), counts above 8.33 / 11.11 / 16.67 / 33.3 / 50 / 100 ms, worst.
"""
from __future__ import annotations

import sys
from pathlib import Path


def summary(path: Path, t0: float, t1: float) -> str:
    t, ft = 0.0, []
    for line in path.read_text(errors="replace").splitlines():
        p = line.split()
        if len(p) != 2:
            continue
        ms = float(p[1])
        t += ms / 1000.0
        if t0 <= t <= t1:
            ft.append(ms)
    if not ft:
        return f"{path.name}: no frames in {t0:.0f}..{t1:.0f} s (log ends at {t:.0f} s)"
    s = sorted(ft)
    n = len(s)

    def pct(q: float) -> float:
        return s[min(n - 1, int(q * n))]

    def low(frac: float) -> float:
        k = max(1, int(n * frac))
        return 1000.0 / (sum(s[-k:]) / k)

    over = {lim: sum(1 for v in ft if v > lim) for lim in (8.34, 11.12, 16.68, 33.4, 50.0, 100.0)}
    return (f"{path.name}: {n / (sum(ft) / 1000):.1f} fps | p50 {pct(.5):.2f} p95 {pct(.95):.2f} p99 {pct(.99):.2f} "
            f"p99.9 {pct(.999):.1f} ms | 1% low {low(.01):.1f} 0.1% low {low(.001):.1f} | "
            f">8.3 {over[8.34]} >11.1 {over[11.12]} >16.7 {over[16.68]} >33 {over[33.4]} >50 {over[50.0]} "
            f">100 {over[100.0]} | worst {s[-1]:.0f} ms ({n} frames)")


def main() -> int:
    args = sys.argv[1:]
    t0, t1 = 70.0, 160.0
    if "--from" in args:
        i = args.index("--from"); t0 = float(args[i + 1]); del args[i:i + 2]
    if "--to" in args:
        i = args.index("--to"); t1 = float(args[i + 1]); del args[i:i + 2]
    for a in args:
        print(summary(Path(a), t0, t1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
