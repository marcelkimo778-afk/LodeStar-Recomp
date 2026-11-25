#!/usr/bin/env python3
"""Symbolize SW_PROFILE=1 output: python Engine/Tools/prof_report.py <game log> [N]

Takes the last "[prof]" block of the log, maps each exe RVA to the function
in build_work/out/sw_app.map, and prints functions by samples.  Generated
guest functions (sw_f_<offset>) are also shown as module+offset when the
offset is known (main base 0x100000, sdk 0x4410000, subsdk0 0x5150000,
subsdk1 0x5800000 - generated names carry the runtime address).
"""
import bisect
import re
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAP = ROOT / "build_work" / "out" / "sw_app.map"


def load_map():
    base, syms = 0x140000000, []
    for line in MAP.read_text(errors="replace").splitlines():
        m = re.match(r"\s*Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m.group(1), 16)
        m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f?\s*(\S+)?", line)
        if m:
            addr = int(m.group(2), 16)
            if addr >= base:
                syms.append((addr - base, m.group(1), m.group(3) or ""))
    syms.sort()
    return syms


def main() -> int:
    log, n = Path(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 40
    lines = log.read_text(errors="replace").splitlines()
    starts = [i for i, l in enumerate(lines) if l.startswith("[prof]") and "samples" in l]
    if not starts:
        print("no [prof] block (run with SW_PROFILE=1)")
        return 1
    head = lines[starts[-1]]
    total = int(re.search(r"\] (\d+) samples", head).group(1))
    rows = []
    for l in lines[starts[-1] + 1:]:
        m = re.match(r"\[prof\] ([0-9a-f]+) (\d+) ", l)
        if not m:
            break
        rows.append((int(m.group(1), 16), int(m.group(2))))
    syms = load_map()
    keys = [s[0] for s in syms]
    by_fn = Counter()
    for rva, cnt in rows:
        i = bisect.bisect_right(keys, rva) - 1
        name, obj = (syms[i][1], syms[i][2]) if i >= 0 else ("?", "")
        by_fn[(name, obj)] += cnt
    print(head)
    print(f"(top {len(rows)} RVAs = {sum(c for _, c in rows)} of {total} samples)")
    for (name, obj), cnt in by_fn.most_common(n):
        extra = ""
        m = re.match(r"sw_f_([0-9a-f]+)$", name)
        if m:
            a = int(m.group(1), 16)
            for mod, b in (("subsdk1", 0x5800000), ("subsdk0", 0x5150000), ("sdk", 0x4410000), ("main", 0x100000)):
                if a >= b:
                    extra = f"  = {mod}+{a - b:#x}"
                    break
        print(f"{100.0 * cnt / total:6.2f}%  {cnt:6d}  {name}  [{obj}]{extra}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
