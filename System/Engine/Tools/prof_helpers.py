"""All-thread view of a SW_PROFILE=all run: share of every non-game symbol (runtime, helpers, CRT) of all
samples taken in the exe, and the game-code (sw_f_*) total.

    python Engine/Tools/prof_helpers.py <SW_PROFILE_OUT file> [N]"""
import bisect
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prof_report import load_map  # noqa: E402

path = Path(sys.argv[1])
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
syms = load_map()
addrs = [s[0] for s in syms]
tot = 0
by = Counter()
for line in Path(str(path) + ".threads").read_text(errors="replace").splitlines():
    p = line.split()
    if not p or not p[0].startswith("t") or p[0] in ("thread", "ticks"):
        continue
    try:
        v, c = int(p[1], 16), int(p[2])
    except (ValueError, IndexError):
        continue
    if v >> 38:          # ntdll / call-site records
        continue
    i = bisect.bisect_right(addrs, v) - 1
    s, o = (syms[i][1], syms[i][2]) if i >= 0 else ("?", "?")
    tot += c
    by["<game code sw_f_*>" if s.startswith("sw_f_") else f"{s} [{o}]"] += c
print(f"{tot} samples in the exe (all threads)")
for k, c in by.most_common(n):
    print(f"  {100 * c / tot:5.1f}%  {k}")
