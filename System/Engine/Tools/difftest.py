#!/usr/bin/env python3
"""Differential test: our lifter+emitter vs the Unicorn ARM64 emulator.

Samples real instructions from a game module, runs each one with random
register/flag state in Unicorn (reference) and in our generated C (all
compiled once into one harness with the host toolchain), and reports every
instruction whose resulting registers, flags or memory differ.

    python Engine/Tools/difftest.py [--module main] [--count 3000] [--seed 1] [--memory]

Default mode: integer/bitfield/logical/shift/multiply/divide/csel/flag
instructions.  --memory: loads and stores (incl. pairs, pre/post-index)
against a 64 KiB buffer at 0x200000 that both sides start from identically;
registers and sp point into it, and the buffer hash is compared too.
"""
from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Engine" / "Source"))

from swrecomp.arm64.decoder import Unsupported, decode  # noqa: E402
from swrecomp.arm64.emit import emit_function  # noqa: E402
from swrecomp.arm64.lift import Lifter  # noqa: E402
from swrecomp.formats.exefs import open_exefs  # noqa: E402
import swrecomp.pipeline.build as build_mod  # noqa: E402

import unicorn  # noqa: E402
from unicorn import arm64_const as A  # noqa: E402

RET = 0xD65F03C0
CODE = 0x10000
MEM_BASE = 0x200000
MEM_SIZE = 0x10000
# X29/X30 are not contiguous with X0..X28 in Unicorn's register numbering.
UC_X = [A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]

ALU_SKIP = ("b", "cb", "tb", "ld", "st", "ret", "br", "bl", "svc", "msr", "mrs", "udf",
            "hint", "nop", "dmb", "dsb", "isb", "clrex", "prfm", "adr", "cas", "swp",
            "yield", "wf", "sev", "brk", "hlt", "dc", "ic", "sys")
MEM_PREFIXES = ("ldr", "str", "ldp", "stp", "ldur", "stur", "ldnp", "stnp", "ldxp", "ldaxp", "stxp", "stlxp")


def _has_vreg(insn) -> bool:
    return any(type(op).__name__ == "VReg" for op in insn.operands)


def interesting_alu(insn) -> bool:
    if isinstance(insn, Unsupported):
        return False
    m = insn.mnemonic
    if m.startswith(("v", "f")) or m in ("umov", "smov", "dup", "ins", "movi"):
        return False
    if any(m == p or m.startswith(p) for p in ALU_SKIP):
        return False
    if _has_vreg(insn):
        return False
    # SP operands are included: sp starts at a valid address and is compared.
    # (Excluding them hid NEG decoding as "sp - Xm".)
    return True


def interesting_mem(insn) -> bool:
    if isinstance(insn, Unsupported):
        return False
    if not insn.mnemonic.startswith(MEM_PREFIXES):
        return False
    if insn.notes.get("literal"):
        return False          # PC-relative literal loads read code memory
    return not _has_vreg(insn)


def interesting_simd(insn, memory: bool) -> bool:
    """FP / SIMD data processing (or, with memory, vector loads/stores)."""
    if isinstance(insn, Unsupported):
        return False
    m = insn.mnemonic
    is_mem = m.startswith(MEM_PREFIXES) or m.startswith(("ld1", "st1", "ld2", "st2", "ld3", "st3", "ld4", "st4"))
    if memory:
        return is_mem and _has_vreg(insn) and not insn.notes.get("literal")
    if is_mem or any(m == p or m.startswith(p) for p in ("b", "cb", "tb", "ret", "br", "bl", "svc",
                                                           "msr", "mrs", "udf", "hint", "nop")):
        return False
    return _has_vreg(insn) or m.startswith(("f", "scvtf", "ucvtf"))


SPECIAL_F32 = (0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x00000001, 0x80000001,
               0x007FFFFF, 0x00400000, 0x3F800000, 0xBF800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x00800000,
               0x3F000000, 0x4B000000)
SPECIAL_F64 = (0x0, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000,
               0x1, 0x000FFFFFFFFFFFFF, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FEFFFFFFFFFFFFF)
_SPECIAL = False
_FZ = False


def rand_vreg(rng) -> int:
    """128 bits made of finite floats (NaN propagation differs x86 vs ARM).
    --special: mostly +-0, +-inf, NaN, denormals, FLT_MAX (as 4 f32 or 2 f64 lanes)."""
    import struct
    v = 0
    if _SPECIAL and rng.random() < 0.8:
        if rng.random() < 0.5:
            for lane in range(4):
                v |= rng.choice(SPECIAL_F32) << (32 * lane)
        else:
            for lane in range(2):
                v |= rng.choice(SPECIAL_F64) << (64 * lane)
        return v
    for lane in range(4):
        f = rng.choice((rng.uniform(-1000, 1000), rng.uniform(-1, 1), float(rng.randrange(-100, 100)), 0.0))
        v |= struct.unpack("<I", struct.pack("<f", f))[0] << (32 * lane)
    return v


def _nan_only(a: int, b: int) -> bool:
    """True when a and b differ only in lanes that are NaN on both sides (NaN sign/payload:
    x86 vs Arm default NaN), viewed as f32 lanes or as f64 lanes."""
    def isnan32(x): return (x >> 23) & 0xFF == 0xFF and x & 0x7FFFFF
    def isnan64(x): return (x >> 52) & 0x7FF == 0x7FF and x & 0xFFFFFFFFFFFFF
    ok32 = all(((a >> s) & 0xFFFFFFFF) == ((b >> s) & 0xFFFFFFFF)
               or (isnan32((a >> s) & 0xFFFFFFFF) and isnan32((b >> s) & 0xFFFFFFFF)) for s in (0, 32, 64, 96))
    ok64 = all(((a >> s) & (2**64 - 1)) == ((b >> s) & (2**64 - 1))
               or (isnan64((a >> s) & (2**64 - 1)) and isnan64((b >> s) & (2**64 - 1))) for s in (0, 64))
    return ok32 or ok64


def fnv(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def unicorn_run(word, regs, nzcv, sp, mem, vregs):
    mu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    mu.mem_map(CODE, 0x1000)
    mu.mem_write(CODE, word.to_bytes(4, "little") + RET.to_bytes(4, "little"))
    mu.mem_map(MEM_BASE, MEM_SIZE)
    mu.mem_write(MEM_BASE, mem)
    for i in range(31):
        mu.reg_write(UC_X[i], regs[i])
    for i in range(32):
        mu.reg_write(A.UC_ARM64_REG_Q0 + i, vregs[i])
    mu.reg_write(A.UC_ARM64_REG_SP, sp)
    mu.reg_write(A.UC_ARM64_REG_NZCV, nzcv << 28)
    if _FZ:
        mu.reg_write(A.UC_ARM64_REG_FPCR, 1 << 24)      # FPCR.FZ, as the game runs
    mu.emu_start(CODE, CODE + 4)
    out = [mu.reg_read(UC_X[i]) for i in range(31)]
    vout = [mu.reg_read(A.UC_ARM64_REG_Q0 + i) for i in range(32)]
    return (out, (mu.reg_read(A.UC_ARM64_REG_NZCV) >> 28) & 0xF,
            mu.reg_read(A.UC_ARM64_REG_SP), fnv(bytes(mu.mem_read(MEM_BASE, MEM_SIZE))), vout)


HARNESS = r'''
#include <stdio.h>
#include <string.h>
#define SW_RT_IMPL  /* the harness defines its own load/store functions */
#include "sw_rt.h"
#include "sw_simd6.h"
SwCpu *(sw_call)(SwCpu *c, u64 t) { (void)t; return c; }
SwCpu *(sw_call_reg)(SwCpu *c, u64 t) { (void)t; return c; }
void sw_dispatch(SwCpu *c, u64 t) { (void)c; (void)t; }
SwCpu *(sw_svc)(SwCpu *c, u32 i) { (void)i; return c; }
void sw_trap(SwCpu *c, u64 a, const char *w) { (void)c; (void)a; (void)w; }
void sw_report(const char *f, ...) { (void)f; }
u64 sw_mrs(SwCpu *c, int r) { (void)c; (void)r; return 0; }
void sw_msr(SwCpu *c, int r, u64 v) { (void)c; (void)r; (void)v; }
void sw_memory_barrier(void) {}
static u8 g_mem[0x10000];
static int g_fault;
static u8 *mp(u64 a, u64 n) {
    if (a < 0x200000ULL || a + n > 0x210000ULL) { g_fault = 1; return 0; }
    return g_mem + (a - 0x200000ULL);
}
u8 *sw_mem_ptr(SwCpu *c, u64 a, u64 s, int w) { (void)c; (void)w; return mp(a, s); }
#define LD(T, NAME) T NAME(SwCpu *c, u64 a) { T v = 0; u8 *p = mp(a, sizeof v); (void)c; if (p) memcpy(&v, p, sizeof v); return v; }
LD(u8, sw_load8) LD(u16, sw_load16) LD(u32, sw_load32) LD(u64, sw_load64)
LD(s8, sw_load8s) LD(s16, sw_load16s) LD(s32, sw_load32s)
#define ST(T, NAME) void NAME(SwCpu *c, u64 a, u64 v) { T t = (T)v; u8 *p = mp(a, sizeof t); (void)c; if (p) memcpy(p, &t, sizeof t); }
ST(u8, sw_store8) ST(u16, sw_store16) ST(u32, sw_store32) ST(u64, sw_store64)
sw_v128 sw_loadv(SwCpu *c, u64 a, unsigned n) { sw_v128 v; u8 *p = mp(a, n); (void)c; memset(&v, 0, sizeof v); if (p) memcpy(&v, p, n); return v; }
void sw_storev(SwCpu *c, u64 a, sw_v128 v, unsigned n) { u8 *p = mp(a, n); (void)c; if (p) memcpy(p, &v, n); }
static unsigned long long fnv(const u8 *d, unsigned n) {
    unsigned long long h = 0xCBF29CE484222325ULL;
    for (unsigned i = 0; i < n; i++) h = (h ^ d[i]) * 0x100000001B3ULL;
    return h;
}
static void dump(int i, SwCpu *c) {
    printf("%d", i);
    for (int r = 0; r < 31; r++) printf(" %llx", (unsigned long long)c->x[r]);
    printf(" %d %llx %llx %d", (c->nf << 3) | (c->zf << 2) | (c->cf << 1) | c->vf,
           (unsigned long long)c->sp, fnv(g_mem, sizeof g_mem), g_fault);
    for (int r = 0; r < 32; r++)
        printf(" %llx:%llx", (unsigned long long)c->v[r].d[1], (unsigned long long)c->v[r].d[0]);
    printf("\n");
}
'''


def build_harness(tests, mem, workdir: Path) -> Path:
    parts = [HARNESS, "static const u8 g_init[0x10000] = {%s};" % ",".join(str(b) for b in mem)]
    calls = []
    for idx, (word, regs, nzcv, sp, vregs) in enumerate(tests):
        code = word.to_bytes(4, "little") + RET.to_bytes(4, "little")
        parts.append(emit_function(Lifter(code).lift_function(0, name="t%d" % idx, boundary=8), 0))
        vinit = ", ".join("0x%xULL, 0x%xULL" % (v & (2**64 - 1), v >> 64) for v in vregs)
        calls.append(
            "  { static const u64 r[31] = {%s}; static const u64 vv[64] = {%s};"
            " memset(&c, 0, sizeof c); memcpy(c.x, r, sizeof r); memcpy(c.v, vv, sizeof vv);"
            " c.sp = 0x%xULL; g_fault = 0; memcpy(g_mem, g_init, sizeof g_mem);"
            " c.nf = %d; c.zf = %d; c.cf = %d; c.vf = %d; t%d(&c); dump(%d, &c); }"
            % (", ".join("0x%xULL" % r for r in regs), vinit, sp,
               (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1, idx, idx))
    fz = "  _mm_setcsr(_mm_getcsr() | 0x8040u);   /* FPCR.FZ -> FTZ+DAZ (sw_fpcr_apply) */\n" if _FZ else ""
    parts.append("#include <xmmintrin.h>\nint main(void) {\n  static SwCpu c;\n" + fz + "\n".join(calls) + "\n  return 0;\n}\n")
    src = workdir / "difftest_harness.c"
    src.write_text("\n".join(parts))
    return src


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--module", default="main")
    ap.add_argument("--count", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--memory", action="store_true", help="test loads/stores instead of ALU ops")
    ap.add_argument("--simd", action="store_true", help="FP/SIMD instructions (with --memory: vector loads/stores)")
    ap.add_argument("--only", help="comma-separated mnemonics to test (e.g. adcs,fmadd)")
    ap.add_argument("--range", help="module offsets LO-HI (hex): test the instructions of that code range, each several times with random inputs (a suspect function)")
    ap.add_argument("--special", action="store_true", help="vector inputs mostly +-0, +-inf, NaN, denormals (Arm special cases)")
    ap.add_argument("--fz", action="store_true", help="run with FPCR.FZ (flush-to-zero) on both sides, as the game does")
    args = ap.parse_args()
    global _SPECIAL, _FZ
    _SPECIAL = args.special
    _FZ = args.fz
    rng = random.Random(args.seed)
    only = set(args.only.split(",")) if args.only else None
    if args.simd:
        pick = lambda insn: interesting_simd(insn, args.memory)  # noqa: E731
    else:
        pick = interesting_mem if args.memory else interesting_alu
    mem = bytes(rng.getrandbits(8) for _ in range(MEM_SIZE))

    nso = open_exefs(ROOT.parent / "Game/exefs").load_nso(args.module, ROOT / "Build/Cache/modules")
    text = nso.text
    seen: set[int] = set()
    tests = []
    attempts = 0
    while len(tests) < args.count and attempts < 50_000_000:
        attempts += 1               # cap: --only may match fewer distinct words
        if args.range:
            lo, hi = (int(x, 16) for x in args.range.split('-'))
            off = lo + rng.randrange(0, (hi - lo) // 4) * 4
        else:
            off = rng.randrange(0, len(text) // 4) * 4
        word = int.from_bytes(text[off:off + 4], "little")
        insn = decode(0, word)
        if (word in seen and not args.range) or not pick(insn) or (only and insn.mnemonic not in only):
            continue
        seen.add(word)
        if args.memory:
            # every register is a plausible pointer into the middle of the buffer,
            # 16-byte aligned so scaled offsets stay in range
            regs = [MEM_BASE + 0x6000 + 16 * rng.randrange(0, 0x100) for _ in range(31)]
        else:
            regs = [rng.choice((rng.getrandbits(64), rng.getrandbits(32),
                                rng.randrange(-64, 64) & (2**64 - 1), 1 << rng.randrange(64), 0))
                    for _ in range(31)]
        vregs = [rand_vreg(rng) for _ in range(32)]
        tests.append((word, regs, rng.getrandbits(4), MEM_BASE + 0x8000, vregs))

    expected = []
    for word, regs, nzcv, sp, vregs in tests:
        try:
            expected.append(unicorn_run(word, regs, nzcv, sp, mem, vregs))
        except unicorn.UcError:
            expected.append(None)       # faults in the reference: not comparable

    tool = build_mod.find_toolchain()
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        src = build_harness(tests, mem, work)
        exe = work / "difftest.exe"
        inc = ROOT / "Engine" / "Runtime" / "include"
        if tool.name == "msvc":
            cmd = [tool.compiler, "/nologo", "/O1", "/w", "/bigobj", "/I" + str(inc), str(src),
                   "/Fe:" + str(exe), "/Fo" + str(work) + "\\"]
        else:
            cmd = [tool.compiler, "-O1", "-w", "-I", str(inc), str(src), "-o", str(exe)]
        r = subprocess.run(cmd, env=tool.env, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            print((r.stdout + r.stderr)[-3000:])
            return 2
        out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.splitlines()

    got = {}
    for line in out:
        f = line.split()
        if len(f) == 68 and f[0].isdigit():
            vv = []
            for pair in f[36:68]:
                hi, lo = pair.split(":")
                vv.append((int(hi, 16) << 64) | int(lo, 16))
            got[int(f[0])] = ([int(v, 16) for v in f[1:32]], int(f[32]), int(f[33], 16),
                              int(f[34], 16), int(f[35]), vv)

    bad = defaultdict(list)
    ok = skipped = 0
    for idx, (word, regs, nzcv, sp, _v) in enumerate(tests):
        exp = expected[idx]
        if exp is None or idx not in got:
            skipped += 1
            continue
        er, ef, esp, emem, ev = exp
        gr, gf, gsp, gmem, fault, gv = got[idx]
        diffs = ["x%d exp %x got %x" % (i, er[i], gr[i]) for i in range(31) if er[i] != gr[i]]
        if ef != gf:
            diffs.append("nzcv exp %x got %x" % (ef, gf))
        if esp != gsp:
            diffs.append("sp exp %x got %x" % (esp, gsp))
        if emem != gmem:
            diffs.append("memory differs" + (" (fault)" if fault else ""))
        diffs += ["v%d exp %032x got %032x" % (i, ev[i], gv[i]) for i in range(32)
                  if ev[i] != gv[i] and not _nan_only(ev[i], gv[i])]
        if diffs:
            bad[decode(0, word).mnemonic].append((word, diffs))
        else:
            ok += 1
    total_bad = sum(len(v) for v in bad.values())
    mode = (" simd" if args.simd else "") + (" memory" if args.memory else "")
    print("difftest %s%s: %d instructions, %d match, %d differ, %d skipped (reference fault)"
          % (args.module, mode, len(tests), ok, total_bad, skipped))
    for m, items in sorted(bad.items(), key=lambda kv: -len(kv[1])):
        word, diffs = items[0]
        print("  %-8s %4d differ  e.g. 0x%08x: %s" % (m, len(items), word, "; ".join(diffs[:3])))
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
