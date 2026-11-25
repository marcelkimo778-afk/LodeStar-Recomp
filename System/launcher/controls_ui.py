"""Controls window: "Keyboard / Mouse" and "Controller" tabs.

Keyboard / Mouse: mouse camera (sensitivity, invert), every Switch action
bound to up to 4 keys or mouse buttons.
Controller: a pad drawn in the middle - click a button on it, then press the
button on your controller (yuzu / Eden style); pick which controller, stick
options, and named profiles (Config/controller_profiles/*.json).

Data and file formats live in controls.py (no tkinter there, so it is tested).
"""
from __future__ import annotations

import tkinter as tk
from pathlib import Path
from tkinter import messagebox, simpledialog, ttk

import controls
import pad_art
import ui_kit

_INPUT_SHORT = {"A": "Xbox A", "B": "Xbox B", "X": "Xbox X", "Y": "Xbox Y", "START": "Start", "BACK": "Back",
                "DUP": "D-pad up", "DDOWN": "D-pad down", "DLEFT": "D-pad left", "DRIGHT": "D-pad right"}


def _short_inputs(keys: list[str], kind: str = "x", vid: int | None = None) -> str:
    if not keys:
        return "(none)"
    if kind == "x":
        return " / ".join(_INPUT_SHORT.get(k, k) for k in keys)
    return " / ".join(controls.gen_input_label(k, vid) for k in keys)


def _device_text(dev: str) -> str:
    if dev == "any":
        return "Any connected controller"
    if dev.startswith("x"):
        return f"Xbox controller {int(dev[1]) + 1}  (XInput)"
    return f"Controller {dev[2:]}"


class ControlsWindow:
    def __init__(self, root: tk.Misc, config_dir: Path, colors: dict, start_tab: int = 0):
        self.c = colors
        self.cfg = config_dir
        self.keys_path = config_dir / "controls.json"
        self.mouse_path = config_dir / "mouse.json"
        self.pad_path = config_dir / "controller.json"
        self.profiles_dir = config_dir / "controller_profiles"
        self.binds = controls.load(self.keys_path)
        self.mouse = controls.load_mouse(self.mouse_path)
        self.pad = controls.load_pad(self.pad_path)
        self._capture_job = None

        w = self.win = tk.Toplevel(root)
        w.title("Controls")
        ui_kit.window_icon(w, "controls")
        w.configure(bg=colors["card"])
        w.transient(root)
        w.geometry("1100x780")
        w.minsize(1040, 720)
        nb = ttk.Notebook(w)
        nb.pack(fill="both", expand=True, padx=8, pady=8)
        kb = ttk.Frame(nb, style="Card.TFrame", padding=12)
        pad = ttk.Frame(nb, style="Card.TFrame", padding=12)
        nb.add(kb, text="  Keyboard / Mouse  ")
        nb.add(pad, text="  Controller  ")
        self._build_keyboard(kb)
        self._build_controller(pad)
        if start_tab == 1:
            nb.select(pad)
        bottom = ttk.Frame(w, style="Card.TFrame", padding=(12, 0, 12, 10))
        bottom.pack(fill="x")
        ttk.Label(bottom, text="Changes apply the next time the game starts.",
                  style="Muted.TLabel").pack(side="left")
        ttk.Button(bottom, text="Close", command=self.close).pack(side="right")
        w.protocol("WM_DELETE_WINDOW", self.close)
        w.after(30, lambda: ui_kit.title_bar(w))       # dark title bar in the dark theme (once the frame exists)

    def close(self) -> None:
        self._stop_capture()
        self.win.destroy()

    # ------------------------------------------------------------------ keyboard / mouse
    def _build_keyboard(self, f: ttk.Frame) -> None:
        ttk.Label(f, text="Mouse", style="Head.TLabel").pack(anchor="w")
        mrow = ttk.Frame(f, style="Card.TFrame")
        mrow.pack(fill="x", pady=(4, 2))
        self.m_cam = tk.BooleanVar(value=self.mouse["camera"])
        self.m_inv = tk.BooleanVar(value=self.mouse["invert_y"])
        self.m_sens = tk.DoubleVar(value=self.mouse["sens"])
        ttk.Checkbutton(mrow, text="Move the camera with the mouse", variable=self.m_cam,
                        command=self._store_mouse).pack(side="left")
        ttk.Checkbutton(mrow, text="Invert up/down", variable=self.m_inv,
                        command=self._store_mouse).pack(side="left", padx=(16, 0))
        srow = ttk.Frame(f, style="Card.TFrame")
        srow.pack(fill="x", pady=(2, 2))
        ttk.Label(srow, text="Sensitivity", style="Card.TLabel").pack(side="left")
        self.m_sens_lbl = ttk.Label(srow, text="", style="Card.TLabel", width=5)
        scale = ttk.Scale(srow, from_=0.1, to=5.0, variable=self.m_sens, length=300,
                          command=lambda _v: self._store_mouse())
        scale.pack(side="left", padx=8)
        self.m_sens_lbl.pack(side="left")
        ttk.Label(f, text="In the game window the cursor is hidden while the mouse moves the camera; "
                  "F2 frees it (and captures it again). Mouse buttons can be bound to actions below.",
                  style="Muted.TLabel", wraplength=660, justify="left").pack(anchor="w", pady=(0, 10))
        self._store_mouse(save=False)

        ttk.Label(f, text="Keys", style="Head.TLabel").pack(anchor="w")
        ttk.Label(f, text="Select an action, 'Set key', then press a key or click a mouse button over the list.",
                  style="Muted.TLabel", wraplength=660, justify="left").pack(anchor="w", pady=(2, 6))
        tree = self.k_tree = ttk.Treeview(f, columns=("action", "keys"), show="headings", height=14)
        tree.heading("action", text="Action")
        tree.heading("keys", text="Keys / mouse buttons")
        tree.column("action", width=250, anchor="w")
        tree.column("keys", width=380, anchor="w")
        tree.pack(fill="both", expand=True)
        self.k_status = tk.StringVar()
        ttk.Label(f, textvariable=self.k_status, style="Muted.TLabel", wraplength=660,
                  justify="left").pack(anchor="w", pady=(6, 0))
        row = ttk.Frame(f, style="Card.TFrame")
        row.pack(fill="x", pady=(8, 0))
        for text, cmd in (("Set key", lambda: self._k_capture(False)), ("Add key", lambda: self._k_capture(True)),
                          ("Clear", self._k_clear), ("Default", self._k_default), ("Reset all", self._k_reset)):
            ttk.Button(row, text=text, command=cmd).pack(side="left", padx=(0, 6))
        tree.bind("<Double-1>", lambda _e: self._k_capture(False))
        self._k_refresh()

    def _store_mouse(self, save: bool = True) -> None:
        self.mouse = controls.normalize_mouse({"camera": self.m_cam.get(), "invert_y": self.m_inv.get(),
                                               "sens": round(self.m_sens.get(), 2)})
        self.m_sens_lbl.configure(text="%.1f" % self.mouse["sens"])
        if save:
            controls.save_mouse(self.mouse_path, self.mouse)

    def _k_refresh(self, keep: str | None = None) -> None:
        t = self.k_tree
        t.delete(*t.get_children())
        for name, label in controls.ACTIONS:
            t.insert("", "end", iid=name, values=(label, controls.describe(self.binds[name])))
        if keep:
            t.selection_set(keep)
            t.see(keep)
        clash = controls.conflicts(self.binds)
        self.k_status.set(("Same key on several actions: " + "; ".join(clash)) if clash else "")

    def _k_selected(self) -> str | None:
        sel = self.k_tree.selection()
        return sel[0] if sel else None

    def _k_store(self, name: str) -> None:
        self.binds.update(controls.normalize(self.binds))
        controls.save(self.keys_path, self.binds)
        self._k_refresh(name)

    def _k_capture(self, add: bool) -> None:
        name = self._k_selected()
        t = self.k_tree
        if not name:
            self.k_status.set("Select an action first.")
            return
        if add and len(self.binds[name]) >= controls.MAX_KEYS:
            self.k_status.set(f"At most {controls.MAX_KEYS} keys per action.")
            return
        label = dict(controls.ACTIONS)[name]
        self.k_status.set(f"Press a key or click a mouse button for '{label}'...  (Esc cancels)")

        def done() -> None:
            t.unbind("<KeyPress>")
            t.unbind("<ButtonPress>")

        def take(vk: int) -> None:
            done()
            if vk in controls.RESERVED or not 0 < vk < 256:
                self.k_status.set(f"{controls.key_name(vk)} cannot be used (F1 = menu, F2 = mouse capture, F3 = FPS counter).")
                return
            self.binds[name] = (self.binds[name] + [vk]) if add else [vk]
            self._k_store(name)

        def on_key(ev) -> str:
            if ev.keysym == "Escape":
                done()
                self.k_status.set("Cancelled.")
                return "break"
            take(int(ev.keycode))
            return "break"

        def on_button(ev) -> str:
            vk = controls.MOUSE_BUTTONS.get(int(ev.num))
            if vk:
                take(vk)
            return "break"
        # bind after this click has been handled, so the click on the button itself is not taken
        self.win.after(150, lambda: (t.bind("<KeyPress>", on_key), t.bind("<ButtonPress>", on_button), t.focus_set()))

    def _k_clear(self) -> None:
        name = self._k_selected()
        if name:
            self.binds[name] = []
            self._k_store(name)

    def _k_default(self) -> None:
        name = self._k_selected()
        if name:
            self.binds[name] = list(controls.DEFAULTS[name])
            self._k_store(name)

    def _k_reset(self) -> None:
        if messagebox.askyesno("Controls", "Reset every key to its default?", parent=self.win):
            self.binds = controls.normalize({})
            controls.save(self.keys_path, self.binds)
            self._k_refresh()

    # ------------------------------------------------------------------ controller
    # Eden / yuzu style: device (top left), profile (top right), mapping
    # buttons for the left half of the pad on the left, right half on the
    # right, a Switch Pro controller in the middle that shows live what the
    # game would receive (pressed buttons light up, stick dots move).
    # Layout as in the Switch emulators' controller page (Eden / yuzu style; own drawing and code): framed groups
    # with a caption over every binding, the controller in the middle.  (group title, [(name, caption, row, col)])
    _LEFT_GROUPS = [
        ("Left Stick", "stick_l"),
        ("D-Pad", [("DPAD_LEFT", "Left", 0, 0), ("DPAD_UP", "Up", 0, 1),
                   ("DPAD_DOWN", "Down", 1, 0), ("DPAD_RIGHT", "Right", 1, 1)]),
    ]
    _RIGHT_GROUPS = [
        ("Face Buttons", [("X", "X", 0, 0), ("A", "A", 0, 1), ("Y", "Y", 1, 0), ("B", "B", 1, 1)]),
        ("Right Stick", "stick_r"),
    ]
    _TOP_GROUPS = [("ZL", "ZL"), ("L", "L"), ("MINUS", "Minus"), ("PLUS", "Plus"), ("ZR", "ZR"), ("R", "R")]
    _BOTTOM_GROUPS = [("CAPTURE", "Capture"), ("HOME", "Home")]

    def _group(self, parent, title: str) -> tk.Frame:
        """A framed group: centred title above a thin-bordered box (returns the box)."""
        c = self.c
        outer = tk.Frame(parent, bg=c["card"])
        tk.Label(outer, text=title, bg=c["card"], fg=c["text"], font=("Segoe UI", 9)).pack()
        box = tk.Frame(outer, bg=c["card"], highlightthickness=1, highlightbackground=c["border"], padx=4, pady=3)
        box.pack(fill="both", expand=True)
        box.outer = outer
        return box

    def _bind_button(self, parent, name: str, caption: str, row: int, col: int, command=None,
                     clear: bool = True) -> ttk.Button:
        """A caption over a binding button (the button shows what the Switch input is bound to)."""
        c = self.c
        cell = tk.Frame(parent, bg=c["card"])
        cell.grid(row=row, column=col, padx=2, pady=1, sticky="n")
        if caption:
            tk.Label(cell, text=caption, bg=c["card"], fg=c["text"], font=("Segoe UI", 9)).pack()
        b = ttk.Button(cell, width=11, style="Map.TButton",
                       command=command or (lambda n=name: self._p_capture(n)))
        b.pack()
        if clear:
            b.bind("<Button-3>", lambda _e, n=name: self._p_clear(n))
        self.p_btns[name] = b
        return b

    def _build_controller(self, f: ttk.Frame) -> None:
        c = self.c
        st = ttk.Style(self.win)
        st.configure("Map.TButton", padding=(4, 5), font=("Segoe UI", 9))
        st.configure("MapOn.TButton", padding=(4, 5), font=("Segoe UI", 9),          # same font: no size change
                     background=c["accent"], foreground="white")
        st.map("MapOn.TButton", background=[("active", c["accent"])])
        st.configure("MapCap.TButton", padding=(4, 5), font=("Segoe UI", 9),
                     background=c["warn"], foreground="black")
        st.map("MapCap.TButton", background=[("active", c["warn"])])
        self.p_btns: dict[str, ttk.Button] = {}

        # --- top: input device, profile
        top = tk.Frame(f, bg=c["card"])
        top.pack(fill="x")
        dev = self._group(top, "Input Device")
        dev.outer.pack(side="left", fill="x", expand=True)
        self.p_dev = tk.StringVar()
        self.p_dev_box = ttk.Combobox(dev, textvariable=self.p_dev, state="readonly", width=40,
                                      postcommand=lambda: self._p_devices(rescan=True))
        self.p_dev_box.pack(side="left", fill="x", expand=True, padx=(0, 4))
        self.p_dev_box.bind("<<ComboboxSelected>>", lambda _e: self._p_set_device())
        ttk.Button(dev, text="Refresh", command=lambda: self._p_devices(rescan=True)).pack(side="left")
        prof = self._group(top, "Profile")
        prof.outer.pack(side="left", fill="x", expand=True, padx=(8, 0))
        self.p_prof = tk.StringVar()
        self.p_prof_box = ttk.Combobox(prof, textvariable=self.p_prof, state="readonly", width=24)
        self.p_prof_box.pack(side="left", fill="x", expand=True, padx=(0, 4))
        for text, cmd in (("Load", self._p_load), ("Save as...", self._p_save_as), ("Delete", self._p_delete)):
            ttk.Button(prof, text=text, command=cmd).pack(side="left", padx=(0, 3))

        # --- body: left groups | middle (top row, controller, bottom row) | right groups
        body = tk.Frame(f, bg=c["card"])
        body.pack(fill="both", expand=True, pady=(8, 0))
        left = tk.Frame(body, bg=c["card"])
        mid = tk.Frame(body, bg=c["card"])
        right = tk.Frame(body, bg=c["card"])
        left.pack(side="left", fill="y")
        right.pack(side="right", fill="y")
        mid.pack(side="left", fill="both", expand=True, padx=8)
        for parent, groups in ((left, self._LEFT_GROUPS), (right, self._RIGHT_GROUPS)):
            for title, spec in groups:
                g = self._group(parent, title)
                g.outer.pack(fill="x", pady=(0, 6))
                if isinstance(spec, str):
                    self._p_stick_group(g, spec)
                else:
                    for name, cap, r, col in spec:
                        self._bind_button(g, name, cap, r, col)

        row = tk.Frame(mid, bg=c["card"])
        row.pack()
        for name, cap in self._TOP_GROUPS:
            g = self._group(row, cap)
            g.outer.pack(side="left", padx=2)
            self._bind_button(g, name, "", 0, 0)
        cv = self.p_canvas = tk.Canvas(mid, width=pad_art.W, height=pad_art.H, bg=c["card"], highlightthickness=0)
        cv.pack(pady=(6, 0))
        bot = tk.Frame(mid, bg=c["card"])
        bot.pack()
        for name, cap in self._BOTTOM_GROUPS:
            g = self._group(bot, cap)
            g.outer.pack(side="left", padx=2)
            self._bind_button(g, name, "", 0, 0)
        # the live line and the status: fixed size (lines x width), so changing text never moves the layout
        self.p_live = tk.StringVar(value="")
        tk.Label(mid, textvariable=self.p_live, bg=c["card"], fg=c["muted"], font=("Segoe UI", 9), anchor="n",
                 justify="center", wraplength=470, height=3, width=74).pack(pady=(6, 0))
        self.p_status = tk.StringVar(value="Click a button (or the controller), then press the button you want on "
                                           "your controller.  Right-click clears it.")
        tk.Label(mid, textvariable=self.p_status, bg=c["card"], fg=c["muted"], font=("Segoe UI", 9), anchor="n",
                 justify="center", wraplength=470, height=3, width=74).pack(pady=(2, 0))

        # --- bottom bar: deadzone | map all, defaults, clear
        bar = tk.Frame(f, bg=c["card"])
        bar.pack(fill="x", pady=(8, 0))
        dz = self._group(bar, "Stick deadzone")
        dz.outer.pack(side="left")
        self.p_dead = tk.IntVar(value=self.pad["deadzone"])
        ttk.Scale(dz, from_=0, to=50, variable=self.p_dead, length=200,
                  command=lambda _v: self._p_options()).pack(side="left", padx=(2, 6))
        self.p_dead_lbl = ttk.Label(dz, text="", style="Card.TLabel", width=5)
        self.p_dead_lbl.pack(side="left")
        tk.Label(bar, text="Any controller works, USB or Bluetooth: Xbox pads (XInput), PlayStation, Switch Pro, "
                 "8BitDo and other HID pads.  Xbox and HID pads keep separate layouts.", bg=c["card"],
                 fg=c["muted"], font=("Segoe UI", 8), wraplength=380, justify="center").pack(side="left",
                                                                                          expand=True)
        for text, cmd in (("Clear", self._p_clear_all), ("Defaults", self._p_default), ("Map all...", self._p_wizard)):
            ttk.Button(bar, text=text, command=cmd).pack(side="right", padx=(4, 0), anchor="s")

        self._capture = None          # ("button", name) or ("stick", "l"/"r") while waiting for input
        self._wizard: list[str] | None = None   # "Map all": the Switch buttons still to ask for
        self._lit: set[str] = set()
        self._hid: list[dict] = []
        self._hid_t = 0.0
        self._kind = controls.device_kind(self.pad["device"])
        self._vid = None
        self._p_draw_pad()
        self._p_devices(rescan=False)
        self._p_profiles()
        self._p_refresh()
        self._poll_job = self.win.after(40, self._p_poll)

    def _p_stick_group(self, g: tk.Frame, which: str) -> None:
        """Left / Up / Down / Right (each learns the whole stick), Pressed, Invert Y."""
        side = "l" if which == "stick_l" else "r"
        for d, cap, r, col in (("left", "Left", 0, 0), ("up", "Up", 0, 1), ("down", "Down", 1, 0),
                               ("right", "Right", 1, 1)):
            self._bind_button(g, f"{which}_{d}", cap, r, col, command=lambda s=side: self._p_capture_stick(s),
                              clear=False)
        name = "LSTICK" if side == "l" else "RSTICK"
        self._bind_button(g, name, "Pressed", 2, 0)
        if side == "l":
            var = self.p_inv_l = tk.BooleanVar(value=self.pad["invert_ly"])
        else:
            var = self.p_inv_r = tk.BooleanVar(value=self.pad["invert_ry"])
        cell = tk.Frame(g, bg=self.c["card"])
        cell.grid(row=2, column=1, sticky="s", pady=(0, 6))
        ttk.Checkbutton(cell, text="Invert Y", variable=var, command=self._p_options).pack()

    # --- drawing: the controller picture (launcher/pad_art.py) ---------------------------------------
    def _p_draw_pad(self) -> None:
        self.p_art = pad_art.PadArt(self.p_canvas, self.c, self._p_capture)

    def _p_refresh(self) -> None:
        """Texts of the mapping buttons (what each Switch input is bound to)."""
        m = self._p_map()
        for name, label in controls.PAD_ACTIONS:
            b = self.p_btns.get(name)
            if b is not None:
                b.configure(text=_short_inputs(m[name], self._kind, self._vid))
        swap = self.pad["swap_sticks"]
        ga = self.pad["gaxes"]
        for which, side in (("stick_l", "l"), ("stick_r", "r")):
            if self._kind == "j":             # HID pad: the learned axes, as "Axis X-" etc.
                x, y = ga[side + "x"], ga[side + "y"]
                texts = {"left": f"Axis {x}-", "right": f"Axis {x}+", "up": f"Axis {y}-", "down": f"Axis {y}+"}
            else:                             # Xbox pad: which physical stick, and the direction
                src = ("R" if side == "l" else "L") if swap else ("L" if side == "l" else "R")
                texts = {d: f"{src} stick {arrow}" for d, arrow in
                         (("left", "←"), ("right", "→"), ("up", "↑"), ("down", "↓"))}
            for d in ("up", "left", "right", "down"):
                self.p_btns[f"{which}_{d}"].configure(text=texts[d])
        self.p_inv_l.set(self.pad["invert_ly"])
        self.p_inv_r.set(self.pad["invert_ry"])
        self.p_dead.set(self.pad["deadzone"])
        self.p_dead_lbl.configure(text="%d%%" % self.pad["deadzone"])
        dup = controls.layout_conflicts(m)
        acts = dict(controls.PAD_ACTIONS)
        self._dup_text = "" if not dup else "\nOn two Switch buttons (one press does both): " + "; ".join(
            f"{controls.input_label(k, self._kind, self._vid)} = {' + '.join(acts[n] for n in v)}" for k, v in dup.items())
        self._lit = set()            # force the next poll to repaint

    # --- live view + capture -------------------------------------------
    def _p_map(self) -> dict:
        """The layout being edited: Xbox (XInput) or HID, by the active controller's kind."""
        return self.pad["map"] if self._kind == "x" else self.pad["gmap"]

    def _p_read(self):
        """(state, None) - the chosen controller's state (controls.read_device) or None."""
        import time
        now = time.monotonic()
        if now - self._hid_t > 3.0:            # pads plugged in / paired while the window is open
            self._hid = controls.hid_scan(rescan=False)
            self._hid_t = now
        st = controls.read_device(self.pad["device"], self._hid)
        kind = st["kind"] if st else controls.device_kind(self.pad["device"])
        vid = st["vid"] if st else self._vid
        if (kind, vid) != (self._kind, self._vid):
            self._kind, self._vid = kind, vid
            self._capture = None
            self._p_refresh()
        return st, None

    def _p_poll(self) -> None:
        try:
            if not self.win.winfo_exists():
                return
        except tk.TclError:
            return
        full, _ = self._p_read()
        cv, c = self.p_canvas, self.c
        if self._capture and full is not None:
            self._p_capture_tick(full)
        btns, ls, rs = controls.pad_view(full, self.pad)
        lit = set(btns)
        self.p_art.set_sticks(ls, rs)
        for side, (x, y) in (("l", ls), ("r", rs)):
            if abs(x) > 0.5 or abs(y) > 0.5:
                which = "stick_l" if side == "l" else "stick_r"
                if y > 0.5:
                    lit.add(f"{which}_up")
                if y < -0.5:
                    lit.add(f"{which}_down")
                if x < -0.5:
                    lit.add(f"{which}_left")
                if x > 0.5:
                    lit.add(f"{which}_right")
        if lit != self._lit:
            self.p_art.draw({n for n in lit if not n.startswith("stick_")})
            cap = self._capture[1] if self._capture and self._capture[0] == "button" else None
            for name, b in self.p_btns.items():
                style = "MapCap.TButton" if name == cap else ("MapOn.TButton" if name in lit else "Map.TButton")
                if str(b.cget("style")) != style:
                    b.configure(style=style)
            self._lit = lit
        if full is None:
            self.p_live.set("No controller connected" + ("" if self.pad["device"] == "any"
                                                          else " (the chosen one) - pick another or press Refresh"))
        else:
            names = [dict(controls.PAD_ACTIONS)[n] for n, _ in controls.PAD_ACTIONS if n in btns]
            order = controls.PAD_INPUTS if full["kind"] == "x" else controls.GEN_INPUTS
            raw = [controls.input_label(k, full["kind"], full["vid"]) for k in sorted(full["pressed"], key=order.index)
                   if not (len(k) == 2 and k[1] in "+-")]          # axis halves: the stick line shows those
            self.p_live.set(f"{full['name']}:  Switch " + (" + ".join(names) if names else "(nothing)")
                            + "   |   your controller: " + (" + ".join(raw) if raw else "(nothing)")
                            + f"\nleft stick {ls[0]:+.2f} {ls[1]:+.2f}   right stick {rs[0]:+.2f} {rs[1]:+.2f}"
                            + self._dup_text)
        self._poll_job = self.win.after(40, self._p_poll)

    def _p_capture(self, name: str, wizard: bool = False) -> None:
        if not wizard:
            self._wizard = None           # a click on a button ends "Map all"
        full, _ = self._p_read()
        if full is None:
            self.p_status.set("No controller found. Connect one (or pick another input device) and press Refresh.")
            return
        self._capture = ("button", name, set(full["pressed"]), 125)
        self._lit = {"-"}            # repaint: the waiting button turns yellow
        self.p_status.set(f"Press the button for Switch {dict(controls.PAD_ACTIONS)[name]}...  (5 s)")

    def _p_capture_stick(self, side: str) -> None:
        full, _ = self._p_read()
        if full is None:
            self.p_status.set("No controller found. Connect one and press Refresh.")
            return
        which = "left" if side == "l" else "right"
        if full["kind"] == "j":
            # HID pad: learn the two axes - first left/right, then up/down
            self._capture = ("axis_x", side, dict(full["axes"]), 150)
            self.p_status.set(f"Switch {which} stick: push your stick fully LEFT and RIGHT...")
            return
        self._capture = ("stick", side, None, 125)
        self.p_status.set(f"Move the stick you want as the Switch {which} stick...")

    def _p_capture_tick(self, full) -> None:
        kind, name, held, left = self._capture
        if kind == "button":
            now = set(full["pressed"])
            new = now - held
            if new:
                order = controls.PAD_INPUTS if full["kind"] == "x" else controls.GEN_INPUTS
                inp = sorted(new, key=order.index)[0]
                taken = controls.assign_input(self._p_map(), name, inp)
                self._capture = None
                self._p_save()
                self._p_refresh()
                acts = dict(controls.PAD_ACTIONS)
                msg = f"Switch {acts[name]} = {controls.input_label(inp, full['kind'], full['vid'])}."
                if taken:
                    msg += "  (Taken off " + ", ".join(acts[t] for t in taken) + ".)"
                self.p_status.set(msg)
                self._p_wizard_next(msg)
                return
            held = held & now
        elif kind in ("axis_x", "axis_y"):
            base = held if kind == "axis_x" else held[0]
            skip = None if kind == "axis_x" else held[1]
            moved = [(abs(v - base.get(a, 0.0)), a) for a, v in full["axes"].items() if a != skip]
            amount, axis = max(moved) if moved else (0.0, None)
            if axis and amount > 0.6:
                which = "left" if name == "l" else "right"
                if kind == "axis_x":
                    self._capture = ("axis_y", name, (base, axis), 150)
                    self.p_status.set(f"Switch {which} stick: left/right = axis {axis}. Now push it fully UP "
                                      f"and DOWN...")
                    return
                self.pad["gaxes"][name + "x"], self.pad["gaxes"][name + "y"] = skip, axis
                self._capture = None
                self._p_save()
                self._p_refresh()
                self.p_status.set(f"Switch {which} stick = axes {skip} (left/right) and {axis} (up/down). "
                                  f"Use 'Invert Y' if up and down are the wrong way round.")
                return
        else:
            lx, ly, rx, ry = full["sticks"]
            lmag = max(abs(lx), abs(ly))
            rmag = max(abs(rx), abs(ry))
            if max(lmag, rmag) > 0.6:
                phys = "l" if lmag >= rmag else "r"
                self.pad["swap_sticks"] = phys != name
                self._capture = None
                self._p_save()
                self._p_refresh()
                self.p_status.set(f"Switch {'left' if name == 'l' else 'right'} stick = your "
                                  f"{'left' if phys == 'l' else 'right'} stick.")
                return
        left -= 1
        if left <= 0:
            self._capture = None
            self._lit = {"-"}
            msg = "Nothing pressed - unchanged."
            if kind == "button" and self._wizard is not None:
                msg = f"Switch {dict(controls.PAD_ACTIONS)[name]}: nothing pressed, kept as it was."
            self.p_status.set(msg)
            self._p_wizard_next(msg)
            return
        self._capture = (kind, name, held, left)

    # --- "Map all": every Switch button in turn ------------------------
    def _p_wizard(self) -> None:
        full, _ = self._p_read()
        if full is None:
            self.p_status.set("No controller found. Connect one (or pick another input device) and press Refresh.")
            return
        self._wizard = [n for n, _ in controls.PAD_ACTIONS]
        self._p_wizard_next("")

    def _p_wizard_next(self, done_msg: str) -> None:
        """After a press (or a timeout) in "Map all": ask for the next Switch button; the end sums up."""
        if self._wizard is None:
            return
        if not self._wizard:
            self._wizard = None
            dup = controls.layout_conflicts(self._p_map())
            self.p_status.set("All buttons set." + ("" if not dup else "  Still on two buttons: " + "; ".join(
                f"{controls.input_label(k, self._kind, self._vid)} ({' + '.join(v)})" for k, v in dup.items())))
            return
        name = self._wizard.pop(0)
        self._p_capture(name, wizard=True)
        left = len(self._wizard)
        self.p_status.set((done_msg + "\n" if done_msg else "") +
                          f"Map all: press the button for Switch {dict(controls.PAD_ACTIONS)[name]}  "
                          f"({left} more after this; 5 s each, wait to keep a button as it is)")

    def _stop_capture(self) -> None:
        self._capture = None
        job = getattr(self, "_poll_job", None)
        if job is not None:
            try:
                self.win.after_cancel(job)
            except tk.TclError:
                pass
            self._poll_job = None

    # --- settings / profiles -------------------------------------------
    def _p_save(self) -> None:
        self.pad = controls.normalize_pad(self.pad)
        controls.save_pad(self.pad_path, self.pad)

    def _p_devices(self, rescan: bool = True) -> None:
        """Fill the device list with the controllers connected right now (the chosen one stays listed)."""
        import time
        self._hid = controls.hid_scan(rescan=rescan)
        self._hid_t = time.monotonic()
        devs = controls.list_devices(self._hid)
        cur = self.pad["device"]
        if cur not in [d for d, _ in devs]:
            devs.append((cur, f"{_device_text(cur)}  (not connected)"))
        self.p_dev_ids = [d for d, _ in devs]
        self.p_dev_labels = [lab for _, lab in devs]
        self.p_dev_box.configure(values=self.p_dev_labels)
        self.p_dev.set(self.p_dev_labels[self.p_dev_ids.index(cur)])

    def _p_set_device(self) -> None:
        i = self.p_dev_labels.index(self.p_dev.get()) if self.p_dev.get() in self.p_dev_labels else 0
        self.pad["device"] = self.p_dev_ids[i]
        self._p_save()
        n = len(self.p_dev_ids) - 1
        self.p_status.set(f"The game will use: {self.p_dev_labels[i].strip()}." +
                          ("" if n else "  (No controller connected right now.)"))

    def _p_options(self) -> None:
        self.pad.update(invert_ly=self.p_inv_l.get(), invert_ry=self.p_inv_r.get(), deadzone=int(self.p_dead.get()))
        self.p_dead_lbl.configure(text="%d%%" % int(self.p_dead.get()))
        self._p_save()

    def _p_clear(self, name: str) -> None:
        self._capture = None
        self._p_map()[name] = []
        self._p_save()
        self._p_refresh()
        self.p_status.set(f"Switch {dict(controls.PAD_ACTIONS)[name]} cleared.")

    def _p_clear_all(self) -> None:
        kind = "Xbox" if self._kind == "x" else "HID controller"
        if messagebox.askyesno("Controller", f"Unbind every button of the {kind} layout?", parent=self.win):
            self.pad["map" if self._kind == "x" else "gmap"] = {n: [] for n, _ in controls.PAD_ACTIONS}
            self._p_save()
            self._p_refresh()

    def _p_profiles(self, select: str | None = None) -> None:
        names = controls.list_profiles(self.profiles_dir)
        self.p_prof_box.configure(values=names)
        if select in names:
            self.p_prof.set(select)
        elif self.p_prof.get() not in names:
            self.p_prof.set(names[0] if names else "")

    def _p_load(self) -> None:
        name = self.p_prof.get()
        if not name:
            self.p_status.set("No profile saved yet - use 'Save as...'.")
            return
        dev = self.pad["device"]
        self.pad = controls.load_pad(self.profiles_dir / f"{name}.json")
        self.pad["device"] = dev               # the profile is the layout; the device stays as chosen
        self._p_save()
        self._p_refresh()
        self.p_status.set(f"Profile '{name}' loaded and active.")

    def _p_save_as(self) -> None:
        name = simpledialog.askstring("Save profile", "Profile name:", parent=self.win,
                                      initialvalue=self.p_prof.get() or "My controller")
        if name is None:
            return
        name = name.strip()
        if not controls.profile_name_ok(name):
            self.p_status.set("Use a short name without  < > : \" / \\ | ? *")
            return
        path = self.profiles_dir / f"{name}.json"
        if path.exists() and not messagebox.askyesno("Save profile", f"Replace profile '{name}'?", parent=self.win):
            return
        controls.save_pad(path, self.pad)
        self._p_profiles(select=name)
        self.p_status.set(f"Saved profile '{name}'.")

    def _p_delete(self) -> None:
        name = self.p_prof.get()
        if not name:
            return
        if not messagebox.askyesno("Delete profile", f"Delete profile '{name}'?", parent=self.win):
            return
        try:
            (self.profiles_dir / f"{name}.json").unlink()
        except OSError as e:
            self.p_status.set(f"Could not delete: {e}")
            return
        self.p_prof.set("")
        self._p_profiles()
        self.p_status.set(f"Deleted profile '{name}' (the active layout is unchanged).")

    def _p_default(self) -> None:
        if not messagebox.askyesno("Controller", "Back to the default layouts (Switch button positions: Xbox B / "
                                   "PlayStation Circle = Switch A)?", parent=self.win):
            return
        self.pad = controls.normalize_pad({"device": self.pad["device"]})
        self._p_save()
        self._p_refresh()
        self.p_status.set("Default layout restored.")


def game_env(config_dir: Path, out_dir: Path) -> dict[str, str]:
    """Environment for the game: key map, controller map and mouse settings files."""
    env: dict[str, str] = {}
    env["SW_KEYMAP"] = str(controls.write_keymap(out_dir / "controls.txt", controls.load(config_dir / "controls.json")))
    env["SW_PADMAP"] = str(controls.write_padmap(out_dir / "controller.txt",
                                                 controls.load_pad(config_dir / "controller.json")))
    env.update(controls.mouse_env(controls.load_mouse(config_dir / "mouse.json")))
    return env
