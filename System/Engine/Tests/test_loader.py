#!/usr/bin/env python3
"""
Loader tests — verify NSO loading, relocation application, function discovery, init_array counts.

Run once the build is ready (sw_app.exe exists).
"""

import unittest
import sys
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "Engine" / "Source"))

from swrecomp.formats.exefs import open_exefs  # noqa: E402


class LoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exefs = open_exefs(ROOT / "build_work/exefs_100")
        cls.cache_dir = ROOT / "Build" / "Cache" / "modules"
        cls.cache_dir.mkdir(parents=True, exist_ok=True)

    def test_nso_modules_load(self):
        """Core ExeFS modules load and have valid MOD0 headers."""
        modules = ["main", "rtld", "sdk", "subsdk0", "subsdk1"]
        for name in modules:
            with self.subTest(module=name):
                nso = self.exefs.load_nso(name, self.cache_dir)
                self.assertIsNotNone(nso.text)
                self.assertGreater(len(nso.text), 0)
                self.assertIsNotNone(nso.rodata)
                self.assertIsNotNone(nso.data)
                # MOD0 at text offset 0x8 for most modules; rtld may not have one
                mod0_off = 0x8
                if name == "rtld":
                    # rtld doesn't have a MOD0 header; just verify it loads
                    pass
                else:
                    self.assertGreater(len(nso.text), mod0_off + 0x48)
                    mod0_magic = int.from_bytes(nso.text[mod0_off:mod0_off+4], "little")
                    self.assertEqual(mod0_magic, 0x30444f4d, f"{name}: bad MOD0 magic at 0x8")  # "MOD0"

    def test_main_module_size_matches_sw_program(self):
        """Main module total size matches expected (verified by build.py)."""
        nso = self.exefs.load_nso("main", self.cache_dir)
        # Total = text + rodata + data + bss
        total = len(nso.text) + len(nso.rodata) + len(nso.data) + nso.bss_size
        # Observed value from actual module
        self.assertEqual(total, 0x430a5c0, f"main: got {total:#x}, expected 0x430a5c0")

    def test_module_sizes_nonzero(self):
        """All modules have nonzero text/rodata/data."""
        for name in ["main", "rtld", "sdk", "subsdk0", "subsdk1"]:
            with self.subTest(module=name):
                nso = self.exefs.load_nso(name, self.cache_dir)
                self.assertGreater(len(nso.text), 0)
                self.assertGreater(len(nso.rodata), 0)
                self.assertGreater(len(nso.data), 0)

    def test_dynamic_section_parsing(self):
        """Each module parses dynamic section without error."""
        for name in ["main", "sdk", "subsdk0"]:
            with self.subTest(module=name):
                nso = self.exefs.load_nso(name, self.cache_dir)
                dyn = nso.parse_dynamic()
                self.assertIsNotNone(dyn, f"{name}: no dynamic section")
                # Check for required tags
                tags = dyn.tags
                # DT_RELA(7), DT_RELASZ(8), DT_STRTAB(5), DT_SYMTAB(6)
                self.assertIn(7, tags, f"{name}: DT_RELA missing")
                self.assertIn(8, tags, f"{name}: DT_RELASZ missing")
                self.assertIn(5, tags, f"{name}: DT_STRTAB missing")
                self.assertIn(6, tags, f"{name}: DT_SYMTAB missing")

    def test_relocations_apply_cleanly(self):
        """Relocations apply without zero-value relocations (for non-weak symbols)."""
        exe = ROOT / "build_work" / "out" / "LodeStar Game.exe"
        if not exe.exists():
            self.skipTest("LodeStar Game.exe not built yet")
        
        result = subprocess.run([str(exe), "--reloc-audit", "--max-dispatch", "10000"],
                              capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, f"--reloc-audit failed: {result.stderr}")
        
        # Check output for zero-value relocations
        self.assertNotIn("ZERO-VALUE RELOCATION", result.stdout,
            "Found zero-value relocations (should only be weak undefined)")

    def test_init_array_counts_main(self):
        """Main module has expected init_array count (observed: 1892 entries)."""
        nso = self.exefs.load_nso("main", self.cache_dir)
        dyn = nso.parse_dynamic()
        
        # Find DT_INIT_ARRAYSZ (tag 27 = 0x1b)
        init_array_sz = dyn.tags.get(27, 0)
        
        entry_count = init_array_sz // 8
        self.assertEqual(entry_count, 1892, f"main init_array: {entry_count} entries, expected 1892")

    def test_init_array_counts_sdk(self):
        """SDK module has expected init_array count (observed: 115 entries)."""
        nso = self.exefs.load_nso("sdk", self.cache_dir)
        dyn = nso.parse_dynamic()
        
        init_array_sz = dyn.tags.get(27, 0)
        
        entry_count = init_array_sz // 8
        self.assertEqual(entry_count, 115, f"sdk init_array: {entry_count} entries, expected 115")

    def test_init_array_counts_subsdk0(self):
        """subsdk0 module has expected init_array count (observed: 44 entries)."""
        nso = self.exefs.load_nso("subsdk0", self.cache_dir)
        dyn = nso.parse_dynamic()
        
        init_array_sz = dyn.tags.get(27, 0)
        
        entry_count = init_array_sz // 8
        self.assertEqual(entry_count, 44, f"subsdk0 init_array: {entry_count} entries, expected 44")

    def test_nnmain_and_nninitstartup_symbols_exist(self):
        """nnMain and nninitStartup symbols exist in main module dynsym."""
        nso = self.exefs.load_nso("main", self.cache_dir)
        symtab = nso.symbols()
        
        names = {s.name for s in symtab}
        
        self.assertIn("nnMain", names, "nnMain not in dynsym")
        self.assertIn("nninitStartup", names, "nninitStartup not in dynsym")
        
        # Check sizes
        for sym in symtab:
            if sym.name == "nnMain":
                self.assertEqual(sym.size, 0xc4, f"nnMain size {sym.size:#x} != 0xc4")
            elif sym.name == "nninitStartup":
                self.assertEqual(sym.size, 0x248, f"nninitStartup size {sym.size:#x} != 0x248")

    def test_relocation_types_exist_main(self):
        """Main module has all expected relocation types."""
        nso = self.exefs.load_nso("main", self.cache_dir)
        dyn = nso.parse_dynamic()
        relocs = dyn.relocations
        
        types_found = set()
        for r in relocs:
            types_found.add(r[1])  # r = (offset, type, addend, symbol)
        
        R_AARCH64_RELATIVE = 1027
        R_AARCH64_GLOB_DAT = 1025
        R_AARCH64_JUMP_SLOT = 1026
        R_AARCH64_ABS64 = 257
        
        expected = {R_AARCH64_RELATIVE, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64}
        for t in expected:
            self.assertIn(t, types_found, f"Missing relocation type {t}")


if __name__ == "__main__":
    unittest.main(verbosity=2)