"""Launcher side of mod support (launcher/mods_cfg.py) and the runtime's --list-mods against the test fixture."""
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "launcher"))
import mods_cfg  # noqa: E402

FIXTURE = ROOT / "Engine" / "Tests" / "mods_fixture"
EXE = ROOT / "build_work" / "out" / "LodeStar Game.exe"


def test_load_defaults_and_comments(tmp_path):
    assert mods_cfg.load(tmp_path / "missing.json")["enabled"] is True
    p = tmp_path / "mods.json"
    p.write_text('{\n  // a comment\n  "enabled": false,\n  "mods": ["A"]\n}\n', encoding="utf-8")
    cfg = mods_cfg.load(p)
    assert cfg["enabled"] is False and cfg["mods"] == ["A"]
    p.write_text("{ broken", encoding="utf-8")
    assert mods_cfg.load(p)["enabled"] is True              # broken file: defaults, never a crash


def test_rows_order_and_config_roundtrip(tmp_path):
    listing = {"mods": [
        {"name": "B", "priority": 2, "enabled": False, "why_off": "disabled in the mod config", "romfs": True,
         "romfs_files": 3, "exefs": False, "cheats": []},
        {"name": "A", "priority": 1, "enabled": True, "romfs": False, "exefs": True, "exefs_files": 1,
         "cheats": [{"name": "Money", "enabled": True}, {"name": "HP", "enabled": False}]},
    ]}
    rows = mods_cfg.rows_from_listing(listing)
    assert [r["name"] for r in rows] == ["A", "B"]
    assert mods_cfg.describe(rows[0]) == "1 patch file(s), 2 cheat(s), 1 on"
    assert mods_cfg.move(rows, 0, 1) == 1 and [r["name"] for r in rows] == ["B", "A"]
    assert mods_cfg.move(rows, 1, 1) == 1                    # cannot move past the end
    cfg = mods_cfg.config_from_rows({"enabled": True, "disabled": ["x"], "cheats": {}}, rows)
    assert cfg["mods"] == [{"name": "B", "enabled": False}, {"name": "A", "enabled": True, "cheats": ["Money"]}]
    assert "disabled" not in cfg and "cheats" not in cfg
    rows[0]["marker"] = True                                 # off by its .disabled file: config keeps it "on"
    assert mods_cfg.config_from_rows({}, rows)["mods"][0] == {"name": "B", "enabled": True}
    rows[0]["marker"] = False
    mods_cfg.save(tmp_path / "Config" / "mods.json", cfg)
    assert mods_cfg.load(tmp_path / "Config" / "mods.json")["mods"] == cfg["mods"]
    assert mods_cfg.game_env(tmp_path / "Config") == {"SW_MODS_CONFIG": str(tmp_path / "Config" / "mods.json")}
    assert mods_cfg.game_env(tmp_path / "nothing") == {}


@pytest.mark.skipif(not EXE.is_file(), reason="LodeStar Game.exe not built")
def test_list_mods_on_fixture(tmp_path):
    cfg = tmp_path / "mods.json"
    shutil.copyfile(FIXTURE / "mods.json", cfg)
    res = mods_cfg.list_mods(EXE, cfg, {"SW_MODS_DIR": str(FIXTURE / "mods")})
    names = [m["name"] for m in sorted(res["mods"], key=lambda m: m["priority"])]
    assert names[:3] == ["ModBeta", "ModAlpha", "ModOff"]       # the config's order first
    assert set(names) == {"ModBeta", "ModAlpha", "ModOff", "atmosphere", "ModRyu", "ModYuzu", "PatchSet"}
    off = next(m for m in res["mods"] if m["name"] == "ModOff")
    assert off["enabled"] is False and off["why_off"] == ".disabled file in the mod folder"
    assert res["conflicts"] == 1
    # the fixture's patches / cheats are for a made-up build id: the real game reports them as other versions
    assert any("another game version" in m for m in res["messages"])
    # "enabled": false still lists (the launcher shows mods while the switch is off)
    cfg.write_text(json.dumps({"enabled": False, "search_default_dirs": False}), encoding="utf-8")
    res = mods_cfg.list_mods(EXE, cfg, {"SW_MODS_DIR": str(FIXTURE / "mods")})
    assert res["enabled"] is False and len(res["mods"]) == 7


@pytest.mark.skipif(sys.platform != "win32", reason="MSVC test")
def test_runtime_mod_test_bat():
    """Engine/Tests/test_mods.bat: sw_mods.c + sw_romfs.c built against stubs (romfs overlay, patches, cheats)."""
    bat = ROOT / "Engine" / "Tests" / "test_mods.bat"
    vc = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    vc2 = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")
    if not vc.is_file() and not vc2.is_file():
        pytest.skip("no MSVC")
    r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, errors="replace", timeout=600)
    assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-2000:]
    assert "test_mods: OK" in r.stdout


@pytest.mark.skipif(sys.platform != "win32", reason="MSVC test")
def test_modbrowser_core_bat():
    """Engine/Tests/test_modbrowser.bat: mock source, install pipeline, version compatibility (offline)."""
    bat = ROOT / "Engine" / "Tests" / "test_modbrowser.bat"
    vc = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    vc2 = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")
    if not vc.is_file() and not vc2.is_file():
        pytest.skip("no MSVC")
    r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True, errors="replace", timeout=600)
    assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-2000:]
    assert "test_modbrowser: OK" in r.stdout
