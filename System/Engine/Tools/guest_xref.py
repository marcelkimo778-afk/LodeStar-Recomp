"""Find loads/stores to a global: adrp Xn, PAGE followed (within 16 insns) by
ldr/str [Xn, #off] (64/32-bit unsigned-offset forms) or add Xn, Xn, #off."""
import sys, struct
from pathlib import Path
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "Engine" / "Source"))
from swrecomp.formats.exefs import open_exefs

BASES = {"main": 0x100000, "sdk": 0x4410000, "subsdk0": 0x5150000, "subsdk1": 0x5800000}
mod = sys.argv[1]
target = int(sys.argv[2], 16)
base = BASES[mod]
ex = open_exefs(root.parent / "Game" / "exefs")
nso = ex.load_nso(mod, root / "Build" / "Cache" / "modules")
text = nso.text
words = struct.unpack("<%dI" % (len(text) // 4), text[: len(text) // 4 * 4])
page = target & ~0xFFF
lo = target & 0xFFF
for i, w in enumerate(words):
    if (w & 0x9F000000) != 0x90000000:
        continue
    pc = base + 4 * i
    imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    if (pc & ~0xFFF) + (imm << 12) != page:
        continue
    rd = w & 31
    for j in range(1, 80):
        if i + j >= len(words):
            break
        v = words[i + j]
        rn = (v >> 5) & 31
        if rn != rd:
            continue
        kind = None
        if (v & 0xFFC00000) == 0xF9000000: kind, off = "str64", ((v >> 10) & 0xFFF) * 8
        elif (v & 0xFFC00000) == 0xF9400000: kind, off = "ldr64", ((v >> 10) & 0xFFF) * 8
        elif (v & 0xFFC00000) == 0xB9000000: kind, off = "str32", ((v >> 10) & 0xFFF) * 4
        elif (v & 0xFFC00000) == 0xB9400000: kind, off = "ldr32", ((v >> 10) & 0xFFF) * 4
        elif (v & 0xFFC00000) == 0x91000000: kind, off = "add", (v >> 10) & 0xFFF
        elif (v & 0xFFC00000) in (0xA9000000, 0xA9400000):
            imm7 = (v >> 15) & 0x7F
            if imm7 & 0x40: imm7 -= 0x80
            kind = "stp" if (v & 0xFFC00000) == 0xA9000000 else "ldp"
            off = imm7 * 8
            if off + 8 == lo: off = lo; kind += "(2nd)"
        if kind and kind[:3] in ("add","str","stp") and lo-0x100 <= off <= lo:
            print(f"{pc + 4*j:#x} {kind} {off:#x}")
            break
