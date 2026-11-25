"""AArch64 instruction decoder.

Scope is the subset of A64 that appears in Nintendo Switch user-mode game code
and that the lifter can translate with correct semantics.  Anything else decodes
to ``None`` with a reason, never to a silent no-op.

Encodings follow the Arm Architecture Reference Manual (DDI 0487) field layouts.
Every case here is exercised by tests/test_arm64_decode.py against hand-assembled
encodings that carry their expected mnemonic and operands.
"""

from __future__ import annotations

from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# Operand model
# --------------------------------------------------------------------------

COND_NAMES = ("eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
              "hi", "ls", "ge", "lt", "gt", "le", "al", "nv")


@dataclass(frozen=True)
class Reg:
    """A general-purpose register operand."""

    num: int
    width: int          # 32 or 64
    sp: bool = False    # num == 31 means SP (True) or ZR (False)

    def __str__(self) -> str:
        if self.num == 31:
            if self.sp:
                return "sp" if self.width == 64 else "wsp"
            return "xzr" if self.width == 64 else "wzr"
        return ("x%d" if self.width == 64 else "w%d") % self.num


@dataclass(frozen=True)
class VReg:
    """A SIMD/FP register operand.

    ``width`` is the operand width in bits (128 for Q, 64 for D, 32 for S, 16 H,
    8 B).  ``arr`` is (lane_bits, lanes) for whole-vector arrangements such as
    ``4s`` or ``16b``.
    """

    num: int
    width: int = 128
    arr: tuple[int, int] | None = None
    lane: int | None = None

    def __str__(self) -> str:
        base = "v%d" % self.num
        if self.lane is not None and self.arr is not None:
            lane_bits, _ = self.arr
            suffix = {8: "b", 16: "h", 32: "s", 64: "d"}[lane_bits]
            return "%s.%s[%d]" % (base, suffix, self.lane)
        if self.arr is not None:
            lane_bits, lanes = self.arr
            suffix = {8: "b", 16: "h", 32: "s", 64: "d"}[lane_bits]
            return "%s.%d%s" % (base, lanes, suffix)
        return {128: "q", 64: "d", 32: "s", 16: "h", 8: "b"}[self.width] + str(self.num)


@dataclass
class Insn:
    addr: int
    word: int
    mnemonic: str
    operands: list
    sets_flags: bool = False
    is_branch: bool = False
    is_terminator: bool = False
    notes: dict = field(default_factory=dict)

    def __str__(self) -> str:
        ops = ", ".join(str(o) for o in self.operands)
        return "%-8s %s" % (self.mnemonic, ops)


class Unsupported:
    """Marker returned for encodings outside the supported subset."""

    __slots__ = ("word", "reason", "addr")

    def __init__(self, word: int, reason: str, addr: int = 0) -> None:
        self.word = word
        self.reason = reason
        self.addr = addr

    def __repr__(self) -> str:
        return "<unsupported 0x%08x: %s>" % (self.word, self.reason)


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

def sign_extend(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def decode_bit_masks(n: int, imms: int, immr: int, width: int) -> tuple[int, int]:
    """Decode a logical-immediate (N:immr:imms) into (wmask, tmask)."""
    combined = (n << 6) | (~imms & 0x3F)
    length = combined.bit_length() - 1
    if length < 1:
        raise ValueError("invalid logical immediate")
    size = 1 << length
    if size > width:
        raise ValueError("logical immediate wider than register")
    levels = size - 1
    s = imms & levels
    r = immr & levels
    # Note: s == levels is legal for bitfield instructions (it yields an all-ones
    # mask); only the logical-immediate encoding forbids it, and such an encodings
    # never occurs in compiler output.  DecodeBitMasks itself has no such rule.
    esize = size
    welem = (1 << (s + 1)) - 1
    if r:
        welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
    wmask = 0
    for i in range(width // esize):
        wmask |= welem << (i * esize)
    # ARM DecodeBitMasks: tmask = Replicate(Ones(d + 1)), d = (S - R) mod esize.
    # (It used S + 1, which broke every SBFM/UBFM/BFM alias that depends on
    # tmask: sxtw/asr/sbfx/ubfx/lsr/bfxil ...)
    d = (s - r) & levels
    telem = (1 << (d + 1)) - 1
    tmask = 0
    for i in range(width // esize):
        tmask |= telem << (i * esize)
    return wmask, tmask


def _reg(n: int, width: int, sp: bool = False) -> Reg:
    return Reg(n, width, sp)


def _vreg(n: int, width: int = 128, arr=None, lane=None) -> VReg:
    return VReg(n, width, arr, lane)


# Load/store size/opc table.  (size, opc) -> (mnemonic, access_bytes, is_float, is_signed)
def _ldst_kind(size: int, opc: int, vector: bool):
    """Classify a load/store by size/opc fields.

    Returns (mnemonic, bytes, signed, is_vector, is_simd_scalar).
    """
    if vector:
        # SIMD&FP: size 00..11 selects B/H/S/D; size==00 with opc 10/11 is the
        # Q (128-bit) form.  opc even = store, odd = load.
        if size == 0 and opc in (2, 3):
            bytes_ = 16
        else:
            bytes_ = {0: 1, 1: 2, 2: 4, 3: 8}.get(size)
        if bytes_ is None:
            return None
        if opc in (0, 2):
            return ("str", bytes_, False, True, bytes_ in (1, 2, 4, 8))
        if opc in (1, 3):
            return ("ldr", bytes_, False, True, bytes_ in (1, 2, 4, 8))
        return None
    # Integer: size 0..3 for 8/16/32/64-bit.  opc 0 = store, 1 = zero-ext load,
    # 2 = sign-ext load to 64 bits (LDRSB/LDRSH/LDRSW Xt), 3 = sign-ext load to
    # 32 bits (LDRSB/LDRSH Wt).  size 3 + opc 2 is PRFM, a prefetch hint: it
    # used to decode as an 8-byte load that clobbered register Rt.
    bytes_ = 1 << size
    if opc == 0:
        return ("str", bytes_, False, False, False)
    if opc == 1:
        return ("ldr", bytes_, False, False, False)
    if opc == 2:
        if size == 3:
            return ("prfm", 8, False, False, False)
        return ("ldrs", bytes_, True, False, False)
    if opc == 3:
        if size >= 2:
            return None  # unallocated (no 32/64-bit sign-extending load to Wt)
        return ("ldrs", bytes_, True, False, False)
    return None


# --------------------------------------------------------------------------
# Decoder
# --------------------------------------------------------------------------

# 0xe7ffdefe is a permanently undefined encoding that Nintendo's toolchain
# emits as a trap: between functions as padding and as the target of failed
# asserts (e.g. `cbnz w0, <trap>`).  Executing it faults on hardware, so it
# must lift to a trap that ends the block, never to a NOP that falls through
# into the next function.
TRAP_WORD = 0xE7FFDEFE


def decode(addr: int, word: int) -> Insn | Unsupported:
    w = word
    if w == 0 or w == TRAP_WORD:
        # UDF #0 / TRAP_WORD: permanently undefined.  Decoded (not
        # skipped) so the lifter can emit an explicit trap at the exact site.
        return Insn(addr, w, "udf", [], is_terminator=True)
    try:
        result = _decode_inner(addr, w)
    except ValueError as exc:  # malformed immediate encodings
        return Unsupported(w, str(exc), addr)
    if result is None:
        return Unsupported(w, "no matching encoding in supported subset", addr)
    return result


def _decode_inner(addr: int, w: int):
    # CRC32 sits in data-processing (2 source), whose decoder rejects it.
    if (w & 0x7FE0E000) == 0x1AC04000:
        return _decode_simd_batch6(addr, w)
    # --- PC-relative addressing ---------------------------------------
    if (w & 0x1F000000) == 0x10000000:
        op = (w >> 31) & 1
        immlo = (w >> 29) & 3
        immhi = (w >> 5) & 0x7FFFF
        imm = sign_extend((immhi << 2) | immlo, 21)
        rd = w & 0x1F
        if op == 0:
            return Insn(addr, w, "adr", [_reg(rd, 64), imm])
        return Insn(addr, w, "adrp", [_reg(rd, 64), imm << 12])

    # --- Add/subtract (immediate) -------------------------------------
    if (w & 0x1F000000) == 0x11000000:
        sf = (w >> 31) & 1
        op = (w >> 30) & 1
        s = (w >> 29) & 1
        shift = (w >> 22) & 1
        imm12 = (w >> 10) & 0xFFF
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        imm = imm12 << (12 if shift else 0)
        mnem = ("sub" if op else "add") + ("s" if s else "")
        # With S set, register 31 is XZR (flags only), never SP.
        return Insn(addr, w, mnem, [_reg(rd, width, not s), _reg(rn, width, True), imm],
                    sets_flags=bool(s))

    # --- Logical (immediate) ------------------------------------------
    if (w & 0x1F800000) == 0x12000000:
        sf = (w >> 31) & 1
        opc = (w >> 29) & 3
        n = (w >> 22) & 1
        immr = (w >> 16) & 0x3F
        imms = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        if sf == 0 and n == 1:
            return None
        wmask, _tmask = decode_bit_masks(n, imms, immr, width)
        mnem = ("and", "orr", "eor", "ands")[opc]
        return Insn(addr, w, mnem, [_reg(rd, width, opc != 3), _reg(rn, width), wmask],
                    sets_flags=(opc == 3))

    # --- Move wide (immediate) ----------------------------------------
    if (w & 0x1F800000) == 0x12800000:
        sf = (w >> 31) & 1
        opc = (w >> 29) & 3
        hw = (w >> 21) & 3
        imm16 = (w >> 5) & 0xFFFF
        rd = w & 0x1F
        width = 64 if sf else 32
        if opc == 1:
            return None  # unallocated
        if not sf and hw > 1:
            return None
        shift = hw * 16
        mnem = ("movn", None, "movz", "movk")[opc]
        return Insn(addr, w, mnem, [_reg(rd, width), imm16, shift])

    # --- Bitfield ------------------------------------------------------
    if (w & 0x1F800000) == 0x13000000:
        sf = (w >> 31) & 1
        opc = (w >> 29) & 3
        n = (w >> 22) & 1
        immr = (w >> 16) & 0x3F
        imms = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        if opc == 3 or n != sf:
            return None
        mnem = ("sbfm", "bfm", "ubfm")[opc]
        return Insn(addr, w, mnem, [_reg(rd, width), _reg(rn, width), immr, imms])

    # --- Extract -------------------------------------------------------
    if (w & 0x1F800000) == 0x13800000:
        sf = (w >> 31) & 1
        op21 = (w >> 29) & 3
        n = (w >> 22) & 1
        o0 = (w >> 21) & 1
        rm = (w >> 16) & 0x1F
        imms = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        if op21 != 0 or n != sf or o0 != 0:
            return None
        width = 64 if sf else 32
        if (not sf and imms > 31) or (sf and imms > 63):
            return None
        return Insn(addr, w, "extr", [_reg(rd, width), _reg(rn, width), _reg(rm, width), imms])

    # --- Conditional branch (immediate) --------------------------------
    if (w & 0xFF000010) == 0x54000000:
        cond = w & 0xF
        imm19 = sign_extend((w >> 5) & 0x7FFFF, 19) << 2
        return Insn(addr, w, "b.%s" % COND_NAMES[cond], [addr + imm19],
                    is_branch=True, notes={"cond": cond})

    # --- Compare and branch --------------------------------------------
    if (w & 0x7E000000) == 0x34000000:
        sf = (w >> 31) & 1
        op = (w >> 24) & 1
        imm19 = sign_extend((w >> 5) & 0x7FFFF, 19) << 2
        rt = w & 0x1F
        width = 64 if sf else 32
        mnem = "cbnz" if op else "cbz"
        return Insn(addr, w, mnem, [_reg(rt, width), addr + imm19], is_branch=True)

    # --- Test and branch ----------------------------------------------
    if (w & 0x7E000000) == 0x36000000:
        b5 = (w >> 31) & 1
        op = (w >> 24) & 1
        b40 = (w >> 19) & 0x1F
        imm14 = sign_extend((w >> 5) & 0x3FFF, 14) << 2
        rt = w & 0x1F
        bit = (b5 << 5) | b40
        mnem = "tbnz" if op else "tbz"
        return Insn(addr, w, mnem, [_reg(rt, 64), bit, addr + imm14], is_branch=True)

    # --- Unconditional branch (immediate) ------------------------------
    if (w & 0x7C000000) == 0x14000000:
        op = (w >> 31) & 1
        imm26 = sign_extend(w & 0x3FFFFFF, 26) << 2
        mnem = "bl" if op else "b"
        return Insn(addr, w, mnem, [addr + imm26], is_branch=True, is_terminator=not op)

    # --- Unconditional branch (register) -------------------------------
    # BR/BLR/RET differ only in the opc field (bits 21-24): 0, 1, 2.  The
    # masked constants below already include the opc value, so no additional
    # opc test is needed (an earlier version tested opc==0 for all three and
    # therefore rejected every BLR and RET).
    if (w & 0xFFFFFC1F) in (0xD61F0000, 0xD63F0000, 0xD65F0000):
        rn = (w >> 5) & 0x1F
        if (w & 0xFFFFFC1F) == 0xD61F0000:
            return Insn(addr, w, "br", [_reg(rn, 64)], is_branch=True, is_terminator=True)
        if (w & 0xFFFFFC1F) == 0xD63F0000:
            return Insn(addr, w, "blr", [_reg(rn, 64)], is_branch=True, is_terminator=True)
        return Insn(addr, w, "ret", [_reg(rn, 64)], is_branch=True, is_terminator=True)

    # --- System instructions -------------------------------------------
    if (w & 0xFFC00000) == 0xD5000000:
        # MRS: L=1 op0=1 1 ; MSR: L=0
        l = (w >> 21) & 1
        o0 = (w >> 19) & 3
        op1 = (w >> 16) & 7
        crn = (w >> 12) & 0xF
        crm = (w >> 8) & 0xF
        op2 = (w >> 5) & 7
        rt = w & 0x1F
        op0 = ((w >> 19) & 3)
        sysreg = (o0, op1, crn, crm, op2)
        if l:
            return Insn(addr, w, "mrs", [_reg(rt, 64), sysreg], notes={"sysreg": sysreg})
        return Insn(addr, w, "msr", [sysreg, _reg(rt, 64)], notes={"sysreg": sysreg})

    if (w & 0xFFE0001F) == 0xD4000001:
        imm16 = (w >> 5) & 0xFFFF
        return Insn(addr, w, "svc", [imm16], is_branch=True,
                    notes={"svc": True})

    if (w & 0xFFFFF01F) == 0xD503201F:
        # HINT space: NOP, YIELD, WFE, WFI, SEV, SEVL, ESB, ...
        crn = (w >> 12) & 0xF
        op2 = (w >> 5) & 7
        hint = (crn << 3) | op2
        names = {0: "nop", 1: "yield", 2: "wfe", 3: "wfi", 4: "sev", 5: "sevl",
                 16: "esb", 20: "csdb", 8: "hint"}
        return Insn(addr, w, names.get(hint, "hint"), [])

    if (w & 0xFFFFF09F) == 0xD503301F:
        op2 = (w >> 5) & 7
        ops = {2: "clrex", 4: "dsb", 5: "dmb", 6: "isb"}
        mnem = ops.get(op2)
        if mnem:
            return Insn(addr, w, mnem, [(w >> 8) & 0xF])

    # --- Data processing (register): add/sub shifted --------------------
    if (w & 0x1F200000) == 0x0B000000:
        sf = (w >> 31) & 1
        op = (w >> 30) & 1
        s = (w >> 29) & 1
        shift_type = (w >> 22) & 3
        rm = (w >> 16) & 0x1F
        imm6 = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        if shift_type == 3 or (not sf and imm6 > 31):
            return None
        mnem = ("sub" if op else "add") + ("s" if s else "")
        shift = ("lsl", "lsr", "asr")[shift_type]
        # Shifted-register form: register 31 is XZR for Rd, Rn and Rm (never
        # SP).  Rn was SP for 64-bit, so NEG Xd, Xm (SUB Xd, XZR, Xm) computed
        # sp - Xm (VammManager region alignment masks became garbage).
        return Insn(addr, w, mnem, [_reg(rd, width), _reg(rn, width),
                                    _reg(rm, width), shift, imm6], sets_flags=bool(s),
                    notes={"shift": shift, "shift_amount": imm6, "extended": False})

    # --- Data processing (register): add/sub extended -------------------
    if (w & 0x1F200000) == 0x0B200000:
        sf = (w >> 31) & 1
        op = (w >> 30) & 1
        s = (w >> 29) & 1
        rm = (w >> 16) & 0x1F
        option = (w >> 13) & 7
        imm3 = (w >> 10) & 7
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        if imm3 > 4:
            return None
        mnem = ("sub" if op else "add") + ("s" if s else "")
        return Insn(addr, w, mnem, [_reg(rd, width, not s), _reg(rn, width, True),
                                    _reg(rm, 64 if option in (3, 7) else 32), "ext", option, imm3],
                    sets_flags=bool(s),
                    notes={"extended": True, "option": option, "shift_amount": imm3})

    # --- Crypto AES: 0100111000101000 0 opc 10 Rn Rd (opc 4..7) -------
    if (w & 0xFFFFCC00) == 0x4E284800 or (w & 0xFFFFCC00) == 0x4E284C00 or \
            (w & 0xFFFFEC00) == 0x4E284800:
        opc = (w >> 12) & 0x1F
        name = {4: "aese", 5: "aesd", 6: "aesmc", 7: "aesimc"}.get(opc)
        if name:
            return Insn(addr, w, name, [_vreg(w & 0x1F, 128), _vreg((w >> 5) & 0x1F, 128)])

    # --- LDn/STn (single structure, n = 1..4) and LDnR ----------------------
    #   0 Q 0 01101 0 L R 00000 opc S size Rn Rt        (no offset)
    #   0 Q 0 01101 1 L R Rm    opc S size Rn Rt        (post-index)
    #   n = ((opc & 1) << 1 | R) + 1; registers Vt .. Vt+n-1 (mod 32)
    if (w & 0xBF9F0000) == 0x0D000000 or (w & 0xBF800000) == 0x0D800000:
        q = (w >> 30) & 1
        load = (w >> 22) & 1
        post = (w >> 23) & 1
        rbit = (w >> 21) & 1
        opc = (w >> 13) & 7
        sbit = (w >> 12) & 1
        size = (w >> 10) & 3
        rm = (w >> 16) & 0x1F
        rn, rt = (w >> 5) & 0x1F, w & 0x1F
        nreg = (((opc & 1) << 1) | rbit) + 1
        kind = opc & 6
        if kind == 6:                      # LDnR
            if not load or sbit:
                return None
            ebits = 8 << size
            name = "ld1r" if nreg == 1 else "ldnr"
            return Insn(addr, w, name, [_vreg(rt, 128), _reg(rn, 64, True)],
                        notes={"lane_bits": ebits, "q": q, "post": post, "rm": rm, "n": nreg})
        if kind == 0:
            ebits, idx = 8, (q << 3) | (sbit << 2) | size
        elif kind == 2:
            if size & 1:
                return None
            ebits, idx = 16, (q << 2) | (sbit << 1) | (size >> 1)
        else:                              # kind == 4
            if size == 0:
                ebits, idx = 32, (q << 1) | sbit
            elif size == 1 and not sbit:
                ebits, idx = 64, q
            else:
                return None
        if nreg == 1:
            name = "ld1_lane" if load else "st1_lane"
        else:
            name = "ldn_lane" if load else "stn_lane"
        return Insn(addr, w, name, [_vreg(rt, 128, None, idx), _reg(rn, 64, True)],
                    notes={"lane_bits": ebits, "index": idx, "post": post, "rm": rm, "n": nreg})

    # --- Crypto SHA: three-register 01011110 000 Rm 0 opc 00 Rn Rd ------
    if (w & 0xFFE08C00) == 0x5E000000:
        opc = (w >> 12) & 7
        names = ("sha1c", "sha1p", "sha1m", "sha1su0", "sha256h", "sha256h2", "sha256su1")
        if opc < 7:
            return Insn(addr, w, names[opc], [_vreg(w & 0x1F, 128), _vreg((w >> 5) & 0x1F, 128),
                                             _vreg((w >> 16) & 0x1F, 128)])
    # --- Crypto SHA: two-register 0101111000101000 0 opc 10 Rn Rd -------
    if (w & 0xFFFFCC00) == 0x5E280800:
        opc = (w >> 12) & 3
        if opc < 3:
            return Insn(addr, w, ("sha1h", "sha1su1", "sha256su0")[opc],
                        [_vreg(w & 0x1F, 128), _vreg((w >> 5) & 0x1F, 128)])

    # --- Load/store exclusive pair: 1 sz 001000 0 L 1 Rs o0 Rt2 Rn Rt ---
    if (w & 0xBFA00000) == 0x88200000:
        sz = (w >> 30) & 1
        load = (w >> 22) & 1
        o0 = (w >> 15) & 1
        width = 64 if sz else 32
        rt, rt2, rn, rs = w & 0x1F, (w >> 10) & 0x1F, (w >> 5) & 0x1F, (w >> 16) & 0x1F
        if load:
            return Insn(addr, w, "ldaxp" if o0 else "ldxp",
                        [_reg(rt, width), _reg(rt2, width), _reg(rn, 64, True)],
                        notes={"bytes": width // 8})
        return Insn(addr, w, "stlxp" if o0 else "stxp",
                    [_reg(rs, 32), _reg(rt, width), _reg(rt2, width), _reg(rn, 64, True)],
                    notes={"bytes": width // 8})

    # --- Add/subtract with carry: sf op S 11010000 Rm 000000 Rn Rd ------
    if (w & 0x1FE0FC00) == 0x1A000000:
        sf = (w >> 31) & 1
        op = (w >> 30) & 1
        s_bit = (w >> 29) & 1
        width = 64 if sf else 32
        mnem = ("sbc" if op else "adc") + ("s" if s_bit else "")
        return Insn(addr, w, mnem, [_reg(w & 0x1F, width), _reg((w >> 5) & 0x1F, width),
                                    _reg((w >> 16) & 0x1F, width)], sets_flags=bool(s_bit))

    # --- FP data-processing (3 source): FMADD/FMSUB/FNMADD/FNMSUB ---------
    if (w & 0xFF000000) == 0x1F000000:
        ftype = (w >> 22) & 3
        if ftype not in (0, 1):
            return None
        width = 32 if ftype == 0 else 64
        o1 = (w >> 21) & 1
        o0 = (w >> 15) & 1
        mnem = (("fmadd", "fmsub"), ("fnmadd", "fnmsub"))[o1][o0]
        return Insn(addr, w, mnem, [_vreg(w & 0x1F, width), _vreg((w >> 5) & 0x1F, width),
                                    _vreg((w >> 16) & 0x1F, width), _vreg((w >> 10) & 0x1F, width)])

    # --- Logical (shifted register) ------------------------------------
    if (w & 0x1F000000) == 0x0A000000:
        sf = (w >> 31) & 1
        opc = (w >> 29) & 3
        shift_type = (w >> 22) & 3
        n = (w >> 21) & 1
        rm = (w >> 16) & 0x1F
        imm6 = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        if not sf and imm6 > 31:
            return None
        base = ("and", "orr", "eor", "ands")[opc]
        if n:
            base = {"and": "bic", "orr": "orn", "eor": "eon", "ands": "bics"}[base]
        shift = ("lsl", "lsr", "asr", "ror")[shift_type]   # ROR used to be rejected
        return Insn(addr, w, base, [_reg(rd, width, opc != 3), _reg(rn, width, False),
                                    _reg(rm, width), shift, imm6],
                    sets_flags=(opc == 3), notes={"n": n})

    # --- Conditional compare (register / immediate) --------------------
    if (w & 0x7FE00400) in (0x3A400000, 0x7A400000, 0xBA400000, 0xFA400000):
        sf = (w >> 31) & 1
        op = (w >> 30) & 1                 # 1 = ccmp, 0 = ccmn
        immediate = (w >> 11) & 1
        imm5 = (w >> 16) & 0x1F
        rm = (w >> 16) & 0x1F
        cond = (w >> 12) & 0xF
        rn = (w >> 5) & 0x1F
        nzcv = w & 0xF
        width = 64 if sf else 32
        operand = imm5 if immediate else _reg(rm, width)
        return Insn(addr, w, "ccmp" if op else "ccmn",
                    [_reg(rn, width), operand, COND_NAMES[cond], nzcv], sets_flags=True,
                    notes={"cond": cond, "nzcv": nzcv, "immediate": bool(immediate),
                           "op": "ccmp" if op else "ccmn", "width": width})

    # --- Conditional select --------------------------------------------
    if (w & 0x1FE00000) == 0x1A800000:
        sf = (w >> 31) & 1
        op = (w >> 30) & 1
        s = (w >> 29) & 1
        rm = (w >> 16) & 0x1F
        cond = (w >> 12) & 0xF
        op2 = (w >> 10) & 3
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        if s != 0:
            return None
        # op/op2 select the variant; the aliases CSET/CINV/CSETM/CNEG are the
        # same encodings with Rn == Rm == XZR and are lifted from these operands.
        mnem = {(0, 0): "csel", (0, 1): "csinc", (1, 0): "csinv", (1, 1): "csneg"}[(op, op2)]
        return Insn(addr, w, mnem, [_reg(rd, width), _reg(rn, width), _reg(rm, width), COND_NAMES[cond]],
                    notes={"cond": cond})

    # --- Data processing (2 source) ------------------------------------
    # bit 30 must be clear: the 1-source class below shares the masked bits.
    if (w & 0x5FE00000) == 0x1AC00000:
        sf = (w >> 31) & 1
        s = (w >> 29) & 1
        rm = (w >> 16) & 0x1F
        opcode = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        if s:
            return None
        two = {0b000010: "udiv", 0b000011: "sdiv",
               0b001000: "lsl", 0b001001: "lsr", 0b001010: "asr", 0b001011: "ror"}
        if opcode in two:
            return Insn(addr, w, two[opcode], [_reg(rd, width), _reg(rn, width), _reg(rm, width)])
        if opcode in (0b010000, 0b010001):  # CRC32/CRC32C, not supported
            return None
        return None

    # --- Data processing (1 source) ------------------------------------
    if (w & 0x5FE00000) == 0x5AC00000:
        sf = (w >> 31) & 1
        opcode = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        width = 64 if sf else 32
        # opcode 2 is REV for 32-bit and REV32 for 64-bit; opcode 3 is REV for
        # 64-bit only (there is no 32-bit REV32).
        if opcode == 0x00:
            mnem = "rbit"
        elif opcode == 0x01:
            mnem = "rev16"
        elif opcode == 0x02:
            mnem = "rev" if width == 32 else "rev32"
        elif opcode == 0x03:
            if width == 32:
                return None
            mnem = "rev"
        elif opcode == 0x04:
            mnem = "clz"
        elif opcode == 0x05:
            mnem = "cls"
        else:
            return None
        return Insn(addr, w, mnem, [_reg(rd, width), _reg(rn, width)])

    # --- Data processing (3 source) ------------------------------------
    if (w & 0x1F000000) == 0x1B000000:
        sf = (w >> 31) & 1
        op54 = (w >> 29) & 3
        op31 = (w >> 21) & 7
        o0 = (w >> 15) & 1
        rm = (w >> 16) & 0x1F
        ra = (w >> 10) & 0x1F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        if op54 != 0:
            return None
        width = 64 if sf else 32
        if op31 == 0:
            if o0 == 0:
                return Insn(addr, w, "madd", [_reg(rd, width), _reg(rn, width), _reg(rm, width), _reg(ra, width)])
            return Insn(addr, w, "msub", [_reg(rd, width), _reg(rn, width), _reg(rm, width), _reg(ra, width)])
        if op31 in (1, 5) and sf == 1:
            mnem = "smaddl" if (op31 == 1 and o0 == 0) else "smsubl" if op31 == 1 else "umaddl" if o0 == 0 else "umsubl"
            return Insn(addr, w, mnem, [_reg(rd, 64), _reg(rn, 32), _reg(rm, 32), _reg(ra, 64)])
        # op31 010 = SMULH, 110 = UMULH (UMULH used to decode as SMULH)
        if op31 == 2 and o0 == 0 and sf == 1:
            return Insn(addr, w, "smulh", [_reg(rd, 64), _reg(rn, 64), _reg(rm, 64)])
        if op31 == 6 and o0 == 0 and sf == 1:
            return Insn(addr, w, "umulh", [_reg(rd, 64), _reg(rn, 64), _reg(rm, 64)])
        return None

    # --- Load/store: unsigned offset ------------------------------------
    if (w & 0x3B000000) == 0x39000000:
        size = (w >> 30) & 3
        vector = (w >> 26) & 1
        opc = (w >> 22) & 3
        imm12 = (w >> 10) & 0xFFF
        rn = (w >> 5) & 0x1F
        rt = w & 0x1F
        kind = _ldst_kind(size, opc, vector)
        if kind is None:
            return None
        mnem, nbytes, signed, is_vec, scalar = kind
        if mnem == "prfm":
            return Insn(addr, w, "prfm", [])
        rt_op = _vreg(rt, nbytes * 8) if is_vec else _reg(rt, 32 if not signed and nbytes == 4 else (32 if signed and nbytes <= 4 else 64))
        if not is_vec:
            if signed:
                rt_op = _reg(rt, 32 if opc == 3 else 64)
            else:
                rt_op = _reg(rt, 64 if nbytes == 8 else 32)
        return Insn(addr, w, mnem, [rt_op, _reg(rn, 64, True), imm12 * nbytes],
                    notes={"bytes": nbytes, "signed": signed, "vector": is_vec,
                           "addressing": "offset", "writeback": False})

    # --- Load/store: unscaled / pre / post -------------------------------
    # size 111 V 00 opc 0 imm9 op2 Rn Rt ; op2 00 unscaled, 01 post, 10
    # unprivileged, 11 pre.  op2==10 is excluded here so the register-offset
    # form below (which shares bits 24-25 == 00) still gets a chance.
    if (w & 0x3B000000) == 0x38000000 and ((w >> 10) & 3) != 2:
        size = (w >> 30) & 3
        vector = (w >> 26) & 1
        opc = (w >> 22) & 3
        imm9 = sign_extend((w >> 12) & 0x1FF, 9)
        mode_bits = (w >> 10) & 3
        rn = (w >> 5) & 0x1F
        rt = w & 0x1F
        kind = _ldst_kind(size, opc, vector)
        if kind is None or mode_bits == 2:
            return None
        mnem, nbytes, signed, is_vec, _scalar = kind
        if mnem == "prfm":
            return Insn(addr, w, "prfm", [])
        if is_vec:
            rt_op = _vreg(rt, nbytes * 8)
        else:
            rt_op = _reg(rt, 32 if (signed and opc == 3) or (not signed and nbytes <= 4) else 64)
        mode = {0: "unscaled", 1: "post", 3: "pre"}[mode_bits]
        return Insn(addr, w, mnem, [rt_op, _reg(rn, 64, True), imm9],
                    notes={"bytes": nbytes, "signed": signed, "vector": is_vec,
                           "addressing": mode, "writeback": mode != "unscaled"})

    # --- Load/store: register offset -------------------------------------
    if (w & 0x3B200C00) == 0x38200800:
        size = (w >> 30) & 3
        vector = (w >> 26) & 1
        opc = (w >> 22) & 3
        rm = (w >> 16) & 0x1F
        option = (w >> 13) & 7
        s = (w >> 12) & 1
        rn = (w >> 5) & 0x1F
        rt = w & 0x1F
        kind = _ldst_kind(size, opc, vector)
        if kind is None:
            return None
        mnem, nbytes, signed, is_vec, _scalar = kind
        if mnem == "prfm":
            return Insn(addr, w, "prfm", [])
        if is_vec:
            rt_op = _vreg(rt, nbytes * 8)
        else:
            rt_op = _reg(rt, 32 if (signed and opc == 3) or (not signed and nbytes <= 4) else 64)
        return Insn(addr, w, mnem, [rt_op, _reg(rn, 64, True), _reg(rm, 64 if option == 3 else 32), option, s],
                    notes={"bytes": nbytes, "signed": signed, "vector": is_vec,
                           "addressing": "register", "writeback": False,
                           "option": option, "scale": s})

    # --- Load/store: pair ------------------------------------------------
    if (w & 0x3A000000) == 0x28000000:
        opc = (w >> 30) & 3
        vector = (w >> 26) & 1
        load = (w >> 22) & 1
        imm7 = sign_extend((w >> 15) & 0x7F, 7)
        rt2 = (w >> 10) & 0x1F
        rn = (w >> 5) & 0x1F
        rt = w & 0x1F
        mode_bits = (w >> 23) & 3
        if vector:
            if opc == 0:
                nbytes = 4
            elif opc == 1:
                nbytes = 8
            elif opc == 2:
                nbytes = 16
            else:
                return None
        else:
            if opc == 0:
                nbytes = 4
            elif opc == 2:
                nbytes = 8
            elif opc == 1 and load:
                nbytes = 4          # LDPSW: two sign-extended words into Xt, Xt2
            else:
                return None
        mode = {0: "noalloc", 1: "post", 2: "offset", 3: "pre"}[mode_bits]
        mnem = ("ldp" if load else "stp")
        if vector:
            a = _vreg(rt, nbytes * 8)
            b = _vreg(rt2, nbytes * 8)
        else:
            pw = 64 if (nbytes == 8 or opc == 1) else 32
            a = _reg(rt, pw)
            b = _reg(rt2, pw)
        return Insn(addr, w, mnem, [a, b, _reg(rn, 64, True), imm7 * nbytes],
                    notes={"bytes": nbytes, "vector": vector, "addressing": mode,
                           "writeback": mode in ("pre", "post"), "load": bool(load),
                           "signed": (not vector and opc == 1)})

    # --- Load register (literal) -----------------------------------------
    if (w & 0x3B000000) == 0x18000000:
        opc = (w >> 30) & 3
        vector = (w >> 26) & 1
        imm19 = sign_extend((w >> 5) & 0x7FFFF, 19) << 2
        rt = w & 0x1F
        target = addr + imm19
        if vector:
            width = {0: 32, 1: 64, 2: 128}.get(opc)
            if width is None:
                return None
            return Insn(addr, w, "ldr", [_vreg(rt, width), target],
                        notes={"literal": True, "bytes": width // 8, "vector": True})
        if opc == 3:
            return Insn(addr, w, "prfm", [rt, target])
        if opc == 2:
            return None
        return Insn(addr, w, "ldr", [_reg(rt, 64 if opc == 1 else 32), target],
                    notes={"literal": True, "bytes": 8 if opc == 1 else 4, "vector": False})

    # --- Load/store exclusive --------------------------------------------
    if (w & 0x3F000000) == 0x08000000:
        size = (w >> 30) & 3
        o2 = (w >> 23) & 1
        load = (w >> 22) & 1
        o1 = (w >> 21) & 1
        rs = (w >> 16) & 0x1F
        o0 = (w >> 15) & 1
        rt2 = (w >> 10) & 0x1F
        rn = (w >> 5) & 0x1F
        rt = w & 0x1F
        if o2 == 0 and o1 == 0 and o0 == 0:
            nbytes = 1 << size
            if load:
                return Insn(addr, w, "ldxr", [_reg(rt, nbytes * 8), _reg(rn, 64, True)],
                            notes={"bytes": nbytes})
            return Insn(addr, w, "stxr", [_reg(rs, 32), _reg(rt, nbytes * 8), _reg(rn, 64, True)],
                        notes={"bytes": nbytes})
        # Acquire/release forms: LDAXR/STLXR (o2=0,o0=1) keep the status
        # register in Rs; LDAR/STLR/LDAPR (o2=1,o0=1) use Rt2 == 11111.
        if o2 == 0 and o1 == 0 and o0 == 1:
            nbytes = 1 << size
            if load:
                return Insn(addr, w, "ldaxr", [_reg(rt, nbytes * 8), _reg(rn, 64, True)],
                            notes={"bytes": nbytes})
            return Insn(addr, w, "stlxr", [_reg(rs, 32), _reg(rt, nbytes * 8), _reg(rn, 64, True)],
                        notes={"bytes": nbytes})
        if o2 == 1 and o0 == 1 and rt2 == 0x1F:
            nbytes = 1 << size
            if load:
                return Insn(addr, w, "ldar", [_reg(rt, nbytes * 8), _reg(rn, 64, True)],
                            notes={"bytes": nbytes})
            return Insn(addr, w, "stlr", [_reg(rt, nbytes * 8), _reg(rn, 64, True)],
                        notes={"bytes": nbytes})
        return None

    # --- Floating point: 2-source -----------------------------------------
    # bits 11-10 == 10 distinguishes the 2-source group from FCMP/FCMPE and
    # the integer<->FP conversions, which share bits 23-21.
    if (w & 0x5F200C00) == 0x1E200800:
        ftype = (w >> 22) & 3
        rm = (w >> 16) & 0x1F
        opcode = (w >> 12) & 0xF
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        if ftype not in (0, 1):
            return None
        width = 32 if ftype == 0 else 64
        names = {0: "fmul", 1: "fdiv", 2: "fadd", 3: "fsub", 4: "fmax", 5: "fmin",
                 6: "fmaxnm", 7: "fminnm", 8: "fnmul"}
        if opcode in names:
            return Insn(addr, w, names[opcode], [_vreg(rd, width), _vreg(rn, width), _vreg(rm, width)])
        return None

    # --- Floating point: 1-source / compare / immediate --------------------
    if (w & 0x5F207C00) == 0x1E204000:
        ftype = (w >> 22) & 3
        opcode = (w >> 15) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        if ftype not in (0, 1):
            return None
        width = 32 if ftype == 0 else 64
        # Arm ARM FP data-processing (1 source): 0b0001xx = FCVT to S/D/H,
        # 0b001000.. = FRINTN, P, M, Z, A, -, X, I.  (The old table put
        # frintn/frintz/frintp/frintm on the FCVT opcodes.)
        one = {0: "fmov", 1: "fabs", 2: "fneg", 3: "fsqrt",
               4: "fcvt", 5: "fcvt", 7: "fcvt",
               8: "frintn", 9: "frintp", 10: "frintm", 11: "frintz",
               12: "frinta", 14: "frintx", 15: "frinti"}
        if opcode in one:
            if one[opcode] == "fcvt":
                # FCVT: source precision from ftype, destination from opc (bits
                # 16..15: 0 = S, 1 = D, 3 = H).  It used to assume the reverse.
                dst_w = {0: 32, 1: 64, 3: 16}.get((w >> 15) & 3)
                if dst_w is None:
                    return None
                return Insn(addr, w, "fcvt", [_vreg(rd, dst_w), _vreg(rn, width)])
            return Insn(addr, w, one[opcode], [_vreg(rd, width), _vreg(rn, width)])
        return None

    if (w & 0x5F203C00) == 0x1E202000:
        ftype = (w >> 22) & 3
        rm = (w >> 16) & 0x1F
        op = (w >> 14) & 3
        rn = (w >> 5) & 0x1F
        if ftype not in (0, 1) or op != 0:
            return None
        width = 32 if ftype == 0 else 64
        # opcode2 bit 4 = signalling (fcmpe), bit 3 = compare with #0.0 (Rm
        # ignored).  The zero form used to compare against register Rm.
        mnem = "fcmpe" if (w >> 4) & 1 else "fcmp"
        return Insn(addr, w, mnem, [_vreg(rn, width), _vreg(rm, width)], sets_flags=True,
                    notes={"zero": bool((w >> 3) & 1)})

    # FCSEL Vd, Vn, Vm, cond (bits 11..10 = 11).  It used to fall into the
    # FMOV-immediate pattern below, which was too loose.
    if (w & 0x5F200C00) == 0x1E200C00:
        ftype = (w >> 22) & 3
        if ftype not in (0, 1):
            return None
        width = 32 if ftype == 0 else 64
        return Insn(addr, w, "fcsel", [_vreg(w & 0x1F, width), _vreg((w >> 5) & 0x1F, width),
                                       _vreg((w >> 16) & 0x1F, width)], notes={"cond": (w >> 12) & 0xF})

    if (w & 0x5F201FE0) == 0x1E201000:
        ftype = (w >> 22) & 3
        imm8 = (w >> 13) & 0xFF
        rd = w & 0x1F
        if ftype not in (0, 1):
            return None
        width = 32 if ftype == 0 else 64
        return Insn(addr, w, "fmov", [_vreg(rd, width), ("fpimm", imm8, width)])

    # --- FP <-> integer conversions / FMOV GPR (bits 15..10 = 000000) -----
    if (w & 0x5F20FC00) == 0x1E200000:
        sf = (w >> 31) & 1
        ftype = (w >> 22) & 3
        rmode = (w >> 19) & 3
        opcode = (w >> 16) & 7
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        if ftype not in (0, 1):
            return None
        width = 32 if ftype == 0 else 64
        gpr = 32 if sf == 0 else 64
        if rmode == 0 and opcode == 6:
            return Insn(addr, w, "fmov", [_reg(rd, gpr), _vreg(rn, width)])
        if rmode == 0 and opcode == 7:
            return Insn(addr, w, "fmov", [_vreg(rd, width), _reg(rn, gpr)])
        # SCVTF/UCVTF and FCVT{N,P,M,Z,A}{S,U}.  Two earlier patterns for these
        # could never match (their value had bits the mask cleared), so every
        # int<->float conversion was unsupported and trapped.
        if rmode == 0 and opcode in (2, 3):
            return Insn(addr, w, "scvtf" if opcode == 2 else "ucvtf", [_vreg(rd, width), _reg(rn, gpr)])
        if opcode in (0, 1):
            mode = "npmz"[rmode]
            return Insn(addr, w, "fcvt%s%s" % (mode, "s" if opcode == 0 else "u"),
                        [_reg(rd, gpr), _vreg(rn, width)])
        if rmode == 0 and opcode in (4, 5):
            return Insn(addr, w, "fcvta%s" % ("s" if opcode == 4 else "u"), [_reg(rd, gpr), _vreg(rn, width)])
        return None

    # --- SIMD three same (integer/FP) --------------------------------------
    decoded = _decode_simd_three_same(addr, w)
    if decoded is not None:
        return decoded

    # --- SIMD two-register misc (DUP/UMOV/SMOV/INS) ------------------------
    decoded = _decode_simd_two_misc(addr, w)
    if decoded is not None:
        return decoded

    # --- SIMD modified immediate / MOVI ------------------------------------
    # --- SIMD batch 4 (by element, permutes, ADDV, FP misc, TBL, FCCMP) --
    decoded = _decode_simd_batch4(addr, w)
    if decoded is not None:
        return decoded

    # --- SIMD two-register misc: REV16/32/64, CNT, NOT, CLZ --------------
    decoded = _decode_simd_2reg_misc(addr, w)
    if decoded is not None:
        return decoded

    decoded = _decode_simd_batch6(addr, w)
    if decoded is not None:
        return decoded
    decoded = _decode_simd_modified_imm(addr, w)
    if decoded is not None:
        return decoded

    return None


# Advanced SIMD three-same opcode table, derived from the Arm ARM and verified
# against Capstone on the game's own .text (see tests/test_decoder_diff.py).
#
#   (U, opcode) -> mnemonic for integer lanes; lanes are 8 << size bits.
_SIMD_3SAME_INT = {
    (0, 0x06): "cmgt", (1, 0x06): "cmhi",
    (0, 0x07): "cmge", (1, 0x07): "cmhs",
    (0, 0x08): "sshl", (1, 0x08): "ushl",
    (0, 0x0c): "smax", (1, 0x0c): "umax",
    (0, 0x0d): "smin", (1, 0x0d): "umin",
    (0, 0x10): "add", (1, 0x10): "sub",
    (0, 0x11): "cmtst", (1, 0x11): "cmeq",
    (0, 0x12): "mla",
    (0, 0x13): "mul",
    (0, 0x17): "addp",
}
# Floating-point three-same: (U, bit23, opcode) -> mnemonic; lane size is
# 32 bits when bit22 (sz) is 0 and 64 bits when it is 1.
# U selects between operation pairs: FADD/FSUB, FMLA/FMLS, etc.
# bit23 (size[1]) further distinguishes FCMGE/FCGT and FMAX/FMIN variants.
_SIMD_3SAME_FP = {
    # Exactly per the Arm ARM "Advanced SIMD three same" FP encodings
    # (U = bit 29, a = bit 23, opcode = bits 15..11).  The old table mapped
    # e.g. FSUB -> fadd and FRECPS/FRSQRTS -> fdiv.  Operations the emitter
    # has no helper for yet (faddp, fabd, fmulx, frecps, ...) are left out so
    # they trap instead of silently computing something else.
    (0, 0, 0x19): "fmla", (0, 1, 0x19): "fmls",
    (0, 0, 0x1a): "fadd", (0, 1, 0x1a): "fsub", (1, 0, 0x1a): "faddp",
    (0, 0, 0x1f): "frecps", (0, 1, 0x1f): "frsqrts",
    (1, 0, 0x1b): "fmul",
    (0, 0, 0x1c): "fcmeq", (1, 0, 0x1c): "fcmge", (1, 1, 0x1c): "fcmgt",
    (0, 0, 0x1e): "fmax", (0, 1, 0x1e): "fmin",
    (1, 0, 0x1e): "fmaxp", (1, 1, 0x1e): "fminp",
    (1, 0, 0x1f): "fdiv",
}
# Bytewise logical (size selects the operation).
_SIMD_3SAME_LOGIC = {0: ("and", "bic", "orr", "orn"), 1: ("eor", "bsl", "bit", "bif")}


def _decode_simd_three_same(addr: int, w: int):
    # 0 Q U 01110 size 1 Rm opcode 1 Rn Rd
    if (w & 0x9F200400) != 0x0E200400:
        return None
    q = (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    rm = (w >> 16) & 0x1F
    opcode = (w >> 11) & 0x1F
    rn = (w >> 5) & 0x1F
    rd = w & 0x1F
    bit23 = (size >> 1) & 1
    sz = size & 1
    if opcode == 0x03:
        name = _SIMD_3SAME_LOGIC[u][size]
        lanes = 16 if q else 8
        arr = (8, lanes)
        return Insn(addr, w, "v" + name,
                    [_vreg(rd, 128, arr), _vreg(rn, 128, arr), _vreg(rm, 128, arr)],
                    notes={"lane_bits": 8, "lanes": lanes, "q": q})
    fp = _SIMD_3SAME_FP.get((u, bit23, opcode))
    if fp is not None:
        lane_bits = 32 if sz == 0 else 64
        lanes = ((16 if q else 8) * 8) // lane_bits
        arr = (lane_bits, lanes)
        return Insn(addr, w, "v" + fp,
                    [_vreg(rd, 128, arr), _vreg(rn, 128, arr), _vreg(rm, 128, arr)],
                    notes={"lane_bits": lane_bits, "lanes": lanes, "q": q})
    name = _SIMD_3SAME_INT.get((u, opcode))
    if name is None:
        return None
    lane_bits = {0: 8, 1: 16, 2: 32, 3: 64}[size]
    lanes = (16 if q else 8) * 8 // lane_bits
    arr = (lane_bits, lanes)
    return Insn(addr, w, "v" + name,
                [_vreg(rd, 128, arr), _vreg(rn, 128, arr), _vreg(rm, 128, arr)],
                notes={"lane_bits": lane_bits, "lanes": lanes, "q": q})


def _decode_simd_two_misc(addr: int, w: int):
    # Advanced SIMD copy class: 0 Q op 01110000 imm5 0000 code Rn Rd, where the
    # low six bits (15..10) select the operation.  Verified against Capstone:
    #   code 0x01 op=0 DUP(element), 0x03 DUP(general), 0x01 op=1 INS(element),
    #   0x07 INS(general), 0x0F UMOV, 0x0B SMOV.
    if (w & 0x9F200400) == 0x0E000400 and (w & 0x00200000) == 0:
        q = (w >> 30) & 1
        op = (w >> 29) & 1
        imm5 = (w >> 16) & 0x1F
        code = (w >> 10) & 0x3F
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        size, idx = _imm5_lane(imm5)
        if size is None:
            return None
        lane_bits = 8 << size
        lanes = (16 if q else 8) * 8 // lane_bits
        if code == 0x01 and op == 0:
            return Insn(addr, w, "vdup_elem", [_vreg(rd, 128, (lane_bits, lanes)),
                                               _vreg(rn, 128, (lane_bits, lanes), idx)],
                        notes={"lane_bits": lane_bits, "lanes": lanes, "q": q})
        if code == 0x03 and op == 0:
            return Insn(addr, w, "vdup", [_vreg(rd, 128, (lane_bits, lanes)), _reg(rn, 32)],
                        notes={"lane_bits": lane_bits, "lanes": lanes, "q": q})
        if (code & 0x21) == 0x01 and op == 1:
            # INS (element): code = 0 imm4 1 (only imm4 == 0 was accepted before),
            # destination lane from imm5, source lane = imm4 >> size
            # (Arm ARM index2 = imm4<3:size>; it used imm4 >> (3 - size)).
            imm4 = (w >> 11) & 0xF
            src_lane = imm4 >> size
            return Insn(addr, w, "vins_elem", [_vreg(rd, 128, (lane_bits, None), idx),
                                               _vreg(rn, 128, (lane_bits, None), src_lane)],
                        notes={"lane_bits": lane_bits, "lane": idx, "q": q})
        if code == 0x07 and op == 0:
            return Insn(addr, w, "vins_general", [_vreg(rd, 128, (lane_bits, None), idx),
                                                   _reg(rn, 32)],
                        notes={"lane_bits": lane_bits, "lane": idx, "q": q})
        if code == 0x0F and op == 0:
            return Insn(addr, w, "umov", [_reg(rd, 32), _vreg(rn, 128, (lane_bits, None), idx)],
                        notes={"lane_bits": lane_bits})
        if code == 0x0B and op == 0:
            return Insn(addr, w, "smov", [_reg(rd, 32), _vreg(rn, 128, (lane_bits, None), idx)],
                        notes={"lane_bits": lane_bits})
        return None

    # EXT Vd.<T>, Vn.<T>, Vm.<T>, #imm  (0 Q 101110 00 0 Rm 0 imm4 0 Rn Rd)
    if (w & 0xBFE08400) == 0x2E000000:
        q = (w >> 30) & 1
        rm = (w >> 16) & 0x1F
        index = (w >> 11) & 0xF
        rn = (w >> 5) & 0x1F
        rd = w & 0x1F
        lane_bits = 8
        lanes = 16 if q else 8
        if index >= lanes:
            return None
        arr = (lane_bits, lanes)
        return Insn(addr, w, "vext",
                    [_vreg(rd, 128, arr), _vreg(rn, 128, arr), _vreg(rm, 128, arr), index],
                    notes={"lane_bits": lane_bits, "lanes": lanes, "index": index, "q": q})
    # SIMD vector move: ORR Vd.16B, Vn.16B, Vm.16B already covered by three-same.
    return None


def _imm5_lane(imm5: int):
    if imm5 & 1:
        return 0, imm5 >> 1
    if imm5 & 2:
        return 1, imm5 >> 2
    if imm5 & 4:
        return 2, imm5 >> 3
    if imm5 & 8:
        return 3, imm5 >> 4
    return None, 0


def _decode_simd_batch4(addr: int, w: int):
    """FP by element, permutes, ADDV, scalar DUP, FP two-reg misc, scalar
    SCVTF/UCVTF (vector register), TBL, FCCMP."""
    rd, rn, rm = w & 0x1F, (w >> 5) & 0x1F, (w >> 16) & 0x1F
    q = (w >> 30) & 1
    # FMLA/FMLS/FMUL (by element): 0 Q 0 01111 1 sz L M Rm opc H 0 Rn Rd
    if (w & 0xBF800400) == 0x0F800000:
        sz = (w >> 22) & 1
        opc = (w >> 12) & 0xF
        name = {1: "fmla", 5: "fmls", 9: "fmul"}.get(opc)
        if name is None:
            return None
        l_bit, h_bit = (w >> 21) & 1, (w >> 11) & 1
        mreg = (((w >> 20) & 1) << 4) | ((w >> 16) & 0xF)
        if sz:
            if l_bit or not q:
                return None
            lane_bits, idx = 64, h_bit
        else:
            lane_bits, idx = 32, (h_bit << 1) | l_bit
        lanes = (128 if q else 64) // lane_bits
        return Insn(addr, w, "v%s_elem" % name, [_vreg(rd, 128), _vreg(rn, 128), _vreg(mreg, 128, None, idx)],
                    notes={"lane_bits": lane_bits, "lanes": lanes, "q": q, "index": idx})
    # UZP/TRN/ZIP: 0 Q 0 01110 size 0 Rm 0 opc 10 Rn Rd
    if (w & 0xBF208C00) == 0x0E000800:
        opc = (w >> 12) & 7
        name = {1: "uzp1", 2: "trn1", 3: "zip1", 5: "uzp2", 6: "trn2", 7: "zip2"}.get(opc)
        size = (w >> 22) & 3
        if name is None or (size == 3 and not q):
            return None
        lane_bits = 8 << size
        return Insn(addr, w, "v" + name, [_vreg(rd, 128), _vreg(rn, 128), _vreg(rm, 128)],
                    notes={"lane_bits": lane_bits, "lanes": (128 if q else 64) // lane_bits, "q": q})
    # ADDV: 0 Q 0 01110 size 11000 11011 10 Rn Rd
    if (w & 0xBF3FFC00) == 0x0E31B800:
        size = (w >> 22) & 3
        if size == 3 or (size == 2 and not q):
            return None
        lane_bits = 8 << size
        return Insn(addr, w, "vaddv", [_vreg(rd, lane_bits), _vreg(rn, 128)],
                    notes={"lane_bits": lane_bits, "lanes": (128 if q else 64) // lane_bits})
    # DUP (element, scalar) = MOV Vd, Vn.T[i]: 01011110000 imm5 000001 Rn Rd
    if (w & 0xFFE0FC00) == 0x5E000400:
        imm5 = (w >> 16) & 0x1F
        size, idx = _imm5_lane(imm5)
        if size is None:
            return None
        return Insn(addr, w, "vdup_scalar", [_vreg(rd, 8 << size), _vreg(rn, 128, None, idx)],
                    notes={"lane_bits": 8 << size, "index": idx})
    # FP two-register misc (vector): 0 Q U 01110 a sz 10000 opcode 10 Rn Rd
    if (w & 0x9F3E0C00) == 0x0E200800 or (w & 0x9F3E0C00) == 0x0EA00800 or \
            (w & 0x9FBE0C00) == 0x0E200800:
        u = (w >> 29) & 1
        a = (w >> 23) & 1
        sz = (w >> 22) & 1
        opcode = (w >> 12) & 0x1F
        table = {(0, 1, 0x0F): "fabs", (1, 1, 0x0F): "fneg", (1, 1, 0x1F): "fsqrt",
                 (0, 1, 0x0C): "fcmgtz", (0, 1, 0x0D): "fcmeqz", (0, 1, 0x0E): "fcmltz",
                 (1, 1, 0x0C): "fcmgez", (1, 1, 0x0D): "fcmlez",
                 (0, 0, 0x1D): "scvtf", (1, 0, 0x1D): "ucvtf",
                 (0, 1, 0x1D): "frecpe", (1, 1, 0x1D): "frsqrte"}
        name = table.get((u, a, opcode))
        if name is not None and not (sz and not q):
            lane_bits = 64 if sz else 32
            return Insn(addr, w, "vf2_" + name, [_vreg(rd, 128), _vreg(rn, 128)],
                        notes={"lane_bits": lane_bits, "lanes": (128 if q else 64) // lane_bits, "q": q})
    # FRECPE / FRSQRTE (scalar): 01 U 11110 1 sz 10000 11101 10 Rn Rd
    if (w & 0xDFBFFC00) == 0x5EA1D800:
        sz = (w >> 22) & 1
        name = "frsqrte" if (w >> 29) & 1 else "frecpe"
        return Insn(addr, w, "vf2_" + name, [_vreg(rd, 64 if sz else 32), _vreg(rn, 64 if sz else 32)],
                    notes={"lane_bits": 64 if sz else 32, "lanes": 1, "q": 0})
    # FABD (scalar): 01 1 11110 1 sz 1 Rm 11010 1 Rn Rd
    if (w & 0xFFA0FC00) == 0x7EA0D400:
        sz = (w >> 22) & 1
        return Insn(addr, w, "vfabd_scalar", [_vreg(rd, 128), _vreg(rn, 128), _vreg(rm, 128)],
                    notes={"esz": 64 if sz else 32})
    # SCVTF/UCVTF (scalar, integer in a vector register): 01 U 11110 0 sz 10000 11101 10 Rn Rd
    if (w & 0xDFBFFC00) == 0x5E21D800:
        sz = (w >> 22) & 1
        name = "ucvtf" if (w >> 29) & 1 else "scvtf"
        return Insn(addr, w, "vf2_" + name, [_vreg(rd, 64 if sz else 32), _vreg(rn, 64 if sz else 32)],
                    notes={"lane_bits": 64 if sz else 32, "lanes": 1, "q": 0})
    # TBL: 0 Q 001110 000 Rm 0 len 0 00 Rn Rd
    if (w & 0xBFE09C00) == 0x0E000000:
        nregs = ((w >> 13) & 3) + 1
        return Insn(addr, w, "vtbl", [_vreg(rd, 128), _vreg(rn, 128), _vreg(rm, 128)],
                    notes={"nregs": nregs, "q": q})
    # FCCMP / FCCMPE: 0 0 0 11110 ftype 1 Rm cond 01 Rn op nzcv
    if (w & 0xFF200C00) in (0x1E200400, 0x1E600400):
        ftype = (w >> 22) & 3
        width = 32 if ftype == 0 else 64
        return Insn(addr, w, "fccmpe" if (w >> 4) & 1 else "fccmp",
                    [_vreg(rn, width), _vreg(rm, width)], sets_flags=True,
                    notes={"cond": (w >> 12) & 0xF, "nzcv": w & 0xF})
    return None


def _decode_simd_2reg_misc(addr: int, w: int):
    """0 Q U 01110 size 10000 opcode 10 Rn Rd (integer forms with runtime
    helpers).  Not decoded before, so e.g. REV32 trapped in nninitStartup."""
    if (w & 0x9F3E0C00) != 0x0E200800:
        return None
    q = (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3
    opcode = (w >> 12) & 0x1F
    rn = (w >> 5) & 0x1F
    rd = w & 0x1F
    lane_bits = 8 << size
    name = None
    if opcode == 0x00 and size <= 2:
        name = "rev32" if u else "rev64"
        if u and size > 1:
            return None
    elif opcode == 0x01 and u == 0 and size == 0:
        name = "rev16"
    elif opcode == 0x05 and size == 0:
        name, lane_bits = ("not", 0) if u else ("cnt", 8)
    elif opcode == 0x04 and u == 1 and size <= 2:
        name = "clz"
    if name is None:
        return None
    lanes = ((16 if q else 8) * 8) // (lane_bits or 8)
    return Insn(addr, w, "v" + name, [_vreg(rd, 128), _vreg(rn, 128)],
                notes={"lane_bits": lane_bits, "lanes": lanes, "q": q})


def _decode_simd_modified_imm(addr: int, w: int):
    """MOVI / MVNI / ORR / BIC / FMOV (vector, immediate): Arm ARM
    AdvSIMDExpandImm(op, cmode, imm8).  The previous version had the 32/16-bit
    cmode table reversed, decoded the 64-bit byte-mask MOVI as MVNI, read the
    FMOV precision from bit 22 and treated ORR/BIC immediates as MOVI."""
    # 0 Q op 0111100000 a b c cmode o2 1 d e f g h Rd
    if (w & 0x9FF80400) != 0x0F000400:
        return None
    q = (w >> 30) & 1
    op = (w >> 29) & 1
    cmode = (w >> 12) & 0xF
    imm8 = ((w >> 16) & 0x7) << 5 | ((w >> 5) & 0x1F)
    rd = w & 0x1F
    hi = cmode >> 1
    if cmode == 0b1111:
        # FMOV Vd.<T>, #imm: op 0 = single (2S/4S), op 1 = double (2D, Q must be 1)
        b = (imm8 >> 6) & 1
        if op:
            if not q:
                return None
            exponent = ((1 - b) << 10) | ((0xFF * b) << 2) | ((imm8 >> 4) & 3)
            bits = ((imm8 >> 7) << 63) | (exponent << 52) | ((imm8 & 0xF) << 48)
            return Insn(addr, w, "vfmovi", [_vreg(rd, 128), bits, 64], notes={"lane_bits": 64, "q": q})
        exponent = ((1 - b) << 7) | ((0x1F * b) << 2) | ((imm8 >> 4) & 3)
        bits = ((imm8 >> 7) << 31) | (exponent << 23) | ((imm8 & 0xF) << 19)
        return Insn(addr, w, "vfmovi", [_vreg(rd, 128), bits, 32], notes={"lane_bits": 32, "q": q})
    if cmode == 0b1110:
        if op == 0:                                   # MOVI 8-bit, replicated
            return Insn(addr, w, "vmovi", [_vreg(rd, 128), imm8, 8], notes={"lane_bits": 8, "q": q})
        value = 0                                     # MOVI 64-bit byte mask
        for i in range(8):
            if (imm8 >> i) & 1:
                value |= 0xFF << (8 * i)
        return Insn(addr, w, "vmovi", [_vreg(rd, 128), value, 64], notes={"lane_bits": 64, "q": q})
    if hi <= 0b011:                                   # 32-bit, imm8 << 8*n
        lane_bits, value = 32, imm8 << (8 * hi)
    elif hi <= 0b101:                                 # 16-bit, imm8 << 8*n
        lane_bits, value = 16, imm8 << (8 * (hi & 1))
    else:                                             # 110x: 32-bit shifting ones (MSL)
        lane_bits = 32
        value = ((imm8 << 8) | 0xFF) if (cmode & 1) == 0 else ((imm8 << 16) | 0xFFFF)
        return Insn(addr, w, "vmvni" if op else "vmovi", [_vreg(rd, 128), value, lane_bits],
                    notes={"lane_bits": lane_bits, "q": q})
    if cmode & 1:                                     # ORR / BIC (vector, immediate)
        return Insn(addr, w, "vbici" if op else "vorri", [_vreg(rd, 128), value, lane_bits],
                    notes={"lane_bits": lane_bits, "q": q})
    return Insn(addr, w, "vmvni" if op else "vmovi", [_vreg(rd, 128), value, lane_bits],
                notes={"lane_bits": lane_bits, "q": q})


# --------------------------------------------------------------------------
# SIMD batch 6.  Most forms lift to one runtime helper call with a uniform
# signature, sw6_<name>(d, n, m, imm, q, esz) -> new Vd (sw_simd6.h), so the
# decoder records the helper and its parameters in ``notes``:
#   helper  C function name      imm  shift / index
#   q       Q bit (64/128-bit)   esz  element bits as documented per helper
# --------------------------------------------------------------------------
def _v6(addr, w, name, rd, rn, rm, q, esz, imm=0):
    ops = [_vreg(rd, 128), _vreg(rn, 128)]
    if rm is not None:
        ops.append(_vreg(rm, 128))
    return Insn(addr, w, "v" + name, ops,
                notes={"helper": "sw6_" + name, "q": q, "esz": esz, "imm": imm})


_B6_3SAME_INT = {
    (0, 0x00): "shadd", (1, 0x00): "uhadd",
    (0, 0x01): "sqadd", (1, 0x01): "uqadd",
    (0, 0x05): "sqsub", (1, 0x05): "uqsub",
    (0, 0x0e): "sabd", (1, 0x0e): "uabd",
    (0, 0x0f): "saba", (1, 0x0f): "uaba",
    (0, 0x14): "smaxp", (1, 0x14): "umaxp",
    (0, 0x15): "sminp", (1, 0x15): "uminp",
    (0, 0x16): "sqdmulh", (1, 0x16): "sqrdmulh",
    (1, 0x12): "mls",
    (0, 0x09): "sqshl", (1, 0x09): "uqshl", (0, 0x0b): "sqrshl", (1, 0x0b): "uqrshl",
}
_B6_3SAME_FP = {
    (1, 1, 0x1a): "fabd",
    (0, 0, 0x18): "fmaxnm", (0, 1, 0x18): "fminnm",
    (1, 0, 0x18): "fmaxnmp", (1, 1, 0x18): "fminnmp",
}
_B6_2MISC_INT = {
    (0, 0x08): "cmgt0", (1, 0x08): "cmge0", (0, 0x09): "cmeq0", (1, 0x09): "cmle0",
    (0, 0x0a): "cmlt0", (0, 0x0b): "abs", (1, 0x0b): "neg",
    (0, 0x07): "sqabs", (1, 0x07): "sqneg",
    (0, 0x02): "saddlp", (1, 0x02): "uaddlp", (0, 0x06): "sadalp", (1, 0x06): "uadalp",
}
_B6_NARROW = {(0, 0x12): "xtn", (1, 0x12): "sqxtun", (0, 0x14): "sqxtn", (1, 0x14): "uqxtn"}
# FP two-register misc: (U, a, opcode) -> name; a = bit 23
_B6_2MISC_FP = {
    (0, 0, 0x18): "frintn", (0, 0, 0x19): "frintm", (0, 1, 0x18): "frintp",
    (0, 1, 0x19): "frintz", (1, 0, 0x18): "frinta", (1, 0, 0x19): "frintx",
    (1, 1, 0x19): "frinti",
    (0, 0, 0x1a): "fcvtns", (0, 0, 0x1b): "fcvtms", (0, 0, 0x1c): "fcvtas",
    (0, 1, 0x1a): "fcvtps", (0, 1, 0x1b): "fcvtzs",
    (1, 0, 0x1a): "fcvtnu", (1, 0, 0x1b): "fcvtmu", (1, 0, 0x1c): "fcvtau",
    (1, 1, 0x1a): "fcvtpu", (1, 1, 0x1b): "fcvtzu",
}
_B6_ACROSS = {(0, 0x03): "saddlv", (1, 0x03): "uaddlv", (0, 0x0a): "smaxv",
              (1, 0x0a): "umaxv", (0, 0x1a): "sminv", (1, 0x1a): "uminv"}
_B6_SHIFT = {
    (0, 0x00): "sshr", (1, 0x00): "ushr", (0, 0x02): "ssra", (1, 0x02): "usra",
    (0, 0x04): "srshr", (1, 0x04): "urshr", (0, 0x06): "srsra", (1, 0x06): "ursra",
    (1, 0x08): "sri", (0, 0x0a): "shl", (1, 0x0a): "sli",
    (0, 0x0e): "sqshli", (1, 0x0e): "uqshli", (1, 0x0c): "sqshlui",
    (0, 0x10): "shrn", (1, 0x10): "sqshrun", (0, 0x11): "rshrn", (1, 0x11): "sqrshrun",
    (0, 0x12): "sqshrn", (1, 0x12): "uqshrn", (0, 0x13): "sqrshrn", (1, 0x13): "uqrshrn",
    (0, 0x14): "sshll", (1, 0x14): "ushll",
    (0, 0x1c): "scvtfx", (1, 0x1c): "ucvtfx", (0, 0x1f): "fcvtzsx", (1, 0x1f): "fcvtzux",
}
_B6_NARROW_SHIFTS = ("shrn", "rshrn", "sqshrn", "uqshrn", "sqrshrn", "uqrshrn",
                     "sqshrun", "sqrshrun")
_B6_LEFT_SHIFTS = ("shl", "sli", "sqshli", "uqshli", "sqshlui", "sshll", "ushll")
_B6_3DIFF = {
    (0, 0x0): "saddl", (1, 0x0): "uaddl", (0, 0x1): "saddw", (1, 0x1): "uaddw",
    (0, 0x2): "ssubl", (1, 0x2): "usubl", (0, 0x3): "ssubw", (1, 0x3): "usubw",
    (0, 0x4): "addhn", (1, 0x4): "raddhn", (0, 0x5): "sabal", (1, 0x5): "uabal",
    (0, 0x6): "subhn", (1, 0x6): "rsubhn", (0, 0x7): "sabdl", (1, 0x7): "uabdl",
    (0, 0x8): "smlal", (1, 0x8): "umlal", (0, 0xa): "smlsl", (1, 0xa): "umlsl",
    (0, 0xc): "smull", (1, 0xc): "umull", (0, 0xe): "pmull",
}


def _decode_simd_batch6(addr: int, w: int):
    rd, rn, rm = w & 0x1F, (w >> 5) & 0x1F, (w >> 16) & 0x1F
    q = (w >> 30) & 1
    u = (w >> 29) & 1
    size = (w >> 22) & 3

    # FMUL/FMLA/FMLS (by element, scalar): 01 0 11111 1 sz L M Rm opc H 0 Rn Rd
    if (w & 0xFF800400) == 0x5F800000:
        sz = (w >> 22) & 1
        name = {1: "fmla", 5: "fmls", 9: "fmul"}.get((w >> 12) & 0xF)
        l_bit, h_bit = (w >> 21) & 1, (w >> 11) & 1
        mreg = (((w >> 20) & 1) << 4) | (rm & 0xF)
        if name is None or (sz and l_bit):
            return None
        lane_bits, idx = (64, h_bit) if sz else (32, (h_bit << 1) | l_bit)
        return Insn(addr, w, "v%s_elem" % name, [_vreg(rd, lane_bits), _vreg(rn, lane_bits),
                                                 _vreg(mreg, 128, None, idx)],
                    notes={"lane_bits": lane_bits, "lanes": 1, "q": 0, "index": idx})

    # Integer MUL/MLA/MLS (by element): 0 Q U 01111 size L M Rm opc H 0 Rn Rd
    if (w & 0x9F000400) == 0x0F000000 and size in (1, 2):
        opc = (w >> 12) & 0xF
        name = {(0, 8): "mul_elem", (1, 0): "mla_elem", (1, 4): "mls_elem"}.get((u, opc))
        if name is not None:
            l_bit, h_bit, m_bit = (w >> 21) & 1, (w >> 11) & 1, (w >> 20) & 1
            if size == 1:
                idx, mreg, esz = (h_bit << 2) | (l_bit << 1) | m_bit, rm & 0xF, 16
            else:
                idx, mreg, esz = (h_bit << 1) | l_bit, (m_bit << 4) | (rm & 0xF), 32
            return _v6(addr, w, name, rd, rn, mreg, q, esz, idx)

    # Three same (extras): 0 Q U 01110 size 1 Rm opcode 1 Rn Rd
    if (w & 0x9F200400) == 0x0E200400:
        opcode = (w >> 11) & 0x1F
        fp = _B6_3SAME_FP.get((u, size >> 1, opcode))
        if fp is not None:
            esz = 64 if size & 1 else 32
            if esz == 64 and not q:
                return None
            return _v6(addr, w, fp, rd, rn, rm, q, esz)
        name = _B6_3SAME_INT.get((u, opcode))
        if name is None or (size == 3 and not q):
            return None
        if name in ("sqdmulh", "sqrdmulh") and size not in (1, 2):
            return None
        if name not in ("sqadd", "uqadd", "sqsub", "uqsub") and size == 3:
            return None
        return _v6(addr, w, name, rd, rn, rm, q, 8 << size)

    # Three different: 0 Q U 01110 size 1 Rm opcode 00 Rn Rd (esz = narrow element)
    if (w & 0x9F200C00) == 0x0E200000:
        name = _B6_3DIFF.get((u, (w >> 12) & 0xF))
        if name is None:
            return None
        if name == "pmull":
            if size not in (0, 3):
                return None
        elif size == 3:
            return None
        return _v6(addr, w, name, rd, rn, rm, q, 8 << size)

    # Two-register misc: 0 Q U 01110 size 10000 opcode 10 Rn Rd
    if (w & 0x9F3E0C00) == 0x0E200800:
        opcode = (w >> 12) & 0x1F
        name = _B6_2MISC_INT.get((u, opcode))
        if name is not None:
            if size == 3 and (not q or name.endswith("lp")):
                return None
            return _v6(addr, w, name, rd, rn, None, q, 8 << size)
        name = _B6_NARROW.get((u, opcode))
        if name is not None:
            if size == 3:
                return None
            return _v6(addr, w, name, rd, rn, None, q, 8 << size)      # esz = narrow element
        if u == 1 and opcode == 0x13:                                  # SHLL / SHLL2
            if size == 3:
                return None
            return _v6(addr, w, "shll", rd, rn, None, q, 8 << size, 8 << size)
        if opcode in (0x16, 0x17) and u == 0 and (size >> 1) == 0:     # FCVTN / FCVTL
            name = "fcvtn" if opcode == 0x16 else "fcvtl"
            return _v6(addr, w, name, rd, rn, None, q, 64 if size & 1 else 32)   # esz = wide element
        name = _B6_2MISC_FP.get((u, size >> 1, opcode))
        if name is not None:
            esz = 64 if size & 1 else 32
            if esz == 64 and not q:
                return None
            return _v6(addr, w, "f2x_" + name, rd, rn, None, q, esz)
        return None

    # Across lanes: 0 Q U 01110 size 11000 opcode 10 Rn Rd
    if (w & 0x9F3E0C00) == 0x0E300800:
        opcode = (w >> 12) & 0x1F
        if opcode in (0x0c, 0x0f):                    # FMAXNMV/FMINNMV/FMAXV/FMINV (4S only)
            if not u or (size & 1) or not q:
                return None
            name = {(0x0c, 0): "fmaxnmv", (0x0c, 1): "fminnmv",
                    (0x0f, 0): "fmaxv", (0x0f, 1): "fminv"}[(opcode, size >> 1)]
            return _v6(addr, w, name, rd, rn, None, q, 32)
        name = _B6_ACROSS.get((u, opcode))
        if name is None or size == 3 or (size == 2 and not q):
            return None
        return _v6(addr, w, name, rd, rn, None, q, 8 << size)

    # Scalar pairwise: 01 U 11110 size 11000 opcode 10 Rn Rd
    if (w & 0xDF3E0C00) == 0x5E300800:
        opcode = (w >> 12) & 0x1F
        if u == 0 and opcode == 0x1b and size == 3:
            return _v6(addr, w, "addp_scalar", rd, rn, None, 1, 64)
        if u == 1 and opcode in (0x0c, 0x0d, 0x0f):
            esz = 64 if size & 1 else 32
            name = {(0x0c, 0): "fmaxnmp", (0x0c, 1): "fminnmp", (0x0d, 0): "faddp",
                    (0x0f, 0): "fmaxp", (0x0f, 1): "fminp"}.get((opcode, size >> 1))
            if name is None:
                return None
            return _v6(addr, w, name + "_scalar", rd, rn, None, 1, esz)
        return None

    # Shift by immediate (vector): 0 Q U 011110 immh immb opcode 1 Rn Rd, immh != 0
    if (w & 0x9F800400) == 0x0F000400 and ((w >> 19) & 0xF):
        immh = (w >> 19) & 0xF
        immhb = (w >> 16) & 0x7F
        name = _B6_SHIFT.get((u, (w >> 11) & 0x1F))
        if name is None:
            return None
        esz = 8 << (immh.bit_length() - 1)
        narrow = name in _B6_NARROW_SHIFTS
        if (narrow or name in ("sshll", "ushll")) and esz == 64:
            return None
        if esz == 64 and not q:
            return None
        if name in ("scvtfx", "ucvtfx", "fcvtzsx", "fcvtzux") and esz < 32:
            return None
        shift = immhb - esz if name in _B6_LEFT_SHIFTS else 2 * esz - immhb
        return _v6(addr, w, name, rd, rn, None, q, esz, shift)

    # LD1/ST1 (multiple) and LD2/3/4, ST2/3/4 (multiple structures)
    #   0 Q 0011000 L 000000 opcode size Rn Rt  /  0 Q 0011001 L 0 Rm opcode size Rn Rt
    if (w & 0xBFBF0000) == 0x0C000000 or (w & 0xBFA00000) == 0x0C800000:
        load = (w >> 22) & 1
        post = (w >> 23) & 1
        size = (w >> 10) & 3
        kind = {0x0: (4, 4), 0x2: (1, 4), 0x4: (3, 3), 0x6: (1, 3), 0x7: (1, 1),
                0x8: (2, 2), 0xa: (1, 2)}.get((w >> 12) & 0xF)
        if kind is None or (size == 3 and not q and kind[0] > 1):
            return None
        selem, nregs = kind
        return Insn(addr, w, ("ld%d_multi" if load else "st%d_multi") % selem,
                    [_vreg(rd, 128), _reg(rn, 64, True)],
                    notes={"selem": selem, "nregs": nregs, "esz": 8 << size, "q": q,
                           "post": post, "rm": rm})

    # FP <-> fixed-point (scalar): sf 0 0 11110 ftype 0 rmode opcode scale Rn Rd
    if (w & 0x7F200000) == 0x1E000000:
        sf = (w >> 31) & 1
        ftype = (w >> 22) & 3
        rmode = (w >> 19) & 3
        opcode = (w >> 16) & 7
        scale = (w >> 10) & 0x3F
        if ftype not in (0, 1) or (sf == 0 and scale < 32):
            return None
        width = 32 if ftype == 0 else 64
        gpr = 64 if sf else 32
        fbits = 64 - scale
        if rmode == 3 and opcode in (0, 1):
            return Insn(addr, w, "fcvtz%s" % ("s" if opcode == 0 else "u"),
                        [_reg(rd, gpr), _vreg(rn, width)], notes={"fbits": fbits})
        if rmode == 0 and opcode in (2, 3):
            return Insn(addr, w, "scvtf" if opcode == 2 else "ucvtf",
                        [_vreg(rd, width), _reg(rn, gpr)], notes={"fbits": fbits})
        return None

    # CRC32{B,H,W,X} / CRC32C*: sf 0 0 11010110 Rm 010 C sz Rn Rd
    if (w & 0x7FE0E000) == 0x1AC04000:
        sf = (w >> 31) & 1
        sz = (w >> 10) & 3
        if (sz == 3) != bool(sf):
            return None
        c = (w >> 12) & 1
        return Insn(addr, w, "crc32%s%s" % ("c" if c else "", "bhwx"[sz]),
                    [_reg(rd, 32), _reg(rn, 32), _reg(rm, 64 if sz == 3 else 32)],
                    notes={"bytes": 1 << sz, "castagnoli": c})
    return None
