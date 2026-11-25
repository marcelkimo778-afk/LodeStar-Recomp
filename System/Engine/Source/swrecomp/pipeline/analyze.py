"""Static analysis: discover functions, lift them and measure coverage."""

from __future__ import annotations

import bisect
import struct
from collections import Counter, deque
from dataclasses import dataclass, field
from pathlib import Path

from ..arm64.decoder import decode, Unsupported
from ..arm64.ir import Function
from ..arm64.lift import Lifter
from ..formats.nso import Nso

# Symbol names (prefixes / substrings / exact) whose functions never return.
# A `bl` to one of these ends the sweep: fall-through after the call is dead.
_NOReturn_PREFIXES = (
    "abort", "svcAbort", "svcExit", "__cxa_throw", "__assert_fail",
    "__stack_chk_fail", "__builtin_trap",
)
_NOReturn_SUBSTRINGS = ("AbortImpl", "_Exit")
_NOReturn_EXACT = {"exit", "_exit", "quick_exit"}


def _is_noreturn_name(name: str) -> bool:
    if name in _NOReturn_EXACT:
        return True
    for p in _NOReturn_PREFIXES:
        if name.startswith(p):
            return True
    for s in _NOReturn_SUBSTRINGS:
        if s in name:
            return True
    return False


@dataclass
class AnalysisResult:
    module: str
    functions: dict[int, Function] = field(default_factory=dict)
    seeds: dict[int, str] = field(default_factory=dict)
    unsupported: Counter = field(default_factory=Counter)
    mnemonic_counts: Counter = field(default_factory=Counter)
    instruction_total: int = 0
    instruction_supported: int = 0
    scan_targets: int = 0
    errors: list[str] = field(default_factory=list)
    lifter: Lifter | None = None   # re-lifts functions whose IR was dropped

    def coverage(self) -> dict:
        hooks = {}
        for fn in self.functions.values():
            for _addr, word, reason in fn.unsupported:
                key = "0x%08x: %s" % (word, reason)
                hooks[key] = hooks.get(key, 0) + 1
        return {
            "module": self.module,
            "functions": len(self.functions),
            "instructions": self.instruction_total,
            "supported": self.instruction_supported,
            "supported_pct": round(100.0 * self.instruction_supported / max(1, self.instruction_total), 3),
            "unsupported_instructions": dict(self.unsupported),
            "top_mnemonics": self.mnemonic_counts.most_common(25),
            "unsupported_sites": sorted(hooks.items(), key=lambda kv: -kv[1])[:50],
            "scan_targets": self.scan_targets,
            "errors": self.errors,
        }


def _scan_branch_targets(nso: Nso) -> set[int]:
    """Collect every BL target in .text with a linear sweep.

    This is a discovery heuristic (documented in docs/recompiler-design.md): it
    finds static functions that no direct caller seeds, and never *executes*
    anything.  Targets outside .text are ignored.
    """
    text = nso.text
    text_size = len(text)
    targets: set[int] = set()
    for off in range(0, text_size - 4, 4):
        word = int.from_bytes(text[off:off + 4], "little")
        # BL only.  Most `b` targets are jumps *inside* a function (loops,
        # if/else); seeding them made every such block a separate overlapping
        # function.  Real tail-call targets are found while lifting.
        if (word & 0xFC000000) != 0x94000000:
            continue
        imm = word & 0x03FFFFFF
        if imm & 0x02000000:
            imm -= 0x04000000
        target = off + (imm << 2)
        if 0 <= target < text_size and target % 4 == 0 and target != off:
            targets.add(target)
    return targets


def _scan_code_address_refs(nso: Nso) -> set[int]:
    """Code addresses formed with `adrp xR, page ; add xR, xR, #lo12`.

    Callbacks passed as arguments (e.g. to a once-init or thread helper) are
    materialised this way and called through a register later; they appear
    neither as bl targets nor in relocations.  Any such address inside .text
    is a function entry.
    """
    text = nso.text
    n = len(text)
    targets: set[int] = set()
    for off in range(0, n - 4, 4):
        w = int.from_bytes(text[off:off + 4], "little")
        if (w & 0x9F000000) != 0x90000000:            # adrp
            continue
        rd = w & 31
        imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        page = (off & ~0xFFF) + (imm << 12)
        if not (0 <= page < n):
            continue
        for k in range(1, 5):                          # matching add within 4 insns
            o = off + 4 * k
            if o > n - 4:
                break
            v = int.from_bytes(text[o:o + 4], "little")
            if (v & 0xFFC00000) == 0x91000000 and (v & 31) == rd and ((v >> 5) & 31) == rd:
                t = page + ((v >> 10) & 0xFFF)
                if 0 <= t < n and t % 4 == 0:
                    targets.add(t)
                break
            if (v & 31) == rd and (v & 0x9F000000) != 0x90000000:
                break                                  # rd overwritten by something else
    return targets


def _jt_cond_after(word, at: int) -> int | None:
    """b.hi -> 1 (N+1 entries), b.hs -> 0 (N entries) within 6 insns of a cmp."""
    for j in range(1, 7):
        v = word(at + 4 * j)
        if (v & 0xFF000010) == 0x54000000:
            cond = v & 0xF
            return {8: 1, 2: 0}.get(cond)
        if (v & 0x1F000000) in (0x11000000, 0x0B000000) and (v >> 29) & 1:
            return None                              # another flag setter first (adds/subs)
        if (v & 0x1F000000) == 0x0A000000 and (v >> 29) & 3 == 3:
            return None                              # ands/bics (orr = mov, opc 01, sets no flags)
        if (v & 0x7C000000) == 0x14000000 or v == 0xD65F03C0:
            return None
    return None


def _jt_branch_bound(word, first: int) -> int | None:
    """Bound from a compare-and-branch that jumps *to* the table code:

        cmp  xI, #N
        b.ls table_block        (N+1 entries)   /  b.lo table_block (N entries)
        ...
    table_block:                (starts after an unconditional b / ret / br)
        adrp/adr/ldrb ... br
    """
    start = first
    for _ in range(16):                      # walk back to the block start
        v = word(start - 4)
        if (v & 0xFC000000) == 0x14000000 or v == 0xD65F03C0 or (v & 0xFFFFFC1F) == 0xD61F0000:
            break
        start -= 4
    else:
        return None
    best = None
    for o in range(start - 4, max(0, start - 0x8000), -4):
        v = word(o)
        if (v & 0xFF000010) != 0x54000000:   # b.cond
            continue
        imm = (v >> 5) & 0x7FFFF
        if imm & 0x40000:
            imm -= 0x80000
        if o + imm * 4 != start:
            continue
        extra = {9: 1, 3: 0}.get(v & 0xF)    # ls -> N+1, lo/cc -> N
        if extra is None:
            continue
        for j in range(1, 7):                # the cmp right before the branch
            c = word(o - 4 * j)
            if (c & 0x7FC0001F) == 0x7100001F:
                n = ((c >> 10) & 0xFFF) + extra
                if n > 0 and (best is None or n > best):
                    best = n
                break
    return best


def _reg_max(word, at: int, reg: int, depth: int = 0, limit: int = 96) -> int | None:
    """Largest value reg can hold just before `at`, from the nearest earlier write.

    Understands the bit-field writes clang uses to build switch indexes
    without a cmp (main+0x2f67724: `ubfiz w23, w9, #1, #1` ... `and w12, w19,
    #1; orr w12, w12, w23; ldrb [table, x12]` -> 4 entries): and #imm,
    ubfx/ubfiz/lsr/lsl (UBFM), orr/mov of registers.  None when unknown.
    """
    if depth > 4:
        return None
    for k in range(1, limit):
        a = at - 4 * k
        v = word(a)
        if v == 0xD65F03C0:
            return None
        if (v & 31) != reg:
            continue
        if (v & 0xFF000010) == 0x54000000 or (v & 0x7E000000) in (0x34000000, 0x36000000) \
                or (v & 0x7C000000) == 0x14000000:
            continue                                 # branches
        if (v & 0x0A000000) == 0x08000000 and not (v >> 22) & 1:
            continue                                 # store: Rt is read
        sf = (v >> 31) & 1
        size = 64 if sf else 32
        if (v & 0x7F800000) == 0x12000000:          # and #imm
            from ..arm64.decoder import decode_bit_masks
            try:
                mask, _ = decode_bit_masks((v >> 22) & 1, (v >> 10) & 0x3F, (v >> 16) & 0x3F, size)
            except ValueError:
                return None
            return mask
        if (v & 0x7F800000) == 0x53000000:          # UBFM: ubfx / ubfiz / lsr / lsl
            immr, imms = (v >> 16) & 0x3F, (v >> 10) & 0x3F
            if imms >= immr:
                return (1 << (imms - immr + 1)) - 1
            return ((1 << (imms + 1)) - 1) << (size - immr)
        if (v & 0x7F20FC00) == 0x2A000000:          # orr Rd, Rn, Rm (no shift)
            rn, rm = (v >> 5) & 31, (v >> 16) & 31
            b = _reg_max(word, a, rm, depth + 1)
            if rn == 31:
                return b                             # mov
            if b is None:
                return None
            c = _reg_max(word, a, rn, depth + 1)
            if c is None:
                return None
            m = b | c
            return (1 << m.bit_length()) - 1         # covers every OR of the two
        return None
    return None


def _jt_bound(word, ld_off: int, index_reg: int) -> int | None:
    reg, sub = index_reg, 0
    for k in range(1, 64):
        at = ld_off - 4 * k
        v = word(at)
        if v == 0xD65F03C0:
            return None
        # cmp wReg, #N  (subs wzr, wReg, #imm12, no shift; 32 or 64 bit)
        if (v & 0x7FC0001F) == 0x7100001F and ((v >> 5) & 31) == reg:
            extra = _jt_cond_after(word, at)
            if extra is None:
                return None
            count = ((v >> 10) & 0xFFF) + extra - sub
            return count if count > 0 else None
        if (v & 31) != reg:
            continue
        if (v & 0x7F800000) == 0x12000000:          # and wReg/xReg, xJ, #imm (logical immediate)
            from ..arm64.decoder import decode_bit_masks
            sf = (v >> 31) & 1
            try:
                mask, _ = decode_bit_masks((v >> 22) & 1, (v >> 10) & 0x3F, (v >> 16) & 0x3F,
                                           64 if sf else 32)
            except ValueError:
                return None
            if mask and (mask & (mask + 1)) == 0 and mask < 4096:   # low-bit mask 2^k - 1
                count = mask + 1 - sub
                return count if count > 0 else None
            return None
        if (v & 0x7FE0FFE0) == 0x2A0003E0:          # mov wReg, wJ (orr wReg, wzr, wJ)
            reg = (v >> 16) & 31
            continue
        if (v & 0x7FC00000) == 0x51000000:          # sub wReg, wJ, #k
            sub += (v >> 10) & 0xFFF
            reg = (v >> 5) & 31
            continue
        if (v & 0xFF000010) == 0x54000000 or (v & 0x7E000000) in (0x34000000, 0x36000000) \
                or (v & 0x7C000000) == 0x14000000:
            continue                                 # branches only read / encode imm
        if (v & 0x0A000000) == 0x08000000 and not (v >> 22) & 1:
            continue                                 # store: Rt is read
        # no cmp: a bit-field index (orr of masked bits) still has a range
        m = _reg_max(word, at + 4, reg)
        if m is not None and m < 4096:
            count = m + 1 - sub
            return count if count > 0 else None
        return None                                  # index written by something else
    return None


def _jt_far_base(word, first: int, rt: int, limit: int = 8192) -> int | None:
    """Table base held in a register set up long before the switch.

    Loops over a byte-coded stream keep the table address in a callee-saved
    register for the whole function (main+0xf7aac8: `adrp x21` + `add x21`
    in the prologue, `ldrb w25, [x21, x8]` hundreds of instructions later).
    Walk back to the nearest `add xT, xT, #lo` preceded by `adrp xT`, and
    give up if anything else writes xT in between.  The case targets read
    from the table are checked by the caller.
    """
    lo = None
    for k in range(1, limit):
        at = first - 4 * k
        if at < 0:
            return None
        v = word(at)
        if (v & 31) != rt:
            continue
        if lo is None and (v & 0xFFC00000) == 0x91000000 and ((v >> 5) & 31) == rt:
            lo = (v >> 10) & 0xFFF                  # add xT, xT, #lo
            continue
        if lo is not None and (v & 0x9F000000) == 0x90000000:   # adrp xT
            page = (((v >> 5) & 0x7FFFF) << 2) | ((v >> 29) & 3)
            if page & (1 << 20):
                page -= 1 << 21
            return (at & ~0xFFF) + (page << 12) + lo
        if lo is None and (v & 0xFFE0FFE0) == 0xAA0003E0:       # mov xT, xS: follow the copy
            rt = (v >> 16) & 31                     # main+0x2f3d8a8: mov x13, x28 ... ldrb [x13, x8]
            continue
        # Rd/Rt field matches: harmless only for stores, branches and
        # flag-only compares (Rd = 31 is never rt here)
        if (v & 0x0A000000) == 0x08000000 and not (v >> 22) & 1:
            continue                                # store: Rt is read
        if (v & 0x7C000000) in (0x14000000, 0x54000000) or (v & 0x7E000000) in (0x34000000, 0x36000000):
            continue                                # b / b.cond / cbz / tbz
        # SIMD/FP registers are a separate file: `ldr q4` / `fadd v4` do not
        # touch x4 (main+0x2f336b8: adrp x4 ... ldr q4, fmul v4 ... ldrh [x4])
        to_gpr = ((v & 0x9F000000) == 0x1E000000                         # scalar FP incl. fmov/fcvt to Wd/Xd
                  or ((v & 0xBFE08400) == 0x0E000400 and (v >> 11) & 0xF in (5, 7)))   # smov / umov
        if (v >> 25) & 7 == 7 and not to_gpr:
            continue                                # SIMD/FP data processing that writes a V register
        if (v & 0x0A000000) == 0x08000000 and (v >> 26) & 1 and (v & 0x3B000000) != 0x18000000:
            continue                                # vector load: Rt is a V register
        return None                                 # xT written by something else
    return None


def _scan_jump_tables(nso: Nso) -> set[int]:
    """All case targets (see _scan_jump_table_map)."""
    out: set[int] = set()
    for ts in _scan_jump_table_map(nso).values():
        out |= ts
    return out


def _scan_jump_table_map(nso: Nso) -> dict[int, set[int]]:
    """Case targets of compiler switch tables (each becomes a function entry).

    `br` to a case block needs an entry the runtime can dispatch to; C cannot
    jump into the middle of another function.  Clang's compressed tables:

        cmp   wI, #N ; b.hi/b.hs default      (bound, within ~24 insns)
        adrp  xT, page ; add xT, xT, #lo      (table address)
        adr   xA, anchor
        ldrb  wE, [xT, xI]  |  ldrh wE, [xT, xI, lsl #1]
        add   xA, xA, xE, lsl #2
        br    xA                              target = anchor + entry * 4
    """
    text = nso.text
    n = len(text)

    def word(off: int) -> int:
        return int.from_bytes(text[off:off + 4], "little") if 0 <= off <= n - 4 else 0

    tables: dict[int, set[int]] = {}
    for off in range(0, n - 4, 4):
        w = word(off)
        if (w & 0xFFFFFC1F) != 0xD61F0000:          # br xA
            continue
        targets = tables.setdefault(off, set())
        ra = (w >> 5) & 31
        # The pieces may be interleaved with unrelated instructions, so each
        # is searched for in a short window instead of at a fixed position.
        def find(lo_off: int, hi_off: int, pred):
            for o in range(hi_off, lo_off - 4, -4):
                if pred(word(o)):
                    return o
            return None

        # clang may schedule a long computation between the target add and
        # the br (main+0x2879878: add, 25 FP insns, br x11), so look back
        # up to 64 insns; the nearest match wins
        add_off = find(off - 256, off - 4, lambda v: (v & 0xFFE0FC00) == 0x8B000800
                       and (v & 31) == ra and ((v >> 5) & 31) == ra)
        if add_off is None:
            continue
        re_ = (word(add_off) >> 16) & 31
        ld_off = find(add_off - 24, add_off - 4, lambda v: (v & 31) == re_ and (
            (v & 0xFFE0FC00) == 0x38606800 or (v & 0xFFE0FC00) == 0x78607800))
        adr_off = find(add_off - 24, add_off - 4,
                       lambda v: (v & 0x9F000000) == 0x10000000 and (v & 31) == ra)
        if ld_off is None or adr_off is None:
            continue
        ld = word(ld_off)
        esize = 1 if (ld & 0xFFE0FC00) == 0x38606800 else 2
        rt = (ld >> 5) & 31
        adr = word(adr_off)
        imm = (((adr >> 5) & 0x7FFFF) << 2) | ((adr >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        anchor = adr_off + imm
        # table base: adrp xT + add xT, xT, #lo, and the cmp/b.hi bound, in the
        # preceding window
        base = None
        lo = None
        count = None
        first = min(ld_off, adr_off)
        # A `ret` in between does not end this search: clang often places an
        # early-return path between the table set-up (before a loop) and the
        # dispatch (main+0xcb422c: adrp/add x9, then ret, then br) - stopping
        # there lost the table and its case blocks were never recompiled.
        for k in range(1, 64):
            at = first - 4 * k
            v = word(at)
            if lo is None and (v & 0xFFC00000) == 0x91000000 and (v & 31) == rt                     and ((v >> 5) & 31) == rt:
                lo = (v >> 10) & 0xFFF
            elif lo is not None and base is None and (v & 0x9F000000) == 0x90000000                     and (v & 31) == rt:
                page = ((((v >> 5) & 0x7FFFF) << 2) | ((v >> 29) & 3))
                if page & (1 << 20):
                    page -= 1 << 21
                base = (at & ~0xFFF) + (page << 12) + lo
        if base is None:
            base = _jt_far_base(word, first, rt)
        # bound, index-aware first: follow the load's index register back
        # through `mov wI, wJ` / `sub wI, wJ, #k` to `cmp wJ, #N` whose
        # b.hi/b.hs may come a few instructions later (clang schedules loads
        # and stores in between; QueryV_ in nlibsdk's CentralHeap does this).
        aware = _jt_bound(word, ld_off, (ld >> 16) & 31)
        # old rule too (nearest cmp #N directly followed by b.hi / b.hs); the
        # larger bound wins so no previously found case target is lost
        count = None
        for k in range(1, 48):
            at = first - 4 * k
            v = word(at)
            if v == 0xD65F03C0:
                break
            if (v & 0x7F80001F) == 0x7100001F and not (v >> 22) & 1:
                nxt = word(at + 4)
                if (nxt & 0xFF00001F) == 0x54000008:
                    count = ((v >> 10) & 0xFFF) + 1
                elif (nxt & 0xFF00001F) == 0x54000002:
                    count = (v >> 10) & 0xFFF
                if count is not None:
                    break
        if aware is not None and (count is None or aware > count):
            count = aware
        branched = _jt_branch_bound(word, first)
        if branched is not None and (count is None or branched > count):
            count = branched
        if base is not None and not count:
            # No bound at all: the index is a state value the code trusts (main+0x1ba3830: `ldr w8, [x19,
            # #0x464]; sub w8, w8, #1`, no cmp).  Take entries while their targets stay in a plausible range
            # after the anchor (case blocks follow the dispatch).  An entry past the real end only adds a
            # function boundary (fall-through between entries is handled), never wrong code.
            limit = 256
            table = nso.read_vaddr(base, limit * esize) or b""
            for i in range(len(table) // esize):
                e = int.from_bytes(table[i * esize:(i + 1) * esize], "little")
                t = anchor + e * 4
                if not (anchor <= t < anchor + 0x8000) or word(t) == 0:
                    break
                targets.add(t)
            continue
        if base is None or not count or count > 4096:
            continue
        table = nso.read_vaddr(base, count * esize)
        if not table or len(table) < count * esize:
            continue
        for i in range(count):
            entry = int.from_bytes(table[i * esize:(i + 1) * esize], "little")
            t = anchor + entry * 4
            if 0 <= t < n and word(t) != 0:        # zero = padding past the table's real end
                targets.add(t)
    return {k: v for k, v in tables.items() if v}


# ---- parallel pre-lift ---------------------------------------------------------
# With a boundary (cfg_extent) a lift depends only on (address, boundary, the
# module's text, noreturn set and jump tables): allow_cold_blocks is then only
# recorded on the Function.  So every known entry can be lifted ahead of time in
# worker processes; the serial worklist below is unchanged and takes the cached
# result instead of lifting (same set, same order, same output).
_PL_LIFTER = None


def _prelift_init(text, noreturn, jump_tables, max_bytes) -> None:
    global _PL_LIFTER
    _PL_LIFTER = Lifter(text, noreturn=noreturn, max_function_bytes=max_bytes)
    _PL_LIFTER.jump_tables = jump_tables


def _unsupported_traps(fn) -> int:
    n = 0
    for blk in fn.blocks:
        for op in blk.ops:
            if op.kind == "trap" and "unsupported instruction" in (op.text or ""):
                n += 1
    return n


def _prelift_batch(batch):
    out = []
    for addr, boundary in batch:
        try:
            fn = _PL_LIFTER.lift_function(addr, allow_cold_blocks=True, boundary=boundary)
        except Exception:
            continue                      # the serial path lifts it again and reports the error
        traps = _unsupported_traps(fn)
        fn.blocks = []
        out.append((addr, boundary, fn, traps))
    return out


def _prelift_all(lifter, text, addrs, boundary_of, jobs: int) -> dict:
    from concurrent.futures import ProcessPoolExecutor
    todo = [(a, boundary_of(a)) for a in addrs if a + 4 <= len(text) and a % 4 == 0]
    batches = [todo[i:i + 256] for i in range(0, len(todo), 256)]
    cache = {}
    with ProcessPoolExecutor(max_workers=jobs, initializer=_prelift_init,
                             initargs=(text, set(lifter.noreturn), dict(lifter.jump_tables),
                                       lifter.max_function_bytes)) as pool:
        for res in pool.map(_prelift_batch, batches):
            for addr, boundary, fn, traps in res:
                cache[addr] = (boundary, fn, traps)
    return cache


def analyze_module(nso: Nso, module_name: str, max_functions: int = 4000,
                   scan_all: bool = True, extra_seeds: list[int] | None = None,
                   keep_ir: bool = True, mnemonics: bool = True) -> AnalysisResult:
    """Discover and lift every function reachable from the module's seeds.

    keep_ir=False drops each function's lifted blocks right after analysis and
    keeps only its metadata (address, size, calls, counts).  A whole-game run
    has ~1M functions / 60M instructions; holding every IR op at once needs
    25+ GB.  Emission re-lifts each function from ``result.lifter`` instead.
    """
    result = AnalysisResult(module=module_name)
    text = nso.text
    # Noreturn addresses from dynsym: bl to these ends the sweep.
    noreturn: set[int] = set()
    for sym in nso.symbols():
        if sym.kind == 2 and not sym.is_import and _is_noreturn_name(sym.name):
            noreturn.add(sym.value)
    # Functions are bounded by the next known entry (cfg_extent), so the byte
    # cap is only a safety net against runaway data; the largest real function
    # seen in this game is ~61 KB.
    lifter = Lifter(text, noreturn=noreturn, max_function_bytes=0x40000)
    result.lifter = lifter

    seeds: list[tuple[int, str, bool]] = []
    try:
        dynamic = nso.parse_dynamic()
    except Exception as exc:  # malformed dynamic must not abort analysis
        result.errors.append("dynamic section: %s" % exc)
        dynamic = None
    if dynamic and dynamic.init is not None and 0 <= dynamic.init < len(text):
        seeds.append((dynamic.init, "dynamic.init", True))
    if dynamic:
        # Entries are filled in by RELATIVE relocations; use the resolved array.
        for idx, entry in enumerate(dynamic.resolved_init_array()[:8192]):
            if entry and 0 <= entry < len(text) and entry % 4 == 0:
                seeds.append((entry, "init_array[%d]" % idx, True))
    for sym in nso.exports:
        if sym.kind in (0, 2) and 0 <= sym.value < len(text) and sym.value % 4 == 0:
            # Plain exports are speculative: they are only guaranteed when a
            # relocation elsewhere actually references them (those arrive as
            # explicit seeds).  Constructors below are never speculative.
            seeds.append((sym.value, "export:%s" % sym.name, False))
    explicit_seeds = [(a, "explicit-seed", True) for a in (extra_seeds or [])]
    if not seeds and not explicit_seeds:
        seeds.append((0, "module-start", True))

    # Two-phase worklist: the startup closure (init array + exports + every
    # callee reachable from them) is completed before scan-only candidates are
    # considered.  A single FIFO queue let thousands of scan seeds starve the
    # callees of the init functions, which then trapped at startup.
    # Explicit seeds (runtime feedback and exports other modules relocate
    # against) come first: a function cap must never drop a symbol that is
    # already linked into another module's image.
    # A "required" entry is part of the startup closure and therefore bypasses
    # the speculative function cap; dropping it would leave a hole that the
    # first constructor immediately dispatches into.
    queue = deque(explicit_seeds + seeds)
    scanned: deque[tuple[int, str, bool]] = deque()
    pointer_seeds: deque[tuple[int, str, bool]] = deque()
    jt_all: set[int] = set()
    if scan_all:
        # Functions referenced only by address (vtables, callback tables) never
        # appear as BL targets.  Every R_AARCH64_RELATIVE addend that lands in
        # .text is an executable pointer and therefore a real function entry.
        text_size = len(text)
        for r_offset, r_type, addend, _sym in dynamic.relocations if dynamic else ():
            if r_type != 1027:
                continue
            if 0 <= addend < text_size and addend % 4 == 0:
                pointer_seeds.append((addend, "pointer@0x%x" % r_offset, False))
        # Switch tables: the function that owns the `br` lifts its cases as
        # its own blocks (C switch, lift.py jump_tables).  They are no longer
        # entries up front - as boundaries they cut the owner short and every
        # case re-emitted the shared code after it (23.6% of all blocks were
        # duplicates).  Cases no owner takes in still get entries afterwards.
        jt_map = _scan_jump_table_map(nso)
        lifter.jump_tables = {k: tuple(sorted(v)) for k, v in jt_map.items()}
        jt_all = set().union(*jt_map.values()) if jt_map else set()
        # Callbacks whose address is built in code (adrp + add).
        for target in sorted(_scan_code_address_refs(nso)):
            pointer_seeds.append((target, "code-address", False))
        for target in sorted(_scan_branch_targets(nso)):
            if len(scanned) < max_functions * 4:
                scanned.append((target, "bl-scan", False))
            else:
                break
        result.scan_targets = len(seeds) + len(pointer_seeds) + len(scanned)

    # Every address known to start a function before lifting begins.  Each
    # function extends from its start to the next of these (cfg_extent).  The
    # set is fixed up front so extents do not depend on discovery order and
    # can be re-lifted identically at emit time.
    entries = sorted({a for a, _w, _r in list(queue) + list(pointer_seeds) + list(scanned)
                      if 0 <= a < len(text) and a % 4 == 0})

    def boundary_of(addr: int) -> int:
        i = bisect.bisect_right(entries, addr)
        return entries[i] if i < len(entries) else len(text)

    # SW_RECOMP_JOBS (default: CPU count; 1 = off): pre-lift every entry in parallel when the IR is dropped
    # anyway (keep_ir=False) and the run is large enough to pay for the worker start-up.
    import os
    jobs = int(os.environ.get("SW_RECOMP_JOBS", "0") or 0) or (os.cpu_count() or 1)
    prelifted: dict = {}
    if not keep_ir and jobs > 1 and len(entries) > 2000:
        prelifted = _prelift_all(lifter, text, entries, boundary_of, jobs)

    seen: set[int] = set()
    jt_pending = bool(jt_all)
    while queue or pointer_seeds or scanned or jt_pending:
        if not (queue or pointer_seeds or scanned):
            jt_pending = False
            handled: set[int] = set()
            for f in result.functions.values():
                handled |= f.jt_local
            for t in sorted(jt_all - handled - set(result.functions)):
                pointer_seeds.append((t, "jumptable", False))
            continue
        if queue:
            addr, why, required = queue.popleft()
        elif pointer_seeds:
            addr, why, required = pointer_seeds.popleft()
        else:
            addr, why, required = scanned.popleft()
        if len(result.functions) >= max_functions and not required:
            # The cap bounds speculative discovery only.  Required entries
            # (init_array, exports, explicit seeds and their callees) are the
            # startup closure; dropping one would leave a dispatch hole.
            continue
        if addr in seen:
            continue
        seen.add(addr)
        if addr + 4 > len(text) or addr % 4:
            continue
        first = decode(addr, int.from_bytes(text[addr:addr + 4], "little"))
        if isinstance(first, Unsupported):
            result.errors.append("seed 0x%x (%s) is not a supported instruction: %s"
                                 % (addr, why, first.reason))
            continue
        # Only trusted entries (calls, exports, init, pointer tables) may pull
        # in cold blocks: a speculative `bl`-scan seed can point into data,
        # where branch-shaped words would otherwise grow into huge fake
        # functions and megabytes of generated C.
        bnd = boundary_of(addr)
        hit = prelifted.pop(addr, None)
        if hit is not None and hit[0] == bnd:
            _b, fn, traps = hit
            fn.allow_cold_blocks = why != "bl-scan"
        else:
            fn = lifter.lift_function(addr, allow_cold_blocks=(why != "bl-scan"), boundary=bnd)
            traps = _unsupported_traps(fn)
        if fn.instruction_count == 0:
            continue
        result.instruction_total += traps
        result.instruction_total += fn.instruction_count + len(fn.unsupported)
        result.instruction_supported += fn.instruction_count
        if not keep_ir:
            fn.blocks = []
        result.functions[addr] = fn
        result.seeds[addr] = why
        # Lift callees immediately (depth first): a function cap must always
        # cover the startup closure, and sdk exports alone would otherwise fill
        # the whole budget before a callee of the running code is reached.
        for target in sorted(fn.calls | fn.tail_calls, reverse=True):
            if target not in seen:
                queue.appendleft((target, "call from 0x%x" % addr, required))
        for _a, word, reason in fn.unsupported:
            result.unsupported[reason] += 1

    # Mnemonic histogram over every discovered function (decodes every instruction once more; only the
    # analyze report shows it, recompile passes mnemonics=False).
    for fn in (result.functions.values() if mnemonics else ()):
        for insn_addr in range(fn.addr, fn.addr + fn.size, 4):
            word = int.from_bytes(text[insn_addr:insn_addr + 4], "little")
            insn = decode(insn_addr, word)
            if not isinstance(insn, Unsupported):
                result.mnemonic_counts[insn.mnemonic] += 1
    return result
