"""The launcher's mod browser (launcher/modbrowser_cli.py) against the real mb_cli.exe and the offline mock source:
search, pagination, details, thumbnails, install (+ refused executables, blocked versions, install anyway),
install from file, installed list, enable / disable, uninstall, offline behaviour."""
import json
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "launcher"))
import modbrowser_cli as mbc  # noqa: E402

EXE = ROOT / "build_work" / "out" / "mb_cli.exe"
FIXTURE = ROOT / "Engine" / "Tests" / "modbrowser_fixture"
TARGET = "AABBCCDD11223344556677889900AABBCCDDEEFF"

pytestmark = pytest.mark.skipif(not EXE.is_file(), reason="mb_cli.exe not built (python build_work/build.py)")


@pytest.fixture
def cli(tmp_path):
    (tmp_path / "mods").mkdir()
    return mbc.Cli(exe=EXE, mods_root=tmp_path / "mods", settings=tmp_path / "modbrowser.json", cache=tmp_path / "cache",
                   mock=FIXTURE, extra_env={})


def _call(cli, cmd, **kw):
    kw.setdefault("build_id", TARGET)
    return cli.call(cmd, **kw)


def _install(cli, mod_id, allow=False):
    cli_args = cli._args("install", source="mock", id=mod_id, build_id=TARGET, allow="1" if allow else None)
    import subprocess
    proc = subprocess.Popen(cli_args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    lines = list(mbc.read_lines(proc))
    assert lines and lines[-1]["type"] in ("result", "error")
    assert any(ln.get("stage") == "download" for ln in lines if ln["type"] == "progress")
    return lines[-1]


def test_sources_and_search(cli):
    s = _call(cli, "sources")
    ids = [x["id"] for x in s["sources"]]
    assert ids[0] == "mock" and "gamebanana" in ids and "nexus" in ids and "github" in ids
    gb = next(x for x in s["sources"] if x["id"] == "gamebanana")
    assert not gb["available"] and gb["why"] and not gb["verified"]          # needs a game id; unverified
    r = _call(cli, "search", source="mock", per_page=10, page=1)
    assert len(r["mods"]) == 10 and r["more"] and r["mods"][0]["id"] == "1"
    assert r["mods"][0]["compat"]["compat"] == "match"
    r4 = _call(cli, "search", source="mock", per_page=10, page=4)
    assert r4["mods"] and not r4["more"]
    assert [m["id"] for m in _call(cli, "search", source="mock", q="shiny")["mods"]] == ["1"]
    with pytest.raises(mbc.CliError):
        _call(cli, "search", source="gamebanana")                          # the error text, never a crash


def test_details_and_thumbnail(cli):
    d = _call(cli, "details", source="mock", id="2")
    assert d["name"] == "Cool Mod" and d["files"] and d["compat"]["compat"] == "mismatch"   # names 1.1.1
    p = cli.thumbnail("mock", d["thumb_url"])
    assert p.is_file() and p.read_bytes()[1:4] == b"PNG"
    assert cli.thumbnail("mock", d["thumb_url"]) == p                         # cached


def test_install_update_uninstall_and_refusals(cli, tmp_path):
    res = _install(cli, "1")
    assert res["state"] == "done" and res["version"]["compat"] == "match"
    folder = Path(res["folders"][0])
    meta = json.loads((folder / "mod_meta.json").read_text(encoding="utf-8"))
    assert meta["source"] == "mock" and meta["mod_id"] == "1" and meta["author"] == "Tester" and meta["compat"] == "match"
    res = _install(cli, "7")                                                  # executables refused, rest installed
    assert res["state"] == "done" and any("installer.exe" in r for r in res["refused"])
    res = _install(cli, "5")                                                  # zip slip
    assert res["state"] == "failed" and "unsafe" in res["message"]
    res = _install(cli, "9")                                                  # exefs patch for another build
    assert res["state"] == "blocked"
    assert _install(cli, "9", allow=True)["state"] == "done"                  # install anyway
    inst = _call(cli, "installed")["mods"]
    assert {m["name"] for m in inst} == {"Shiny Textures", "Has An Exe", "Wrong Build Patch"}
    wrong = next(m for m in inst if m["name"] == "Wrong Build Patch")
    assert wrong["compat"] == "mismatch"
    _call(cli, "enable", folder=wrong["folder"], on=0)
    assert (Path(wrong["folder"]) / ".disabled").is_file()
    _call(cli, "enable", folder=wrong["folder"], on=1)
    assert not (Path(wrong["folder"]) / ".disabled").exists()
    _call(cli, "uninstall", folder=str(folder))
    assert not folder.exists()
    with pytest.raises(mbc.CliError):
        _call(cli, "uninstall", folder=str(tmp_path))                        # outside the mods folder: refused
    assert not list((Path(cli.mods_root) / ".mb_tmp").glob("*")) if (Path(cli.mods_root) / ".mb_tmp").exists() else True


def test_install_local_file_and_links(cli):
    import subprocess
    zip_path = FIXTURE / "files" / "bare_bin.zip"
    proc = subprocess.Popen(cli._args("install-local", path=str(zip_path), build_id=TARGET), stdout=subprocess.PIPE)
    res = list(mbc.read_lines(proc))[-1]
    assert res["state"] == "done" and Path(res["folders"][0]).name == "bare_bin"
    assert zip_path.is_file()                                                 # the user's file is never deleted
    with pytest.raises(mbc.CliError):
        _call(cli, "install-local", path="http://example.invalid/mod.zip")    # https only


def test_offline_source_fails_softly(cli):
    cli.extra_env["SW_MB_MOCK_OFFLINE"] = "1"
    with pytest.raises(mbc.CliError) as e:
        _call(cli, "search", source="mock")
    assert "offline" in str(e.value)


def test_helpers():
    assert mbc.version_newer("1.10", "1.9") and not mbc.version_newer("1.0", "1.0") and not mbc.version_newer("", "1")
    assert mbc.compat_badge({"compat": "match"}, "1.0.0") == ("v1.0.0", "good")
    assert mbc.compat_badge({"compat": "mismatch", "detected": "1.1.1"}, "1.0.0") == ("Wrong version (1.1.1)", "bad")
    assert mbc.compat_badge({}, "1.0.0") == ("Unknown version", "warn")
    assert mbc.fmt_size(1536) == "2 KB" and mbc.fmt_count(12345) == "12k"


def test_norm_repo_and_repo_rows(tmp_path):
    assert mbc.norm_repo("https://github.com/TLin-Y/Switch-Ultrawide-Mods") == "TLin-Y/Switch-Ultrawide-Mods"
    assert mbc.norm_repo("github.com/a/b.git/tree/main/x") == "a/b"
    assert mbc.norm_repo(" owner/repo ") == "owner/repo"
    assert mbc.norm_repo("not a link") == "" and mbc.norm_repo("https://example.com") == ""
    builtin = [{"repo": "x/one", "name": "One", "mods": 3}, {"repo": "y/two", "name": "Two", "mods": 1}]
    rows = mbc.repo_rows({"github_repos": ["https://github.com/me/mine"]}, builtin)   # first open: built-ins added
    assert [r["repo"] for r in rows] == ["me/mine", "x/one", "y/two"] and rows[1]["builtin"] and rows[1]["name"] == "One"
    rows = [r for r in rows if r["repo"] != "x/one"]                                  # delete a built-in, rename one
    rows[0]["name"] = "My mods"
    st = mbc.apply_repo_rows({}, rows, builtin)
    assert st["github_repos"] == ["me/mine", "y/two"] and st["github_removed"] == ["x/one"]
    assert st["github_names"] == {"me/mine": "My mods"}
    again = mbc.repo_rows(st, builtin)                                                # the deleted one stays deleted
    assert [r["repo"] for r in again] == ["me/mine", "y/two"] and again[0]["name"] == "My mods"
    idx = tmp_path / "i.json"
    idx.write_text('{"repos": [{"repo": "a/b", "name": "AB", "ok": true, "mods": 2}, {"repo": "c/d", "ok": true, "mods": 0}]}')
    assert mbc.builtin_repos(idx) == [{"repo": "a/b", "name": "AB", "mods": 2}]
