"""IR -> C emission regressions.

These tests target control-flow forms that are easy to drop on the floor
because they live in ``Block.terminator`` rather than ``Block.ops``.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # project root
sys.path.insert(0, str(ROOT / "Engine" / "Source"))

from swrecomp.arm64.emit import emit_function  # noqa: E402
from swrecomp.arm64.lift import Lifter  # noqa: E402


def _branch_word(target: int, pc: int = 0) -> int:
    """Encode ``b target`` at ``pc``."""
    return 0x14000000 | (((target - pc) >> 2) & 0x03FFFFFF)


class TailBranchEmissionTests(unittest.TestCase):
    def test_out_of_line_branch_stub_is_not_empty(self):
        """A one-instruction ``b`` alias stub must dispatch to its target.

        sdk and game modules expose many out-of-line thunks of the form
        ``foo: b real_implementation``.  The lifter stores that branch as the
        block terminator; if the emitter ignores terminators the generated
        function returns immediately and callers observe a bogus success.
        """
        text = _branch_word(0x1000).to_bytes(4, "little")
        fn = Lifter(text).lift_function(0)
        self.assertEqual(len(fn.tail_calls), 1)
        c = emit_function(fn, base=0x100000)
        self.assertIn("sw_dispatch(c, 0x101000ULL);", c)
        self.assertNotIn("/*no-dst*/", c)

    def test_cur_fn_is_an_absolute_address(self):
        """The audit/dispatch trace must report linked addresses, not offsets."""
        text = _branch_word(0x1000).to_bytes(4, "little")
        fn = Lifter(text).lift_function(0)
        c = emit_function(fn, base=0x100000)
        self.assertIn("c->cur_fn = 0x100000ULL;", c)

    def test_intra_function_branch_keeps_explicit_label(self):
        """A backwards ``b`` inside the function must emit a real ``goto``."""
        # 0x0: b 0x8 ; 0x4: nop ; 0x8: ret
        words = [0x14000002, 0xD503201F, 0xD65F03C0]
        text = b"".join(w.to_bytes(4, "little") for w in words)
        fn = Lifter(text).lift_function(0)
        c = emit_function(fn)
        self.assertIn("goto L_00000008;", c)


class AddressingModeTests(unittest.TestCase):
    """Post-indexed accesses must read the un-updated base register.

    ``ldr x21, [sp], #0x30`` loads from the current ``sp`` and only then adds
    ``0x30``.  Using ``sp + 0x30`` as the access address both reads the wrong
    slot and shifts every later stack access by one frame.
    """

    # Real encoding lifted from sdk 0x347e00: ldr x21, [sp], #0x30
    POST_INDEX_LDR = 0xF84307F5
    # Real encoding from sdk 0x347df8: ldp x29, x30, [sp, #0x20]
    OFFSET_LDP = 0xA9427BFD
    RET = 0xD65F03C0

    def _emit(self, words):
        text = b"".join(w.to_bytes(4, "little") for w in words)
        return emit_function(Lifter(text).lift_function(0))

    def test_post_index_load_uses_unmodified_base(self):
        c = self._emit([self.POST_INDEX_LDR, self.RET])
        # The access address is the base itself ...
        self.assertIn("t1 = (u64)(sp);", c)
        self.assertIn("sw_load64(c, t1)", c)
        # ... and the writeback adds the displacement afterwards.
        self.assertIn("t2 = (u64)(sp + 0x0000000000000030ULL);", c)
        self.assertIn("sp = (u64)(t2);", c)

    def test_post_index_load_does_not_add_displacement_to_access(self):
        c = self._emit([self.POST_INDEX_LDR, self.RET])
        load_line = next(l for l in c.splitlines() if "sw_load64" in l)
        self.assertNotIn("0x30", load_line)
        self.assertNotIn("0x0000000000000030", load_line)

    def test_offset_load_still_adds_displacement(self):
        # ldr x0, [x1, #8] -> 0xF9400420
        c = self._emit([0xF9400420, self.RET])
        self.assertIn("sw_load64(c, t1)", c)
        self.assertIn("t1 = (u64)(x1 + 0x0000000000000008ULL);", c)

    def test_post_index_ldp_loads_from_base_then_writes_back(self):
        # ldp x29, x30, [sp], #0x10 -> 0xA8C17BFD (the usual epilogue)
        c = self._emit([0xA8C17BFD, self.RET])
        self.assertIn("t1 = (u64)(sp);", c)
        self.assertIn("x29 = (u64)(sw_load64(c, t1));", c)
        self.assertIn("x30 = (u64)(sw_load64(c, t2));", c)
        self.assertIn("t3 = (u64)(sp + 0x0000000000000010ULL);", c)
        self.assertIn("sp = (u64)(t3);", c)


if __name__ == "__main__":
    unittest.main()


class CfgExtentTests(unittest.TestCase):
    """cfg_extent: whole functions bounded by the next known entry."""

    # 0x00 cbz w0, 0x0c ; 0x04 b 0x08 ; 0x08 ret ; 0x0c nop ; 0x10 <next function>
    CODE = b"".join(w.to_bytes(4, "little") for w in
                    (0x34000060, 0x14000001, 0xD65F03C0, 0xD503201F, 0xD65F03C0))

    def test_block_after_terminator_is_included(self):
        end, insns = Lifter(self.CODE).cfg_extent(0, boundary=0x10)
        self.assertEqual([i.addr for i in insns], [0x0, 0x4, 0x8, 0xC])
        self.assertEqual(end, 0x10)

    def test_extent_stops_at_boundary(self):
        end, _ = Lifter(self.CODE).cfg_extent(0, boundary=0xC)
        self.assertEqual(end, 0xC)

    def test_fall_out_becomes_tail_call_not_return(self):
        """Code that falls through past the function's end must continue at
        the next address; it used to drop into the epilogue and return."""
        fn = Lifter(self.CODE).lift_function(0, boundary=0x10)
        self.assertIn(0x10, fn.tail_calls)
        terms = [b.terminator.kind for b in fn.blocks if b.terminator is not None]
        self.assertIn("tail_call", terms)


class TestBitBranchTests(unittest.TestCase):
    """tbz branches when the bit is 0, tbnz when it is 1 (both used to be inverted)."""

    @staticmethod
    def _emit(word: int) -> str:
        # word at 0x0, then ret at 0x4 and ret at the branch target 0x8
        code = b"".join(w.to_bytes(4, "little") for w in (word, 0xD65F03C0, 0xD65F03C0))
        return emit_function(Lifter(code).lift_function(0, boundary=0xC))

    def test_tbz_branches_on_clear_bit(self):
        c = self._emit(0x36000048)            # tbz w8, #0, 0x8
        self.assertIn("if (!(((((u64)x8) >> 0) & 1))) goto L_00000008;", c)

    def test_tbnz_branches_on_set_bit(self):
        c = self._emit(0x37000048)            # tbnz w8, #0, 0x8
        self.assertIn("if (((((u64)x8) >> 0) & 1)) goto L_00000008;", c)


class JumpTableScanTests(unittest.TestCase):
    """_scan_jump_tables decodes clang's compressed switch tables."""

    def test_byte_table(self):
        from swrecomp.pipeline.analyze import _scan_jump_tables

        words = [
            0x7100091F,  # 0x00 cmp w8, #2
            0x54000108,  # 0x04 b.hi 0x24
            0x90000009,  # 0x08 adrp x9, 0x0 (page of this code)
            0x91010129,  # 0x0c add x9, x9, #0x40  (table at 0x40)
            0x1000008A,  # 0x10 adr x10, 0x20 (anchor)
            0x3868692B,  # 0x14 ldrb w11, [x9, x8]
            0x8B0B094A,  # 0x18 add x10, x10, x11, lsl #2
            0xD61F0140,  # 0x1c br x10
        ] + [0xD65F03C0] * 8   # 0x20.. case bodies (ret)
        code = b"".join(w.to_bytes(4, "little") for w in words)
        code += bytes([0, 1, 3]) + b"\0" * 13  # table at 0x40: entries 0, 1, 3

        class FakeNso:
            text = code
            def read_vaddr(self, off, size):
                return code[off:off + size]

        self.assertEqual(_scan_jump_tables(FakeNso()), {0x20, 0x24, 0x2C})


class SystemInstructionTests(unittest.TestCase):
    """op0 == 0 'msr' encodings are hints/barriers, not system registers."""

    def test_clrex_and_dmb_do_not_trap(self):
        for word in (0xD503305F, 0xD5033BBF):   # clrex, dmb ish
            code = word.to_bytes(4, "little") + (0xD65F03C0).to_bytes(4, "little")
            c = emit_function(Lifter(code).lift_function(0, boundary=8))
            self.assertNotIn("unsupported", c, hex(word))


class BitfieldTests(unittest.TestCase):
    """SBFM/UBFM/BFM use tmask from DecodeBitMasks (d = S - R)."""

    @staticmethod
    def _emit(word: int) -> str:
        code = word.to_bytes(4, "little") + (0xD65F03C0).to_bytes(4, "little")
        return emit_function(Lifter(code).lift_function(0, boundary=8))

    def test_sxtw_fills_upper_half(self):
        # sxtw x9, w9 used to OR in 0 for negative values (zero-extension)
        self.assertIn("0xffffffff00000000", self._emit(0x93407D29).lower())

    def test_decode_bit_masks_tmask(self):
        from swrecomp.arm64.decoder import decode_bit_masks
        self.assertEqual(decode_bit_masks(1, 63, 4, 64)[1], 0x0FFFFFFFFFFFFFFF)   # lsr #4
        self.assertEqual(decode_bit_masks(1, 19, 8, 64)[1], 0xFFF)                # ubfx #8, #12
        self.assertEqual(decode_bit_masks(1, 31, 0, 64)[1], 0xFFFFFFFF)           # sxtw


class PartialVectorWriteTests(unittest.TestCase):
    def test_lane_insert_after_call_reloads_destination(self):
        """``blr x8; mov v3.s[3], v0.s[0]`` must reload v3 after the call.

        The insert keeps lanes 0-2 of v3 (an HFA matrix row returned in
        q0-q3).  Treating it as a full write left v3 stale after the call, so
        object and camera translations came out ~0 (main+0x2957dec).
        """
        words = [0xD63F0100,   # blr x8
                 0x6E1C0403,   # mov v3.s[3], v0.s[0]
                 0x3D800FE3,   # str q3, [sp, #0x30]
                 0xD65F03C0]   # ret
        text = b"".join(w.to_bytes(4, "little") for w in words)
        fn = Lifter(text).lift_function(0)
        c = emit_function(fn, base=0x100000)
        call = "sw_call_r(c, x8, ic_);" if "sw_call_r(c, x8, ic_);" in c else "sw_call_reg(c, x8);"   # site cache or not
        after_call = c.split(call, 1)[1]
        self.assertIn("v3 = c->v[3];", after_call.split("sw_storev", 1)[0])


class ExclusiveMonitorTests(unittest.TestCase):
    """LDXR/STXR must go through the exclusive monitor (compare and swap), not a
    plain store that always succeeds - guest threads may run on several cores."""

    def _emit(self, words):
        text = b"".join(w.to_bytes(4, "little") for w in words + [0xD65F03C0])
        return emit_function(Lifter(text).lift_function(0), base=0x100000)

    def test_ldxr_stxr(self):
        c = self._emit([0x885F7C08,    # ldxr w8, [x0]
                        0x11000508,    # add w8, w8, #1
                        0x88097C08])   # stxr w9, w8, [x0]
        self.assertIn("sw_excl_load4(c,", c)
        self.assertIn("x9 = (u64)(sw_excl_store4(c,", c)
        self.assertNotIn("x9 = (u64)(0x", c)          # the old "always succeeds"

    def test_pair_and_clrex(self):
        c = self._emit([0xC87F2408,    # ldxp x8, x9, [x0]
                        0xC8292408,    # stxp w9, x8, x9, [x0]
                        0xD503305F])   # clrex
        self.assertIn("sw_excl_load8(c,", c)
        self.assertIn("sw_excl_load_hi8(c,", c)
        self.assertIn("sw_excl_store2_8(c,", c)
        self.assertIn("c->excl_tag = 0;", c)


class PairwiseHalfWidthTests(unittest.TestCase):
    def _c(self, word: int) -> str:
        text = word.to_bytes(4, "little") + (0xD65F03C0).to_bytes(4, "little")   # ...; ret
        return emit_function(Lifter(text).lift_function(0), base=0x100000)

    def test_addp_8b_pairs_low_halves_of_both_operands(self):
        """addp v0.8b, v1.8b, v2.8b: lanes 0-3 from v1's low half, 4-7 from v2's
        (found by the lockstep sweep at main+0x1c3b0: lanes 4-7 came from v1's top)."""
        c = self._c(0x0E22BC20)
        self.assertIn("sw_v_catlo(", c)

    def test_addp_16b_uses_full_registers(self):
        c = self._c(0x4E22BC20)
        self.assertNotIn("sw_v_catlo(", c)
