#!/usr/bin/env python3
"""Report of a SW_PROFILE=all run (sw_runtime.c profile_all_thread).

    python Engine/Tools/prof_threads.py <SW_PROFILE_OUT file> [N] [--hot OUT.txt [--cover 0.9]]

Reads <file>.threads: per thread the CPU share (samples / ticks of 2 ms) and its
top N exe functions.  --hot writes the generated chunks that hold --cover of all
generated-code samples (one "sw_chunk_NNNN" per line) for build.py SW_HOT_CHUNKS.
"""
from __future__ import annotations

import bisect
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prof_report import load_map  # noqa: E402


def main() -> int:
    args = sys.argv[1:]
    hot_out, cover = None, 0.9
    if "--hot" in args:
        i = args.index("--hot"); hot_out = args[i + 1]; del args[i:i + 2]
    if "--cover" in args:
        i = args.index("--cover"); cover = float(args[i + 1]); del args[i:i + 2]
    path = Path(args[0])
    n = int(args[1]) if len(args) > 1 else 8
    syms = load_map()
    addrs = [s[0] for s in syms]

    def sym(rva: int) -> tuple[str, str]:
        i = bisect.bisect_right(addrs, rva) - 1
        return (syms[i][1], syms[i][2]) if i >= 0 else ("?", "?")

    threads, ticks = {}, 1
    per = defaultdict(Counter)
    calls, ntd = defaultdict(Counter), defaultdict(Counter)
    ntexp = []
    for line in Path(str(path) + ".threads").read_text(errors="replace").splitlines():
        p = line.split()
        if p[0] == "thread":
            threads[int(p[1]) + 1] = (" ".join(p[2:-3]), float(p[-3]), int(p[-2]), int(p[-1]))
        elif p[0] == "ntexp":
            ntexp.append((int(p[1], 16), p[2]))
        elif p[0] == "ticks":
            ticks = int(p[1])
        elif p[0].startswith("t"):
            k, v, c = int(p[0][1:]), int(p[1], 16), int(p[2])
            if v >> 39:
                calls[k][v & ((1 << 39) - 1)] += c
            elif v >> 38:
                ntd[k][v & ((1 << 38) - 1)] += c
            else:
                per[k][v] += c
    ntexp.sort()
    nta = [a for a, _ in ntexp]

    def ntname(off: int) -> str:
        i = bisect.bisect_right(nta, off) - 1
        return ntexp[i][1] if i >= 0 else "?"

    by_chunk = Counter()
    order = sorted(threads.items(), key=lambda kv: -kv[1][1])
    for k, (name, core, cnt, inexe) in order:
        if core < 1.0 or not cnt:          # under 1% of a core
            continue
        fn, obj = Counter(), Counter()
        for rva, c in per[k].items():
            s, o = sym(rva)
            fn[(s, o)] += c
            obj[o] += c
            if o.startswith("sw_chunk"):
                by_chunk[o.split(".")[0]] += c
        gen = sum(c for o, c in obj.items() if o.startswith("sw_chunk"))
        print(f"\n== {name}: {core:.1f}% of a core, in exe {100 * inexe / max(cnt, 1):.0f}%, "
              f"generated code {100 * gen / max(cnt, 1):.0f}%")
        for o, c in Counter({o: c for o, c in obj.items() if not o.startswith("sw_chunk")}).most_common(5):
            print(f"   {100 * c / cnt:5.1f}%  [{o}]")
        for (s, o), c in fn.most_common(n):
            print(f"   {100 * c / cnt:5.1f}%  {s}  [{o}]")
        if ntd[k]:
            nn = Counter()
            for off, c in ntd[k].items():
                nn[ntname(off)] += c
            print("   -- in ntdll: " + ", ".join(f"{x} {100 * c / cnt:.1f}%" for x, c in nn.most_common(6)))
        if calls[k]:
            cc = Counter()
            for rva, c in calls[k].items():
                cc[sym(rva)[0]] += c
            print("   -- outside the exe, called from: " + ", ".join(f"{x} {100 * c / cnt:.1f}%" for x, c in cc.most_common(8)))
    if hot_out:
        tot = sum(by_chunk.values())
        acc, out = 0, []
        for ch, c in by_chunk.most_common():
            if acc >= cover * tot:
                break
            out.append(ch)
            acc += c
        Path(hot_out).write_text("\n".join(out) + "\n", encoding="utf-8")
        print(f"\nhot chunks: {len(out)} of {len(by_chunk)} with samples hold {100 * acc / max(tot, 1):.1f}% "
              f"of generated-code time -> {hot_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
