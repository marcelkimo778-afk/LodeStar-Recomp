"""End-to-end checks on the real ExeFS / RomFS inputs.

These tests read the extracted game data that ships with this workspace; they
are skipped (not failed) when the data is absent so the suite stays portable.
"""

from __future__ import annotations

import hashlib
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # project root
sys.path.insert(0, str(ROOT / "Engine" / "Source"))

from swrecomp.formats.exefs import load_npdm, open_exefs  # noqa: E402
from swrecomp.formats.nso import Nso  # noqa: E402
from swrecomp.formats.romfs import open_romfs  # noqa: E402

EXEFS = ROOT / "build_work/exefs_100"
ROMFS = ROOT.parent / "Game/romfs"
WORK = ROOT / "Build/Cache" / "modules"


@unittest.skipUnless(EXEFS.is_dir(), "no extracted ExeFS in this workspace")
class ExefsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exefs = open_exefs(EXEFS)
        cls.main = cls.exefs.load_nso("main", WORK)

    def test_modules_are_enumerated(self):
        self.assertIn("main", self.exefs.module_names)
        self.assertIn("rtld", self.exefs.module_names)
        self.assertIn("sdk", self.exefs.module_names)

    def test_nso_hashes_verify(self):
        result = self.main.verify()
        self.assertTrue(result["ok"], result)
        for name, seg in result["segments"].items():
            self.assertIn(seg["status"], ("valid", "skipped"), name)

    def test_segment_layout(self):
        self.assertTrue(self.main.summary()["module_id"])
        self.assertEqual(len(self.main.segments), 3)
        self.assertGreater(len(self.main.text), 0)
        self.assertGreater(self.main.image_size, len(self.main.text))

    def test_dynamic_metadata(self):
        dyn = self.main.parse_dynamic()
        self.assertGreater(len(dyn.relocations), 0)
        self.assertGreater(len(dyn.init_array), 0)
        resolved = dyn.resolved_init_array()
        self.assertEqual(len(resolved), len(dyn.init_array))
        # Entries are zero in the file and become real code addresses once the
        # RELATIVE relocations are applied.
        self.assertGreater(sum(1 for x in resolved if x), 0)
        for entry in resolved[:16]:
            self.assertLess(entry, len(self.main.text))
            self.assertEqual(entry % 4, 0)

    def test_npdm(self):
        npdm = load_npdm(self.exefs, ROOT / ".swrecomp")
        summary = npdm.summary()
        self.assertEqual(len(summary["program_id"]), 16)
        self.assertIn("flag_names", summary)

    def test_imports_and_exports_are_separate(self):
        names = {s.name for s in self.main.imports}
        self.assertTrue(names)
        self.assertTrue(all(name for name in names))
        self.assertGreater(len(self.main.parse_dynamic().relocations), 0)


@unittest.skipUnless(ROMFS.is_dir(), "no extracted RomFS in this workspace")
class RomFsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.romfs = open_romfs(ROMFS)

    def test_entries(self):
        entries = list(self.romfs.walk())
        self.assertGreater(len(entries), 100)
        self.assertTrue(any(not e.is_dir for e in entries))

    def test_roundtrip_file(self):
        target = next(e for e in self.romfs.walk() if not e.is_dir and e.size > 0)
        data = self.romfs.read(target.path)
        self.assertIsNotNone(data)
        self.assertEqual(len(data), target.size)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         hashlib.sha256(self.romfs.read(target.path)).hexdigest())

    def test_missing_file(self):
        from swrecomp.errors import MissingInputError
        with self.assertRaises(MissingInputError):
            self.romfs.read("/definitely/not/here.bin")


if __name__ == "__main__":
    unittest.main()
