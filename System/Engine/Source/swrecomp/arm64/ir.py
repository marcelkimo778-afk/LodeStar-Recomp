"""A small, explicit intermediate representation.

The lifter produces one ``Function`` of ``Block``s of ``Op``s; the emitter turns
that into C.  Keeping an IR (rather than emitting during decode) lets us run
liveness analysis, report unsupported instructions with context, and unit-test
lifting independently of code generation.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field


@dataclass
class Op:
    kind: str
    dst: str | None = None
    a: object = None
    b: object = None
    c: object = None
    imm: int = 0
    args: tuple = ()
    text: str = ""
    addr: int = 0  # guest instruction address (diagnostics: fault attribution)

    @staticmethod
    def _reg_like(value) -> bool:
        if not isinstance(value, str):
            return False
        if value == "sp":
            return True
        return len(value) > 1 and value[0] in "xv" and value[1:].isdigit()

    def reads(self) -> set[str]:
        """Conservatively collect every register operand of the op.

        Over-approximating reads is safe (it only keeps more values live);
        missing one generates undeclared locals in emitted C, so the scan is
        generic over the a/b/c operand slots.
        """
        k = self.kind
        out: set[str] = {v for v in (self.a, self.b, self.c) if self._reg_like(v)}
        if k == "muladd" and self._reg_like(self.dst):
            pass
        if k in ("vmla", "vbit") and self._reg_like(self.dst):
            out.add(self.dst)
        if k == "vcrypto" and self.args and self.args[0] not in ("sha1h", "aesmc", "aesimc") and self._reg_like(self.dst):
            out.add(self.dst)          # SHA ops read the destination (except SHA1H)
        if k == "vtbl" and self.args:
            out |= set(self.args[0])   # table registers
        if k in ("velem",) and self.args and self.args[0] != "fmul_elem" and self._reg_like(self.dst):
            out.add(self.dst)          # FMLA/FMLS accumulate into Vd
        if k == "vlogic_imm" and self._reg_like(self.dst):
            out.add(self.dst)          # ORR/BIC immediate: read-modify-write
        if k in ("vlane_ins", "vgpr_to_lane", "vlane_copy") and self._reg_like(self.dst) \
                and os.environ.get("SWRECOMP_OLD_LANE_LIVENESS") != "1":
            # INS / MOV Vd.T[i] / LD1 {Vd.T}[i] write one lane and keep the
            # others.  Without this the register was "dead" before the insert:
            # after `blr get_matrix; mov v3.s[3], v0.s[0]` v3 was not reloaded,
            # so the returned matrix's translation row was a stale local
            # (props and the camera at ~0 -> the "glitched 3D" picture).
            out.add(self.dst)
        if k in ("cond_jump", "cond_tail_call", "ccmp", "fpcmp", "flags_cmp", "sel", "fcsel", "adc", "fccmp"):
            out |= {"nf", "zf", "cf", "vf"}
        return out

    def writes(self) -> set[str]:
        k = self.kind
        if k in ("store", "cond_jump", "jump", "ret", "call", "trap", "barrier", "nop"):
            return set()
        if k == "call_indirect":
            return set()
        if k == "jump_indirect":
            return set()
        if k in ("flags_add", "flags_sub", "flags_logical", "flags_cmp", "fpcmp", "ccmp", "fccmp"):
            return set()
        if k == "svc":
            return set()
        if k == "msr":
            return set()
        return {self.dst} if self.dst else set()


@dataclass
class Block:
    label: str
    ops: list[Op] = field(default_factory=list)
    terminator: Op | None = None
    successors: list[str] = field(default_factory=list)


@dataclass
class Function:
    addr: int
    name: str
    blocks: list[Block] = field(default_factory=list)
    size: int = 0
    unsupported: list[tuple[int, int, str]] = field(default_factory=list)
    calls: set[int] = field(default_factory=set)
    tail_calls: set[int] = field(default_factory=set)
    indirect: int = 0
    instruction_count: int = 0
    # Lift option, kept so a function whose blocks were dropped after analysis
    # can be re-lifted identically at emit time (see analyze_module keep_ir).
    allow_cold_blocks: bool = True
    boundary: int = 0   # next known function entry (cfg_extent); 0 = legacy sweep
    # switch-table case targets this function jumps to locally (jump_table op)
    jt_local: set[int] = field(default_factory=set)

    def all_ops(self):
        for b in self.blocks:
            for op in b.ops:
                yield op
            if b.terminator is not None:
                yield b.terminator

    def unsupported_reasons(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for _addr, _word, reason in self.unsupported:
            out[reason] = out.get(reason, 0) + 1
        return out
