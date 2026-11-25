"""updater: version compare, what an update needs, protected paths, applying a release zip."""
import importlib.util
import zipfile
from pathlib import Path

_spec = importlib.util.spec_from_file_location("updater", Path(__file__).resolve().parents[1] / "launcher" / "updater.py")
up = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(up)


def test_versions():
    assert up.newer("v0.2.0", "0.1.0") and up.newer("0.10.0", "0.9.9") and not up.newer("0.1.0", "0.1.0")
    assert up.vtuple("v1.2") == (1, 2, 0)


def test_kind_of_change():
    assert up.kind_of_change(["System/launcher/launcher.py", "README.md"]) == "launcher"
    assert up.kind_of_change(["System/launcher/x.py", "System/Engine/Runtime/src/sw_thread.c"]) == "rebuild"
    assert up.kind_of_change(["System/build_work/build.py"]) == "rebuild"
    assert up.kind_of_change(["System/Engine/Runtime/gfx/vk/vk_fsr.c", "System/Engine/Source/swrecomp/arm64/emit.py"]) == "recompile"


def test_protected_and_apply(tmp_path):
    for p in ("Game/exefs/main", "Mods/x.txt", "Settings/launcher.json", "System/build_work/out/save/main",
              "System/build_work/gen/sw_chunk_0001.c"):
        assert up.protected(p)
    assert not up.protected("System/launcher/launcher.py") and not up.protected("LodeStar.exe")
    root = tmp_path / "install"
    (root / "Settings").mkdir(parents=True)
    (root / "Settings" / "launcher.json").write_text("mine")
    z = tmp_path / "r.zip"
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("LodeStar/System/launcher/launcher.py", "new")
        f.writestr("LodeStar/Settings/launcher.json", "theirs")
        f.writestr("LodeStar/../evil.txt", "x")
    assert up.apply_files(z, root) == 1
    assert (root / "System/launcher/launcher.py").read_text() == "new"
    assert (root / "Settings/launcher.json").read_text() == "mine"
    assert not (tmp_path / "evil.txt").exists()
