"""launcher/shader_prep.py: cache status (invalidation) and the Build Shaders progress parser."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from launcher import shader_prep  # noqa: E402

GPU = "10de:2f04:9a4a8000:aa"


def write(d: Path, name: str, obj: dict) -> None:
    (d / name).write_text(json.dumps(obj), encoding="utf-8")


def manifest(**kw) -> dict:
    m = {"format": 2, "status": "complete", "variants": 100, "recorded": 10, "census": 90,
         "translator": "X", "gpu": GPU, "date": "2026-11-10 21:22"}
    m.update(kw)
    return m


class StatusTests(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        self.d = Path(self.t.name)

    def tearDown(self):
        self.t.cleanup()

    def test_none(self):
        self.assertEqual(shader_prep.cache_status(self.d, "X", GPU)["state"], "none")
        write(self.d, "prepared.json", {"status": "partial"})
        self.assertEqual(shader_prep.cache_status(self.d, "X", GPU)["button"], "Build Shaders")

    def test_ready(self):
        write(self.d, "prepared.json", manifest())
        write(self.d, "census.json", {"programs": 90})
        st = shader_prep.cache_status(self.d, "X", GPU)
        self.assertEqual((st["state"], st["button"]), ("ready", "Rebuild Shaders"))
        self.assertNotIn("all", st["text"].lower().split())          # no claim of complete coverage
        self.assertEqual(shader_prep.cache_status(self.d, "X", None)["state"], "ready")   # driver not known yet

    def test_translator_changed(self):
        write(self.d, "prepared.json", manifest())
        self.assertEqual(shader_prep.cache_status(self.d, "Y", GPU)["state"], "rebuild")

    def test_game_files_changed(self):
        write(self.d, "prepared.json", manifest())
        write(self.d, "census.json", {"programs": 95})
        self.assertEqual(shader_prep.cache_status(self.d, "X", GPU)["state"], "rebuild")

    def test_old_format(self):
        write(self.d, "prepared.json", manifest(format=1))
        self.assertEqual(shader_prep.cache_status(self.d, "X", GPU)["state"], "rebuild")

    def test_driver_changed_then_warmed(self):
        write(self.d, "prepared.json", manifest())
        st = shader_prep.cache_status(self.d, "X", "10de:2f04:9b000000:bb")
        self.assertEqual((st["state"], st["button"]), ("refresh", "Refresh Shaders"))
        write(self.d, "warmed.json", {"gpu": "10de:2f04:9b000000:bb"})
        self.assertEqual(shader_prep.cache_status(self.d, "X", "10de:2f04:9b000000:bb")["state"], "ready")

    def test_gpu_key_parse(self):
        self.assertEqual(shader_prep.gpu_key_from_output("x\n[gpu] key 10de:1:2:ab\n[gpu] name N\n"), "10de:1:2:ab")
        self.assertEqual(shader_prep.gpu_key_from_output("[gpu] error: no Vulkan device\n"), "")


class ProgressTests(unittest.TestCase):
    def test_build_sequence(self):
        p = shader_prep.ShaderPrepProgress()
        self.assertTrue(p.feed("[census] scanning 1000/2005 files, 5000 shaders"))
        self.assertIn("Scanning", p.describe())
        p.feed("[census] done: 11387 shader programs (vertex 1) from 3 containers")
        p.feed("[prep] translating: 11599 variants (already translated ones are reused)")
        p.feed("[gfx] gfx_shader: prepare: 5000 of 11599 pairs, 4000 translated, 1000 ms")
        self.assertIn("5,000 of 11,599", p.describe())
        f1 = p.fraction
        p.feed("[gfx] vk_pipeline: stage library prebuild preparation progress: 4000 of 11553 variants in 10 ms; kept")
        self.assertGreater(p.fraction, f1)
        self.assertIn("driver cache", p.describe())
        p.feed("[prep] done: 11599 variants (212 recorded, 11387 census) in 81.9 s; x/prepared.json")
        self.assertEqual(p.fraction, 1.0)
        self.assertTrue(p.describe().startswith("Done"))

    def test_failed_and_warm(self):
        p = shader_prep.ShaderPrepProgress()
        p.feed("[prep] failed after 2.0 s (rc 1); the previous preparation record is kept")
        self.assertTrue(p.describe().startswith("Failed"))
        w = shader_prep.ShaderPrepProgress(warm_only=True)
        w.feed("[prep] warming: driver cache from x")
        w.feed("[gfx] vk_pipeline: stage library prebuild preparation done: 10 of 10 variants in 5 ms")
        self.assertGreater(w.fraction, 0.9)


if __name__ == "__main__":
    unittest.main()
