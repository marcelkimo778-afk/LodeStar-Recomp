import importlib.util
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# loaded from its file: putting launcher/ on sys.path would hide the launcher package
_spec = importlib.util.spec_from_file_location("controls", ROOT / "launcher" / "controls.py")
controls = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(controls)


def test_actions_match_runtime():
    src = (ROOT / "Engine" / "Runtime" / "src" / "sw_hid.c").read_text(encoding="utf-8")
    block = re.search(r"k_names\[K_COUNT\] = \{(.*?)\};", src, re.S).group(1)
    runtime = re.findall(r'"([A-Z0-9_]+)"', block)
    assert sorted(runtime) == sorted(n for n, _ in controls.ACTIONS)   # matched by name, order is display only


def test_defaults_and_normalize():
    b = controls.normalize({"A": [65, 65, 300, 0x72], "B": "junk"})
    assert b["A"] == [65]                       # dupes, out of range and F3 dropped
    assert b["B"] == []                          # junk -> unbound
    assert b["X"] == controls.DEFAULTS["X"]      # missing -> default
    assert len(b) == len(controls.ACTIONS)


def test_roundtrip_and_keymap(tmp_path):
    binds = controls.normalize({})
    binds["A"] = [ord("Z")]
    controls.save(tmp_path / "c.json", binds)
    again = controls.load(tmp_path / "c.json")
    assert again["A"] == [90]
    km = controls.write_keymap(tmp_path / "k.txt", again).read_text()
    assert "\nA=90\n" in km and "LS_UP=87" in km


def test_names_and_conflicts():
    assert controls.key_name(13) == "Enter"
    assert controls.key_name(ord("Q")) == "Q"
    assert controls.key_name(0x64) == "Num 4"
    b = controls.normalize({})
    b["B"] = [13]
    assert any("Enter" in c for c in controls.conflicts(b))


def test_pad_names_match_runtime():
    src = (ROOT / "Engine" / "Runtime" / "src" / "sw_input.c").read_text(encoding="utf-8")
    pi = re.findall(r'"([A-Z]+)"', re.search(r"pi_names\[PI_COUNT\] = \{(.*?)\};", src, re.S).group(1))
    sb = re.findall(r'"([A-Z_]+)"', re.search(r"sb_names\[18\] = \{(.*?)\};", src, re.S).group(1))
    assert pi == controls.PAD_INPUTS
    assert sb == [n for n, _ in controls.PAD_ACTIONS]          # same order: SW_BTN bit order


def test_pad_normalize_and_padmap(tmp_path):
    p = controls.normalize_pad({"map": {"A": ["RT", "RT", "NOPE"], "B": "junk"}, "device": "7",
                                "deadzone": 99, "swap_sticks": 1})
    assert p["map"]["A"] == ["RT"]
    assert p["map"]["B"] == []                                   # invalid -> unbound, not default
    assert p["map"]["X"] == ["Y"]                                # missing -> default (Switch layout)
    assert p["device"] == "any" and p["deadzone"] == 50 and p["swap_sticks"] is True
    text = controls.write_padmap(tmp_path / "pad.txt", p).read_text()
    assert "DEVICE=any" in text and "A=RT" in text and "SWAP_STICKS=1" in text and "\nB=\n" in text


def test_pad_profiles(tmp_path):
    d = tmp_path / "profiles"
    assert controls.list_profiles(d) == []
    controls.save_pad(d / "Couch.json", {"device": "1"})
    assert controls.list_profiles(d) == ["Couch"]
    assert controls.load_pad(d / "Couch.json")["device"] == "x1"           # old slot numbers -> XInput ids
    assert controls.profile_name_ok("My pad 2") and not controls.profile_name_ok("a/b")
    assert not controls.profile_name_ok("  ") and not controls.profile_name_ok("..")


def test_mouse_settings(tmp_path):
    m = controls.normalize_mouse({"sens": 50, "camera": 0})
    assert m["sens"] == 10.0 and m["camera"] is False
    env = controls.mouse_env({"sens": 2})
    assert env == {"SW_MOUSE_CAMERA": "1", "SW_MOUSE_SENS": "2.00", "SW_MOUSE_INVERT_Y": "0"}
    assert controls.normalize({"ZR": [controls.VK_LBUTTON]})["ZR"] == [1]   # mouse buttons bind like keys
    assert controls.key_name(controls.VK_RBUTTON) == "Mouse right"
    assert 0x71 in controls.RESERVED                                         # F2 = mouse capture


def test_hid_defaults_match_runtime():
    """GEN_DEFAULTS / GEN_AXES_DEFAULT = sw_input.c g_gm / g_gax (B<n> = index n-1, POV = GI_POV+i)."""
    src = (ROOT / "Engine" / "Runtime" / "src" / "sw_input.c").read_text(encoding="utf-8")
    block = re.search(r"g_gm\[18\]\[4\] = \{(.*?)\};", src, re.S).group(1)
    firsts = re.findall(r"\{\s*(GI_POV \+ \d|\d+),", block)
    names = [n for n, _ in controls.PAD_ACTIONS]
    assert len(firsts) == 18
    for name, f in zip(names, firsts):
        want = controls.POV[int(f[-1])] if f.startswith("GI_POV") else f"B{int(f) + 1}"
        assert controls.GEN_DEFAULTS[name] == [want], name
    gax = re.search(r"g_gax\[4\] = \{(.*?)\};", src).group(1)
    assert [controls.AXES[int(i)] for i in gax.split(",")] == [controls.GEN_AXES_DEFAULT[k] for k in ("lx", "ly", "rx", "ry")]


def test_devices_and_hid_padmap(tmp_path):
    nd = controls.normalize_device
    assert nd("2") == "x2" and nd("x3") == "x3" and nd("x4") == "any" and nd("") == "any"
    assert nd("j:054c:09cc") == "j:054C:09CC" and nd("j:054C:09CC:1") == "j:054C:09CC:1"
    assert nd("j:054C") == "any" and nd("j:054C:09CC:0") == "any"
    assert controls.device_kind("j:054C:09CC") == "j" and controls.device_kind("any") == "x"
    p = controls.normalize_pad({"device": "j:1949:0402", "gmap": {"A": ["B2", "POV_UP", "Z+", "B33", "LB"]},
                                "gaxes": {"lx": "U", "ry": "Q"}})
    assert p["gmap"]["A"] == ["B2", "POV_UP", "Z+"]            # B33 / Xbox names are not HID inputs
    assert p["gmap"]["B"] == ["B2"] and p["map"]["A"] == ["B"]  # missing -> each layout's default
    assert p["gaxes"] == {"lx": "U", "ly": "Y", "rx": "Z", "ry": "R"}
    text = controls.write_padmap(tmp_path / "pad.txt", p).read_text()
    assert "DEVICE=j:1949:0402" in text and "\nGA=B2,POV_UP,Z+\n" in text and "\nA=B\n" in text
    assert "\nG_LX=U\n" in text and "\nGHOME=B13\n" in text


def test_pov_and_pad_view():
    assert controls.pov_dirs(0xFFFF) == set()
    assert controls.pov_dirs(0) == {"POV_UP"}
    assert controls.pov_dirs(4500) == {"POV_UP", "POV_RIGHT"}
    assert controls.pov_dirs(18000) == {"POV_DOWN"}
    assert controls.pov_dirs(27000) == {"POV_LEFT"}
    st = {"kind": "j", "pressed": {"B3", "POV_LEFT"}, "axes": {"X": 0.9, "Y": -1.0, "Z": 0.0, "R": 0.02},
          "sticks": None, "vid": 0x054C}
    btns, ls, rs = controls.pad_view(st, {"swap_sticks": True})
    assert btns == {"A", "DPAD_LEFT"}
    assert ls == (0.9, 1.0) and rs == (0.0, 0.0)               # HID y flipped (up = +), deadzone, no swap
    assert controls.gen_input_label("B3", 0x054C) == "Circle" and controls.gen_input_label("B3") == "Button 3"


def test_assign_moves_input():
    """One press = one Switch button: assigning takes the input off the others (Minus left on the pad's ZL)."""
    lay = controls.normalize_pad({"gmap": {"ZL": ["B9"]}})["gmap"]
    assert lay["MINUS"] == ["B9"]                                      # the PlayStation default: same button as ZL
    assert controls.layout_conflicts(lay) == {"B9": ["ZL", "MINUS"]}
    assert controls.assign_input(lay, "MINUS", "B11") == ["LSTICK"]         # B11 = L3 in the PlayStation default
    assert lay["MINUS"] == ["B11"] and lay["ZL"] == ["B9"]
    assert controls.layout_conflicts(lay) == {}
    assert controls.assign_input(lay, "HOME", "B11") == ["MINUS"]       # moved, not doubled
    assert lay["HOME"] == ["B11"] and lay["MINUS"] == []
