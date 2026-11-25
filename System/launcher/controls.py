"""Keyboard bindings for the game (no tkinter here, so it is testable).

The runtime (Engine/Runtime/src/sw_hid.c) reads SW_KEYMAP=<file> with lines
`NAME=vk,vk,...` (Windows virtual-key codes, decimal).  ACTIONS and DEFAULTS
must match k_names / k_map there.  A controller (XInput or any HID pad) always works too.
"""
from __future__ import annotations

import json
from pathlib import Path

MAX_KEYS = 4

# (name in the file, label in the launcher)
ACTIONS = [
    ("A", "A  (confirm / talk)"), ("B", "B  (back / dodge)"), ("X", "X"), ("Y", "Y"),
    ("L", "L"), ("R", "R"), ("ZL", "ZL  (aim)"), ("ZR", "ZR  (throw)"),
    ("PLUS", "+  (menu)"), ("MINUS", "-  (map)"), ("LSTICK", "Left stick click"),
    ("RSTICK", "Right stick click"),
    ("DPAD_UP", "D-pad up"), ("DPAD_DOWN", "D-pad down"), ("DPAD_LEFT", "D-pad left"),
    ("DPAD_RIGHT", "D-pad right"),
    ("LS_UP", "Move up"), ("LS_DOWN", "Move down"), ("LS_LEFT", "Move left"), ("LS_RIGHT", "Move right"),
    ("RS_UP", "Camera up"), ("RS_DOWN", "Camera down"), ("RS_LEFT", "Camera left"),
    ("RS_RIGHT", "Camera right"),
]

VK_RETURN, VK_SPACE, VK_BACK, VK_TAB, VK_SHIFT, VK_LSHIFT = 13, 32, 8, 9, 16, 0xA0
VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN = 0x25, 0x26, 0x27, 0x28
VK_NUMPAD = 0x60   # numpad 0..9 = 0x60..0x69

DEFAULTS: dict[str, list[int]] = {
    "A": [VK_RETURN, VK_SPACE, ord("K")], "B": [VK_BACK, ord("L")], "X": [ord("I")], "Y": [ord("J")],
    "L": [ord("Q")], "R": [ord("E")], "ZL": [ord("1")], "ZR": [ord("3")],
    "PLUS": [VK_TAB, ord("P")], "MINUS": [ord("M")], "LSTICK": [VK_LSHIFT], "RSTICK": [ord("R")],
    "DPAD_UP": [VK_UP], "DPAD_DOWN": [VK_DOWN], "DPAD_LEFT": [VK_LEFT], "DPAD_RIGHT": [VK_RIGHT],
    "LS_UP": [ord("W")], "LS_DOWN": [ord("S")], "LS_LEFT": [ord("A")], "LS_RIGHT": [ord("D")],
    "RS_UP": [VK_NUMPAD + 8], "RS_DOWN": [VK_NUMPAD + 5, VK_NUMPAD + 2],
    "RS_LEFT": [VK_NUMPAD + 4], "RS_RIGHT": [VK_NUMPAD + 6],
}

_NAMES = {VK_RETURN: "Enter", VK_SPACE: "Space", VK_BACK: "Backspace", VK_TAB: "Tab", VK_SHIFT: "Shift",
          VK_LSHIFT: "Left Shift", 0xA1: "Right Shift", 17: "Ctrl", 0xA2: "Left Ctrl", 0xA3: "Right Ctrl",
          18: "Alt", 20: "Caps Lock", 27: "Esc", VK_LEFT: "Left", VK_UP: "Up", VK_RIGHT: "Right",
          VK_DOWN: "Down", 33: "Page Up", 34: "Page Down", 35: "End", 36: "Home", 45: "Insert", 46: "Delete",
          0xBA: ";", 0xBB: "=", 0xBC: ",", 0xBD: "-", 0xBE: ".", 0xBF: "/", 0xC0: "`", 0xDB: "[",
          0xDC: "\\", 0xDD: "]", 0xDE: "'", 0x6A: "Num *", 0x6B: "Num +", 0x6D: "Num -", 0x6E: "Num .",
          0x6F: "Num /"}

# keys the game window uses itself (F1 = in-game menu, F3 = FPS counter); not bindable
RESERVED = {0x70, 0x72}


def key_name(vk: int) -> str:
    if vk in _NAMES:
        return _NAMES[vk]
    if 0x30 <= vk <= 0x39 or 0x41 <= vk <= 0x5A:
        return chr(vk)
    if VK_NUMPAD <= vk <= VK_NUMPAD + 9:
        return f"Num {vk - VK_NUMPAD}"
    if 0x70 <= vk <= 0x87:
        return f"F{vk - 0x6F}"
    return f"Key {vk}"


def describe(keys: list[int]) -> str:
    return ", ".join(key_name(k) for k in keys) if keys else "(none)"


def normalize(binds: dict) -> dict[str, list[int]]:
    """Every action present, at most MAX_KEYS valid codes each, no duplicates."""
    out: dict[str, list[int]] = {}
    for name, _label in ACTIONS:
        raw = binds.get(name, DEFAULTS[name])
        keys: list[int] = []
        for k in raw if isinstance(raw, list) else []:
            if isinstance(k, int) and 0 < k < 256 and k not in keys and k not in RESERVED:
                keys.append(k)
        out[name] = keys[:MAX_KEYS]
    return out


def load(path: Path) -> dict[str, list[int]]:
    try:
        return normalize(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return normalize({})


def save(path: Path, binds: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(normalize(binds), indent=1), encoding="utf-8")


def write_keymap(path: Path, binds: dict) -> Path:
    """The runtime's SW_KEYMAP file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# written by the launcher (Controls); NAME=Windows virtual-key codes"]
    for name, keys in normalize(binds).items():
        lines.append(f"{name}=" + ",".join(str(k) for k in keys))
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    return path


def conflicts(binds: dict) -> list[str]:
    """Keys bound to more than one action (allowed, but worth showing)."""
    seen: dict[int, list[str]] = {}
    for name, keys in normalize(binds).items():
        for k in keys:
            seen.setdefault(k, []).append(name)
    return [f"{key_name(k)}: {' + '.join(v)}" for k, v in seen.items() if len(v) > 1]


# ---------------------------------------------------------------------------
# Mouse (Keyboard / Mouse tab).  Mouse buttons are bound like keys (Windows
# virtual-key codes 1, 2, 4, 5, 6); the runtime reads them with the keyboard.
# ---------------------------------------------------------------------------
VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 = 1, 2, 4, 5, 6
_NAMES.update({VK_LBUTTON: "Mouse left", VK_RBUTTON: "Mouse right", VK_MBUTTON: "Mouse middle",
               VK_XBUTTON1: "Mouse back", VK_XBUTTON2: "Mouse forward"})
RESERVED.add(0x71)          # F2 frees / captures the mouse in the game window
MOUSE_BUTTONS = {1: VK_LBUTTON, 2: VK_MBUTTON, 3: VK_RBUTTON, 4: VK_XBUTTON1, 5: VK_XBUTTON2}  # tk num -> vk

MOUSE_DEFAULTS = {"camera": True, "sens": 1.0, "invert_y": False}


def normalize_mouse(m: dict) -> dict:
    out = dict(MOUSE_DEFAULTS)
    if isinstance(m, dict):
        out["camera"] = bool(m.get("camera", out["camera"]))
        out["invert_y"] = bool(m.get("invert_y", out["invert_y"]))
        try:
            out["sens"] = min(10.0, max(0.1, float(m.get("sens", out["sens"]))))
        except (TypeError, ValueError):
            pass
    return out


def mouse_env(m: dict) -> dict[str, str]:
    m = normalize_mouse(m)
    return {"SW_MOUSE_CAMERA": "1" if m["camera"] else "0", "SW_MOUSE_SENS": "%.2f" % m["sens"],
            "SW_MOUSE_INVERT_Y": "1" if m["invert_y"] else "0"}


def load_mouse(path: Path) -> dict:
    try:
        return normalize_mouse(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return normalize_mouse({})


def save_mouse(path: Path, m: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(normalize_mouse(m), indent=1), encoding="utf-8")


# ---------------------------------------------------------------------------
# Controller (Controller tab).  Must match Engine/Runtime/src/sw_input.c:
# XInput inputs (pi_names), HID inputs (gi_parse) and Switch buttons (sb_names).
# Two kinds of pad, each with its own layout:
#   "x"  Xbox / XInput pads (slots x0..x3)                       -> pad["map"]
#   "j"  every other HID game controller through winmm (PlayStation, Switch Pro,
#        8BitDo, generic; USB or Bluetooth), ids j:VVVV:PPPP[:n]  -> pad["gmap"]
# ---------------------------------------------------------------------------
PAD_INPUTS = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "BACK", "LS", "RS",
              "DUP", "DDOWN", "DLEFT", "DRIGHT"]
PAD_INPUT_LABELS = {"A": "Xbox A", "B": "Xbox B", "X": "Xbox X", "Y": "Xbox Y", "LB": "LB", "RB": "RB",
                    "LT": "LT", "RT": "RT", "START": "Start / Menu", "BACK": "Back / View",
                    "LS": "Left stick press", "RS": "Right stick press", "DUP": "D-pad up",
                    "DDOWN": "D-pad down", "DLEFT": "D-pad left", "DRIGHT": "D-pad right"}
# XInput wButtons bits for the launcher's "press a button" capture
XINPUT_BITS = {"DUP": 0x0001, "DDOWN": 0x0002, "DLEFT": 0x0004, "DRIGHT": 0x0008, "START": 0x0010,
               "BACK": 0x0020, "LS": 0x0040, "RS": 0x0080, "LB": 0x0100, "RB": 0x0200, "A": 0x1000,
               "B": 0x2000, "X": 0x4000, "Y": 0x8000}

AXES = ["X", "Y", "Z", "R", "U", "V"]
POV = ["POV_UP", "POV_DOWN", "POV_LEFT", "POV_RIGHT"]
GEN_INPUTS = [f"B{i}" for i in range(1, 33)] + POV + [a + s for a in AXES for s in "+-"]
# PlayStation numbering (also what most generic HID pads use)
SONY_LABELS = {"B1": "Square", "B2": "Cross", "B3": "Circle", "B4": "Triangle", "B5": "L1", "B6": "R1",
               "B7": "L2", "B8": "R2", "B9": "Share", "B10": "Options", "B11": "L3", "B12": "R3",
               "B13": "PS", "B14": "Touchpad"}
VID_SONY = 0x054C


def gen_input_label(k: str, vid: int | None = None) -> str:
    if vid == VID_SONY and k in SONY_LABELS:
        return SONY_LABELS[k]
    if k.startswith("POV_"):
        return "D-pad " + k[4:].lower()
    if k[:1] == "B" and k[1:].isdigit():
        return "Button " + k[1:]
    if len(k) == 2 and k[0] in AXES:
        return f"Axis {k[0]}{k[1]}"
    return k


def input_label(k: str, kind: str = "x", vid: int | None = None) -> str:
    return PAD_INPUT_LABELS.get(k, k) if kind == "x" else gen_input_label(k, vid)


PAD_ACTIONS = [("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"), ("L", "L"), ("R", "R"), ("ZL", "ZL"),
               ("ZR", "ZR"), ("PLUS", "Plus (+)"), ("MINUS", "Minus (-)"), ("HOME", "Home"), ("CAPTURE", "Capture"),
               ("LSTICK", "L stick press"), ("RSTICK", "R stick press"), ("DPAD_UP", "D-pad up"),
               ("DPAD_DOWN", "D-pad down"), ("DPAD_LEFT", "D-pad left"), ("DPAD_RIGHT", "D-pad right")]
# Switch layout on an Xbox pad: the button in the same place (Xbox B = Switch A)
PAD_DEFAULTS: dict[str, list[str]] = {
    "A": ["B"], "B": ["A"], "X": ["Y"], "Y": ["X"], "L": ["LB"], "R": ["RB"], "ZL": ["LT"], "ZR": ["RT"],
    "PLUS": ["START"], "MINUS": ["BACK"], "HOME": [], "CAPTURE": [], "LSTICK": ["LS"], "RSTICK": ["RS"],
    "DPAD_UP": ["DUP"], "DPAD_DOWN": ["DDOWN"], "DPAD_LEFT": ["DLEFT"], "DPAD_RIGHT": ["DRIGHT"],
}
# ... and on a HID pad with PlayStation numbering (Circle = Switch A); runtime g_gm
GEN_DEFAULTS: dict[str, list[str]] = {
    "A": ["B3"], "B": ["B2"], "X": ["B4"], "Y": ["B1"], "L": ["B5"], "R": ["B6"], "ZL": ["B7"], "ZR": ["B8"],
    "PLUS": ["B10"], "MINUS": ["B9"], "HOME": ["B13"], "CAPTURE": ["B14"], "LSTICK": ["B11"], "RSTICK": ["B12"],
    "DPAD_UP": ["POV_UP"], "DPAD_DOWN": ["POV_DOWN"], "DPAD_LEFT": ["POV_LEFT"], "DPAD_RIGHT": ["POV_RIGHT"],
}
GEN_AXES_DEFAULT = {"lx": "X", "ly": "Y", "rx": "Z", "ry": "R"}   # runtime g_gax
PAD_OPTION_DEFAULTS = {"device": "any", "swap_sticks": False, "invert_ly": False, "invert_ry": False,
                       "deadzone": 8}

_DEV_J = __import__("re").compile(r"^j:[0-9A-F]{4}:[0-9A-F]{4}(:[1-9][0-9]?)?$")


def normalize_device(v) -> str:
    """'any', 'x0'..'x3' (XInput slot; old files say '0'..'3') or 'j:VVVV:PPPP[:n]' (HID pad)."""
    v = str(v).strip()
    if v in ("0", "1", "2", "3"):
        return "x" + v
    if v in ("x0", "x1", "x2", "x3"):
        return v
    if v[:2].lower() == "j:" and _DEV_J.match("j:" + v[2:].upper()):
        return "j:" + v[2:].upper()
    return "any"


def device_kind(dev: str) -> str:
    return "j" if dev.startswith("j:") else "x"


def _norm_map(raw, valid: list[str], defaults: dict[str, list[str]]) -> dict[str, list[str]]:
    raw = raw if isinstance(raw, dict) else {}
    out: dict[str, list[str]] = {}
    for name, _label in PAD_ACTIONS:
        src = raw.get(name, defaults[name])
        keys: list[str] = []
        for k in src if isinstance(src, list) else []:
            if k in valid and k not in keys:
                keys.append(k)
        out[name] = keys[:MAX_KEYS]
    return out


def normalize_pad(p: dict) -> dict:
    """{"map": Xbox layout, "gmap": HID layout, "gaxes": HID sticks, options...} with every key valid."""
    p = p if isinstance(p, dict) else {}
    out: dict = {"map": _norm_map(p.get("map"), PAD_INPUTS, PAD_DEFAULTS),
                 "gmap": _norm_map(p.get("gmap"), GEN_INPUTS, GEN_DEFAULTS)}
    ga = p.get("gaxes") if isinstance(p.get("gaxes"), dict) else {}
    out["gaxes"] = {k: (ga.get(k) if ga.get(k) in AXES + ["-"] else d) for k, d in GEN_AXES_DEFAULT.items()}
    for k, d in PAD_OPTION_DEFAULTS.items():
        v = p.get(k, d)
        if k == "device":
            out[k] = normalize_device(v)
        elif k == "deadzone":
            try:
                out[k] = min(50, max(0, int(v)))
            except (TypeError, ValueError):
                out[k] = d
        else:
            out[k] = bool(v)
    return out


def assign_input(layout: dict[str, list[str]], name: str, inp: str) -> list[str]:
    """Switch button `name` = physical input `inp` alone; the input leaves every other Switch button of the layout
    (one press = one button: a leftover default such as Minus on the pad's ZL button pressed both).
    Returns the Switch buttons it was taken from."""
    taken = [n for n, keys in layout.items() if n != name and inp in keys]
    for n in taken:
        layout[n] = [k for k in layout[n] if k != inp]
    layout[name] = [inp]
    return taken


def layout_conflicts(layout: dict[str, list[str]]) -> dict[str, list[str]]:
    """Physical inputs on more than one Switch button: {input: [Switch buttons]}."""
    seen: dict[str, list[str]] = {}
    for n, keys in layout.items():
        for k in keys:
            seen.setdefault(k, []).append(n)
    return {k: v for k, v in seen.items() if len(v) > 1}


def describe_pad(inputs: list[str], kind: str = "x", vid: int | None = None) -> str:
    return ", ".join(input_label(k, kind, vid) for k in inputs) if inputs else "(none)"


def write_padmap(path: Path, pad: dict) -> Path:
    """The runtime's SW_PADMAP file."""
    p = normalize_pad(pad)
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# written by the launcher (Controls > Controller)",
             f"DEVICE={p['device']}", f"SWAP_STICKS={int(p['swap_sticks'])}",
             f"INVERT_LY={int(p['invert_ly'])}", f"INVERT_RY={int(p['invert_ry'])}",
             f"DEADZONE={p['deadzone']}"]
    for name, keys in p["map"].items():
        lines.append(f"{name}=" + ",".join(keys))
    for name, keys in p["gmap"].items():
        lines.append(f"G{name}=" + ",".join(keys))
    for k in ("lx", "ly", "rx", "ry"):
        lines.append(f"G_{k.upper()}={p['gaxes'][k]}")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")
    return path


# Profiles: Config/controller_profiles/<name>.json; the active one is
# Config/controller.json (what the game uses).
_BAD = set('<>:"/\\|?*')


def profile_name_ok(name: str) -> bool:
    name = name.strip()
    return bool(name) and len(name) <= 40 and not (set(name) & _BAD) and name not in (".", "..")


def list_profiles(folder: Path) -> list[str]:
    try:
        return sorted(p.stem for p in folder.glob("*.json"))
    except OSError:
        return []


def load_pad(path: Path) -> dict:
    try:
        return normalize_pad(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        return normalize_pad({})


def save_pad(path: Path, pad: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(normalize_pad(pad), indent=1), encoding="utf-8")


# ---------------------------------------------------------------------------
# XInput from the launcher (Windows): which slots are connected, and what is
# pressed, for "press a button on your controller".
# ---------------------------------------------------------------------------
_XI = []


def _xinput():
    if _XI:
        return _XI[0]
    import ctypes
    import sys
    lib = None
    if sys.platform == "win32":
        for dll in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
            try:
                lib = getattr(ctypes.windll, dll)
                break
            except OSError:
                continue
    _XI.append(lib)
    return lib


_XSTRUCT = []


def _xstate_type():
    if not _XSTRUCT:
        import ctypes

        class _Pad(ctypes.Structure):
            _fields_ = [("wButtons", ctypes.c_ushort), ("bLeftTrigger", ctypes.c_ubyte),
                        ("bRightTrigger", ctypes.c_ubyte), ("sThumbLX", ctypes.c_short),
                        ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short), ("sThumbRY", ctypes.c_short)]

        class _State(ctypes.Structure):
            _fields_ = [("dwPacketNumber", ctypes.c_ulong), ("Gamepad", _Pad)]
        _XSTRUCT.append(_State)
    return _XSTRUCT[0]


def xinput_full(slot: int):
    """dict(buttons, lt, rt, lx, ly, rx, ry) or None when nothing is in that slot."""
    import ctypes
    x = _xinput()
    if x is None:
        return None
    st = _xstate_type()()
    try:
        if x.XInputGetState(slot, ctypes.byref(st)) != 0:
            return None
    except (OSError, AttributeError):
        return None
    g = st.Gamepad
    return {"buttons": g.wButtons, "lt": g.bLeftTrigger, "rt": g.bRightTrigger,
            "lx": g.sThumbLX, "ly": g.sThumbLY, "rx": g.sThumbRX, "ry": g.sThumbRY}


def xinput_state(slot: int):
    """(buttons, left trigger, right trigger) or None when nothing is in that slot."""
    f = xinput_full(slot)
    return None if f is None else (f["buttons"], f["lt"], f["rt"])


def pressed_inputs(state) -> set[str]:
    if not state:
        return set()
    buttons, lt, rt = state
    out = {name for name, bit in XINPUT_BITS.items() if buttons & bit}
    if lt > 64:
        out.add("LT")
    if rt > 64:
        out.add("RT")
    return out


def connected_slots() -> list[int]:
    return [i for i in range(4) if xinput_state(i) is not None]


# ---------------------------------------------------------------------------
# HID game controllers through winmm (what the runtime's poll_joy reads).
# ---------------------------------------------------------------------------
_WM = []


def _winmm():
    if _WM:
        return _WM[0]
    import ctypes
    import sys
    lib = None
    if sys.platform == "win32":
        try:
            lib = ctypes.WinDLL("winmm")
            from ctypes import wintypes as w

            class JOYINFOEX(ctypes.Structure):
                _fields_ = [(n, w.DWORD) for n in ("dwSize", "dwFlags", "dwXpos", "dwYpos", "dwZpos", "dwRpos",
                                                   "dwUpos", "dwVpos", "dwButtons", "dwButtonNumber", "dwPOV",
                                                   "dwReserved1", "dwReserved2")]

            class JOYCAPSW(ctypes.Structure):
                _fields_ = ([("wMid", w.WORD), ("wPid", w.WORD), ("szPname", w.WCHAR * 32)]
                            + [(n, w.UINT) for n in ("wXmin", "wXmax", "wYmin", "wYmax", "wZmin", "wZmax",
                                                     "wNumButtons", "wPeriodMin", "wPeriodMax", "wRmin", "wRmax",
                                                     "wUmin", "wUmax", "wVmin", "wVmax", "wCaps", "wMaxAxes",
                                                     "wNumAxes", "wMaxButtons")]
                            + [("szRegKey", w.WCHAR * 32), ("szOEMVxD", w.WCHAR * 260)])
            lib.joyGetPosEx.argtypes = [w.UINT, ctypes.POINTER(JOYINFOEX)]
            lib.joyGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(JOYCAPSW), w.UINT]
            lib.joyConfigChanged.argtypes = [w.DWORD]
            lib._JI, lib._JC = JOYINFOEX, JOYCAPSW
        except (OSError, AttributeError):
            lib = None
    _WM.append(lib)
    return lib


def _oem_name(vid: int, pid: int) -> str:
    try:
        import winreg
    except ImportError:
        return ""
    key = (r"System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM"
           + f"\\VID_{vid:04X}&PID_{pid:04X}")
    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            with winreg.OpenKey(root, key) as h:
                name, _t = winreg.QueryValueEx(h, "OEMName")
                if isinstance(name, str) and name.strip():
                    return name.strip()
        except OSError:
            continue
    return ""


def _is_xbox(vid: int, name: str) -> bool:
    return vid == 0x045E or any(s in name for s in ("XBOX", "Xbox", "XInput", "XINPUT"))


def hid_scan(rescan: bool = False) -> list[dict]:
    """Connected HID pads: [{id, vid, pid, name, xinput, dev, ranges}] (dev = the DEVICE value)."""
    import ctypes
    wm = _winmm()
    if wm is None:
        return []
    if rescan:
        try:
            wm.joyConfigChanged(0)
        except OSError:
            pass
    out: list[dict] = []
    seen: dict[tuple[int, int], int] = {}
    for jid in range(min(16, wm.joyGetNumDevs())):
        ji = wm._JI(dwSize=ctypes.sizeof(wm._JI), dwFlags=0xFF)
        if wm.joyGetPosEx(jid, ctypes.byref(ji)) != 0:
            continue
        caps = wm._JC()
        if wm.joyGetDevCapsW(jid, ctypes.byref(caps), ctypes.sizeof(caps)) != 0:
            continue
        vid, pid = caps.wMid, caps.wPid
        name = _oem_name(vid, pid) or caps.szPname.strip() or "Game controller"
        n = seen.get((vid, pid), 0)
        seen[(vid, pid)] = n + 1
        has = [True, True] + [bool(caps.wCaps & bit) for bit in (1, 2, 4, 8)]
        rng = [(caps.wXmin, caps.wXmax), (caps.wYmin, caps.wYmax), (caps.wZmin, caps.wZmax),
               (caps.wRmin, caps.wRmax), (caps.wUmin, caps.wUmax), (caps.wVmin, caps.wVmax)]
        out.append({"id": jid, "vid": vid, "pid": pid, "name": name, "xinput": _is_xbox(vid, name),
                    "dev": f"j:{vid:04X}:{pid:04X}" + (f":{n}" if n else ""),
                    "ranges": [r if h else None for r, h in zip(rng, has)]})
    return out


def pov_dirs(pov: int) -> set[str]:
    """winmm POV (hundredths of a degree, 0 = up, 0xFFFF = centred) -> POV_* inputs (runtime joy_read)."""
    if pov == 0xFFFF or pov > 36000:
        return set()
    out = set()
    if pov > 27000 or pov < 9000:
        out.add("POV_UP")
    if 9000 < pov < 27000:
        out.add("POV_DOWN")
    if pov > 18000:
        out.add("POV_LEFT")
    if 0 < pov < 18000:
        out.add("POV_RIGHT")
    return out


def hid_read(info: dict):
    """{"pressed": set of GEN_INPUTS, "axes": {X..V: -1..1}} or None when it stopped answering."""
    import ctypes
    wm = _winmm()
    if wm is None:
        return None
    ji = wm._JI(dwSize=ctypes.sizeof(wm._JI), dwFlags=0xFF | 0x200)   # JOY_RETURNALL | JOY_RETURNPOVCTS
    if wm.joyGetPosEx(info["id"], ctypes.byref(ji)) != 0:
        return None
    pressed = {f"B{i + 1}" for i in range(32) if ji.dwButtons >> i & 1} | pov_dirs(ji.dwPOV)
    raw = (ji.dwXpos, ji.dwYpos, ji.dwZpos, ji.dwRpos, ji.dwUpos, ji.dwVpos)
    axes: dict[str, float] = {}
    for a, r, v in zip(AXES, info["ranges"], raw):
        if r is None or r[1] <= r[0]:
            axes[a] = 0.0
            continue
        axes[a] = max(-1.0, min(1.0, (v - r[0]) / (r[1] - r[0]) * 2 - 1))
        if axes[a] > 0.5:
            pressed.add(a + "+")
        elif axes[a] < -0.5:
            pressed.add(a + "-")
    return {"pressed": pressed, "axes": axes}


# ---------------------------------------------------------------------------
# Both kinds together: the device list and one state shape for the UI.
# ---------------------------------------------------------------------------
def list_devices(hid: list[dict]) -> list[tuple[str, str]]:
    """Connected controllers as (DEVICE value, label), "any" first."""
    out = [("any", "Any connected controller")]
    for s in connected_slots():
        out.append((f"x{s}", f"Xbox controller {s + 1}  (XInput)"))
    for h in hid:
        if not h["xinput"]:
            out.append((h["dev"], f"{h['name']}  ({h['vid']:04X}:{h['pid']:04X})"))
    return out


def _xstate(slot: int):
    f = xinput_full(slot)
    if f is None:
        return None
    return {"kind": "x", "dev": f"x{slot}", "name": f"Xbox controller {slot + 1}", "vid": None,
            "pressed": pressed_inputs((f["buttons"], f["lt"], f["rt"])),
            "sticks": tuple(max(-1.0, min(1.0, f[k] / 32767)) for k in ("lx", "ly", "rx", "ry")),
            "axes": {}}


def _jstate(h: dict):
    r = hid_read(h)
    if r is None:
        return None
    return {"kind": "j", "dev": h["dev"], "name": h["name"], "vid": h["vid"], "pressed": r["pressed"],
            "axes": r["axes"], "sticks": None}


def read_device(dev: str, hid: list[dict]):
    """The chosen controller's state (same choice as the runtime), or None when it is not connected.
    {"kind": "x"|"j", "dev", "name", "vid", "pressed": set, "sticks": (lx, ly, rx, ry) for x, "axes" for j}"""
    dev = normalize_device(dev)
    if dev.startswith("x"):
        return _xstate(int(dev[1]))
    if dev.startswith("j:"):
        for h in hid:
            if h["dev"] == dev:
                return _jstate(h)
        return None
    for s in range(4):
        st = _xstate(s)
        if st:
            return st
    for h in hid:
        if not h["xinput"]:
            st = _jstate(h)
            if st:
                return st
    return None


def pad_view(state, pad: dict) -> tuple[set[str], tuple[float, float], tuple[float, float]]:
    """What the game would see from a read_device() state with this mapping:
    (Switch buttons pressed, left stick (x, y), right stick (x, y)) with -1..1 axes, up = +y."""
    if not state:
        return set(), (0.0, 0.0), (0.0, 0.0)
    p = normalize_pad(pad)
    m = p["map"] if state["kind"] == "x" else p["gmap"]
    btns = {name for name, keys in m.items() if state["pressed"] & set(keys)}
    if state["kind"] == "x":
        lx, ly, rx, ry = state["sticks"]
        if p["swap_sticks"]:
            lx, ly, rx, ry = rx, ry, lx, ly
    else:
        ax = state["axes"]
        g = p["gaxes"]
        lx, rx = ax.get(g["lx"], 0.0), ax.get(g["rx"], 0.0)
        ly, ry = -ax.get(g["ly"], 0.0), -ax.get(g["ry"], 0.0)      # HID y grows downwards
    if p["invert_ly"]:
        ly = -ly
    if p["invert_ry"]:
        ry = -ry
    lim = p["deadzone"] / 100

    def dz(v: float) -> float:
        return 0.0 if -lim < v < lim else v
    return btns, (dz(lx), dz(ly)), (dz(rx), dz(ry))
