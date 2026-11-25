#!/usr/bin/env python3
"""Size statistics of the generated C (build_work/gen): biggest functions and
code emitted more than once (an entry point inside another function's body
re-emits the shared code).

    python Engine/Tools/gen_stats.py [--top N]

For each function: its entry, the guest instructions it covers (from its
L_xxxxxxxx block labels) and its C line count.  "Duplicated" = instruction
addresses covered by more than one function.
"""
import argparse
import re
from collections import Counter
from pathlib import Path

GEN = Path(__file__).resolve().parents[2] / "build_work" / "gen"
FN = re.compile(r"^void sw_f_([0-9a-f]+)\(SwCpu \*c\)")
LBL = re.compile(r"^L_([0-9a-f]+):;")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--gen", default=str(GEN), help="generated C folder (default build_work/gen)")
    a = ap.parse_args()
    fns = []                       # (entry, lines, lo, hi, nlabels)
    cover = Counter()              # block label -> how many functions emit it
    total_lines = 0
    for f in sorted(Path(a.gen).glob("sw_chunk_*.c")):
        cur = None
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                total_lines += 1
                m = FN.match(line)
                if m:
                    if cur:
                        fns.append(cur)
                    cur = [int(m.group(1), 16), 0, None, None, 0]
                    continue
                if cur is None:
                    continue
                cur[1] += 1
                m = LBL.match(line)
                if m:
                    v = int(m.group(1), 16)
                    cover[v] += 1
                    cur[4] += 1
                    cur[2] = v if cur[2] is None else min(cur[2], v)
                    cur[3] = v if cur[3] is None else max(cur[3], v)
        if cur:
            fns.append(cur)
    fn_lines = sum(x[1] for x in fns)
    dup_blocks = sum(n - 1 for n in cover.values() if n > 1)
    all_blocks = sum(cover.values())
    print(f"{len(fns)} functions, {fn_lines:,} lines of C in functions ({total_lines:,} total)")
    print(f"blocks emitted: {all_blocks:,}; distinct {len(cover):,}; "
          f"extra copies {dup_blocks:,} ({100.0 * dup_blocks / max(1, all_blocks):.1f}% of all emitted blocks)")
    # lines spent in blocks that another function also emits (approx: share of blocks per function)
    print(f"\nbiggest functions (lines, blocks, share of blocks also emitted elsewhere):")
    for e, n, lo, hi, nb in sorted(fns, key=lambda x: -x[1])[:a.top]:
        print(f"  sw_f_{e:08x}  {n:8,} lines  {nb:6,} blocks")
    big = sorted(fns, key=lambda x: -x[1])
    buckets = [(10_000, "over 10k lines"), (2_000, "2k-10k"), (0, "under 2k")]
    print()
    prev = None
    for lim, name in buckets:
        sel = [x for x in fns if x[1] >= lim and (prev is None or x[1] < prev)]
        print(f"  {name:15s}: {len(sel):7,} functions, {sum(x[1] for x in sel):12,} lines")
        prev = lim
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
