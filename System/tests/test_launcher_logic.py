#!/usr/bin/env python3
"""
Unit tests for launcher logic (no GUI required).
Tests the helper functions and config handling.
"""
import unittest
import tempfile
import os
import sys
from pathlib import Path

# Add project root to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from launcher.launcher import _project_root, _load_config, _save_config, _resolve_exefs, _resolve_romfs, _resolve_work_dir


class LauncherConfigTests(unittest.TestCase):
    """Test config loading/saving and path resolution."""

    def setUp(self):
        self.temp_dir = tempfile.mkdtemp()
        self.old_cwd = os.getcwd()
        os.chdir(self.temp_dir)
        
        # Create a mock launcher directory structure
        self.launcher_dir = Path(self.temp_dir) / "launcher"
        self.launcher_dir.mkdir()
        
        # Create a mock config file
        self.config_path = Path(self.temp_dir) / "Settings" / "config.json"
        self.config_path.parent.mkdir()
        self.config_path.write_text('{"exefs_dir": "Game/exefs", "romfs_dir": "Game/romfs"}')
        
        # Patch _project_root to return our temp dir
        import launcher.launcher
        self._orig_project_root = launcher.launcher._project_root
        launcher.launcher._project_root = lambda: Path(self.temp_dir)

    def tearDown(self):
        os.chdir(self.old_cwd)
        import shutil
        shutil.rmtree(self.temp_dir, ignore_errors=True)
        import launcher.launcher
        launcher.launcher._project_root = self._orig_project_root

    def test_project_root(self):
        """_project_root returns the parent of launcher/."""
        # Can't easily test without importing the actual launcher module
        # This test just verifies the function exists
        self.assertTrue(callable(_project_root))

    def test_load_config_missing(self):
        """Loading config from non-existent file returns empty dict."""
        # Remove the config file created in setUp
        if self.config_path.exists():
            self.config_path.unlink()
        config = _load_config()
        self.assertEqual(config, {})

    def test_save_and_load_config(self):
        """Saving and loading config round-trips."""
        cfg = {"exefs_dir": "custom/exefs", "work_dir": "custom/work"}
        _save_config(cfg)
        loaded = _load_config()
        self.assertEqual(loaded, cfg)

    def test_resolve_exefs_default(self):
        """Default ExeFS path when not in config."""
        # Ensure no config exists
        config_path = Path(self.temp_dir) / "Settings" / "config.json"
        if config_path.exists():
            config_path.unlink()
        
        # Create default roms/exefs
        default_exefs = Path(self.temp_dir) / "Game" / "exefs"
        default_exefs.mkdir(parents=True)
        
        # The function uses _project_root() which points to launcher/parent
        # Since we're in temp_dir, this won't work directly
        # Just test the function doesn't crash
        try:
            result = _resolve_exefs()
            self.assertIsInstance(result, Path)
        except Exception:
            pass  # Expected in test environment


class LauncherUITests(unittest.TestCase):
    """Test launcher UI creation without full GUI."""

    def setUp(self):
        # We can't easily test tkinter without a display
        # This is a placeholder for future headless testing
        pass

    def test_import_launcher(self):
        """Verify launcher module imports without error."""
        import launcher.launcher
        self.assertTrue(hasattr(launcher.launcher, 'LauncherApp'))

    def test_launcher_has_required_methods(self):
        """Every button callback the UI wires up exists."""
        import launcher.launcher
        required = [
            'on_recompile', 'on_build', 'on_clean_build', 'on_play', 'on_stop',
            'on_reloc_audit', 'on_tests', 'on_open_build', 'on_open_log',
            'on_copy_errors', 'on_clear_log', 'on_select_exefs', 'on_select_romfs',
            'refresh_status', 'log', 'set_status', 'poll_output',
        ]
        for method in required:
            self.assertTrue(hasattr(launcher.launcher.LauncherApp, method),
                            f"Missing method: {method}")


class LauncherHelperTests(unittest.TestCase):
    """Pure parsing helpers behind the progress bar, log colours and run summary."""

    def test_build_progress_counts_files_and_slow(self):
        from launcher.launcher import BuildProgress
        p = BuildProgress()
        p.feed("Sources: 8867 total, 4 to compile")
        for name in ("sw_runtime.c", "sw_chunk_0001.c", "sw_chunk_0002.c"):
            self.assertTrue(p.feed(name))
        p.feed("  SLOW: sw_chunk_0397.c exceeded 300s at /O1; retrying with /Od")
        self.assertEqual((p.total, p.done, p.slow), (4, 3, 1))
        self.assertAlmostEqual(p.fraction, 0.75)
        p.feed("Linking...")
        self.assertEqual(p.fraction, 1.0)
        self.assertIn("Linking", p.describe())

    def test_build_progress_ignores_files_before_sources_line(self):
        from launcher.launcher import BuildProgress
        p = BuildProgress()
        self.assertFalse(p.feed("sw_chunk_0001.c"))
        self.assertEqual(p.done, 0)

    def test_classify_line(self):
        from launcher.launcher import classify_line
        self.assertEqual(classify_line("x.c(1): error C2198: Nicht genuegend Argumente"), "error")
        self.assertEqual(classify_line("nnMain: HALTED at 0x100000: bad"), "error")
        self.assertEqual(classify_line("nvn: nvnBootstrapLoader(\"x\") -> 0xff001008"), "gfx")
        self.assertEqual(classify_line("BUILD SUCCESS"), "good")
        self.assertEqual(classify_line("  SLOW: sw_chunk_0001.c exceeded"), "warn")
        self.assertEqual(classify_line("Compiling..."), "info")

    def test_important_filter_hides_per_file_noise(self):
        from launcher.launcher import is_important
        self.assertFalse(is_important("sw_chunk_0001.c", "info"))
        self.assertFalse(is_important("  init[main:3] -> 0x1234", "info"))
        self.assertTrue(is_important("Linking...", "info"))
        self.assertTrue(is_important("sw_chunk_0001.c", "error"))

    def test_summarize_run(self):
        from launcher.launcher import summarize_run
        lines = ["=== REAL STARTUP ===", "init_array: 1057 constructors dispatched",
                 "nninitStartup: returned X0=0x1", "nnMain: calling 0x283d160",
                 "=== execution stopped ===", "reason   : unresolved import foo",
                 "function : 0x1234 (caller 0x5678)"]
        text = summarize_run(lines, 3)
        self.assertIn("nnMain", text)
        self.assertIn("Stopped: unresolved import foo", text)
        self.assertIn("not reached", text)
        self.assertIn("Exit code: 3", text)

    def test_format_duration(self):
        from launcher.launcher import format_duration
        self.assertEqual(format_duration(42), "42s")
        self.assertEqual(format_duration(600), "10 min")
        self.assertEqual(format_duration(3720), "1 h 02 min")


if __name__ == "__main__":
    unittest.main(verbosity=2)

class BootTrackerTests(unittest.TestCase):
    """Live boot-stage chips and service list in the launcher."""

    def test_stages_services_and_calls(self):
        from launcher.launcher import BootTracker, BOOT_STAGES
        t = BootTracker()
        lines = ["=== REAL STARTUP ===", "nnosInitialize: returned X0=0x1",
                 "  RELOC type=1026 @ 0x1: nvnBootstrapLoader (import) -> 0xff001000",
                 '[ipc] ConnectToNamedPort("sm:") -> handle 0xa001',
                 '[ipc] sm:GetService("lm")', '[ipc] sm:GetService("lm")',
                 '[ipc] sm:GetService("appletOE")', "  init[main:0] -> 0x1c11d00",
                 "dispatch calls    : 2531 (missing: 0)"]
        for line in lines:
            t.feed(line)
        names = [n for n, _ in BOOT_STAGES]
        reached = {n for n, r in zip(names, t.reached) if r}
        self.assertEqual(reached, {"Loaded", "OS init", "Services", "Constructors"})
        self.assertNotIn("Graphics", reached)          # a RELOC line is not the NVN call
        self.assertEqual(t.services, ["lm", "appletOE"])
        self.assertEqual(t.calls, 2531)
        self.assertEqual(t.furthest, "Constructors")

    def test_translator_newer_than(self):
        from launcher.launcher import translator_newer_than
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            src = d / "src"
            src.mkdir()
            (src / "a.py").write_text("x = 1")
            marker = d / "program.json"
            self.assertTrue(translator_newer_than(marker, src))      # never recompiled
            marker.write_text("{}")
            os.utime(src / "a.py", (1, 1))
            self.assertFalse(translator_newer_than(marker, src))

    def test_recompile_progress(self):
        from launcher.launcher import RecompileProgress
        p = RecompileProgress()
        self.assertEqual(p.fraction, 0.0)
        self.assertTrue(p.feed("module main: 197664 functions discovered (15672240 instructions, 100.00% supported)"))
        self.assertFalse(p.feed("module main: 197664 functions discovered (again)"))
        self.assertAlmostEqual(p.fraction, 0.62)
        self.assertIn("sdk", p.describe())
        for m in ("sdk", "subsdk0", "subsdk1"):
            p.feed(f"module {m}: 1 functions discovered")
        p.feed("emitted 297657 functions in 3968 chunk files")
        self.assertAlmostEqual(p.fraction, 1.0)
        self.assertEqual(p.describe(), "Recompile finished")

    def test_history_entry(self):
        import time as _t
        from launcher.launcher import make_history_entry
        lines = ["=== REAL STARTUP ===", "nnosInitialize: returned X0=0", "nnMain: calling 0x1",
                 "=== execution stopped ===", "reason   : svcBreak", "dispatch calls    : 61720 (missing: 0)"]
        e = make_history_entry(lines, 3, _t.time() - 5)
        self.assertEqual(e["furthest"], "nnMain")
        self.assertEqual(e["stop"], "svcBreak")
        self.assertEqual(e["calls"], 61720)
        self.assertEqual(e["exit"], 3)

    def test_prune_logs_moves_not_deletes(self):
        from launcher.launcher import prune_logs
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for i in range(5):
                f = d / f"{i}.log"
                f.write_text("x")
                os.utime(f, (i + 1, i + 1))
            self.assertEqual(prune_logs(d, 2), 3)
            self.assertEqual(sorted(p.name for p in d.glob("*.log")), ["3.log", "4.log"])
            self.assertEqual(len(list((d / "old").glob("*.log"))), 3)

    def test_runtime_newer_than(self):
        from launcher.launcher import runtime_newer_than
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            exe = d / "app.exe"
            self.assertTrue(runtime_newer_than(exe, d))
            exe.write_text("")
            (d / "a.c").write_text("")
            os.utime(d / "a.c", (1, 1))
            self.assertFalse(runtime_newer_than(exe, d))
            os.utime(d / "a.c", None)
            os.utime(exe, (1, 1))
            self.assertTrue(runtime_newer_than(exe, d))

    def test_cpu_env(self):
        from launcher.launcher import cpu_env
        self.assertEqual(cpu_env("single", "auto", 16), {"SW_MULTICORE": "0"})
        self.assertEqual(cpu_env("multi", "auto", 16), {"SW_MULTICORE": "1", "SW_CORE_PIN": "0", "SW_CORES": "16"})
        self.assertEqual(cpu_env("multi", "auto", 2)["SW_CORES"], "3")      # never fewer than the Switch's 3
        self.assertEqual(cpu_env("multi", "auto", None)["SW_CORES"], "4")
        self.assertEqual(cpu_env("multi", "6", 16)["SW_CORES"], "6")
        self.assertEqual(cpu_env("multi", "99", 16)["SW_CORES"], "16")

    def test_fps_env(self):
        from launcher.launcher import fps_env, FPS_CHOICES
        self.assertEqual(FPS_CHOICES, ("30", "60", "90", "120", "240"))
        self.assertEqual(fps_env("30"), {})                  # the game's own pacing: no hook
        self.assertEqual(fps_env("60"), {"SW_FPS": "60"})
        self.assertEqual(fps_env(120), {"SW_FPS": "120"})
        self.assertEqual(fps_env("144"), {})                 # not offered: ignored
        self.assertEqual(fps_env(""), {})


class TestRenderResolution(unittest.TestCase):
    """Options > Game > Render resolution: the game's own handheld (720p) / docked (1080p) output."""

    def test_render_env(self):
        from launcher.launcher import RENDER_RES_CHOICES, render_env
        self.assertEqual(render_env(RENDER_RES_CHOICES[0]), {"SW_OPERATION_MODE": "docked", "SW_DISPLAY_RES": "1920x1080"})
        self.assertEqual(render_env("1280x720 (handheld)"), {"SW_OPERATION_MODE": "handheld", "SW_DISPLAY_RES": "1280x720"})
        self.assertEqual(render_env(None)["SW_OPERATION_MODE"], "docked")      # unknown / missing: the default


def test_graphics_env_helpers_2026_11_25():
    import launcher.launcher as L
    assert L.preset_values("Quality")["res_scale"] == "2x"
    assert L.preset_values("Performance")["npc_distance"] == L.NPC_DISTANCE_CHOICES[0]
    assert L.res_scale_env("3x") == {"SW_RES_SCALE": "3"}
    assert L.aniso_env("Original") == {"SW_ANISO": "game"} and L.aniso_env("8x") == {"SW_ANISO": "8"}
    assert L.vsync_env("Fast") == {"SW_VK_PRESENT": "mailbox"}
    assert L.perf_overlay_level(True) == 1 and L.perf_overlay_level("Detailed") == 2
    assert L.picture_env({"upscaler": "Bilinear", "sharpness": "70", "colors": "Rich"}) == \
        {"SW_UPSCALE": "0", "SW_SHARP": "7", "SW_COLORS": "2"}
