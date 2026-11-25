"""Map main-module offsets of game 1.1.1 (roms/exefs) to 1.0.0 (build_work/exefs_100) by masked code match
usage: python mapfn.py OFF111 [OFF111 ...]     (hex module offsets, as nxaevora writes main+X)
prints the 1.0.0 module offset and runtime address (0x100000 + offset)."""
import sys
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Engine" / "Source"))
from swrecomp.formats.exefs import open_exefs  # noqa: E402

CACHE = ROOT / "Build" / "Cache" / "nso_map"
CACHE.mkdir(exist_ok=True)


def text_of(exefs, tag):
    f = CACHE / f"text_{tag}.bin"
    if f.exists():
        return f.read_bytes()
    nso = open_exefs(ROOT / exefs).load_nso("main", CACHE / tag)
    s = nso.segments[0]
    t = nso.read_vaddr(s.mem_offset, s.size)
    f.write_bytes(t)
    return t


def mask(words):
    w = words.astype(np.uint32)
    m = np.full_like(w, 0xFFFFFFFF)
    op = w >> 26
    m[(op & 0x1F) == 0x05] = 0xFC000000                       # B / BL imm26
    m[(w & 0x9F000000) == 0x90000000] = 0x9F00001F             # ADRP
    m[(w & 0x9F000000) == 0x10000000] = 0x9F00001F             # ADR
    m[(w & 0xFF000010) == 0x54000000] = 0xFF00001F             # B.cond
    m[(w & 0x7E000000) == 0x34000000] = 0xFF00001F             # CBZ/CBNZ
    m[(w & 0x7E000000) == 0x36000000] = 0xFFF8001F             # TBZ/TBNZ
    m[(w & 0x3B000000) == 0x18000000] = 0xFF00001F             # LDR literal
    # ADD imm / LDR/STR unsigned imm after ADRP carry lo12: mask the imm12 for ADD (x) imm and LDR/STR uimm
    m[(w & 0xFF800000) == 0x91000000] = 0xFFC003FF & 0xFF8003FF
    return w & m


def main():
    a = np.frombuffer(text_of("../Game/exefs", "111"), dtype="<u4")
    b = np.frombuffer(text_of("build_work/exefs_100", "100"), dtype="<u4")
    ma, mb = mask(a), mask(b)
    N = 24
    for arg in sys.argv[1:]:
        off = int(arg, 16)
        i = off // 4
        key = ma[i:i + N]
        # candidates: positions where the first word matches, then check the rest
        cand = np.nonzero(mb == key[0])[0]
        hits = [j for j in cand if j + N <= len(mb) and np.array_equal(mb[j:j + N], key)]
        if len(hits) != 1:
            # longer key to disambiguate
            for n2 in (48, 96, 192):
                key2 = ma[i:i + n2]
                hits = [j for j in hits if np.array_equal(mb[j:j + n2], key2)] if hits else \
                       [j for j in cand if j + n2 <= len(mb) and np.array_equal(mb[j:j + n2], key2)]
                if len(hits) <= 1:
                    break
        # extent of the match
        desc = []
        for j in hits[:4]:
            k = 0
            while i + k < len(ma) and j + k < len(mb) and ma[i + k] == mb[j + k]:
                k += 1
            desc.append(f"1.0.0 main+{j*4:X} (addr 0x{0x100000 + j*4:x}), {k} words equal")
        print(f"1.1.1 main+{off:X}: {len(hits)} hit(s): " + ("; ".join(desc) if desc else "none"))


if __name__ == "__main__":
    main()
