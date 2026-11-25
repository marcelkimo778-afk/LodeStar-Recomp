import sys, bisect, pickle, os
from pathlib import Path
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "Engine" / "Source"))
from swrecomp.formats.exefs import open_exefs

BASES = {"main": 0x100000, "sdk": 0x4410000, "subsdk0": 0x5150000, "subsdk1": 0x5800000}
cache = root / "Build" / "Cache" / "guest_syms.pkl"
if cache.exists():
    table = pickle.loads(cache.read_bytes())
else:
    ex = open_exefs(root.parent / "Game" / "exefs")
    table = []
    for name, base in BASES.items():
        nso = ex.load_nso(name, root / "Build" / "Cache" / "modules")
        for s in nso.symbols():
            v = getattr(s, "value", None) or getattr(s, "address", None)
            if v:
                table.append((base + v, getattr(s, "size", 0) or 0, s.name, name))
    table.sort()
    cache.write_bytes(pickle.dumps(table))
addrs = [t[0] for t in table]
try:
    import cxxfilt
    dem = cxxfilt.demangle
except Exception:
    dem = lambda x: x
for a in sys.argv[1:]:
    x = int(a, 16)
    i = bisect.bisect_right(addrs, x) - 1
    if i >= 0:
        b, sz, n, m = table[i]
        print(f"{a}: {m} {dem(n)} +0x{x-b:x} (size 0x{sz:x})")
    else:
        print(a, "?")
