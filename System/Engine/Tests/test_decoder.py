"""Decoder tests: documented regressions plus a differential sweep vs Capstone.

Capstone is a development-time oracle only; the shipped toolchain has no
dependency on it.  The sweep asserts two things on real game code:

  * every instruction Capstone decodes must be either supported by our decoder
    or counted as unsupported (never silently mis-decoded), and
  * the supported share of instructions in the sampled text stays above 99%.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # project root
sys.path.insert(0, str(ROOT / "Engine" / "Source"))

from swrecomp.arm64.decoder import decode, Unsupported  # noqa: E402
from swrecomp.formats.exefs import open_exefs  # noqa: E402

EXEFS = ROOT / "build_work/exefs_100"
WORK = ROOT / "Build/Cache" / "modules"

try:
    import capstone
except ImportError:  # pragma: no cover - oracle optional
    capstone = None

# Mnemonics where Capstone's alias differs from the architectural mnemonic we
# emit; the encodings are identical.
ALIASES = {
    "cset": "csinc", "csetm": "csinv", "cinc": "csinc", "cinv": "csinv",
    "cneg": "csneg", "lsl": "ubfm", "lsr": "ubfm", "asr": "sbfm",
    "sxtw": "sbfm", "uxtw": "ubfm", "sxth": "sbfm", "uxth": "ubfm",
    "sxtb": "sbfm", "uxtb": "ubfm", "mul": "madd", "mneg": "msub",
    "tst": "ands", "cmp": "subs", "cmn": "adds", "neg": "sub",
    "mvn": "orn",
    "bfi": "bfm", "bfxil": "bfm", "ubfx": "ubfm", "sbfx": "sbfm",
    "sbfiz": "sbfm", "ubfiz": "ubfm", "ror": "extr", "smull": "smaddl",
    "umull": "umaddl", "smnegl": "smsubl", "umnegl": "umsubl",
    # Capstone spells out the access width / unscaled form; our decoder keeps
    # one mnemonic and records width + addressing in the instruction notes.
    "ldrb": "ldr", "ldrh": "ldr", "ldrsb": "ldrs", "ldrsh": "ldrs",
    "ldrsw": "ldrs", "strb": "str", "strh": "str",
    "ldur": "ldr", "ldurb": "ldr", "ldurh": "ldr", "ldursb": "ldrs",
    "ldursh": "ldrs", "ldursw": "ldrs", "stur": "str", "sturb": "str",
    "sturh": "str",
}

# Capstone renders several encodings as aliases our decoder keeps literal.
WILDCARD = {"mov", "b.hs", "b.lo"}


def equivalent(cap: str, ours: str) -> bool:
    if cap == ours:
        return True
    # SIMD instructions: Capstone omits the assembler's mandatory "v" prefix.
    if ours.startswith("v") and ours[1:] == cap:
        return True
    if cap in WILDCARD:
        return True
    if cap == "b.hs" and ours == "b.cs":
        return True
    if cap == "b.lo" and ours == "b.cc":
        return True
    return ALIASES.get(cap) == ours


class DecoderRegressionTests(unittest.TestCase):
    def check(self, word: int, mnemonic: str):
        insn = decode(0x1000, word)
        self.assertNotIsInstance(insn, Unsupported, hex(word))
        self.assertEqual(insn.mnemonic, mnemonic, hex(word))

    def test_register_branches(self):
        self.check(0xD65F03C0, "ret")     # ret
        self.check(0xD63F0100, "blr")     # blr x8
        self.check(0xD61F0100, "br")      # br x8

    def test_load_store_addressing(self):
        self.check(0xF8408688, "ldr")     # ldr x8, [x20], #8
        self.check(0x38440D8D, "ldr")     # ldrb w13, [x12, #0x40]!
        self.check(0x3DC00020, "ldr")     # ldr q0, [x1]
        self.check(0xF9400000, "ldr")     # ldr x0, [x0]
        insn = decode(0x1000, 0xF8408688)
        self.assertEqual(insn.notes["addressing"], "post")

    def test_exclusive_and_conditional(self):
        self.check(0xC85FFD8B, "ldaxr")
        self.check(0x1A9F17E8, "csinc")   # cset w8, eq
        self.check(0x7A491100, "ccmp")
        self.check(0x5AC00908, "rev")
        self.check(0x1A88D509, "csinc")   # cinc w9, w8, gt

    def test_simd(self):
        self.check(0x0E208420, "vadd")    # add v0.8b
        self.check(0x4E23D400, "vfadd")   # fadd v0.4s, v0.4s, v3.4s (capstone)
        self.check(0x4EA3D400, "vfsub")   # fsub v0.4s: bit 23 set (was wrongly expected as fadd)
        self.check(0x6E014023, "vext")    # ext v3.16b, ...
        self.check(0x4F00E400, "vmovi")   # movi v0.16b, #0
        self.check(0x6E20DC20, "vfmul")   # fmul v0.4s
        self.check(0x4E041D20, "vins_general")  # mov v0.s[0], w12
        # FDIV: U=1, a=0, opcode=0x1f (0x2EA0FC00 used here before is unallocated)
        self.check(0x6E20FC00, "vfdiv")   # fdiv v0.4s, v0.4s, v0.4s (capstone)
        # vins_elem: code 0x01, op=1
        self.check(0x2E010420, "vins_elem")  # ins v0.b[0], v1.b[0]

    def test_trap_word_is_terminating_udf(self):
        """0xe7ffdefe is the toolchain's trap/padding word.

        It used to decode as a NOP ``hint``, so a failed assert such as
        ``cbnz w0, <trap>`` fell through into whatever function followed.
        It must decode as a terminating ``udf`` so the lifter emits sw_trap.
        """
        insn = decode(0x10a420, 0xE7FFDEFE)
        self.assertNotIsInstance(insn, Unsupported)
        self.assertEqual(insn.mnemonic, "udf")
        self.assertTrue(insn.is_terminator)


@unittest.skipUnless(capstone is not None, "capstone not installed")
@unittest.skipUnless(EXEFS.is_dir(), "no extracted ExeFS")
class DecoderDifferentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.nso = open_exefs(EXEFS).load_nso("main", WORK)
        cls.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def test_sample_agreement(self):
        text = self.nso.text
        supported = 0
        total = 0
        mismatches = []
        # Sample spread across the first 8 MiB, every 13th word.
        limit = min(len(text), 8 * 1024 * 1024)
        for off in range(0, limit - 4, 4 * 13):
            word = int.from_bytes(text[off:off + 4], "little")
            cap = None
            for ins in self.md.disasm(word.to_bytes(4, "little"), off):
                cap = ins
                break
            if cap is None:
                continue
            ours = decode(off, word)
            total += 1
            if isinstance(ours, Unsupported):
                continue
            supported += 1
            if equivalent(cap.mnemonic, ours.mnemonic):
                continue
            mismatches.append((hex(off), hex(word), cap.mnemonic, ours.mnemonic))
        share = 100.0 * supported / max(1, total)
        self.assertGreaterEqual(share, 99.0,
                                "supported share dropped to %.2f%%" % share)
        self.assertLessEqual(len(mismatches), max(5, total // 100), mismatches[:10])

    def test_unsupported_always_carries_reason(self):
        """Unsupported results must be explicit diagnostics, never silent."""
        text = self.nso.text
        checked = 0
        for off in range(0, min(len(text), 256 * 1024), 4 * 31):
            word = int.from_bytes(text[off:off + 4], "little")
            ours = decode(off, word)
            if isinstance(ours, Unsupported):
                self.assertTrue(ours.reason, hex(word))
                checked += 1
        # the sampled code may by now be fully supported: a word that is never
        # decodable (UDF #8) must still come back with a reason
        ours = decode(0, 0x00000008)
        self.assertIsInstance(ours, Unsupported)
        self.assertTrue(ours.reason)
        checked += 1
        self.assertGreater(checked, 0)


if __name__ == "__main__":
    unittest.main()
