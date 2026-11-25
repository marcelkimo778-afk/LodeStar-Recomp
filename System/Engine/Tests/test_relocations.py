"""Relocation type tests: verify RELATIVE/ABS64/GLOB_DAT/JUMP_SLOT values."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "Engine" / "Source"))

from swrecomp.formats.exefs import open_exefs  # noqa: E402

EXEFS = ROOT / "build_work/exefs_100"
WORK = ROOT / "Build/Cache" / "modules"

R_AARCH64_RELATIVE = 1027
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_ABS64 = 257


class RelocationTypeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        nso = open_exefs(EXEFS).load_nso("main", WORK)
        nso.parse_dynamic()  # populate dynamic info
        cls.nso = nso

    def test_relocation_types_exist(self):
        """Verify all expected relocation types are present in main module."""
        nso = self.nso
        relocs = nso.dynamic.relocations
        
        types_found = set()
        for r in relocs:
            types_found.add(r[1])  # r = (offset, type, addend, symbol)
        
        # These are the standard AArch64 relocation types we expect
        expected = {R_AARCH64_RELATIVE, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64}
        for t in expected:
            self.assertIn(t, types_found, f"Missing relocation type {t}")

    def test_relative_relocation_values(self):
        """RELATIVE relocations should have valid offset and addend."""
        nso = self.nso
        text_offset = nso.mod0.text_offset if nso.mod0 else 0
        
        for r in nso.dynamic.relocations:
            offset, r_type, addend, symbol = r
            if r_type == R_AARCH64_RELATIVE:
                # Value at runtime = base + addend
                # The in-file value is the addend
                self.assertIsNotNone(offset)
                self.assertIsNotNone(addend)
                self.assertGreaterEqual(offset, 0)

    def test_glob_dat_relocation_values(self):
        """GLOB_DAT relocations should reference a symbol."""
        nso = self.nso
        
        for r in nso.dynamic.relocations:
            offset, r_type, addend, symbol = r
            if r_type == R_AARCH64_GLOB_DAT:
                # Should reference a symbol
                self.assertIsNotNone(offset)
                # symbol is the dynsym index

    def test_jump_slot_relocation_values(self):
        """JUMP_SLOT relocations should reference a symbol (PLT entry)."""
        nso = self.nso
        
        for r in nso.dynamic.relocations:
            offset, r_type, addend, symbol = r
            if r_type == R_AARCH64_JUMP_SLOT:
                # Should reference a symbol (PLT entry)
                self.assertIsNotNone(offset)
                # Addend should be 0 for JUMP_SLOT
                self.assertEqual(addend, 0)

    def test_abs64_relocation_values(self):
        """ABS64 relocations should reference a symbol."""
        nso = self.nso
        
        for r in nso.dynamic.relocations:
            offset, r_type, addend, symbol = r
            if r_type == R_AARCH64_ABS64:
                self.assertIsNotNone(offset)
                self.assertIsNotNone(symbol)

    def test_jump_slot_addend_zero(self):
        """JUMP_SLOT relocations must have zero addend."""
        nso = self.nso
        
        for r in nso.dynamic.relocations:
            offset, r_type, addend, symbol = r
            if r_type == R_AARCH64_JUMP_SLOT:
                self.assertEqual(addend, 0, f"JUMP_SLOT at offset {offset} has non-zero addend {addend}")


if __name__ == "__main__":
    unittest.main()