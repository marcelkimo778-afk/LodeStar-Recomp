"""Differential test of lifter+emitter against the Unicorn ARM64 emulator.

Runs Engine/Tools/difftest.py on a small sample of real game instructions.
It caught msub/smsubl, umulh, extr, ccmp flag writeback, bitfield (tmask)
and tbz/tbnz bugs that unit tests had missed.  Needs unicorn, the extracted
ExeFS and a host C compiler.
"""
from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TOOL = ROOT / "Engine" / "Tools" / "difftest.py"

try:
    import unicorn  # noqa: F401
except ImportError:
    unicorn = None


@unittest.skipUnless(unicorn is not None and (ROOT / "build_work/exefs_100/main").is_file(),
                     "needs unicorn and an extracted ExeFS")
class DifferentialTest(unittest.TestCase):
    def test_integer_instructions_match_unicorn(self):
        r = subprocess.run([sys.executable, str(TOOL), "--count", "600", "--seed", "7"],
                           capture_output=True, text=True, timeout=900)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
