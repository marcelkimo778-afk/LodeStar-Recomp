"""Standalone runtime smoke tests.

These run the built application (build/sw_app.exe) and assert the execution
audit invariants: the binary loads real module images, reports that no ARM64
interpreter/JIT is linked, and exposes native service bindings.
"""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent  # Project root
EXE = ROOT / "build_work" / "out" / ("LodeStar Game.exe" if sys.platform == "win32" else "sw_app")


@unittest.skipUnless(EXE.is_file(), "standalone application not built")
class RuntimeTests(unittest.TestCase):
    def run_app(self, *args: str, timeout: int = 60) -> subprocess.CompletedProcess:
        return subprocess.run([str(EXE), *args], cwd=str(EXE.parent),
                              capture_output=True, text=True, errors="replace",
                              timeout=timeout)

    def test_audit(self):
        out = self.run_app("--audit")
        self.assertEqual(out.returncode, 0, out.stderr)
        text = out.stdout
        self.assertIn("standalone execution audit", text)
        self.assertIn("arm64 interpreter : none linked", text)
        self.assertIn("mode              : standalone native", text)
        # main, sdk, subsdk0, subsdk1 + the synthetic imports table
        self.assertRegex(text, r"modules loaded    : [4-9]")
        self.assertRegex(text, r"recompiled funcs  : [1-9]\d*")
        self.assertRegex(text, r"guest memory      : [1-9]\d* KiB mapped")

    def test_service_table(self):
        out = self.run_app("--list-services")
        self.assertEqual(out.returncode, 0, out.stderr)
        lines = [l for l in out.stdout.splitlines() if l.strip()]
        self.assertGreater(len(lines), 10)
        # libc (memcpy, ...) now resolves to the game's own sdk exports; the
        # native table holds Horizon services such as nn::os.
        self.assertTrue(any("_ZN2nn2os" in l for l in lines))

    def test_controller_poll(self):
        out = self.run_app("--input-test", timeout=30)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("controller connected:", out.stdout)

    def test_call_unknown_address_fails_cleanly(self):
        out = self.run_app("--call", "0xdead0000")
        self.assertNotEqual(out.returncode, 0)
        self.assertIn("no recompiled function", out.stderr + out.stdout)


if __name__ == "__main__":
    unittest.main()
