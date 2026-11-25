import sys
from pathlib import Path
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "Engine" / "Source"))
from swrecomp.formats.exefs import open_exefs
from swrecomp.arm64.lift import Lifter
from swrecomp.arm64.emit import Emitter

mod, addr = sys.argv[1], int(sys.argv[2], 16)
boundary = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
ex = open_exefs(root.parent / "Game" / "exefs")
nso = ex.load_nso(mod, root / "Build" / "Cache" / "modules")
lf = Lifter(nso.text, noreturn=set(), max_function_bytes=0x40000)
fn = lf.lift_function(addr, boundary=boundary)
e = Emitter(fn, 0)
for i, b in enumerate(fn.blocks):
    print(i, b.label, [op.kind for op in e._block_ops(i)], "succ", e._block_succs(i))
    for op in e._block_ops(i):
        if op.kind in ("call", "call_indirect"):
            print("    live after call @%#x:" % op.addr, sorted(e._live_after.get(id(op), set())))
