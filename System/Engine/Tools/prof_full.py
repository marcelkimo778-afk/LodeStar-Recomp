#!/usr/bin/env python3
"""Report of a full SW_PROFILE_OUT histogram (sw_runtime.c profile_thread).

    python Engine/Tools/prof_full.py <prof_full.txt> [N]

Prints: time per DLL outside the exe; time per exe function; time per object
file (generated chunk / runtime file); and, with SW_PROFILE_STACK=1, which exe
functions the outside samples (waits, driver calls) came from.
"""
from __future__ import annotations

import bisect
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prof_report import load_map  # noqa: E402


def main() -> int:
    path = Path(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    total = outside = 0
    mods: list[tuple[int, str]] = []
    inside: list[tuple[int, int]] = []
    callers: list[tuple[int, int]] = []
    for line in path.read_text(errors="replace").splitlines():
        m = re.match(r"# total (\d+) outside (\d+)", line)
        if m:
            total, outside = int(m.group(1)), int(m.group(2))
            continue
        m = re.match(r"module (\d+) (.+)", line)
        if m:
            mods.append((int(m.group(1)), m.group(2)))
            continue
        m = re.match(r"([0-9a-f]+) (\d+)$", line)
        if m:
            k, c = int(m.group(1), 16), int(m.group(2))
            (callers if k >> 62 & 1 else inside).append((k & ((1 << 62) - 1), c))
    if not total:
        print("empty profile")
        return 1
    syms = load_map()
    keys = [s[0] for s in syms]

    def sym(rva: int) -> tuple[str, str]:
        i = bisect.bisect_right(keys, rva) - 1
        return (syms[i][1], syms[i][2]) if i >= 0 else ("?", "")

    def pct(c: int) -> str:
        return f"{100.0 * c / total:6.2f}%"
    ins = sum(c for _, c in inside)
    print(f"{total} samples: in the exe {pct(ins)}, outside {pct(outside)}")
    print("\n-- outside the exe, by DLL")
    for c, name in sorted(mods, reverse=True)[:12]:
        print(f"{pct(c)}  {name}")
    by_fn, by_obj = Counter(), Counter()
    for rva, c in inside:
        name, obj = sym(rva)
        by_fn[(name, obj)] += c
        by_obj["generated code" if obj.startswith("sw_chunk") else obj] += c
    print("\n-- in the exe, by object")
    for obj, c in by_obj.most_common(12):
        print(f"{pct(c)}  {obj}")
    print(f"\n-- in the exe, top {n} functions")
    for (name, obj), c in by_fn.most_common(n):
        print(f"{pct(c)}  {name}  [{obj}]")
    if callers:
        by_caller = Counter()
        for rva, c in callers:
            by_caller[sym(rva)] += c
        print(f"\n-- outside samples by the exe function that called out (SW_PROFILE_STACK=1), top {n}")
        for (name, obj), c in by_caller.most_common(n):
            print(f"{pct(c)}  {name}  [{obj}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
