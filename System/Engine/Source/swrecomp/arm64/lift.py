"""AArch64 -> IR lifting.

Every supported instruction becomes explicit IR; anything else becomes a
``trap`` op that reports the address, encoding and reason at run time, and is
listed in the coverage report at build time.  Nothing is dropped silently.
"""

from __future__ import annotations

from dataclasses import replace

from .decoder import COND_NAMES, Insn, Reg, Unsupported, VReg, decode, decode_bit_masks
from .ir import Block, Function, Op

FLAG_REGS = ("nf", "zf", "cf", "vf")

# Condition encoded as (kind, imm) evaluated by the emitter via sw_cond().
SYSREG_NAMES = {
    (3, 3, 4, 2, 0): "nzcv",
    (3, 3, 4, 4, 0): "fpcr",
    (3, 3, 4, 4, 1): "fpsr",
    (3, 3, 13, 0, 2): "tpidr_el0",
    (3, 3, 13, 0, 3): "tpidrro_el0",
    (3, 3, 14, 0, 0): "cntfrq_el0",
    (3, 3, 14, 0, 2): "cntvct_el0",
    # CNTPCT_EL0 (physical count): same 19.2 MHz host tick as CNTVCT_EL0.
    (3, 3, 14, 0, 1): "cntvct_el0",
    (3, 3, 0, 0, 1): "ctr_el0",
    (3, 3, 0, 0, 7): "dczid_el0",
    # SCTLR_EL2 - seen in game code, stub for now
    (0, 3, 2, 0, 0): "sctlr_el2",
    # Nintendo Switch-specific system register, seen in game code, stub for now
    (0, 3, 3, 11, 5): "switch_sysreg_0_3_3_11_5",
}


class LiftError(Exception):
    pass


def _gpr(op) -> Reg | None:
    return op if isinstance(op, Reg) else None


class Lifter:
    """Lifts one function at a time from a code image."""

    def __init__(self, text: bytes, code_start: int = 0,
                 max_function_bytes: int = 0x2000,
                 noreturn: set[int] | None = None) -> None:
        self.text = text
        self.code_start = code_start
        self.max_function_bytes = max_function_bytes
        # Addresses of functions that never return (abort, exit, etc.).
        # A `bl` to one of these terminates the sweep; a `bl` to anything
        # else falls through to the next instruction.
        self.noreturn = noreturn or set()
        # br address -> case targets of a recognised switch table (module
        # offsets).  cfg_extent follows them, so the cases are blocks of the
        # function that owns the `br` and the br becomes a C switch.
        self.jump_tables: dict[int, tuple[int, ...]] = {}

    # -- helpers -------------------------------------------------------
    def _word(self, addr: int) -> int | None:
        if addr < 0 or addr + 4 > len(self.text) or addr % 4:
            return None
        return int.from_bytes(self.text[addr:addr + 4], "little")

    def read_insn(self, addr: int) -> Insn | Unsupported | None:
        word = self._word(addr)
        if word is None:
            return None
        return decode(addr, word)

    def _read_const(self, addr: int, size: int) -> int:
        if addr + size > len(self.text):
            raise LiftError("literal load at 0x%x outside .text" % addr)
        return int.from_bytes(self.text[addr:addr + size], "little")

    # -- function discovery -------------------------------------------
    def cfg_extent(self, start: int, boundary: int) -> tuple[int, list[Insn | Unsupported]]:
        """Collect every instruction reachable from ``start`` by intra-procedural
        control flow (fall-through, conditional branches, ``b``), staying inside
        ``[start, boundary)``.

        ``boundary`` is the next *known* function entry (a ``bl`` target,
        export, constructor or code pointer), so blocks the compiler placed
        after an unconditional ``b``/``ret`` are part of the function no matter
        how far away they are, while a ``b`` to another known entry stays a
        tail call.  The old sweep stopped at the first terminator and turned
        every later block into a separate overlapping "function"; on this game
        that translated each instruction ~5 times over.
        """
        limit = min(boundary, start + self.max_function_bytes)
        covered: dict[int, Insn | Unsupported] = {}
        work = [start]
        while work:
            addr = work.pop()
            while start <= addr < limit and addr not in covered:
                insn = self.read_insn(addr)
                if insn is None:
                    break
                covered[addr] = insn
                if isinstance(insn, Unsupported):
                    break
                m = insn.mnemonic
                if m.startswith("b.") or m in ("cbz", "cbnz", "tbz", "tbnz"):
                    t = insn.operands[-1]
                    if isinstance(t, int):
                        work.append(t)
                elif m == "b":
                    t = insn.operands[0]
                    if isinstance(t, int):
                        work.append(t)
                    break
                elif m == "bl":
                    if insn.operands[0] in self.noreturn:
                        break
                elif m == "br" and addr in self.jump_tables:
                    work.extend(self.jump_tables[addr])
                    break
                elif m in ("ret", "br", "udf"):
                    break
                addr += 4
        if not covered:
            return start, []
        return max(covered) + 4, [covered[a] for a in sorted(covered)]

    def function_extent(self, start: int,
                        allow_cold_blocks: bool = True) -> tuple[int, list[Insn | Unsupported]]:
        """Sweep the function, following branch targets into cold blocks.

        A compiler often places a rarely taken block (an assert or a slow path)
        *after* the function's `ret` or after an unconditional `b`.  Stopping at
        the first terminator truncated such functions, which turned their own
        conditional branches into inter-function dispatches that no function
        table can serve.  Instead, every direct branch target that stays near
        the current sweep is followed, and all decoded instructions are merged
        in address order.  A `b` whose target lies far away is a tail call and
        still ends the function.
        """
        limit = start + self.max_function_bytes
        covered: dict[int, Insn | Unsupported] = {}
        # (target, is_conditional_branch).  Conditional-branch targets may start
        # exactly at new_end (cold block after ret); call targets must start
        # strictly after new_end to avoid merging with the next function.
        candidates: list[tuple[int, bool]] = []
        hot_end = start

        for _round in range(4 if allow_cold_blocks else 1):
            # Sweep forward from hot_end until the next unconditional terminator.
            addr = hot_end
            while addr < limit and addr not in covered:
                insn = self.read_insn(addr)
                if insn is None:
                    break
                covered[addr] = insn
                if isinstance(insn, Unsupported):
                    addr += 4
                    break
                m = insn.mnemonic
                if m.startswith("b.") or m in ("cbz", "cbnz", "tbz", "tbnz"):
                    t = insn.operands[-1]
                    if isinstance(t, int) and t >= addr + 4:
                        candidates.append((t, True))  # conditional-branch target
                    addr += 4
                    continue
                if m in ("b", "bl"):
                    t = insn.operands[0]
                    # `b` is an unconditional branch: fall-through is never
                    # reached, so it ends the sweep (forward `b` within the
                    # cold-block window is absorbed as a candidate; far `b` is
                    # a tail call).  `bl` is a call: execution continues at
                    # addr+4 after the callee returns, so we do NOT stop here
                    # unless the target is a proven noreturn function (e.g.
                    # abort, exit) or the padding word 0xe7ffdefe (handled by
                    # the Unsupported break below).
                    if m == "b":
                        if isinstance(t, int) and t >= addr + 4:
                            candidates.append((t, False))  # call/b target
                        addr += 4
                        break
                    # m == "bl"
                    if isinstance(t, int) and t >= addr + 4:
                        candidates.append((t, False))  # call/b target
                    if t in self.noreturn:
                        # Noreturn call: fall-through is dead code.
                        addr += 4
                        break
                    # Normal call: continue sweeping past the call site.
                    addr += 4
                    continue
                if m in ("ret", "br", "udf"):
                    addr += 4
                    break
                addr += 4
            new_end = max(hot_end, addr)
            # Conditional-branch targets may start exactly at new_end (a cold
            # block after ret); call targets must start strictly after new_end
            # to avoid merging with the next function.
            cond_next = min((t for t, is_cond in candidates
                             if is_cond and new_end <= t <= new_end + 0x40
                             and t not in covered and (t >> 8) == (new_end >> 8)),
                            default=None)
            call_next = min((t for t, is_cond in candidates
                             if not is_cond and new_end + 4 <= t <= new_end + 0x40
                             and t not in covered and (t >> 8) == (new_end >> 8)),
                            default=None)
            # Prefer the conditional-branch cold block (more likely to be part
            # of this function); fall back to a call target strictly after.
            nxt = cond_next if cond_next is not None else call_next
            hot_end = new_end
            if nxt is None:
                break
            hot_end = nxt
        if not covered:
            return start, []
        insns = [covered[a] for a in sorted(covered)]
        return max(covered) + 4, insns

    # -- lifting -------------------------------------------------------
    def lift_function(self, start: int, name: str | None = None,
                      allow_cold_blocks: bool = True, boundary: int = 0) -> Function:
        if boundary:
            end, insns = self.cfg_extent(start, boundary)
        else:
            end, insns = self.function_extent(start, allow_cold_blocks)
        fn = Function(addr=start, name=name or ("sw_f_%08x" % start), size=end - start,
                      allow_cold_blocks=allow_cold_blocks, boundary=boundary)
        fn.instruction_count = sum(1 for i in insns if not isinstance(i, Unsupported))
        if len(insns) < 2 and insns and isinstance(insns[0], Unsupported):
            fn.unsupported.append((start, insns[0].word, insns[0].reason))

        addr_set = {i.addr for i in insns}
        targets: set[int] = {start}
        fallthroughs: set[int] = set()
        for insn in insns:
            if isinstance(insn, Unsupported):
                continue
            m = insn.mnemonic
            if m in ("b", "bl"):
                t = insn.operands[0]
                if t in addr_set:
                    targets.add(t)
            elif m.startswith("b.") or m in ("cbz", "cbnz", "tbz", "tbnz"):
                t = insn.operands[-1]
                if t in addr_set:
                    targets.add(t)
                fallthroughs.add(insn.addr + 4)
            elif m == "br" and insn.addr in self.jump_tables:
                targets.update(t for t in self.jump_tables[insn.addr] if t in addr_set)
        targets.update(fallthroughs)

        blocks: list[Block] = []
        current = Block(label="L_%08x" % start)
        blocks.append(current)
        fn.blocks.append(current)

        def new_block(addr: int) -> Block:
            nonlocal current
            blk = Block(label="L_%08x" % addr)
            blocks.append(blk)
            fn.blocks.append(blk)
            current = blk
            return blk

        temp_counter = 0

        def temp() -> str:
            nonlocal temp_counter
            temp_counter += 1
            return "t%d" % temp_counter

        cur_addr = start

        def emit(op: Op) -> None:
            if not op.addr:
                op.addr = cur_addr
            current.ops.append(op)

        def dst_of(reg: Reg) -> str:
            if reg.num == 31:
                return "sp" if reg.sp else "zr"
            return "x%d" % reg.num

        def flush_prefix(reg: Reg | None) -> bool:
            # Writes to xzr are discarded; flags still computed by callers.
            return reg is None or reg.num == 31

        def fall_out(prev) -> None:
            # The previous instruction falls through to code that is not part
            # of this function (cap reached, or the next function follows).
            # Continue there instead of dropping into the epilogue, which used
            # to make such functions silently return to their caller.
            if (prev is not None and not isinstance(prev, Unsupported)
                    and (current.terminator is None or current.terminator.kind == "fallthrough")
                    and prev.mnemonic not in ("ret", "b", "br", "udf")
                    and not (prev.mnemonic == "bl" and prev.operands[0] in self.noreturn)):
                fn.tail_calls.add(prev.addr + 4)
                current.terminator = Op("tail_call", imm=prev.addr + 4)

        prev_insn = None
        for insn in insns:
            cur_addr = insn.addr
            if prev_insn is not None and insn.addr != prev_insn.addr + 4:
                fall_out(prev_insn)
            prev_insn = insn
            # A target starts a block even when the previous block only held a
            # terminator (e.g. an unconditional `b`), which leaves it empty.
            if (insn.addr in targets and insn.addr != start
                    and current.label != "L_%08x" % insn.addr):
                new_block(insn.addr)
            if isinstance(insn, Unsupported):
                fn.unsupported.append((insn.addr, insn.word, insn.reason))
                emit(Op("trap", text="unsupported instruction 0x%08x at 0x%x: %s"
                        % (insn.word, insn.addr, insn.reason), imm=insn.addr))
                current.terminator = Op("ret")
                fn.blocks[-1].successors = []
                continue
            try:
                self._lift_insn(insn, fn, emit, temp, new_block, targets, start, end, current,
                                addr_set)
            except (KeyError, IndexError, TypeError) as exc:
                raise LiftError("internal lifter error at 0x%x (%s, word 0x%08x): %r"
                                % (insn.addr, insn, insn.word, exc)) from exc
            if insn.mnemonic in ("ret",):
                current.terminator = Op("ret")
            elif insn.mnemonic == "b":
                pass  # terminator set inside _lift_insn
            elif insn.mnemonic in ("br",):
                pass
            if insn.mnemonic in ("ret", "b", "br", "udf") and current.terminator is None:
                current.terminator = Op("ret")
        fall_out(prev_insn)

        # Link successors from terminators for later liveness passes.
        for blk in fn.blocks:
            term = blk.terminator
            if term is None:
                continue
            if term.kind == "jump":
                blk.successors = ["L_%08x" % term.imm]
            elif term.kind == "cond_jump":
                blk.successors = ["L_%08x" % term.imm]
            elif term.kind == "call":
                blk.successors = ["L_%08x" % term.imm] if False else []
        return fn

    def _lift_insn(self, insn: Insn, fn: Function, emit, temp, new_block, targets, start, end,
                   current, covered: set[int] | None = None) -> None:
        m = insn.mnemonic
        ops = insn.operands

        def inside(t: int) -> bool:
            """True when a branch target is an instruction of this function.

            The extent sweep follows cold blocks, so the covered set has holes
            where unrelated data sits between blocks; a range test alone would
            emit a jump to a label that was never materialised.
            """
            return (t in covered) if covered is not None else (start <= t < end)

        def put(op: Op) -> None:
            emit(op)

        def width_of(reg: Reg) -> int:
            return reg.width

        def set_reg(reg: Reg, value_op: Op) -> None:
            d = "sp" if (reg.num == 31 and reg.sp) else ("x%d" % reg.num if reg.num != 31 else "zr")
            value_op.dst = d
            if d == "zr":
                return
            put(value_op)

        def reg_name(reg: Reg) -> str:
            if reg.num == 31:
                return "sp" if reg.sp else "zr"
            return "x%d" % reg.num

        def read_reg(reg: Reg) -> str:
            if reg.num == 31:
                if reg.sp:
                    return "sp"
                return "0"
            return "x%d" % reg.num

        def mask(width: int) -> int:
            return 0xFFFFFFFFFFFFFFFF if width == 64 else 0xFFFFFFFF

        # --- PC relative ------------------------------------------------
        if m == "adr":
            rd, imm = ops
            put(Op("addr", dst=reg_name(rd), imm=(insn.addr + imm) & 0xFFFFFFFFFFFFFFFF))
            return
        if m == "adrp":
            rd, imm = ops
            put(Op("addr", dst=reg_name(rd), imm=((insn.addr & ~0xFFF) + imm) & 0xFFFFFFFFFFFFFFFF))
            return

        # --- add/sub immediate ------------------------------------------
        if m in ("add", "adds", "sub", "subs") and len(ops) == 3 and isinstance(ops[2], int):
            rd, rn, imm = ops
            width = width_of(rd)
            width = 64 if rd.num == 31 and rd.width == 64 else rd.width
            if rd.num == 31:
                width = 64 if rd.width == 64 else 32
            opname = "add"            if m.startswith("add") else "sub"
            if m.endswith("s"):
                t = temp()
                put(Op("bin", dst=t, a=read_reg(rn), b=imm, imm=width, args=(opname,)))
                put(Op("flags_add" if opname == "add" else "flags_sub",
                       a=read_reg(rn), b=imm, imm=width))
                if rd.num == 31:
                    if rd.sp:
                        put(Op("copy", dst="sp", a=t))
                else:
                    put(Op("copy", dst=("zr" if rd.num == 31 else "x%d" % rd.num), a=t))
                return
            put(Op("bin", dst=reg_name(rd) if reg_name(rd) != "sp" else "sp",
                   a=read_reg(rn), b=imm, imm=width, args=(opname,)))
            return

        # --- logical immediate ------------------------------------------
        if m in ("and", "orr", "eor", "ands") and len(ops) == 3 and isinstance(ops[2], int):
            rd, rn, imm = ops
            width = rd.width
            if m == "ands":
                t = temp()
                put(Op("bin", dst=t, a=read_reg(rn), b=imm, imm=width, args=("and",)))
                put(Op("flags_logical", a=t, imm=width))
                if rd.num != 31:
                    put(Op("copy", dst=("zr" if rd.num == 31 else "x%d" % rd.num), a=t))
                elif rd.sp:
                    put(Op("copy", dst="sp", a=t))
                return
            put(Op("bin", dst=reg_name(rd), a=read_reg(rn), b=imm, imm=width, args=(m,)))
            if reg_name(rd) == "zr":
                current.ops.pop()
            return

        # --- move wide ---------------------------------------------------
        if m in ("movn", "movz", "movk") and len(ops) == 3:
            rd, imm16, shift = ops
            width = rd.width
            if m == "movz":
                put(Op("const", dst=reg_name(rd), imm=(imm16 << shift) & mask(width)))
            elif m == "movn":
                put(Op("const", dst=reg_name(rd), imm=(~(imm16 << shift)) & mask(width)))
            else:
                put(Op("movk", dst=reg_name(rd), a=reg_name(rd), b=imm16, imm=shift, args=(width,)))
            return

        # --- bitfield ----------------------------------------------------
        if m in ("sbfm", "bfm", "ubfm") and len(ops) == 4:
            rd, rn, immr, imms = ops
            width = rd.width
            put(Op("bitfield", dst=reg_name(rd), a=read_reg(rn), imm=imms,
                   b=immr, c=reg_name(rd), args=(m, width)))
            return

        # --- extract -----------------------------------------------------
        if m == "extr" and len(ops) == 4:
            rd, rn, rm, lsb = ops
            width = rd.width
            put(Op("extr", dst=reg_name(rd), a=read_reg(rn), b=read_reg(rm), imm=lsb, args=(width,)))
            return

        # --- add/sub shifted or extended --------------------------------
        if m in ("add", "adds", "sub", "subs") and len(ops) >= 4 and not isinstance(ops[2], int):
            rd, rn, rm = ops[0], ops[1], ops[2]
            width = rd.width
            if insn.notes.get("extended"):
                option = insn.notes["option"]
                amount = insn.notes["shift_amount"]
                t = temp()
                put(Op("extend", dst=t, a=read_reg(rm), imm=amount, args=(option, width_of(rm))))
                operand = t
            else:
                shift = insn.notes["shift"]
                amount = insn.notes["shift_amount"]
                if amount == 0:
                    operand = read_reg(rm)
                else:
                    t = temp()
                    put(Op("bin", dst=t, a=read_reg(rm), b=amount, imm=width, args=(shift,)))
                    operand = t
            opname = "add" if m.startswith("add") else "sub"
            if m.endswith("s"):
                t = temp()
                put(Op("bin", dst=t, a=read_reg(rn), b=operand, imm=width, args=(opname,)))
                put(Op("flags_add" if opname == "add" else "flags_sub", a=read_reg(rn), b=operand, imm=width))
                if rd.num == 31:
                    return
                if rd.num == 31:
                    if rd.sp:
                        put(Op("copy", dst="sp", a=t))
                else:
                    put(Op("copy", dst=("zr" if rd.num == 31 else "x%d" % rd.num), a=t))
                return
            put(Op("bin", dst=reg_name(rd), a=read_reg(rn), b=operand, imm=width, args=(opname,)))
            if reg_name(rd) == "zr":
                current.ops.pop()
            return

        # --- logical shifted ---------------------------------------------
        if m in ("and", "orr", "eor", "bic", "orn", "eon", "ands", "bics") and len(ops) == 5:
            rd, rn, rm = ops[0], ops[1], ops[2]
            width = rd.width
            shift = ops[3]
            amount = ops[4]
            if amount:
                t = temp()
                put(Op("bin", dst=t, a=read_reg(rm), b=amount, imm=width, args=(shift,)))
                operand = t
            else:
                operand = read_reg(rm)
            base = {"bic": "and", "orn": "orr", "eon": "eor", "bics": "ands"}.get(m, m)
            negated = m in ("bic", "orn", "eon", "bics")
            if negated:
                n = temp()
                put(Op("not", dst=n, a=operand, imm=width))
                operand = n
            if base == "ands":
                t = temp()
                put(Op("bin", dst=t, a=read_reg(rn), b=operand, imm=width, args=("and",)))
                put(Op("flags_logical", a=t, imm=width))
                if rd.num != 31:
                    put(Op("copy", dst=("zr" if rd.num == 31 else "x%d" % rd.num), a=t))
                elif rd.sp:
                    put(Op("copy", dst="sp", a=t))
                return
            put(Op("bin", dst=reg_name(rd), a=read_reg(rn), b=operand, imm=width, args=(base,)))
            if reg_name(rd) == "zr":
                current.ops.pop()
            return

        # --- conditional compare ------------------------------------------
        if m in ("ccmp", "ccmn"):
            rn, operand, _cond_name, nzcv = ops
            width = insn.notes["width"]
            if isinstance(operand, Reg):
                opnd = read_reg(operand)
            else:
                opnd = operand
            put(Op("ccmp", a=read_reg(rn), b=opnd, imm=insn.notes["cond"],
                   args=(m, width, nzcv, insn.notes["immediate"])))
            return

        # --- 1-source integer (RBIT/REV*/CLZ/CLS) -------------------------
        if m in ("rbit", "rev16", "rev32", "rev", "clz", "cls") and len(ops) == 2:
            rd, rn = ops
            if reg_name(rd) == "zr":
                return
            put(Op("unop", dst=reg_name(rd), a=read_reg(rn), imm=rd.width, args=(m,)))
            return

        # --- conditional select -------------------------------------------
        if m in ("csel", "csinc", "csinv", "csneg"):
            rd, rn, rm, cond = ops
            put(Op("sel", dst=reg_name(rd), a=read_reg(rn), b=read_reg(rm), imm=insn.notes["cond"],
                   args=(m,), c=rd.width))
            return

        # --- 2-source ------------------------------------------------------
        if m in ("udiv", "sdiv", "lsl", "lsr", "asr", "ror") and len(ops) == 3:
            rd, rn, rm = ops
            width = rd.width
            put(Op("bin", dst=reg_name(rd), a=read_reg(rn), b=read_reg(rm), imm=width, args=(m,)))
            return

        # --- 3-source ------------------------------------------------------
        if m in ("madd", "msub", "smaddl", "smsubl", "umaddl", "umsubl", "smulh", "umulh"):
            if m in ("smulh", "umulh"):
                rd, rn, rm = ops
                put(Op("bin", dst=reg_name(rd), a=read_reg(rn), b=read_reg(rm), imm=64, args=(m,)))
                return
            rd, rn, rm, ra = ops
            width = rd.width
            put(Op("muladd", dst=reg_name(rd), a=read_reg(rn), b=read_reg(rm), c=read_reg(ra),
                   imm=width, args=(m,)))
            return

        # --- branches -------------------------------------------------------
        if m == "b":
            target = ops[0]
            if inside(target):
                current.terminator = Op("jump", imm=target)
                fn.tail_calls.discard(target)
            else:
                fn.tail_calls.add(target)
                current.terminator = Op("tail_call", imm=target)
            return
        if m == "bl":
            target = ops[0]
            fn.calls.add(target)
            # BL writes the return address to x30.  Guest code that stores LR
            # as data (setjmp, backtraces, Lua's error handling) reads it;
            # without this setjmp saved a stale x30 and longjmp "returned" to 0.
            put(Op("addr", dst="x30", imm=(insn.addr + 4) & 0xFFFFFFFFFFFFFFFF))
            put(Op("call", imm=target))
            return
        if m.startswith("b."):
            target = ops[0]
            fn.calls.discard(target)
            if inside(target):
                put(Op("cond_jump", imm=insn.notes["cond"], a="L_%08x" % target))
                current.terminator = Op("fallthrough")
            else:
                fn.tail_calls.add(target)
                put(Op("cond_tail_call", imm=insn.notes["cond"], a=target))
            return
        if m in ("cbz", "cbnz"):
            rt, target = ops
            negate = m == "cbnz"
            if inside(target):
                put(Op("cond_jump_zero", a=read_reg(rt), imm=(1 if negate else 0), args=(rt.width,),
                       text="L_%08x" % target))
                current.terminator = Op("fallthrough")
            else:
                fn.tail_calls.add(target)
                put(Op("cond_tail_call_zero", a=read_reg(rt), imm=(1 if negate else 0),
                       b=target, args=(rt.width,)))
            return
        if m in ("tbz", "tbnz"):
            rt, bit, target = ops
            if inside(target):
                put(Op("cond_jump_bit", a=read_reg(rt), imm=bit, args=(m == "tbnz",),
                       text="L_%08x" % target))
                current.terminator = Op("fallthrough")
            else:
                fn.tail_calls.add(target)
                put(Op("cond_tail_call_bit", a=read_reg(rt), imm=bit, b=target, args=(m == "tbnz",)))
            return
        if m == "ret":
            put(Op("ret"))
            return
        if m == "br":
            fn.indirect += 1
            local = sorted({t for t in self.jump_tables.get(insn.addr, ()) if inside(t)})
            if local:
                # switch table: cases inside this function become gotos; any other
                # value still dispatches (sw_dispatch), so a missed case is safe
                fn.jt_local.update(local)
                put(Op("jump_table", a=read_reg(ops[0]), args=local))
                current.terminator = Op("jump_table")
                return
            put(Op("jump_indirect", a=read_reg(ops[0])))
            current.terminator = Op("jump_indirect")
            return
        if m == "blr":
            fn.indirect += 1
            target_reg = read_reg(ops[0])
            if target_reg != "x30":       # BLR x30 reads the old value: leave x30 alone there
                put(Op("addr", dst="x30", imm=(insn.addr + 4) & 0xFFFFFFFFFFFFFFFF))
            put(Op("call_indirect", a=target_reg))
            return

        # --- load/store ------------------------------------------------------
        if m in ("ldr", "str", "ldrs") and (len(ops) >= 3 or insn.notes.get("literal")):
            self._lift_ldst(insn, put, temp, read_reg, set_reg, fn)
            return
        if m in ("ldp", "stp"):
            self._lift_ldst(insn, put, temp, read_reg, set_reg, fn)
            return
        if m in ("ldxr", "stxr", "ldaxr", "stlxr", "ldar", "stlr"):
            self._lift_exclusive(insn, put, temp, read_reg)
            return

        # --- system -----------------------------------------------------------
        if m == "svc":
            put(Op("svc", imm=ops[0]))
            return
        if m == "mrs":
            rt, sysreg = ops
            name = SYSREG_NAMES.get(tuple(sysreg))
            if name is None:
                put(Op("trap", text="unsupported system register %s at 0x%x" % (sysreg, insn.addr),
                       imm=insn.addr))
                return
            put(Op("mrs", dst=reg_name(rt), args=(name,)))
            return
        if m in ("msr", "mrs") and tuple(ops[0])[0] == 0:
            # op0 == 0 is not a system register: it is the hint (CRn=2),
            # barrier/CLREX (CRn=3, op2 2=clrex 4=dsb 5=dmb 6=isb) and PSTATE
            # (CRn=4) space, which the decoder hands over as msr.  CLREX in
            # nnosInitialize used to trap as "unsupported system register".
            _op0, _op1, crn, _crm, op2 = tuple(ops[0])
            if crn == 3:
                put(Op("barrier", args=({2: "clrex", 4: "dsb", 5: "dmb", 6: "isb"}.get(op2, "dmb"),)))
            else:
                put(Op("nop"))
            return
        if m == "msr":
            sysreg, rt = ops
            name = SYSREG_NAMES.get(tuple(sysreg))
            if name is None:
                put(Op("trap", text="unsupported system register write %s at 0x%x" % (sysreg, insn.addr),
                       imm=insn.addr))
                return
            put(Op("msr", a=read_reg(rt), args=(name,)))
            return
        if m in ("nop", "yield", "wfe", "wfi", "sev", "sevl", "hint", "esb", "csdb"):
            put(Op("nop"))
            return
        if m in ("dsb", "dmb", "isb", "clrex"):
            put(Op("barrier", args=(m,)))
            return
        if m == "udf":
            put(Op("trap", text="undefined instruction (udf) at 0x%x" % insn.addr, imm=insn.addr))
            return

        # --- floating point ----------------------------------------------------
        if m in ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"):
            rd, rn, rm = ops
            width = rd.width
            put(Op("fpop", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=(m, width)))
            return
        if m in ("fmov", "fabs", "fneg", "fsqrt", "fcvt", "frintn", "frintp", "frintm",
                 "frintz", "frinta", "frintx", "frinti"):
            if len(ops) == 2 and isinstance(ops[0], VReg) and isinstance(ops[1], VReg):
                rd, rn = ops
                if m == "fmov":
                    put(Op("vcopy", dst="v%d" % rd.num, a="v%d" % rn.num, imm=rd.width))
                elif m == "fcvt":
                    put(Op("fcvt", dst="v%d" % rd.num, a="v%d" % rn.num, args=(rd.width, rn.width)))
                else:
                    put(Op("funop", dst="v%d" % rd.num, a="v%d" % rn.num, args=(m, rd.width)))
                return
            if len(ops) == 2 and isinstance(ops[1], tuple) and ops[1][0] == "fpimm":
                rd, (_tag, imm8, width) = ops
                put(Op("fmov_imm", dst="v%d" % rd.num, imm=imm8, args=(width,)))
                return
            if len(ops) == 2 and isinstance(ops[0], Reg):
                # fmov between GPR and FP register
                gpr, vec = ops
                if isinstance(vec, VReg):
                    put(Op("fmov_to_gpr", dst=reg_name(gpr), a="v%d" % vec.num, args=(vec.width,)))
                    return
                put(Op("fmov_from_gpr", dst="v%d" % vec.num, a=read_reg(gpr), args=(vec.width,)))
                return
            if len(ops) == 2 and isinstance(ops[0], VReg):
                vec, gpr = ops      # fmov Vd, Xn (names were swapped: wrote Xn's number)
                put(Op("fmov_from_gpr", dst="v%d" % vec.num, a=read_reg(gpr), args=(vec.width,)))
                return
            put(Op("trap", text="unsupported fp form %s at 0x%x" % (m, insn.addr), imm=insn.addr))
            return
        if m in ("fccmp", "fccmpe"):
            rn, rm = ops
            put(Op("fccmp", a="v%d" % rn.num, b="v%d" % rm.num, imm=insn.notes["cond"],
                   args=(rn.width, insn.notes["nzcv"])))
            return
        if m == "fcsel":
            rd, rn, rm = ops
            put(Op("fcsel", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   imm=insn.notes["cond"], args=(rd.width,)))
            return
        if m in ("aese", "aesd"):
            rd, rn = ops
            put(Op("vcrypto", dst="v%d" % rd.num, a="v%d" % rn.num, args=(m, 2)))
            return
        if m in ("aesmc", "aesimc"):
            rd, rn = ops
            put(Op("vcrypto", dst="v%d" % rd.num, a="v%d" % rn.num, args=(m, 1)))
            return
        if m in ("ldn_lane", "stn_lane", "ldnr"):
            # n structures: element i of the memory sequence goes to V(t+i)
            vt, rn = ops
            nreg = insn.notes["n"]
            ebits = insn.notes["lane_bits"]
            nbytes = ebits // 8
            base = temp()
            put(Op("copy", dst=base, a=read_reg(rn)))
            for i in range(nreg):
                vreg = "v%d" % ((vt.num + i) % 32)
                ea = temp()
                put(Op("bin", dst=ea, a=base, b=i * nbytes, imm=64, args=("add",)))
                val = temp()
                if m == "ldn_lane":
                    put(Op("load", dst=val, a=ea, imm=nbytes, args=("u", 64)))
                    put(Op("vgpr_to_lane", dst=vreg, a=val, imm=ebits, b=insn.notes["index"]))
                elif m == "stn_lane":
                    put(Op("vlane_to_gpr", dst=val, a=vreg, imm=ebits, b=insn.notes["index"], args=(False,)))
                    put(Op("store", a=ea, b=val, imm=nbytes))
                else:
                    put(Op("load", dst=val, a=ea, imm=nbytes, args=("u", 64)))
                    put(Op("vdup_gpr", dst=vreg, a=val, imm=ebits))
                    self._simd_q(insn, vreg, put)
            if insn.notes["post"]:
                rmn = insn.notes["rm"]
                step = temp()
                if rmn == 31:
                    put(Op("bin", dst=step, a=base, b=nreg * nbytes, imm=64, args=("add",)))
                else:
                    put(Op("bin", dst=step, a=base, b="x%d" % rmn, imm=64, args=("add",)))
                put(Op("copy", dst=("sp" if rn.num == 31 else "x%d" % rn.num), a=step))
            return
        if m in ("ld1_lane", "st1_lane", "ld1r"):
            vt, rn = ops
            nbytes = insn.notes["lane_bits"] // 8
            base = temp()
            put(Op("copy", dst=base, a=read_reg(rn)))
            val = temp()
            if m == "ld1_lane":
                put(Op("load", dst=val, a=base, imm=nbytes, args=("u", 64)))
                put(Op("vgpr_to_lane", dst="v%d" % vt.num, a=val, imm=insn.notes["lane_bits"],
                       b=insn.notes["index"]))
            elif m == "st1_lane":
                put(Op("vlane_to_gpr", dst=val, a="v%d" % vt.num, imm=insn.notes["lane_bits"],
                       b=insn.notes["index"], args=(False,)))
                put(Op("store", a=base, b=val, imm=nbytes))
            else:
                put(Op("load", dst=val, a=base, imm=nbytes, args=("u", 64)))
                put(Op("vdup_gpr", dst="v%d" % vt.num, a=val, imm=insn.notes["lane_bits"]))
                self._simd_q(insn, "v%d" % vt.num, put)
            if insn.notes["post"]:
                rmn = insn.notes["rm"]
                step = temp()
                if rmn == 31:
                    put(Op("bin", dst=step, a=base, b=nbytes, imm=64, args=("add",)))
                else:
                    put(Op("bin", dst=step, a=base, b="x%d" % rmn, imm=64, args=("add",)))
                put(Op("copy", dst=("sp" if rn.num == 31 else "x%d" % rn.num), a=step))
            return
        if m in ("sha1c", "sha1p", "sha1m", "sha1su0", "sha256h", "sha256h2", "sha256su1"):
            rd, rn, rm = ops
            put(Op("vcrypto", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num, args=(m, 3)))
            return
        if m in ("sha1h", "sha1su1", "sha256su0"):
            rd, rn = ops
            put(Op("vcrypto", dst="v%d" % rd.num, a="v%d" % rn.num, args=(m, 2)))
            return
        if m in ("ldxp", "ldaxp", "stxp", "stlxp"):
            # Exclusive pair: the first load arms the monitor, the second
            # records the high value; the store compares and swaps both at once
            # (sw_excl_store2_N).
            nbytes = insn.notes["bytes"]
            rn = ops[-1]
            base = temp()
            put(Op("copy", dst=base, a=read_reg(rn)))
            hi = temp()
            put(Op("bin", dst=hi, a=base, b=nbytes, imm=64, args=("add",)))
            if m.startswith("ld"):
                rt, rt2 = ops[0], ops[1]
                put(Op("excl_load", dst=("zr" if rt.num == 31 else "x%d" % rt.num), a=base, imm=nbytes))
                put(Op("excl_load_hi", dst=("zr" if rt2.num == 31 else "x%d" % rt2.num), a=hi, imm=nbytes))
            else:
                rs, rt, rt2 = ops[0], ops[1], ops[2]
                put(Op("excl_store2", dst=("zr" if rs.num == 31 else "x%d" % rs.num), a=base,
                       b=("zr" if rt.num == 31 else "x%d" % rt.num),
                       c=("zr" if rt2.num == 31 else "x%d" % rt2.num), imm=nbytes))
            return
        if m in ("adc", "adcs", "sbc", "sbcs"):
            rd, rn, rm = ops
            put(Op("adc", dst=reg_name(rd), a=read_reg(rn), b=read_reg(rm), imm=rd.width,
                   args=(m.startswith("sbc"), m.endswith("s"))))
            return
        if m in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
            rd, rn, rm, ra = ops
            put(Op("fmadd", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   c="v%d" % ra.num, args=(m, rd.width)))
            return
        if m in ("fcmp", "fcmpe"):
            rn, rm = ops
            put(Op("fpcmp", a="v%d" % rn.num, b=(None if insn.notes.get("zero") else "v%d" % rm.num),
                   args=(rn.width,)))
            return
        if m in ("scvtf", "ucvtf"):
            rd, rn = ops
            put(Op("int_to_fp", dst="v%d" % rd.num, a=read_reg(rn), imm=rd.width,
                   args=(rn.width, m == "ucvtf", insn.notes.get("fbits", 0))))
            return
        if m in ("fcvtzs", "fcvtzu", "fcvtns", "fcvtnu", "fcvtps", "fcvtpu",
                 "fcvtms", "fcvtmu", "fcvtas", "fcvtau"):
            rd, rn = ops
            put(Op("fp_to_int", dst=reg_name(rd), a="v%d" % rn.num, imm=rd.width,
                   args=(rn.width, m.endswith("s"), m[4], insn.notes.get("fbits", 0))))
            return
        if m == "prfm":
            put(Op("nop"))
            return
        if m.startswith("crc32"):
            rd, rn, rm = ops
            put(Op("crc32", dst=reg_name(rd), a=read_reg(rn), b=read_reg(rm),
                   args=(insn.notes["bytes"], insn.notes["castagnoli"])))
            return
        if m.endswith("_multi"):
            self._lift_ldst_multi(insn, put, temp, read_reg)
            return

        # --- SIMD ---------------------------------------------------------------
        if m.startswith("v") or m in ("umov", "smov"):
            self._lift_simd(insn, put, temp, read_reg)
            return

        put(Op("trap", text="unlifted instruction %s at 0x%x (0x%08x)"
               % (m, insn.addr, insn.word), imm=insn.addr))

    # -- load/store lifting -------------------------------------------------
    def _lift_ldst(self, insn: Insn, put, temp, read_reg, set_reg, fn) -> None:
        m = insn.mnemonic
        ops = insn.operands
        notes = insn.notes
        nbytes = notes["bytes"]
        is_vec = notes.get("vector", False)
        signed = notes.get("signed", False)
        mode = notes.get("addressing", "offset")

        if notes.get("literal"):
            rt, target = ops
            addr = temp()
            put(Op("addr", dst=addr, imm=target & 0xFFFFFFFFFFFFFFFF))
            if is_vec:
                put(Op("loadv", dst="v%d" % rt.num, a=addr, imm=nbytes))
            else:
                put(Op("load", dst="x%d" % rt.num if rt.num != 31 else "zr", a=addr, imm=nbytes,
                       args=("s" if signed else "u", rt.width)))
            return

        if m in ("ldp", "stp"):
            rt, rt2, rn = ops[0], ops[1], ops[2]
            disp = ops[3]
            base = read_reg(rn)
            addr = temp()
            if notes.get("addressing") == "post":
                put(Op("copy", dst=addr, a=base))
            else:
                put(Op("bin", dst=addr, a=base, b=disp, imm=64, args=("add",)))
            def g(name_reg):
                return "zr" if name_reg.num == 31 else "x%d" % name_reg.num

            if m == "ldp":
                if is_vec:
                    put(Op("loadv", dst="v%d" % rt.num, a=addr, imm=nbytes))
                    a2 = temp()
                    put(Op("bin", dst=a2, a=addr, b=nbytes, imm=64, args=("add",)))
                    put(Op("loadv", dst="v%d" % rt2.num, a=a2, imm=nbytes))
                else:
                    sg = "s" if notes.get("signed") else "u"      # LDPSW
                    put(Op("load", dst=g(rt), a=addr, imm=nbytes, args=(sg, rt.width)))
                    a2 = temp()
                    put(Op("bin", dst=a2, a=addr, b=nbytes, imm=64, args=("add",)))
                    put(Op("load", dst=g(rt2), a=a2, imm=nbytes, args=(sg, rt2.width)))
            else:
                if is_vec:
                    put(Op("storev", a=addr, b="v%d" % rt.num, imm=nbytes))
                    a2 = temp()
                    put(Op("bin", dst=a2, a=addr, b=nbytes, imm=64, args=("add",)))
                    put(Op("storev", a=a2, b="v%d" % rt2.num, imm=nbytes))
                else:
                    put(Op("store", a=addr, b=g(rt), imm=nbytes))
                    a2 = temp()
                    put(Op("bin", dst=a2, a=addr, b=nbytes, imm=64, args=("add",)))
                    put(Op("store", a=a2, b=g(rt2), imm=nbytes))
            if notes.get("writeback"):
                nb = temp()
                put(Op("bin", dst=nb, a=base, b=disp, imm=64, args=("add",)))
                set_reg(rn, Op("copy", a=nb))
            return

        # single register, three operand forms
        rt, rn = ops[0], ops[1]
        base = read_reg(rn)
        if mode == "register":
            rm, option, scale = ops[2], ops[3], ops[4]
            ext = temp()
            put(Op("extend", dst=ext, a=read_reg(rm), imm=(scale and (nbytes.bit_length() - 1) or 0),
                   args=(option, rm.width)))
            addr = temp()
            put(Op("bin", dst=addr, a=base, b=ext, imm=64, args=("add",)))
        else:
            disp = ops[2]
            addr = temp()
            # Post-index accesses use the un-updated base and add the
            # displacement afterwards; pre-index and offset forms use
            # base + displacement for the access itself.
            access_base = base
            if mode == "post":
                put(Op("copy", dst=addr, a=access_base))
            else:
                put(Op("bin", dst=addr, a=access_base,
                       b=disp if disp >= 0 else (disp & 0xFFFFFFFFFFFFFFFF),
                       imm=64, args=("add",)))
        if m == "ldr" or m == "ldrs":
            if is_vec:
                put(Op("loadv", dst="v%d" % rt.num, a=addr, imm=nbytes))
            else:
                put(Op("load", dst="x%d" % rt.num if rt.num != 31 else "zr", a=addr, imm=nbytes,
                       args=("s" if signed else "u", rt.width)))
        else:
            if is_vec:
                put(Op("storev", a=addr, b="v%d" % rt.num, imm=nbytes))
            else:
                put(Op("store", a=addr, b="x%d" % rt.num if rt.num != 31 else "zr", imm=nbytes))
        if mode in ("pre", "post"):
            nb = temp()
            if mode == "pre":
                put(Op("bin", dst=nb, a=base, b=ops[2] & 0xFFFFFFFFFFFFFFFF, imm=64, args=("add",)))
            else:
                put(Op("bin", dst=nb, a=base, b=ops[2] & 0xFFFFFFFFFFFFFFFF, imm=64, args=("add",)))
            set_reg(rn, Op("copy", a=nb))

    def _lift_exclusive(self, insn: Insn, put, temp, read_reg) -> None:
        m = insn.mnemonic
        nbytes = insn.notes["bytes"]
        if m == "ldar":
            rt, rn = insn.operands
            addr = temp()
            put(Op("copy", dst=addr, a=read_reg(rn)))
            put(Op("load", dst=("zr" if rt.num == 31 else "x%d" % rt.num), a=addr, imm=nbytes,
                       args=("u", rt.width)))
            return
        if m in ("ldxr", "ldaxr"):
            # Exclusive load: arms the monitor in SwCpu (sw_excl_loadN, sw_rt.h).
            rt, rn = insn.operands
            addr = temp()
            put(Op("copy", dst=addr, a=read_reg(rn)))
            put(Op("excl_load", dst=("zr" if rt.num == 31 else "x%d" % rt.num), a=addr, imm=nbytes))
            return
        if m in ("stxr", "stlxr"):
            # Exclusive store: compare-and-swap against the value the exclusive
            # load read; status 0 = stored, 1 = retry (guest threads may run on
            # several host cores).
            rs, rt, rn = insn.operands
            addr = temp()
            put(Op("copy", dst=addr, a=read_reg(rn)))
            put(Op("excl_store", dst=("zr" if rs.num == 31 else "x%d" % rs.num), a=addr,
                   b=("zr" if rt.num == 31 else "x%d" % rt.num), imm=nbytes))
            return
        if m == "stlr":
            rt, rn = insn.operands
            addr = temp()
            put(Op("copy", dst=addr, a=read_reg(rn)))
            put(Op("store", a=addr, b=("zr" if rt.num == 31 else "x%d" % rt.num), imm=nbytes))
            put(Op("barrier", args=("stlr",)))
            return

    # -- SIMD lifting --------------------------------------------------------
    def _lift_simd(self, insn: Insn, put, temp, read_reg) -> None:
        # Decoder mnemonics for SIMD carry a "v" prefix (vadd, vins_elem, ...);
        # umov/smov do not.  Compare against the stripped name throughout - several
        # checks used the prefixed name and could never match.
        name = insn.mnemonic
        m = name[1:] if name.startswith("v") else name
        ops = insn.operands
        if name == "vfabd_scalar":
            # |n - m| on lane 0: the vector helper over the low 64 bits, then a
            # scalar-width copy that keeps lane 0 and zeroes the rest
            rd, rn, rm = ("v%d" % o.num for o in ops)
            esz = insn.notes["esz"]
            put(Op("vcall", dst=rd, a=rn, b=rm, c=rd, imm=0, args=("sw6_fabd", 0, esz)))
            put(Op("vcopy", dst=rd, a=rd, imm=esz))
            return
        if "helper" in insn.notes:
            # batch 6: Vd = helper(Vd, Vn, Vm, imm, q, esz); helpers zero what
            # the Arm ARM zeroes (upper half for Q=0, all but lane 0 for scalars)
            rd = "v%d" % ops[0].num
            rn = "v%d" % ops[1].num
            rm = "v%d" % ops[2].num if len(ops) > 2 else rd
            n = insn.notes
            put(Op("vcall", dst=rd, a=rn, b=rm, c=rd, imm=n["imm"],
                   args=(n["helper"], n["q"], n["esz"])))
            return
        if m == "dup" and len(ops) == 2 and isinstance(ops[1], Reg):
            rd, rn = ops
            lane_bits = insn.notes["lane_bits"]
            put(Op("vdup_gpr", dst="v%d" % rd.num, a=read_reg(rn), imm=lane_bits))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m == "dup_elem":
            rd, rn = ops
            lane_bits = insn.notes["lane_bits"]
            put(Op("vdup_lane", dst="v%d" % rd.num, a="v%d" % rn.num, imm=lane_bits,
                   b=rn.lane, args=(insn.notes["lanes"],)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m in ("umov", "smov"):
            rd, rn = ops
            put(Op("vlane_to_gpr", dst="x%d" % rd.num if rd.num != 31 else "zr",
                   a="v%d" % rn.num, imm=insn.notes["lane_bits"],
                   b=rn.lane, args=(m == "smov",)))
            return
        if m == "ins_general":
            rd, rn = ops
            put(Op("vgpr_to_lane", dst="v%d" % rd.num, a=read_reg(rn),
                   imm=insn.notes["lane_bits"], b=insn.notes["lane"]))
            return
        if m == "ins_elem":
            rd, rn = ops
            # Vd.T[dst_lane] = Vn.T[src_lane]; other lanes unchanged
            put(Op("vlane_ins", dst="v%d" % rd.num, a="v%d" % rn.num,
                   imm=insn.notes["lane_bits"], b=rn.lane, c=insn.notes["lane"]))
            return
        if m in ("and", "orr", "eor", "bic", "orn", "bsl", "bit", "bif"):
            rd, rn, rm = ops
            if m == "orr" and rn.num == rm.num:
                # MOV Vd.<T>, Vn.<T> is ORR with the same source twice.
                put(Op("vmov", dst="v%d" % rd.num, a="v%d" % rn.num))
            elif m in ("bsl", "bit", "bif"):
                # Three-operand bitwise select: destination is also a source.
                put(Op("vbit", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                       args=(m,)))
            else:
                put(Op("vop", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                       args=(m, 8, 16)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m in ("fmla", "fmls", "mla", "mls"):
            # Accumulating ops: rd is also an input (rd += rn * rm).
            rd, rn, rm = ops
            put(Op("vmla", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=(m, insn.notes["lane_bits"])))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m in ("fmul_elem", "fmla_elem", "fmls_elem"):
            rd, rn, rm = ops
            put(Op("velem", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=(m, insn.notes["lane_bits"], insn.notes["index"], insn.notes["lanes"])))
            return
        if m == "addv":
            rd, rn = ops
            put(Op("vreduce", dst="v%d" % rd.num, a="v%d" % rn.num,
                   args=("addv", insn.notes["lane_bits"], insn.notes["lanes"])))
            return
        if m == "dup_scalar":
            rd, rn = ops
            put(Op("vdup_scalar", dst="v%d" % rd.num, a="v%d" % rn.num,
                   imm=insn.notes["lane_bits"], b=insn.notes["index"]))
            return
        if m.startswith("f2_"):
            rd, rn = ops
            put(Op("vf2", dst="v%d" % rd.num, a="v%d" % rn.num,
                   args=(m[3:], insn.notes["lane_bits"], insn.notes["lanes"])))
            return
        if m == "tbl":
            rd, rn, rm = ops
            n = insn.notes["nregs"]
            tab = tuple("v%d" % ((rn.num + i) % 32) for i in range(n))
            put(Op("vtbl", dst="v%d" % rd.num, a="v%d" % rm.num, args=(tab, insn.notes["q"])))
            return
        if m == "faddp":
            rd, rn, rm = ops
            put(Op("vfpair", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=(m, insn.notes["lane_bits"], insn.notes["lanes"])))
            return
        if m in ("fmaxp", "fminp"):
            # pairwise FP max/min (used to return without emitting anything)
            rd, rn, rm = ops
            put(Op("vfpair", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=(m, insn.notes["lane_bits"], insn.notes["lanes"])))
            return
        if m in ("add", "sub", "mul", "smax", "smin", "umax", "umin",
                 "cmgt", "cmge", "cmhi", "cmhs", "cmeq", "cmtst", "addp", "shadd", "uhadd",
                 "sabd", "uabd", "sshl", "ushl", "fadd", "fsub", "fmul", "fdiv",
                 "fmax", "fmin", "fcmeq", "fcmge", "fcmgt", "frecps", "frsqrts"):
            rd, rn, rm = ops
            lane_bits = insn.notes["lane_bits"]
            lanes = insn.notes["lanes"]
            put(Op("vop", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=(m, lane_bits, lanes)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m in ("abs", "neg", "not", "mvn", "cnt", "clz", "cls", "rev64", "rev32", "rev16",
                 "xtn", "frintn", "frintm", "frintp", "frintz", "frinta"):
            rd, rn = ops
            lane_bits = insn.notes.get("lane_bits") or 0
            put(Op("vunop", dst="v%d" % rd.num, a="v%d" % rn.num, args=(m, lane_bits)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m in ("scvtf", "ucvtf", "fcvtzs", "fcvtzu"):
            rd, rn = ops
            lane_bits = insn.notes.get("lane_bits") or 32
            put(Op("vintfp", dst="v%d" % rd.num, a="v%d" % rn.num,
                   args=(m, lane_bits, insn.notes.get("lanes", 0))))
            return
        if m == "ext":
            rd, rn, rm, index = ops
            put(Op("vperm", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=("ext", 8, insn.notes["lanes"], index)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m == "fmovi":
            rd = ops[0]
            put(Op("vmovi", dst="v%d" % rd.num, imm=ops[1], args=(ops[2],)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m == "movi":
            rd = ops[0]
            value = ops[1]
            lane_bits = insn.notes.get("lane_bits", 16)
            put(Op("vmovi", dst="v%d" % rd.num, imm=value, args=(lane_bits,)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m in ("orri", "bici"):
            rd = ops[0]
            put(Op("vlogic_imm", dst="v%d" % rd.num, imm=ops[1],
                   args=("orr" if m == "orri" else "bic", insn.notes["lane_bits"])))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m == "mvni":
            rd = ops[0]
            value = ops[1]
            lane_bits = insn.notes.get("lane_bits", 16)
            put(Op("vmvni", dst="v%d" % rd.num, imm=value, args=(lane_bits,)))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        if m in ("zip1", "zip2", "uzp1", "uzp2", "trn1", "trn2", "ext"):
            rd, rn, rm = ops[0], ops[1], ops[2]
            lane_bits = insn.notes.get("lane_bits", 8)
            put(Op("vperm", dst="v%d" % rd.num, a="v%d" % rn.num, b="v%d" % rm.num,
                   args=(m, lane_bits, insn.notes.get("lanes", 0), insn.notes.get("index", 0))))
            self._simd_q(insn, "v%d" % rd.num, put)
            return
        put(Op("trap", text="unlifted SIMD instruction %s at 0x%x (0x%08x)"
               % (insn.mnemonic, insn.addr, insn.word), imm=insn.addr))

    def _lift_ldst_multi(self, insn: Insn, put, temp, read_reg) -> None:
        """LD1-4 / ST1-4 (multiple structures).  Element e of register r sits
        at base + (e * selem + r) * ebytes for LD2/3/4 (interleaved), and at
        base + (r * lanes + e) * ebytes for multi-register LD1/ST1."""
        vt, rn = insn.operands
        n = insn.notes
        selem, nregs, esz, q = n["selem"], n["nregs"], n["esz"], n["q"]
        ebytes = esz // 8
        lanes = (128 if q else 64) // esz
        load = insn.mnemonic.startswith("ld")
        base = temp()
        put(Op("copy", dst=base, a=read_reg(rn)))
        regs = ["v%d" % ((vt.num + r) % 32) for r in range(nregs)]
        if selem == 1:
            for r, reg in enumerate(regs):
                addr = temp()
                put(Op("bin", dst=addr, a=base, b=r * lanes * ebytes, imm=64, args=("add",)))
                if load:
                    put(Op("loadv", dst=reg, a=addr, imm=lanes * ebytes))   # zero-fills Q=0
                else:
                    put(Op("storev", a=addr, b=reg, imm=lanes * ebytes))
        else:
            for e in range(lanes):
                for r, reg in enumerate(regs):
                    addr = temp()
                    put(Op("bin", dst=addr, a=base, b=(e * selem + r) * ebytes, imm=64,
                           args=("add",)))
                    val = temp()
                    if load:
                        put(Op("load", dst=val, a=addr, imm=ebytes, args=("u", 64)))
                        put(Op("vgpr_to_lane", dst=reg, a=val, imm=esz, b=e))
                    else:
                        put(Op("vlane_to_gpr", dst=val, a=reg, imm=esz, b=e, args=(False,)))
                        put(Op("store", a=addr, b=val, imm=ebytes))
            if load and not q:
                for reg in regs:
                    put(Op("vq0", dst=reg, a=reg))
        if n["post"]:
            step = temp()
            if n["rm"] == 31:
                put(Op("bin", dst=step, a=base, b=nregs * lanes * ebytes, imm=64, args=("add",)))
            else:
                put(Op("bin", dst=step, a=base, b="x%d" % n["rm"], imm=64, args=("add",)))
            put(Op("copy", dst=("sp" if rn.num == 31 else "x%d" % rn.num), a=step))

    @staticmethod
    def _simd_q(insn: Insn, dst: str, put) -> None:
        """Zero the upper 64 bits of a vector destination written by a .8B/.2S/
        .4H/.1D (.D) form (Q=0).  AArch64 clears the whole upper half."""
        if insn.notes.get("q", 1) == 0:
            put(Op("vq0", dst=dst, a=dst))
