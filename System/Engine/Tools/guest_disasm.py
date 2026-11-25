import sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "Engine" / "Source"))
from swrecomp.formats.exefs import open_exefs
import capstone

BASES = {"main": 0x100000, "sdk": 0x4410000, "subsdk0": 0x5150000, "subsdk1": 0x5800000}
mod = sys.argv[1]
addr = int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
ex = open_exefs(root.parent / "Game" / "exefs")
nso = ex.load_nso(mod, root / "Build" / "Cache" / "modules")
off = addr - BASES[mod]
img = nso.read_vaddr(off, 4 * n); off = 0
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
for i in md.disasm(bytes(img[off:off + 4 * n]), addr):
    print(f"{i.address:#x}: {i.mnemonic} {i.op_str}")
