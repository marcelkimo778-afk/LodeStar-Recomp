"""IR -> C emission.

Generated functions use the runtime ABI:

    void sw_f_NNNNNNNN(SwCpu *c);

General-purpose registers live in locals for the body of the function; the
canonical architectural state is ``c->x[]``/``c->sp``/flags and is synchronised
at calls, system calls, traps and function exit.  Liveness analysis keeps those
synchronisations minimal instead of spilling all 31 registers every time.
"""

from __future__ import annotations

import os

from .ir import Block, Function, Op

_FAULT_PC = os.environ.get("SWRECOMP_FAULT_PC", "0") == "1"
# Calls skip spilling registers whose c->x copy is already current (Emitter._clean_analysis); default on since
# 2026-11-11 (lockstep: no new mismatches; village walk +10% fps; full build -42%).  SWRECOMP_CLEAN_SPILL=0: old spills.
_CLEAN_SPILL = os.environ.get("SWRECOMP_CLEAN_SPILL", "1") != "0"
# Direct-index calls (2026-11-12): `bl f` / `b f` to a known function entry pass f's index in sw_functions
# (sw_call_i / sw_dispatch_i), so the runtime skips the call-cache hash (its load was the hottest
# instruction of sw_call).  The runtime uses the index only if sw_functions[idx].addr == target.
# SWRECOMP_CALL_IDX=0: the old sw_call / sw_dispatch form.  set_function_index() is called by
# recompile.emit_program (and its workers) with {rebased address: index}.
_CALL_IDX = os.environ.get("SWRECOMP_CALL_IDX", "1") != "0"
_FN_INDEX: dict = {}
# Direct calls (2026-11-17): a call / tail call to a known function also names its C function, so with clang the
# call is a direct C call (`bl`: sw_call_d inlines the runtime's fast path; `b`: SW_TAIL_D, a musttail jump)
# behind the same g_plain_idx guard as sw_call_i (hooked / setjmp / longjmp targets still go through the
# runtime).  MSVC chunks keep the sw_call_i / sw_dispatch_i calls (sw_rt.h).  SWRECOMP_DIRECT=0: off.
_DIRECT = os.environ.get("SWRECOMP_DIRECT", "1") != "0"
_FN_NAME: dict = {}

# ---- calling-convention (AAPCS64) aware spills (SWRECOMP_ABI=1, 2026-11-25) ----------------------------------------
# AAPCS64: flags (NZCV), x9-x17 and v16-v31 are not inputs of a function, are not preserved by a call and are not
# results.  The default emitter treats every written register as needed at every call and `ret`, so each call
# stored the 4 flag bytes, and reloaded x9-x17 / v16-v31 afterwards (they were "live" because the epilogue writes
# every written register back).  ABI mode: `ret` writes back only x0-x8, x18-x30, sp, v0-v15; a call stores only
# argument / callee-saved registers; flags are kept in locals across calls.
# Guest code that breaks the convention (compiler-outlined helpers, hand-written assembly) is found by a facts
# pass over all functions first (recompile.py): `entry` = scratch registers a function reads before writing
# them (its callers then store those too), `ret` = scratch registers callers read after the call returns (the
# function then writes those back at `ret`), `ind` = union of `entry` (indirect calls / jumps store those).
_ABI = os.environ.get("SWRECOMP_ABI", "0") == "1"
_ABI_FACTS: dict | None = None
_SCRATCH = frozenset(["x%d" % i for i in range(9, 18)] + ["v%d" % i for i in range(16, 32)] + ["F"])
_ABI_IN = frozenset(["x%d" % i for i in range(0, 9)] + ["x%d" % i for i in range(18, 31)] +
                    ["v%d" % i for i in range(0, 16)] + ["sp"])
_ABI_RET = _ABI_IN
_FLAG_READ_KINDS = frozenset(("cond_jump", "cond_tail_call", "ccmp", "fccmp", "fcsel", "sel", "adc", "flags_cmp"))
_FLAG_WRITE_KINDS = frozenset(("flags_add", "flags_sub", "flags_logical", "flags_cmp", "fpcmp", "ccmp", "fccmp"))
_NOFACT = frozenset()


_LEAF = os.environ.get("SWRECOMP_LEAF", "1") != "0"   # leaf calls (sw_rt.h sw_call_leaf), with the facts


def set_abi_facts(facts: dict | None) -> None:
    """facts: {"entry": {addr: set}, "ret": {addr: set}, "ind": set} with rebased addresses; None = ABI mode off."""
    global _ABI_FACTS
    _ABI_FACTS = facts


def _flag_reads(op) -> bool:
    if op.kind in _FLAG_READ_KINDS:
        return True
    return op.kind == "mrs" and bool(op.args) and op.args[0] == "nzcv"


def _flag_writes(op) -> bool:
    if op.kind in _FLAG_WRITE_KINDS:
        return True
    if op.kind == "msr" and op.args and op.args[0] == "nzcv":
        return True
    return op.kind == "adc" and bool(op.args) and bool(op.args[1])


def set_function_index(index: dict, names: dict | None = None) -> None:
    global _FN_INDEX, _FN_NAME
    _FN_INDEX = index if _CALL_IDX else {}
    _FN_NAME = (names or {}) if (_CALL_IDX and _DIRECT) else {}

COND_NAMES = ("eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
              "hi", "ls", "ge", "lt", "gt", "le", "al", "nv")


def c_const(value: int, width: int = 64) -> str:
    value &= (1 << width) - 1
    if width == 64:
        return "0x%016xULL" % value
    return "0x%08xU" % value


def _mask(value: int, width: int) -> int:
    return value & ((1 << width) - 1)


class Emitter:
    def __init__(self, fn: Function, base: int = 0, facts_mode: bool = False) -> None:
        self.fn = fn
        self.base = base
        # facts_mode: ABI liveness without any facts (recompile.py facts pass); abi: ABI mode with the facts
        self.facts_mode = facts_mode
        self.abi = facts_mode or (_ABI and _ABI_FACTS is not None)
        self.addr_rb = (fn.addr + base) & 0xFFFFFFFFFFFFFFFF
        self.ret_need = frozenset() if (facts_mode or not self.abi) else \
            frozenset(_ABI_FACTS["ret"].get(self.addr_rb, ()))
        self._live_in: list = []
        self.lines: list[str] = []
        self.tmp = 0
        self.written: set[str] = set()
        self.read: set[str] = set()
        self.flags_written = False
        self.sp_used = False
        self._live_after: dict[int, set[str]] = {}
        self._clean: set[str] = set()
        self._compute_sets()
        self._liveness()

    # -- analysis ------------------------------------------------------
    def _compute_sets(self) -> None:
        for op in self.fn.all_ops():
            self.written |= {w for w in op.writes() if w and w != "zr"}
            self.read |= op.reads()
            # `msr nzcv` and `mrs nzcv` both touch the flag locals, so the
            # prologue/epilogue must save them even when nothing reads flags.
            if op.kind in ("flags_add", "flags_sub", "flags_logical", "flags_cmp", "fpcmp", "ccmp", "fccmp"):
                self.flags_written = True
            elif op.kind in ("mrs", "msr") and op.args and op.args[0] == "nzcv":
                self.flags_written = True
            elif op.kind == "adc" and op.args[1]:
                self.flags_written = True
        self.written.discard("nf")
        for r in ("nf", "zf", "cf", "vf"):
            self.written.discard(r)
        self.flags_read = bool(self.read & {"nf", "zf", "cf", "vf"})
        self.read -= {"nf", "zf", "cf", "vf"}
        self.sp_used = "sp" in self.written or "sp" in self.read

    def _block_succs(self, idx: int) -> list[int]:
        blk = self.fn.blocks[idx]
        out: list[int] = []
        labels = {b.label: i for i, b in enumerate(self.fn.blocks)}
        # Include the terminator: it is often not in blk.ops, and ignoring it
        # made `bl f; b join` look like a fallthrough into the next block in
        # list order, so registers read at the join (x0 at `ret`) were "dead"
        # after the call and not reloaded (StandardAllocator::Reallocate(NULL)
        # returned `this`, which corrupted a static-init guard).
        ops = self._block_ops(idx)
        target_labels: list[str] = []
        # A block with no unconditional exit runs into the next block (e.g. one
        # ending in a call because the next instruction is a branch target).
        # This defaulted to False, so such blocks had no successors and every
        # register was "dead" after the call -> not reloaded -> stale spill.
        fallthrough = True
        for op in ops:
            if op.kind == "cond_jump" and op.a:
                target_labels.append(op.a)
                fallthrough = True
            elif op.kind in ("cond_jump_zero", "cond_jump_bit") and op.text:
                target_labels.append(op.text)
                fallthrough = True
            elif op.kind == "cond_tail_call":
                fallthrough = True
            elif op.kind == "jump":
                target_labels.append("L_%08x" % op.imm)
                fallthrough = False
            elif op.kind == "jump_indirect":
                fallthrough = False
            elif op.kind == "jump_table":
                target_labels.extend("L_%08x" % t for t in op.args)
                fallthrough = False
            elif op.kind == "ret":
                fallthrough = False
            elif op.kind == "tail_call":
                fallthrough = False
        for label in target_labels:
            if label in labels:
                out.append(labels[label])
        if fallthrough and idx + 1 < len(self.fn.blocks):
            out.append(idx + 1)
        return out

    # Ops that leave the function: the epilogue / pre-dispatch spill writes
    # every register the function wrote back to c->x / c->v, so all of them
    # are read there.  Without this, a register written before a call and
    # not otherwise used afterwards (e.g. x0 = arg; bl f; ret) was considered
    # dead after the call, was not reloaded, and the stale local was spilled
    # over the callee's return value (StandardAllocator::Allocate returned
    # its own argument instead of NULL).
    # Calls and syscalls spill every written register before handing over
    # (arguments travel in x0-x7), so they read all of them too: without
    # this, `bl f; bl g` passed g the stale pre-call x0 instead of f's
    # result (GetRelocationTable -> Relocate got the file header).
    _EXIT_KINDS = frozenset(("ret", "tail_call", "cond_tail_call", "cond_tail_call_zero",
                             "cond_tail_call_bit", "jump_indirect", "call", "call_indirect", "svc"))

    def _op_reads(self, op) -> set[str]:
        if self.abi:
            return self._abi_reads(op)
        r = op.reads()
        if op.kind in self._EXIT_KINDS:
            r = r | self._exit_live
        return r

    def _op_writes(self, op) -> set[str]:
        w = op.writes()
        if self.abi and _flag_writes(op):
            w = w | {"F"}
        return w

    # ---- ABI mode (SWRECOMP_ABI=1, see _ABI above) ----
    _TAIL_KINDS = frozenset(("tail_call", "cond_tail_call", "cond_tail_call_zero", "cond_tail_call_bit",
                             "jump_indirect"))

    def _target(self, op):
        """Rebased address of a direct call / tail call that names a known function entry, else None."""
        k = op.kind
        if k in ("call", "tail_call"):
            t = op.imm
        elif k == "cond_tail_call":
            t = op.a
        elif k in ("cond_tail_call_zero", "cond_tail_call_bit"):
            t = op.b
        else:
            return None
        if not isinstance(t, int):
            return None
        t = (t + self.base) & 0xFFFFFFFFFFFFFFFF
        return t if (not _FN_INDEX or t in _FN_INDEX) else None

    def _entry_of(self, t):
        """Scratch registers (and "F") the target may read at entry: facts, or the indirect union."""
        if self.facts_mode:
            return _NOFACT
        if t is None:
            return _ABI_FACTS["ind"]
        return _ABI_FACTS["entry"].get(t, _NOFACT)

    def _abi_need(self, op) -> frozenset:
        """Registers the code after an exit / call may read from c->x / c->v / flags."""
        k = op.kind
        if k == "ret":
            return _ABI_RET | self.ret_need
        if k in ("call", "call_indirect"):
            return _ABI_IN | self._entry_of(self._target(op))
        if k in self._TAIL_KINDS:   # the target returns to our caller: its needs and ours
            return _ABI_IN | self._entry_of(self._target(op)) | self.ret_need
        return frozenset()

    def _abi_reads(self, op) -> set[str]:
        r = op.reads() - {"nf", "zf", "cf", "vf"}
        if op.kind == "bitfield" and op.args and op.args[0] in ("ubfm", "sbfm") and op.c not in (op.a, op.b):
            r.discard(op.c)   # the IR passes the old destination for bfm; ubfm / sbfm overwrite all of it
        if _flag_reads(op):
            r.add("F")
        k = op.kind
        if k == "svc":
            r |= self._exit_live
        elif k == "ret" or k in ("call", "call_indirect") or k in self._TAIL_KINDS:
            need = self._abi_need(op)
            r |= self._exit_live & need
            if "F" in need:
                r.add("F")
        return r

    def abi_facts(self) -> tuple:
        """facts pass: (entry scratch set, [(call target or None, scratch live after)], [tail targets or None],
        [direct callees]).  Only meaningful with facts_mode=True."""
        entry = frozenset(self._live_in[0] & _SCRATCH) if self._live_in else _NOFACT
        tr = os.environ.get("SWRECOMP_ABI_TRACE")
        if tr and int(tr, 16) == self.addr_rb:
            with open(os.environ.get("SWRECOMP_ABI_TRACE_OUT", "abi_trace.txt"), "w") as fh:
                for i in range(len(self.fn.blocks)):
                    fh.write("block %d %s succs %s live_in %s\n" % (i, self.fn.blocks[i].label, self._block_succs(i),
                             sorted(self._live_in[i] & _SCRATCH)))
                    for op in self._block_ops(i):
                        fh.write("   %-16s dst=%s a=%s b=%s c=%s imm=%s args=%s addr=%x reads=%s after=%s\n" % (
                            op.kind, op.dst, op.a, op.b, op.c, op.imm, op.args, op.addr,
                            sorted(self._op_reads(op) & _SCRATCH),
                            sorted(self._live_after.get(id(op), set()) & _SCRATCH)))
        calls, tails, callees = [], [], []
        leaf = True
        for op in self.fn.all_ops():
            k = op.kind
            if k in ("call", "call_indirect", "svc", "trap", "jump_table") or k in self._TAIL_KINDS:
                leaf = False
            if k in ("call", "call_indirect"):
                t = self._target(op)
                after = frozenset(self._live_after.get(id(op), set()) & _SCRATCH)
                if after:
                    calls.append((t, after, (op.addr + self.base) & 0xFFFFFFFFFFFFFFFF))
                if t is not None:
                    callees.append(t)
            elif k in self._TAIL_KINDS:
                tails.append(self._target(op))
        return entry, calls, tails, callees, leaf

    def _liveness(self) -> None:
        """Backward liveness over blocks; used to reload only live registers."""
        self._exit_live = {w for w in self.written
                           if w[0] in "xv" and w[1:].isdigit()} | ({"sp"} if "sp" in self.written else set())
        n = len(self.fn.blocks)
        live_in: list[set[str]] = [set() for _ in range(n)]
        live_out: list[set[str]] = [set() for _ in range(n)]
        succs = [self._block_succs(i) for i in range(n)]
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                out: set[str] = set()
                for s in succs[i]:
                    out |= live_in[s]
                live_out[i] = out
                cur = set(out)
                for op in reversed(self._block_ops(i)):
                    cur -= self._op_writes(op)
                    cur |= self._op_reads(op)
                if cur != live_in[i]:
                    live_in[i] = cur
                    changed = True
        # Map "live after op index" for call-like ops.
        for i, blk in enumerate(self.fn.blocks):
            cur = set(live_out[i])
            for op in reversed(self._block_ops(i)):
                self._live_after[id(op)] = set(cur)
                cur -= self._op_writes(op)
                cur |= self._op_reads(op)
        self._live_in = live_in

    def _block_ops(self, i: int) -> list:
        blk = self.fn.blocks[i]
        ops = list(blk.ops)
        if blk.terminator is not None and (not ops or ops[-1] is not blk.terminator):
            ops.append(blk.terminator)
        return ops

    # -- helpers -------------------------------------------------------
    # Condition codes as C expressions over the flag locals (0/1 values), exactly sw_cond() in sw_rt.h.
    # Emitted inline: at /O1 MSVC called sw_cond out of line (1.6% of all exe samples, 2026-11-11 profile).
    _COND_C = ("zf", "!zf", "cf", "!cf", "nf", "!nf", "vf", "!vf", "(cf && !zf)", "(!cf || zf)",
               "(nf == vf)", "(nf != vf)", "(!zf && nf == vf)", "(zf || nf != vf)", "1", "1")

    @classmethod
    def _cond(cls, code: int) -> str:
        return "(%s)" % cls._COND_C[code & 0xF]

    def label(self, addr: int) -> str:
        return "L_%08x" % addr

    @staticmethod
    def reg(name: str) -> str:
        return name

    def _dst(self, name: str | None) -> str:
        return name or "/*no-dst*/"

    def _is_zr(self, name) -> bool:
        return name == "zr"

    def _val(self, name, width: int = 64) -> str:
        if isinstance(name, int):
            return c_const(name, width)
        if name is None:
            return "0"
        if name == "zr":
            return "0"
        if width == 32:
            return "(u32)%s" % name
        return name

    @staticmethod
    def _is_vec(name) -> bool:
        return isinstance(name, str) and len(name) > 1 and name[0] == "v" and name[1:].isdigit()

    def _set(self, dst: str, expr: str, width: int = 64) -> str:
        if self._is_zr(dst):
            return "  (void)(%s);" % expr
        if self._is_vec(dst):
            # Vector destinations are sw_v128 locals, never integer casts.
            return "  %s = %s;" % (dst, expr)
        if width == 32:
            return "  %s = (u64)(u32)(%s);" % (dst, expr)
        return "  %s = (u64)(%s);" % (dst, expr)

    def spill(self, regs: set[str] | None = None, skip: frozenset | set = frozenset(),
              only: frozenset | set | None = None, flags: bool = True) -> list[str]:
        """skip: registers whose local is known to equal c->x / c->v / c->sp already (clean, see
        _clean_analysis); their stores would write the same value again.  only / flags (ABI mode): store just
        these registers / the flags only when flags is true."""
        out = []
        regs = self.written if regs is None else regs
        for i in range(31):
            name = "x%d" % i
            if name in regs and name not in skip and (only is None or name in only):
                out.append("  c->x[%d] = %s;" % (i, name))
        for i in range(32):   # v0..v31 (the shared x loop dropped v31)
            vname = "v%d" % i
            if vname in self.written and vname not in skip and (only is None or vname in only):
                out.append("  c->v[%d] = %s;" % (i, vname))
        if ("sp" in self.written or self.sp_used) and "sp" not in skip and (only is None or "sp" in only):
            out.append("  c->sp = sp;")
        if self.flags_written and flags:
            out.append("  c->nf = nf; c->zf = zf; c->cf = cf; c->vf = vf;")
        return out

    def _call_spill(self, op) -> list[str]:
        if not self.abi:
            return self.spill(skip=self._clean)
        after = self._live_after.get(id(op), set())
        need = self._abi_need(op) | (after & _SCRATCH)   # scratch read after the call: the callee keeps it
        return self.spill(skip=self._clean, only=need, flags=("F" in need))

    def _call_reload_flags(self, op) -> list[str]:
        """ABI mode: flags read after a call (a non-AAPCS callee, from the facts) come back from c->nf."""
        if self.abi and "F" in self._live_after.get(id(op), set()) and (self.flags_written or self.flags_read):
            return ["  nf = c->nf; zf = c->zf; cf = c->cf; vf = c->vf;"]
        return []

    # Callee-saved general-purpose registers per AAPCS64: x19-x28, x29 (fp).
    # These are NOT reloaded from c->x[] after calls because the guest code
    # expects them to be preserved across function calls.  Reloading them would
    # pick up any corruption a callee left in c->x[], which is a lifter bug.
    _CALLEE_SAVED_GPR = frozenset("x%d" % i for i in range(19, 30))

    # ---- clean registers (SWRECOMP_CLEAN_SPILL=1) ---------------------------------------------
    # A register is "clean" where its local equals its slot in c->x / c->v / c->sp: at entry (locals are
    # loaded from there), after a spill, after a reload.  Across a call, x19-x29 stay clean: the callee
    # restores them (AAPCS64) - the assumption this emitter already makes by never reloading them.  An svc
    # keeps only what is reloaded after it.  Calls then skip the stores of clean registers (about 40% of
    # the spill stores, which are ~1/3 of all generated lines).  Exits (ret, tail calls, indirect jumps)
    # still spill everything.  Forward must-analysis over the same block successors as the liveness.
    _UNIVERSE = frozenset(["x%d" % i for i in range(31)] + ["v%d" % i for i in range(32)] + ["sp"])

    def _reloaded(self, op) -> set[str]:
        regs = self._live_after.get(id(op), set())
        declared = self.written | self.read
        out = {r for r in regs if r[0] == "x" and r[1:].isdigit() and r not in self._CALLEE_SAVED_GPR}
        out |= {r for r in regs if self._is_vec(r) and r in declared}
        if "sp" in regs:
            out.add("sp")
        return out

    def _clean_step(self, op, clean: set[str]) -> set[str]:
        k = op.kind
        if k in ("call", "call_indirect"):
            return set(self._CALLEE_SAVED_GPR) | self._reloaded(op)
        if k == "svc":
            return self._reloaded(op)
        w = op.writes()
        return clean - w if w else clean

    def _clean_analysis(self) -> list[set[str]]:
        n = len(self.fn.blocks)
        succs = [self._block_succs(i) for i in range(n)]
        preds: list[list[int]] = [[] for _ in range(n)]
        for i, ss in enumerate(succs):
            for j in ss:
                preds[j].append(i)
        out = [set(self._UNIVERSE) for _ in range(n)]
        inn: list[set[str]] = [set() for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for i in range(n):
                cin = set(self._UNIVERSE) if i == 0 else None
                for p in preds[i]:
                    cin = set(out[p]) if cin is None else (cin & out[p])
                if cin is None:
                    cin = set()          # no known predecessor: assume nothing
                inn[i] = set(cin)
                for op in self._block_ops(i):
                    cin = self._clean_step(op, cin)
                if cin != out[i]:
                    out[i] = cin
                    changed = True
        return inn

    def reload(self, regs: set[str]) -> list[str]:
        out = []
        for i in range(31):
            name = "x%d" % i
            if name in regs and name not in self._CALLEE_SAVED_GPR:
                out.append("  %s = c->x[%d];" % (name, i))
        # Every live vector register is reloaded, not only ones this function
        # writes: a float / HFA result arrives in v0-v3 of a function that only
        # reads it (`bl get_rect; mov v8.16b, v0.16b`), and without the reload
        # the stale pre-call local was used (viewport {1, 2^33, 5, nan}).
        # SWRECOMP_OLD_VRELOAD=1: the Nov-7 behaviour (only written registers), for bisecting
        declared = self.written if os.environ.get("SWRECOMP_OLD_VRELOAD") == "1" else self.written | self.read
        for i in range(32):
            vname = "v%d" % i
            if vname in regs and vname in declared:
                out.append("  %s = c->v[%d];" % (vname, i))
        if "sp" in regs:
            out.append("  sp = c->sp;")
        return out

    def epilogue_lines(self) -> list[str]:
        out = ["L_ret:"]
        if self.abi:
            need = _ABI_RET | self.ret_need
            out += self.spill(only=need, flags=("F" in need))
        else:
            out += self.spill()
        out.append("#ifdef SW_DEBUG_CHECK_REGS")
        out.append("  if (c->x[19] != __cs_x19 || c->x[20] != __cs_x20 || c->x[21] != __cs_x21 ||")
        out.append("      c->x[22] != __cs_x22 || c->x[23] != __cs_x23 || c->x[24] != __cs_x24 ||")
        out.append("      c->x[25] != __cs_x25 || c->x[26] != __cs_x26 || c->x[27] != __cs_x27 ||")
        out.append("      c->x[28] != __cs_x28 || c->x[29] != __cs_x29 || c->x[30] != __cs_x30 ||")
        out.append("      c->sp != __cs_sp) {")
        out.append("    sw_report(\"CALLEE-SAVED CORRUPT in 0x%x: x19=%llx/%llx x20=%llx/%llx sp=%llx/%llx\\\\n\",")
        out.append("              (unsigned int)__cs_fn_addr,")
        out.append("              (unsigned long long)__cs_x19, (unsigned long long)c->x[19],")
        out.append("              (unsigned long long)__cs_x20, (unsigned long long)c->x[20],")
        out.append("              (unsigned long long)__cs_sp, (unsigned long long)c->sp);")
        out.append("    sw_trap(c, __cs_fn_addr, \"callee-saved register corrupted\");")
        out.append("  }")
        out.append("#endif")
        out.append("  return;")
        return out

    # -- emission ------------------------------------------------------
    def emit(self) -> str:
        fn = self.fn
        lines: list[str] = []
        lines.append("/* %s @ 0x%x, %d instructions, %d bytes */"
                     % (fn.name, fn.addr, fn.instruction_count, fn.size))
        lines.append("void %s(SwCpu *c)" % fn.name)
        lines.append("{")
        lines.append("  c->cur_fn = 0x%xULL;" % ((fn.addr + self.base) & 0xFFFFFFFFFFFFFFFF))
        used = {r for r in (self.written | self.read) if r.startswith("x") and r != "zr"}
        used = {r for r in used if r[1:].isdigit() and int(r[1:]) < 31}
        if used:
            decl = ", ".join("%s = c->x[%d]" % (r, int(r[1:])) for r in sorted(used, key=lambda s: int(s[1:])))
            lines.append("  u64 %s;" % decl)
        vecs = sorted({v for v in (self.written | self.read) if self._is_vec(v)},
                      key=lambda s: int(s[1:]))
        if vecs:
            lines.append("  sw_v128 %s;" % ", ".join("%s = c->v[%s]" % (v, v[1:]) for v in vecs))
        temps = sorted({op.dst for op in fn.all_ops() if op.dst and op.dst.startswith("t") and op.dst[1:].isdigit()},
                       key=lambda s: int(s[1:]))
        if temps:
            lines.append("  u64 %s;" % ", ".join(temps))
        if self.sp_used or "sp" in self.written:
            lines.append("  u64 sp = c->sp;")
        if self.flags_written or self.flags_read:
            lines.append("  u8 nf = c->nf, zf = c->zf, cf = c->cf, vf = c->vf;")
        # --- callee-saved register corruption checker (debug builds) ---
        # x19-x28, x29 (fp), x30 (lr) and sp are callee-saved under AAPCS64.
        # Snapshot at entry; verify at every return path.  A mismatch means a
        # function (or a function it called via sw_dispatch) clobbered a
        # callee-saved register without restoring it -- a lifter/emitter bug.
        lines.append("#ifdef SW_DEBUG_CHECK_REGS")
        lines.append("  u64 __cs_x19 = c->x[19], __cs_x20 = c->x[20], __cs_x21 = c->x[21],")
        lines.append("             __cs_x22 = c->x[22], __cs_x23 = c->x[23], __cs_x24 = c->x[24],")
        lines.append("             __cs_x25 = c->x[25], __cs_x26 = c->x[26], __cs_x27 = c->x[27],")
        lines.append("             __cs_x28 = c->x[28], __cs_x29 = c->x[29], __cs_x30 = c->x[30],")
        lines.append("             __cs_sp = c->sp, __cs_fn_addr = 0x%xULL;" % ((self.fn.addr + self.base) & 0xFFFFFFFFFFFFFFFF))
        lines.append("#endif")
        self.lines = lines
        clean_in = self._clean_analysis() if _CLEAN_SPILL else None
        self._clean = set()
        for i, blk in enumerate(fn.blocks):
            self._clean = set(clean_in[i]) if clean_in is not None else set()
            self.lines.append("%s:;" % blk.label)
            for op in blk.ops:
                self.emit_op(op)
            # A block terminator is control flow the op list does not carry.
            # `b` out of the function (a tail call) and `b` to a cold block are
            # stored only here, so dropping the terminator emitted an empty
            # body for every single-instruction alias stub.
            term = blk.terminator
            if term is not None and term.kind != "fallthrough":
                already = blk.ops and blk.ops[-1].kind == term.kind and \
                    term.kind in ("ret", "jump", "jump_indirect")
                if not already:
                    self.emit_op(term)
        self.lines += self.epilogue_lines()
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"

    def _fresh(self, prefix: str = "s") -> str:
        self.tmp += 1
        return "%s%d" % (prefix, self.tmp)

    def emit_op(self, op: Op) -> None:
        k = op.kind
        handler = getattr(self, "_op_" + k, None)
        if handler is None:
            self.lines.append("  /* unimplemented IR op %s */" % k)
            self.lines.append("  sw_trap(c, 0x%xULL, \"IR op %s not emitted\");" % (self.fn.addr, k))
            return
        # Attribute guest-memory faults to the exact instruction.  Costs a store
        # to c->pc before every memory op (bigger code, slower compile and run),
        # so it is off by default: faults name the function (c->cur_fn).
        # SWRECOMP_FAULT_PC=1 at recompile time brings it back for debugging.
        # svc keeps it (rare; services may log the caller pc).
        if op.addr and (k == "svc" or (_FAULT_PC and k in ("load", "store", "loadv", "storev", "cas",
                                                           "ldxr", "stxr"))):
            self.lines.append("  c->pc = 0x%xULL;" % ((op.addr + self.base) & 0xFFFFFFFFFFFFFFFF))
        handler(op)
        if _CLEAN_SPILL:
            self._clean = self._clean_step(op, self._clean)

    # -- individual ops -------------------------------------------------
    def _op_const(self, op: Op) -> None:
        self.lines.append(self._set(op.dst, c_const(op.imm)))

    def _op_addr(self, op: Op) -> None:
        """A PC-relative address baked into the instruction stream (ADR/ADRP,
        literal pools).  The value is module-relative and must be rebased to
        where this module is actually linked."""
        self.lines.append(self._set(op.dst, c_const((op.imm + self.base) & 0xFFFFFFFFFFFFFFFF)))

    def _op_copy(self, op: Op) -> None:
        self.lines.append(self._set(op.dst, self._val(op.a)))

    def _op_not(self, op: Op) -> None:
        width = op.imm or 64
        if width == 32:
            self.lines.append(self._set(op.dst, "~%s" % self._val(op.a, 32), 32))
        else:
            self.lines.append(self._set(op.dst, "~%s" % self._val(op.a)))

    def _op_end(self, op: Op) -> None:
        self.lines.append(self._set(op.dst, op.a))

    def _op_unop(self, op: Op) -> None:
        kind = op.args[0]
        width = op.imm or 64
        a = self._val(op.a, width)
        table = {"clz": "sw_clz", "cls": "sw_cls", "rbit": "sw_rbit"}
        if kind in table:
            self.lines.append(self._set(op.dst, "%s%d(%s)" % (table[kind], width, a), width))
        elif kind == "rev":
            self.lines.append(self._set(op.dst, "sw_bswap%d(%s)" % (width, a), width))
        elif kind == "rev16":
            self.lines.append(self._set(op.dst, "sw_rev16_%d(%s)" % (width, a), width))
        elif kind == "rev32":
            self.lines.append(self._set(op.dst, "sw_rev32_64(%s)" % a, width))
        else:
            self.lines.append("  sw_trap(c, 0x%xULL, \"unop %s not emitted\");"
                              % (self.fn.addr, kind))

    def _op_bin(self, op: Op) -> None:
        kind = op.args[0]
        width = op.imm or 64
        a = self._val(op.a, width)
        b = self._val(op.b, width)
        bits = width
        if kind in ("add",):
            self.lines.append(self._set(op.dst, "%s + %s" % (a, b), width))
        elif kind == "sub":
            self.lines.append(self._set(op.dst, "%s - %s" % (a, b), width))
        elif kind == "and":
            self.lines.append(self._set(op.dst, "%s & %s" % (a, b), width))
        elif kind == "orr":
            self.lines.append(self._set(op.dst, "%s | %s" % (a, b), width))
        elif kind == "eor":
            self.lines.append(self._set(op.dst, "%s ^ %s" % (a, b), width))
        elif kind in ("lsl", "lsr", "asr", "ror"):
            amt = b if isinstance(op.b, int) else "(%s & %d)" % (b, bits - 1)
            if kind == "lsl":
                expr = "%s << %s" % (a, amt)
            elif kind == "lsr":
                expr = "%s >> %s" % (self._unsigned(a, width), amt)
            elif kind == "asr":
                expr = "%s >> %s" % (self._signed(a, width), amt)
            else:
                expr = "sw_ror%s(%s, %s)" % ("" if width == 64 else "32", a, amt)
            self.lines.append(self._set(op.dst, expr, width))
        elif kind == "udiv":
            self.lines.append(self._set(op.dst, "(%s ? %s / %s : 0)"
                                        % (b, self._unsigned(a, width), b), width))
        elif kind == "sdiv":
            self.lines.append(self._set(op.dst, "sw_sdiv%s(%s, %s)"
                                        % ("" if width == 64 else "32", self._signed(a, width),
                                           self._signed(b, width)), width))
        elif kind in ("smulh", "umulh"):
            self.lines.append(self._set(op.dst, "sw_%s(%s, %s)" % (kind, a, b)))
        else:
            self.lines.append("  sw_trap(c, 0x%xULL, \"bin op %s not emitted\");"
                              % (self.fn.addr, kind))

    @staticmethod
    def _unsigned(expr: str, width: int) -> str:
        return expr if width == 64 else "(u32)(%s)" % expr

    @staticmethod
    def _signed(expr: str, width: int) -> str:
        # 64-bit values are u64 locals: without the cast, ASR (asrv and
        # "add x, y, z, asr #n") was a logical shift.
        return "(s64)(%s)" % expr if width == 64 else "(s32)(%s)" % expr

    def _op_movk(self, op: Op) -> None:
        shift = op.imm
        width = op.args[0]
        keep = ~(0xFFFF << shift) & ((1 << width) - 1)
        expr = "(%s & %s) | (%s)" % (op.a, c_const(keep), c_const(op.b << shift))
        self.lines.append(self._set(op.dst, expr, width))

    def _op_bitfield(self, op: Op) -> None:
        from .decoder import decode_bit_masks
        kind = op.args[0]
        width = op.args[1]
        immr = op.b
        imms = op.imm
        n = 1 if width == 64 else 0
        wmask, tmask = decode_bit_masks(n, imms, immr, width)
        src = self._val(op.a, width)
        if immr == 0:
            ror = src
        else:
            ror = "sw_ror%s(%s, %d)" % ("" if width == 64 else "32", src, immr)
        # ARM pseudocode (with tmask from DecodeBitMasks, d = S - R):
        #   bot  = ROR(src, R) & wmask
        #   UBFM = bot & tmask
        #   SBFM = (Replicate(src<S>) & ~tmask) | (bot & tmask)
        #   BFM  = (dst & ~tmask) | (((dst & ~wmask) | bot) & tmask)
        mask = (1 << width) - 1
        bot = "(%s & %s)" % (ror, c_const(wmask & tmask, width))
        if kind == "ubfm":
            self.lines.append(self._set(op.dst, bot, width))
        elif kind == "sbfm":
            sign = "(((u64)%s >> %d) & 1)" % (src, imms)
            merge = "(%s ? %s : 0)" % (sign, c_const(~tmask & mask, width))
            self.lines.append(self._set(op.dst, "%s | %s" % (bot, merge), width))
        else:  # bfm
            keep = c_const((~tmask | ~wmask) & mask, width)
            self.lines.append(self._set(op.dst, "%s | (%s & %s)"
                                        % (bot, self._val(op.c, width), keep), width))

    def _op_extr(self, op: Op) -> None:
        width = op.args[0]
        lsb = op.imm
        a = self._val(op.a, width)
        b = self._val(op.b, width)
        if lsb == 0:
            self.lines.append(self._set(op.dst, b, width))   # (Rn:Rm) >> 0 = Rm
            return
        # EXTR Rd, Rn, Rm, #lsb = (Rn:Rm) >> lsb: low part from Rm (b), high from Rn (a)
        expr = "(%s >> %d) | (%s << %d)" % (b, lsb, a, width - lsb)
        self.lines.append(self._set(op.dst, expr, width))

    def _op_extend(self, op: Op) -> None:
        option = op.args[0]
        src_width = op.args[1]
        amount = op.imm
        v = self._val(op.a, 32 if src_width == 32 else 64)
        casts = {
            0: "(u64)(u8)(%s)" % v,
            1: "(u64)(u16)(%s)" % v,
            2: "(u64)(u32)(%s)" % v,
            3: "(u64)(%s)" % v,
            4: "(u64)(s64)(s8)(%s)" % v,
            5: "(u64)(s64)(s16)(%s)" % v,
            6: "(u64)(s64)(s32)(%s)" % v,
            7: "(u64)(%s)" % v,
        }
        expr = casts.get(option)
        if expr is None:
            self.lines.append("  sw_trap(c, 0x%xULL, \"extend option %d\");" % (self.fn.addr, option))
            return
        if amount:
            expr = "(%s << %d)" % (expr, amount)
        self.lines.append(self._set(op.dst, expr))

    def _op_sel(self, op: Op) -> None:
        kind = op.args[0]
        width = op.c if isinstance(op.c, int) else 64
        a = self._val(op.a, width)
        b = self._val(op.b, width)
        cond = self._cond(op.imm)
        if kind == "csel":
            expr = "%s ? %s : %s" % (cond, a, b)
        elif kind == "csinc":
            expr = "%s ? %s : (%s + 1)" % (cond, a, b)
        elif kind == "csinv":
            expr = "%s ? %s : ~%s" % (cond, a, b)
        else:
            expr = "%s ? %s : (0 - %s)" % (cond, a, b)
        self.lines.append(self._set(op.dst, expr, width))

    def _op_muladd(self, op: Op) -> None:
        kind = op.args[0]
        width = op.imm or 64
        a = self._val(op.a, width)
        b = self._val(op.b, width)
        c = self._val(op.c, width)
        # madd: Ra + Rn*Rm ; msub: Ra - Rn*Rm  (msub used to emit Rn*Rm - Ra)
        if kind in ("madd", "msub"):
            expr = "%s %s %s * %s" % (c, "+" if kind == "madd" else "-", a, b)
            self.lines.append(self._set(op.dst, expr, width))
        elif kind in ("smaddl", "smsubl"):
            expr = "(u64)((s64)%s %s (s64)(s32)%s * (s64)(s32)%s)" % (
                c, "+" if kind == "smaddl" else "-", a, b)
            self.lines.append(self._set(op.dst, expr))
        elif kind in ("umaddl", "umsubl"):
            expr = "(u64)((u64)%s %s (u64)(u32)%s * (u64)(u32)%s)" % (
                c, "+" if kind == "umaddl" else "-", a, b)
            self.lines.append(self._set(op.dst, expr))
        else:
            self.lines.append("  sw_trap(c, 0x%xULL, \"muladd %s\");" % (self.fn.addr, kind))

    def _op_flags_add(self, op: Op) -> None:
        width = op.imm or 64
        a = self._val(op.a, width)
        b = self._val(op.b, width)
        if width == 64:
            self.lines.append("  { u64 _r = %s + %s; cf = (_r < %s); nf = (u8)(_r >> 63);"
                              " zf = (_r == 0); vf = (u8)((((u64)%s ^ _r) & ((u64)%s ^ _r)) >> 63); }"
                              % (a, b, a, a, b))
        else:
            self.lines.append("  { u32 _r = (u32)%s + (u32)%s; cf = (_r < (u32)%s);"
                              " nf = (u8)(_r >> 31); zf = (_r == 0);"
                              " vf = (u8)(((((u32)%s ^ _r) & ((u32)%s ^ _r)) >> 31) & 1); }"
                              % (a, b, a, a, b))

    def _op_flags_sub(self, op: Op) -> None:
        width = op.imm or 64
        a = self._val(op.a, width)
        b = self._val(op.b, width)
        if width == 64:
            self.lines.append("  { u64 _r = %s - %s; cf = (%s >= %s); nf = (u8)(_r >> 63);"
                              " zf = (_r == 0); vf = (u8)(((((u64)%s ^ (u64)%s) & ((u64)%s ^ _r)) >> 63) & 1); }"
                              % (a, b, a, b, a, b, a))
        else:
            self.lines.append("  { u32 _r = (u32)%s - (u32)%s; cf = ((u32)%s >= (u32)%s);"
                              " nf = (u8)(_r >> 31); zf = (_r == 0);"
                              " vf = (u8)(((((u32)%s ^ (u32)%s) & ((u32)%s ^ _r)) >> 31) & 1); }"
                              % (a, b, a, b, a, b, a))

    def _op_flags_logical(self, op: Op) -> None:
        width = op.imm or 64
        a = self._val(op.a, width)
        if width == 32:
            self.lines.append("  nf = (u8)(((u32)%s >> 31) & 1); zf = ((u32)%s == 0); cf = 0; vf = 0;"
                              % (a, a))
        else:
            self.lines.append("  nf = (u8)(((u64)%s >> 63) & 1); zf = ((u64)%s == 0); cf = 0; vf = 0;"
                              % (a, a))

    def _op_ccmp(self, op: Op) -> None:
        kind, width, nzcv, _immediate = op.args
        a = self._val(op.a, width)
        b = self._val(op.b, width)
        cond = op.imm
        # Conditional compare: when cond holds, flags come from a +/- b;
        # otherwise from the 4-bit immediate (N,Z,C,V).
        if kind == "ccmp":
            if width == 64:
                calc = ("{ u64 _r = %s - %s; cf = (%s >= %s); nf = (u8)(_r >> 63); zf = (_r == 0);"
                        " vf = (u8)(((((u64)%s ^ (u64)%s) & ((u64)%s ^ _r)) >> 63) & 1); }"
                        % (a, b, a, b, a, b, a))
            else:
                calc = ("{ u32 _r = (u32)%s - (u32)%s; cf = ((u32)%s >= (u32)%s); nf = (u8)(_r >> 31);"
                        " zf = (_r == 0); vf = (u8)(((((u32)%s ^ (u32)%s) & ((u32)%s ^ _r)) >> 31) & 1); }"
                        % (a, b, a, b, a, b, a))
        else:
            if width == 64:
                calc = ("{ u64 _r = %s + %s; cf = (_r < %s); nf = (u8)(_r >> 63); zf = (_r == 0);"
                        " vf = (u8)((((u64)%s ^ _r) & ((u64)%s ^ _r)) >> 63); }" % (a, b, a, a, b))
            else:
                calc = ("{ u32 _r = (u32)%s + (u32)%s; cf = (_r < (u32)%s); nf = (u8)(_r >> 31);"
                        " zf = (_r == 0); vf = (u8)(((((u32)%s ^ _r) & ((u32)%s ^ _r)) >> 31) & 1); }"
                        % (a, b, a, a, b))
        self.lines.append("  if %s { %s } else { nf = %du; zf = %du; cf = %du; vf = %du; }"
                          % (self._cond(cond), calc, (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1))

    def _op_fpcmp(self, op: Op) -> None:
        width = op.args[0]
        a = op.a
        if op.b is None:   # fcmp Vn, #0.0
            yv = "0.0" if width == 64 else "0.0f"
        else:
            yv = "%s.%s" % (op.b, "df[0]" if width == 64 else "f[0]")
        if width == 64:
            self.lines.append("  { double _x = %s.df[0], _y = %s;"
                              " nf = (u8)(_x < _y); zf = (u8)(_x == _y);"
                              " cf = (u8)((_x == _x) && (_y == _y) ? (_x >= _y) : 1);"
                              " vf = (u8)(!(_x == _x) || !(_y == _y)); }" % (a, yv))
        else:
            self.lines.append("  { float _x = %s.f[0], _y = %s;"
                              " nf = (u8)(_x < _y); zf = (u8)(_x == _y);"
                              " cf = (u8)((_x == _x) && (_y == _y) ? (_x >= _y) : 1);"
                              " vf = (u8)(!(_x == _x) || !(_y == _y)); }" % (a, yv))

    def _scalar(self, dst: str, width: int, value: str, as_float: bool = True) -> None:
        """Write a scalar FP/SIMD result: AArch64 zeroes the rest of the 128-bit
        register.  The value is computed first because dst may also be a source."""
        if width == 64:
            ctype, field = ("double", "df[0]") if as_float else ("u64", "d[0]")
        elif width == 32:
            ctype, field = ("float", "f[0]") if as_float else ("u32", "s[0]")
        else:
            ctype, field = "u16", "h[0]"
        self.lines.append("  { %s _r = (%s)(%s); %s.d[0] = 0; %s.d[1] = 0; %s.%s = _r; }"
                          % (ctype, ctype, value, dst, dst, dst, field))

    def _op_fcvt(self, op: Op) -> None:
        dst_w, src_w = op.args
        if dst_w == 64 and src_w == 32:
            self._scalar(op.dst, 64, "(double)%s.f[0]" % op.a)
        elif dst_w == 32 and src_w == 64:
            self._scalar(op.dst, 32, "(float)%s.df[0]" % op.a)
        else:
            self.lines.append("  sw_trap(c, 0x%xULL, \"fcvt %d<-%d\");" % (self.fn.addr, dst_w, src_w))

    def _op_load(self, op: Op) -> None:
        signed = op.args[0] == "s"
        width = op.args[1] if len(op.args) > 1 else 64
        size = op.imm
        fn = "sw_load%d%s" % (size * 8, "s" if signed else "")
        if signed and width == 64 and size == 8:
            fn = "sw_load64"
        if signed and width == 32:
            # LDRSB/LDRSH Wt: sign-extend to 32 bits, upper 32 bits zero.  (It
            # used an unsigned load, so the sign was lost, and sign-filled
            # bits 32..63.)
            expr = "(u64)(u32)(s32)%s(c, %s)" % (fn, op.a)
        elif signed:
            expr = "(u64)(s64)%s(c, %s)" % (fn, op.a)
        else:
            expr = "%s(c, %s)" % (fn, op.a)
        self.lines.append(self._set(op.dst, expr))
        if op.dst and op.dst.startswith("x"):
            self.written.add(op.dst)

    def _op_store(self, op: Op) -> None:
        fn = "sw_store%d" % (op.imm * 8)
        self.lines.append("  %s(c, %s, %s);" % (fn, op.a, self._val(op.b)))

    def _op_loadv(self, op: Op) -> None:
        self.lines.append("  %s = sw_loadv(c, %s, %d);" % (op.dst, op.a, op.imm))

    def _op_storev(self, op: Op) -> None:
        self.lines.append("  sw_storev(c, %s, %s, %d);" % (op.a, op.b, op.imm))

    @staticmethod
    def _call_text(target: int, self_addr: int | None = None) -> str:
        idx = _FN_INDEX.get(target)
        if idx is None:
            return "sw_call(c, 0x%xULL);" % target
        name = _FN_NAME.get(target)
        if name:
            if _LEAF and _ABI_FACTS is not None and target in _ABI_FACTS.get("leaf", ()) and self_addr is not None:
                return "{ void %s(SwCpu *); sw_call_leaf(c, 0x%xULL, %du, %s, 0x%xULL); }" % (
                    name, target, idx, name, self_addr)
            return "{ void %s(SwCpu *); sw_call_d(c, 0x%xULL, %du, %s); }" % (name, target, idx, name)
        return "sw_call_i(c, 0x%xULL, %du);" % (target, idx)

    @staticmethod
    def _dispatch_text(target: int) -> str:
        idx = _FN_INDEX.get(target)
        if idx is None:
            return "sw_dispatch(c, 0x%xULL);" % target
        name = _FN_NAME.get(target)
        if name:
            return "{ void %s(SwCpu *); SW_TAIL_D(c, 0x%xULL, %du, %s); }" % (name, target, idx, name)
        return "sw_dispatch_i(c, 0x%xULL, %du);" % (target, idx)

    def _op_call(self, op: Op) -> None:
        self.lines += self._call_spill(op)
        self.lines.append("  " + self._call_text((op.imm + self.base) & 0xFFFFFFFFFFFFFFFF, self.addr_rb))
        self.lines += self.reload(self._live_after.get(id(op), set()))
        self.lines += self._call_reload_flags(op)

    def _op_call_indirect(self, op: Op) -> None:
        self.lines += self._call_spill(op)
        if _DIRECT and _CALL_IDX:   # per-site inline cache of the last target (sw_rt.h sw_call_r_)
            self.lines.append("  { static const void *ic_[2]; sw_call_r(c, %s, ic_); }" % self._val(op.a))
        else:
            self.lines.append("  sw_call_reg(c, %s);" % self._val(op.a))
        self.lines += self.reload(self._live_after.get(id(op), set()))
        self.lines += self._call_reload_flags(op)

    def _op_tail_call(self, op: Op) -> None:
        self.lines += self.spill()
        self.lines.append("  " + self._dispatch_text((op.imm + self.base) & 0xFFFFFFFFFFFFFFFF))
        self.lines.append("  return;")

    def _op_cond_tail_call(self, op: Op) -> None:
        self.lines.append("  if %s {" % self._cond(op.imm))
        for line in self.spill():
            self.lines.append("  " + line)
        self.lines.append("    " + self._dispatch_text((op.a + self.base) & 0xFFFFFFFFFFFFFFFF))
        self.lines.append("    return;")
        self.lines.append("  }")

    def _op_jump(self, op: Op) -> None:
        self.lines.append("  goto %s;" % self.label(op.imm))

    def _op_cond_jump(self, op: Op) -> None:
        self.lines.append("  if %s goto %s;" % (self._cond(op.imm), op.a))

    def _op_cond_tail_call_zero(self, op: Op) -> None:
        width = op.args[0]
        value = self._val(op.a, width)
        test = "((u64)%s == 0)" % value
        if op.imm == 1:
            test = "!(%s)" % test
        self.lines.append("  if (%s) {" % test)
        for line in self.spill():
            self.lines.append("  " + line)
        # op.b is module-relative (was emitted without the base: cbz/tbz to
        # another function dispatched to base-less addresses)
        self.lines.append("    " + self._dispatch_text((op.b + self.base) & 0xFFFFFFFFFFFFFFFF))
        self.lines.append("    return;")
        self.lines.append("  }")

    def _op_cond_tail_call_bit(self, op: Op) -> None:
        test = "((((u64)%s) >> %d) & 1)" % (self._val(op.a), op.imm)
        if not op.args[0]:  # args[0] is "is tbnz"; tbz branches when the bit is 0
            test = "!(%s)" % test
        self.lines.append("  if (%s) {" % test)
        for line in self.spill():
            self.lines.append("  " + line)
        # op.b is module-relative (was emitted without the base: cbz/tbz to
        # another function dispatched to base-less addresses)
        self.lines.append("    " + self._dispatch_text((op.b + self.base) & 0xFFFFFFFFFFFFFFFF))
        self.lines.append("    return;")
        self.lines.append("  }")

    def _op_cond_jump_zero(self, op: Op) -> None:
        width = op.args[0]
        value = self._val(op.a, width)
        negate = op.imm == 1
        test = "(u64)%s == 0" % value
        if negate:
            test = "!(%s)" % test
        target = op.text or ("L_%08x" % (self.fn.addr + self.fn.size))
        self.lines.append("  if (%s) goto %s;" % (test, target))

    def _op_cond_jump_bit(self, op: Op) -> None:
        value = self._val(op.a)
        # args[0] is "is tbnz": tbnz branches when the bit is 1, tbz when it is
        # 0.  This used to be read as "negate", inverting every tbz/tbnz.
        negate = not op.args[0]
        test = "((((u64)%s) >> %d) & 1)" % (value, op.imm)
        if negate:
            test = "!(%s)" % test
        target = op.text or ("L_%08x" % (self.fn.addr + self.fn.size))
        self.lines.append("  if (%s) goto %s;" % (test, target))

    def _op_ret(self, op: Op) -> None:
        self.lines.append("  goto L_ret;")

    def _op_jump_table(self, op: Op) -> None:
        # br through a recognised switch table: local cases are plain gotos
        # (registers stay in locals); anything else dispatches as before.
        v = self._val(op.a)
        self.lines.append("  switch ((u64)%s) {" % v)
        for t in op.args:
            self.lines.append("  case 0x%xULL: goto L_%08x;" % ((t + self.base) & 0xFFFFFFFFFFFFFFFF, t))
        self.lines.append("  default: break;")
        self.lines.append("  }")
        self.lines += self.spill()
        self.lines.append("  " + self._tail_reg_text(v))
        self.lines.append("  return;")

    @staticmethod
    def _tail_reg_text(v: str) -> str:
        """`br xN`: through the per-site cache (sw_rt.h SW_TAIL_R) when direct calls are on."""
        if _DIRECT and _CALL_IDX:
            return "{ static const void *ic_[2]; SW_TAIL_R(c, %s, ic_); }" % v
        return "sw_dispatch(c, %s);" % v

    def _op_jump_indirect(self, op: Op) -> None:
        self.lines += self.spill()
        self.lines.append("  " + self._tail_reg_text(self._val(op.a)))
        self.lines.append("  return;")

    def _op_svc(self, op: Op) -> None:
        self.lines += self.spill()
        self.lines.append("  sw_svc(c, %d);" % op.imm)
        self.lines += self.reload(self._live_after.get(id(op), set()))

    def _op_mrs(self, op: Op) -> None:
        name = op.args[0]
        if name == "nzcv":
            self.lines.append("  %s = ((u64)nf << 31) | ((u64)zf << 30) | ((u64)cf << 29) | ((u64)vf << 28);"
                              % op.dst)
            return
        if name.upper() == "CNTVCT_EL0":   # the game's clock: read inline (sw_rt.h sw_cntvct)
            self.lines.append(self._set(op.dst, "sw_cntvct(c)"))
        else:
            self.lines.append(self._set(op.dst, "sw_mrs(c, SW_SYS_%s)" % name.upper()))

    def _op_msr(self, op: Op) -> None:
        name = op.args[0]
        if name == "nzcv":
            v = self._val(op.a)
            self.lines.append("  nf = (u8)(((u64)%s >> 31) & 1); zf = (u8)(((u64)%s >> 30) & 1);"
                              " cf = (u8)(((u64)%s >> 29) & 1); vf = (u8)(((u64)%s >> 28) & 1);"
                              % (v, v, v, v))
            return
        self.lines.append("  sw_msr(c, SW_SYS_%s, %s);" % (name.upper(), self._val(op.a)))

    def _op_barrier(self, op: Op) -> None:
        kind = op.args[0]
        if kind in ("dmb", "dsb"):
            self.lines.append("  sw_memory_barrier();")
        elif kind == "clrex":
            self.lines.append("  c->excl_tag = 0;")

    # Exclusive monitor (sw_rt.h sw_excl_*): loads arm it, stores compare and swap.
    def _op_excl_load(self, op: Op) -> None:
        self.lines.append(self._set(op.dst, "sw_excl_load%d(c, %s)" % (op.imm, op.a)))

    def _op_excl_load_hi(self, op: Op) -> None:
        self.lines.append(self._set(op.dst, "sw_excl_load_hi%d(c, %s)" % (op.imm, op.a)))

    def _op_excl_store(self, op: Op) -> None:
        self.lines.append(self._set(op.dst, "sw_excl_store%d(c, %s, %s)" % (op.imm, op.a, self._val(op.b))))

    def _op_excl_store2(self, op: Op) -> None:
        self.lines.append(self._set(op.dst, "sw_excl_store2_%d(c, %s, %s, %s)"
                                    % (op.imm, op.a, self._val(op.b), self._val(op.c))))

    def _op_nop(self, op: Op) -> None:
        pass

    def _op_trap(self, op: Op) -> None:
        self.lines.append("  sw_trap(c, 0x%xULL, \"%s\");" % (op.imm or self.fn.addr,
                                                              op.text.replace('"', "'")))
        self.lines.append("  goto L_ret;")

    # -- FP ------------------------------------------------------------
    def _op_vcopy(self, op: Op) -> None:
        width = op.imm or 128
        if width == 128:
            self.lines.append("  %s = %s;" % (op.dst, op.a))
        elif width == 64:
            self._scalar(op.dst, 64, "%s.d[0]" % op.a, as_float=False)   # zero upper bits
        elif width == 32:
            self._scalar(op.dst, 32, "%s.s[0]" % op.a, as_float=False)
        elif width == 16:
            self.lines.append("  %s.h[0] = %s.h[0];" % (op.dst, op.a))
        else:
            self.lines.append("  %s.b[0] = %s.b[0];" % (op.dst, op.a))

    def _op_fpop(self, op: Op) -> None:
        kind = op.args[0]
        width = op.args[1]
        if width == 64:
            fmt, field = "double", "df[0]"   # df = double member (d is u64 bits)
            suffix = "64"
        else:
            fmt, field = "float", "f[0]"
            suffix = "32"
        a = "%s.%s" % (op.a, field)
        b = "%s.%s" % (op.b, field)
        expr = {
            "fadd": "%s + %s" % (a, b),
            "fsub": "%s - %s" % (a, b),
            "fmul": "%s * %s" % (a, b),
            "fdiv": "%s / %s" % (a, b),
            "fmax": "sw_fmax%s(%s, %s)" % (suffix, a, b),
            "fmin": "sw_fmin%s(%s, %s)" % (suffix, a, b),
            "fmaxnm": "sw_fmaxnm%s(%s, %s)" % (suffix, a, b),
            "fminnm": "sw_fminnm%s(%s, %s)" % (suffix, a, b),
            "fnmul": "-(%s * %s)" % (a, b),
        }.get(kind)
        if expr is None:
            self.lines.append("  sw_trap(c, 0x%xULL, \"fp op %s\");" % (self.fn.addr, kind))
            return
        if kind in ("fadd", "fsub", "fmul", "fdiv", "fnmul"):
            expr = "sw_dn%d(%s)" % (width, expr)
        self._scalar(op.dst, width, expr)

    def _op_funop(self, op: Op) -> None:
        kind = op.args[0]
        width = op.args[1]
        field = "df[0]" if width == 64 else "f[0]"
        a = "%s.%s" % (op.a, field)
        if width == 64:
            expr = {
                "fabs": "fabs(%s)" % a,
                "fneg": "(-%s)" % a,
                "fsqrt": "sqrt(%s)" % a,
                "fcvt": "(double)%s.f[0]" % op.a if False else None,
                "frintn": "sw_rint64(%s)" % a,
                "frintp": "ceil(%s)" % a,
                "frintm": "floor(%s)" % a,
                "frintz": "trunc(%s)" % a,
                "frinta": "sw_round64(%s)" % a,
                "frintx": "sw_rint64(%s)" % a,
                "frinti": "sw_rint64(%s)" % a,
            }
        else:
            expr = {
                "fabs": "fabsf(%s)" % a,
                "fneg": "(-%s)" % a,
                "fsqrt": "sqrtf(%s)" % a,
                "frintn": "(float)sw_rint64((double)%s)" % a,
                "frintp": "ceilf(%s)" % a,
                "frintm": "floorf(%s)" % a,
                "frintz": "truncf(%s)" % a,
                "frinta": "(float)sw_round64((double)%s)" % a,
                "frintx": "(float)sw_rint64((double)%s)" % a,
                "frinti": "(float)sw_rint64((double)%s)" % a,
            }
        value = expr.get(kind) if expr else None
        if value is None:
            self.lines.append("  sw_trap(c, 0x%xULL, \"fp unop %s\");" % (self.fn.addr, kind))
            return
        if kind == "fsqrt":
            value = "sw_dn%d(%s)" % (width, value)
        self._scalar(op.dst, width, value)

    def _op_fmov_imm(self, op: Op) -> None:
        from .decoder import VReg
        width = op.args[0]
        bits = vfp_expand_imm(op.imm, width)
        self._scalar(op.dst, width, "0x%xULL" % bits, as_float=False)

    def _op_fmov_to_gpr(self, op: Op) -> None:
        width = op.args[0]
        if width == 64:
            self.lines.append("  %s = %s.d[0];" % (op.dst, op.a))          # raw bits
        elif width == 32:
            self.lines.append("  %s = (u64)%s.s[0];" % (op.dst, op.a))
        else:
            self.lines.append("  %s = (u64)%s.h[0];" % (op.dst, op.a))

    def _op_fmov_from_gpr(self, op: Op) -> None:
        width = op.args[0]
        self._scalar(op.dst, width, self._val(op.a), as_float=False)

    def _op_int_to_fp(self, op: Op) -> None:
        width = op.imm
        src_width = op.args[0]
        unsigned = op.args[1]
        src = self._val(op.a, src_width)
        if unsigned:
            value = "sw_u%d_to_f%d(%s)" % (src_width, width, src)
        else:
            value = "sw_s%d_to_f%d(%s)" % (src_width, width, src)
        fbits = op.args[2] if len(op.args) > 2 else 0
        if fbits:   # fixed point: scaling by 2^-fbits after rounding is exact
            value = "(%s)(%s / %.1f)" % ("double" if width == 64 else "float", value, 2.0 ** fbits)
        self._scalar(op.dst, width, value)

    def _op_fp_to_int(self, op: Op) -> None:
        width = op.args[0]
        signed = op.args[1]
        mode = op.args[2] if len(op.args) > 2 else "z"
        src = "%s.%s" % (op.a, "df[0]" if width == 64 else "f[0]")
        # Round first for the non-truncating forms; the helper then truncates
        # and saturates.  n = nearest even, p = +inf, m = -inf, a = nearest away.
        if mode != "z":
            fn_r = {"n": "sw_rint64", "p": "ceil", "m": "floor", "a": "sw_round64"}[mode]
            src = "%s((double)%s)" % (fn_r, src)
            if width == 32:
                src = "(float)" + src
        fbits = op.args[3] if len(op.args) > 3 else 0
        if fbits:   # fixed point: exact power-of-two scale before truncation
            src = "(%s)(%s * %.1f)" % ("double" if width == 64 else "float", src, 2.0 ** fbits)
        bits = op.imm
        fn = "sw_f%d_to_%s%d" % (width, "s" if signed else "u", bits)
        self.lines.append(self._set(op.dst, "%s(%s)" % (fn, src), bits))

    # -- SIMD ----------------------------------------------------------
    def _op_vop(self, op: Op) -> None:
        kind, lane_bits, lanes = op.args
        name = "sw_v_%s_%d" % (kind, lane_bits)
        if kind in ("and", "orr", "eor", "bic", "orn", "bsl", "bit", "bif"):
            name = "sw_v_%s" % kind
        if kind in ("cmeq", "cmtst"):
            name = "sw_v_%s_%d" % (kind, lane_bits)
        if kind in ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fcmeq", "fcmge", "fcmgt"):
            name = "sw_v_%s_%d" % (kind, lane_bits)
        if kind == "addp" and lanes and lanes * lane_bits == 64:
            # .8B/.4H/.2S: pairs of a's low half, then of b's low half (the
            # 128-bit helper would pair a's whole register); vq0 clears the top
            self.lines.append("  %s = %s(sw_v_catlo(%s, %s), %s);" % (op.dst, name, op.a, op.b, op.b))
            return
        self.lines.append("  %s = %s(%s, %s);" % (op.dst, name, op.a, op.b))

    def _op_vunop(self, op: Op) -> None:
        kind, lane_bits = op.args
        if lane_bits:
            name = "sw_v_%s_%d" % (kind, lane_bits)
        else:
            name = "sw_v_%s" % kind
        self.lines.append("  %s = %s(%s);" % (op.dst, name, op.a))

    def _op_vintfp(self, op: Op) -> None:
        kind, lane_bits, lanes = op.args
        self.lines.append("  %s = sw_v_%s_%d(%s);" % (op.dst, kind, lane_bits, op.a))

    def _op_vmovi(self, op: Op) -> None:
        lane_bits = op.args[0]
        self.lines.append("  %s = sw_v_movi_%d(0x%x);" % (op.dst, lane_bits, op.imm))

    def _op_vmvni(self, op: Op) -> None:
        lane_bits = op.args[0]
        self.lines.append("  %s = sw_v_mvni_%d(0x%x);" % (op.dst, lane_bits, op.imm))

    def _op_vperm(self, op: Op) -> None:
        kind, lane_bits, lanes, index = op.args
        lanes = lanes or (128 // lane_bits)
        self.lines.append("  %s = sw_v_%s_%d(%s, %s, %d, %d);"
                          % (op.dst, kind, lane_bits, op.a, op.b, index, lanes))

    def _op_vmov(self, op: Op) -> None:
        self.lines.append("  %s = %s;" % (op.dst, op.a))

    def _op_vbit(self, op: Op) -> None:
        kind = op.args[0]
        self.lines.append("  %s = sw_v_%s(%s, %s, %s);"
                          % (op.dst, kind, op.dst, op.a, op.b))

    def _op_vmla(self, op: Op) -> None:
        kind, lane_bits = op.args
        self.lines.append("  %s = sw_v_%s_%d(%s, %s, %s);"
                          % (op.dst, kind, lane_bits, op.dst, op.a, op.b))

    def _op_vq0(self, op: Op) -> None:
        self.lines.append("  %s = sw_v_zero_hi(%s);" % (op.dst, op.a))

    def _op_vdup_gpr(self, op: Op) -> None:
        lane_bits = op.imm
        src = self._val(op.a, 64 if lane_bits == 64 else 32)   # .2D reads Xn (was always Wn)
        self.lines.append("  %s = sw_v_dup_gpr_%d(%s);" % (op.dst, lane_bits, src))

    def _op_vdup_lane(self, op: Op) -> None:
        lane_bits = op.imm
        self.lines.append("  %s = sw_v_dup_lane_%d(%s, %d);" % (op.dst, lane_bits, op.a, op.b))

    def _op_vlane_to_gpr(self, op: Op) -> None:
        lane_bits = op.imm
        signed = op.args[0]
        fn = "sw_vlane_%s_%d" % ("s" if signed else "u", lane_bits)
        self.lines.append(self._set(op.dst, "%s(%s, %d)" % (fn, op.a, op.b)))

    def _op_vfpair(self, op: Op) -> None:
        kind, lane_bits, lanes = op.args
        self.lines.append("  %s = sw_v_%s_%d(%s, %s, %d);"
                          % (op.dst, kind, lane_bits, op.a, op.b, lanes))

    def _op_adc(self, op: Op) -> None:
        """ADC/SBC(S): Rd = Rn + (sub ? ~Rm : Rm) + C, flags like ADDS."""
        width = op.imm or 64
        sub, setflags = op.args
        t, sh = ("u64", 63) if width == 64 else ("u32", 31)
        a = "(%s)%s" % (t, self._val(op.a, width))
        b = "(%s)%s%s" % (t, "~" if sub else "", self._val(op.b, width))
        body = "%s _a = %s, _b = %s; %s _t = _a + _b; %s _r = _t + (%s)cf;" % (t, a, b, t, t, t)
        if setflags:
            body += (" nf = (u8)(_r >> %d); zf = (_r == 0); cf = (u8)((_t < _a) | (_r < _t));"
                     " vf = (u8)(((~(_a ^ _b)) & (_a ^ _r)) >> %d);" % (sh, sh))
        self.lines.append("  { %s %s }" % (body, self._set(op.dst, "_r", width).strip()))

    def _op_fmadd(self, op: Op) -> None:
        kind, width = op.args
        fld = "df[0]" if width == 64 else "f[0]"
        fma = "fma" if width == 64 else "fmaf"
        n, m, a = ("%s.%s" % (op.a, fld), "%s.%s" % (op.b, fld), "%s.%s" % (op.c, fld))
        expr = {"fmadd": "%s(%s, %s, %s)" % (fma, n, m, a),
                "fmsub": "%s(-%s, %s, %s)" % (fma, n, m, a),
                "fnmadd": "%s(-%s, %s, -%s)" % (fma, n, m, a),   # ARM: -a - n*m, exact zero is +0
                "fnmsub": "%s(%s, %s, -%s)" % (fma, n, m, a)}[kind]
        self._scalar(op.dst, width, "sw_dn%d(%s)" % (width, expr))

    def _op_vcrypto(self, op: Op) -> None:
        kind, nregs = op.args
        if kind in ("sha1h", "aesmc", "aesimc"):
            self.lines.append("  %s = sw_%s(%s);" % (op.dst, kind, op.a))
        elif nregs == 2:
            self.lines.append("  %s = sw_%s(%s, %s);" % (op.dst, kind, op.dst, op.a))
        else:
            self.lines.append("  %s = sw_%s(%s, %s, %s);" % (op.dst, kind, op.dst, op.a, op.b))

    def _op_velem(self, op: Op) -> None:
        kind, lane_bits, index, lanes = op.args
        self.lines.append("  %s = sw_v_%s_%d(%s, %s, %s, %d, %d);"
                          % (op.dst, kind, lane_bits, op.dst, op.a, op.b, index, lanes))

    def _op_vreduce(self, op: Op) -> None:
        kind, lane_bits, lanes = op.args
        self.lines.append("  %s = sw_v_%s_%d(%s, %d);" % (op.dst, kind, lane_bits, op.a, lanes))

    def _op_vdup_scalar(self, op: Op) -> None:
        fld = {8: "b", 16: "h", 32: "s", 64: "d"}[op.imm]
        self.lines.append("  { sw_v128 _s = %s; memset(&%s, 0, sizeof %s); %s.%s[0] = _s.%s[%d]; }"
                          % (op.a, op.dst, op.dst, op.dst, fld, fld, op.b))

    def _op_vf2(self, op: Op) -> None:
        kind, lane_bits, lanes = op.args
        name = kind if kind in ("scvtf", "ucvtf") or kind.endswith("z") else kind
        suffix = "" if kind.endswith("z") and kind.startswith("fcm") else "2"
        self.lines.append("  %s = sw_v_%s%s_%d(%s, %d);" % (op.dst, name, suffix, lane_bits, op.a, lanes))

    def _op_vtbl(self, op: Op) -> None:
        tab, q = op.args
        self.lines.append("  { sw_v128 _t[4] = {%s}; %s = sw_v_tbl(_t, %d, %s, %d); }"
                          % (", ".join(tab), op.dst, len(tab), op.a, q))

    def _op_fccmp(self, op: Op) -> None:
        width, nzcv = op.args
        fld = "df[0]" if width == 64 else "f[0]"
        t = "double" if width == 64 else "float"
        calc = ("{ %s _x = %s.%s, _y = %s.%s; nf = (u8)(_x < _y); zf = (u8)(_x == _y);"
                " cf = (u8)((_x == _x) && (_y == _y) ? (_x >= _y) : 1); vf = (u8)(!(_x == _x) || !(_y == _y)); }"
                % (t, op.a, fld, op.b, fld))
        self.lines.append("  if %s %s else { nf = %du; zf = %du; cf = %du; vf = %du; }"
                          % (self._cond(op.imm), calc, (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1))

    def _op_fcsel(self, op: Op) -> None:
        width = op.args[0]
        field = "d[0]" if width == 64 else "s[0]"
        self._scalar(op.dst, width, "%s ? %s.%s : %s.%s"
                     % (self._cond(op.imm), op.a, field, op.b, field), as_float=False)

    def _op_vlogic_imm(self, op: Op) -> None:
        kind, lane_bits = op.args
        const = "sw_v_movi_%d(0x%xULL)" % (lane_bits, op.imm)
        fn = "sw_v_orr" if kind == "orr" else "sw_v_bic"
        self.lines.append("  %s = %s(%s, %s);" % (op.dst, fn, op.dst, const))

    def _op_vlane_ins(self, op: Op) -> None:
        field = {8: "b", 16: "h", 32: "s", 64: "d"}[op.imm]
        self.lines.append("  { sw_v128 _s = %s; %s.%s[%d] = _s.%s[%d]; }"
                          % (op.a, op.dst, field, op.c, field, op.b))

    def _op_vgpr_to_lane(self, op: Op) -> None:
        lane_bits = op.imm
        self.lines.append("  sw_vgpr_to_lane_%d(&%s, %d, %s);"
                          % (lane_bits, op.dst, op.b, self._val(op.a, 64 if lane_bits == 64 else 32)))

    def _op_vcall(self, op: Op) -> None:
        fn, q, esz = op.args
        self.lines.append("  %s = %s(%s, %s, %s, %d, %d, %d);"
                          % (op.dst, fn, op.c, op.a, op.b, op.imm, q, esz))

    def _op_crc32(self, op: Op) -> None:
        nbytes, castagnoli = op.args
        self.lines.append(self._set(op.dst, "sw_crc32(%s, %s, %d, %d)"
                                    % (self._val(op.a, 32), self._val(op.b, 64), nbytes, castagnoli), 32))

    def _op_vlane_copy(self, op: Op) -> None:
        lane_bits = op.imm
        self.lines.append("  sw_vlane_copy_%d(&%s, %s, %d);" % (lane_bits, op.dst, op.a, op.b))


def vfp_expand_imm(imm8: int, width: int) -> int:
    """VFPExpandImm: a=sign, (b,c,d)=signed exponent, (e,f,g,h)=fraction nibble.

    Verified against hardware behaviour via Unicorn (tests/test_fp_imm.py).
    """
    sign = (imm8 >> 7) & 1
    e3 = (imm8 >> 4) & 7
    exp_delta = e3 - 8 if e3 >= 4 else e3
    frac = imm8 & 0xF
    if width == 32:
        return (sign << 31) | ((0x80 + exp_delta) << 23) | (frac << 19)
    # double: exponent NOT(b):Replicate(b,8):cd = 0x400 + delta (used 0x80)
    return (sign << 63) | ((0x400 + exp_delta) << 52) | (frac << 48)


def emit_function(fn: Function, base: int = 0) -> str:
    return Emitter(fn, base).emit()


def abi_function_facts(fn: Function, base: int = 0) -> tuple:
    """ABI facts of one function (see _ABI): liveness only, no C."""
    return Emitter(fn, base, facts_mode=True).abi_facts()
