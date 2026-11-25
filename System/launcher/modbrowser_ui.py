"""Mods tab > Browse: the Mod Explorer (MODS.md section 8).

Look: a game's own mod hub, not a web page - near-black surfaces, warm gold accent, big art, quiet motion.  Drawn on
Tk Canvases with launcher/ui_kit.py's easing (the launcher's UI framework: plain tkinter + a Canvas widget kit; no
extra packages).  Keyboard (arrows / Enter / Esc / Tab) and controller (XInput: D-pad / stick, A, B, X, Y, LB / RB)
move a gold focus ring over the cards.

Everything slow runs in mb_cli.exe on worker threads (modbrowser_cli.py); results come back to the Tk thread through
a queue polled with after(), so the launcher never freezes.  Nothing is started, read or sent before the page is
opened the first time; the controller is polled only while the page is visible and the launcher is in front."""
from __future__ import annotations

import ctypes
import os
import queue
import re
import threading
import time
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import filedialog, simpledialog

import modbrowser_cli as mbc
import ui_kit as ui

# ---- the explorer's palette (darker than the launcher, gold instead of blue)
P = {
    "bg": "#0c0d10", "rail": "#101216", "panel": "#16181d", "panel2": "#1d2027", "lift": "#22252d",
    "edge": "#262a32", "edge2": "#343945", "ink": "#ece8df", "muted": "#8e8a80", "faint": "#5d5a54",
    "gold": "#c9a45c", "gold2": "#e3c27a", "goldDim": "#6e5a33", "onGold": "#16130c",
    "good": "#7fbf7a", "warn": "#d9a441", "bad": "#d36a57",
}
CARD_W, IMG_W, IMG_H, GAP = 336, 320, 180, 18
HEAD_H, RAIL_W, TRAY_MIN = 76, 214, 44

NOTICE = ("Mods are made by third parties. They are not checked by this project, and you install them at your own "
          "risk. The explorer only talks to the sources you switch on, always credits the author and links back to "
          "the mod's page, and never installs programs or scripts. Code patches (most \"60 FPS\" / camera patches) "
          "cannot work in this recompiled game and are skipped.")
SETTING_KEYS = ("gamebanana_game_id", "nexus_api_key", "nexus_game_domain", "modio_api_key", "modio_game_id")


def _spaced(s: str) -> str:
    return " ".join(s.upper())


class _Pad:
    """XInput (Windows' own controller API): the first connected pad; read() is None without one."""
    UP, DOWN, LEFT, RIGHT, LB, RB, A, B, X, Y = 0x1, 0x2, 0x4, 0x8, 0x100, 0x200, 0x1000, 0x2000, 0x4000, 0x8000

    class _State(ctypes.Structure):
        _fields_ = [("packet", ctypes.c_uint32), ("buttons", ctypes.c_uint16), ("lt", ctypes.c_uint8),
                    ("rt", ctypes.c_uint8), ("lx", ctypes.c_int16), ("ly", ctypes.c_int16),
                    ("rx", ctypes.c_int16), ("ry", ctypes.c_int16)]

    def __init__(self):
        self.fn = None
        for dll in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
            try:
                self.fn = getattr(ctypes.windll, dll).XInputGetState
                break
            except (AttributeError, OSError):
                continue

    def read(self) -> int | None:
        if not self.fn:
            return None
        st = self._State()
        for i in range(4):
            if self.fn(i, ctypes.byref(st)) == 0:
                b = st.buttons
                if st.ly > 16000:
                    b |= self.UP
                if st.ly < -16000:
                    b |= self.DOWN
                if st.lx < -16000:
                    b |= self.LEFT
                if st.lx > 16000:
                    b |= self.RIGHT
                return b
        return None


class BrowsePanel:
    def __init__(self, parent: tk.Misc, root_dir: Path, out_dir: Path, colors: dict, exefs: Path | None = None):
        self.parent, self.c = parent, colors
        self.root_dir, self.out_dir = Path(root_dir), Path(out_dir)
        mock = os.environ.get("SW_MB_MOCK", "").strip()
        settings = os.environ.get("SW_MB_SETTINGS", "").strip()
        mods_root = os.environ.get("SW_MB_MODS_ROOT", "").strip()   # tests: install somewhere else
        self.cli = mbc.Cli(exe=self.out_dir / "mb_cli.exe", mods_root=Path(mods_root) if mods_root else self.root_dir / "Mods",
                           settings=Path(settings) if settings else self.root_dir / "Settings" / "modbrowser.json",
                           cache=self.out_dir / "mod_cache", exefs=exefs, mock=Path(mock) if mock else None)
        self.q: queue.Queue = queue.Queue()
        self.built = False
        self.sources: list[dict] = []
        self.target = "1.0.0"
        self.src_index = 0
        self.results: list[dict] = []
        self.loading = False
        self.error = ""
        self.page = 1
        self.more = False
        self.sort = 0
        self.only = True
        self.cols = 1
        self.installed: list[dict] = []
        self.images: dict[tuple, tk.PhotoImage] = {}
        self.img_pending: set[tuple] = set()
        self.img_sem = threading.Semaphore(3)
        self.jobs: list[dict] = []
        self.job_lock = threading.Lock()
        self.job_wake = threading.Event()
        self.detail: dict | None = None
        self.detail_tab = 0
        self.detail_img = 0
        self.search_seq = 0
        self.focus = 0                      # focused card (keyboard / controller)
        self.zone = "grid"                  # grid | detail
        self.dfocus = 0                     # focused action on the detail page
        self.cards: list[dict] = []
        self.toasts: list[list] = []
        self.tray_open = False
        self.notice_up = False
        self.hits: dict = {}
        self._hover_key = None
        self._detail_actions: list = []
        self._detail_widgets: list = []
        self.pad = _Pad() if os.name == "nt" else None
        self.pad_prev = 0
        self.pad_next = 0.0

    # ================================================================== threads -> Tk
    def bg(self, fn, done=None):
        def run():
            try:
                res, err = fn(), None
            except Exception as e:          # noqa: BLE001 - every failure becomes a message, never a crash
                res, err = None, e
            self.q.put((done, res, err))
        threading.Thread(target=run, daemon=True).start()

    def _poll(self):
        try:
            while True:
                done, res, err = self.q.get_nowait()
                if done:
                    try:
                        done(res, err)
                    except tk.TclError:
                        pass
        except queue.Empty:
            pass
        try:
            self.frame.after(60, self._poll)
        except tk.TclError:
            pass

    # ================================================================== build / show
    def show(self):
        if not self.built:
            self._build()
            self.frame.after(60, self._poll)
            self.frame.after(60, self._pad_tick)
            self.frame.after(50, self._anim_tick)
            threading.Thread(target=self._job_worker, daemon=True).start()
        if not self.cli.available():
            self.error = "The mod explorer's helper (mb_cli.exe) is missing - build the game first (Build tab)."
            self._draw_grid()
            return
        st = mbc.load_settings(self.cli.settings)
        self.only = bool(st.get("only_compatible", True))
        if not st.get("notice_shown"):
            self.frame.after(30, self._show_notice)
            return
        if self.sources:
            self.refresh_installed()
            return
        self.reload_sources()

    def _build(self):
        self.built = True
        f = self.frame = tk.Frame(self.parent, bg=P["bg"])
        f.pack(fill="both", expand=True)

        def cv(bg=P["bg"], **k):
            return tk.Canvas(f, bg=bg, highlightthickness=0, bd=0, **k)
        self.head = cv()
        self.rail = cv(P["rail"])
        self.grid = cv()
        self.tray = cv(P["rail"])
        self.detail_cv = cv(P["panel"])
        self.toast_cv = cv()
        self.tray_h = TRAY_MIN
        self.tray_tw = ui.Tween(f, lambda v: self._place(), float(TRAY_MIN), 200)
        self.detail_tw = ui.Tween(f, lambda v: self._place(), 0.0, 240)
        self.search_var = tk.StringVar()
        self.search = tk.Entry(self.head, textvariable=self.search_var, bg=P["panel"], fg=P["ink"], relief="flat",
                               insertbackground=P["gold"], font=ui.F(12), highlightthickness=1,
                               highlightbackground=P["edge"], highlightcolor=P["goldDim"])
        self.search.bind("<Return>", lambda _e: (self.grid.focus_set(), self.do_search()))
        self.search.bind("<Escape>", lambda _e: self.grid.focus_set())
        for c in (self.head, self.rail, self.grid, self.tray, self.detail_cv):
            c.bind("<Motion>", lambda e, cc=c: self._motion(cc, e))
            c.bind("<Leave>", lambda e, cc=c: self._motion(cc, None))
            c.bind("<Button-1>", lambda e, cc=c: self._click(cc, e))
        self.grid.bind("<Configure>", lambda _e: not self.notice_up and self._draw_grid())
        self.head.bind("<Configure>", lambda _e: self._draw_head())
        self.rail.bind("<Configure>", lambda _e: self._draw_rail())
        self.tray.bind("<Configure>", lambda _e: self._draw_tray())
        self.detail_cv.bind("<Configure>", lambda _e: self.detail is not None and self._draw_detail())
        for c in (self.grid, self.detail_cv):
            c.bind("<MouseWheel>", lambda e, cc=c: self._wheel(cc, e))
        f.winfo_toplevel().bind("<Key>", self._key, add="+")
        self._place()

    def _place(self):
        f = self.frame
        th = int(self.tray_tw.value)
        self.head.place(x=0, y=0, relwidth=1, height=HEAD_H)
        self.rail.place(x=0, y=HEAD_H, width=RAIL_W, relheight=1, height=-HEAD_H)
        self.grid.place(x=RAIL_W, y=HEAD_H, relwidth=1, width=-RAIL_W, relheight=1, height=-HEAD_H - th)
        self.tray.place(x=RAIL_W, rely=1, y=-th, relwidth=1, width=-RAIL_W, height=th)
        v = self.detail_tw.value
        w = max(1, f.winfo_width() - RAIL_W)
        if v > 0.001:
            self.detail_cv.place(x=RAIL_W + int((1 - v) * w), y=HEAD_H, relwidth=1, width=-RAIL_W,
                                 relheight=1, height=-HEAD_H - th)
            tk.Misc.lift(self.detail_cv)          # (Canvas.lift raises canvas items, not the widget)
        else:
            self.detail_cv.place_forget()
        if self.toasts:
            self.toast_cv.place(relx=1, x=-16, y=HEAD_H + 10, anchor="ne", width=440, height=40)
            tk.Misc.lift(self.toast_cv)
        if th != self.tray_h:
            self.tray_h = th
            self._draw_tray()

    # ================================================================== drawing helpers
    def _hit(self, cv, bbox, action, key=None):
        self.hits.setdefault(cv, []).append((bbox, action, key))

    def _pill(self, cv, x, y, text, action, kind="ghost", size=11, h=34, focused=False, anchor="w"):
        f = ui.F(size, 600)
        w = int(cv.tk.call("font", "measure", f, text)) + 30
        if anchor == "e":
            x -= w
        fill, fg, edge = {"gold": (P["gold"], P["onGold"], P["gold"]), "ghost": (P["panel2"], P["ink"], P["edge2"]),
                          "quiet": (None, P["muted"], None), "bad": (P["panel2"], P["bad"], P["edge2"])}[kind]
        tag = f"p{len(self.hits.get(cv, []))}"
        ui.round_rect(cv, x, y, x + w, y + h, h / 2, fill=fill or cv["bg"], outline=P["gold2"] if focused else (edge or cv["bg"]),
                      width=2 if focused else 1, tags=(tag, tag + "bg"))
        cv.create_text(x + w / 2, y + h / 2, text=text, fill=fg, font=f, tags=(tag, tag + "fg"))
        self._hit(cv, (x, y, x + w, y + h), action, (tag, kind))
        return x + w

    def _chip(self, cv, x, y, text, color):
        f = ui.F(9, 600)
        w = int(cv.tk.call("font", "measure", f, text)) + 18
        ui.round_rect(cv, x, y, x + w, y + 20, 10, fill=ui.mix(P["panel"], color, 0.16), outline=ui.mix(P["panel"], color, 0.55))
        cv.create_text(x + w / 2, y + 10, text=text, fill=color, font=f)
        return x + w + 6

    def _badge(self, compat: dict) -> tuple[str, str]:
        text, kind = mbc.compat_badge(compat, self.target)
        return text, {"good": P["good"], "warn": P["warn"], "bad": P["bad"]}[kind]

    def _image(self, source: str, url: str, width: int, redraw):
        """The PhotoImage when it is ready, else None (fetched once, in the background, then redraw())."""
        if not url:
            return None
        key = (url, width)
        img = self.images.get(key)
        if img is not None or key in self.img_pending:
            return img
        self.img_pending.add(key)

        def work():
            with self.img_sem:
                return self.cli.thumbnail(source, url, width)

        def done(path, err):
            self.img_pending.discard(key)
            if err or not path:
                return
            try:
                self.images[key] = tk.PhotoImage(file=str(path))
            except tk.TclError:
                return
            redraw()
        self.bg(work, done)
        return None

    def toast(self, text: str, kind: str = "info"):
        self.toasts.append([text, kind, time.time()])
        self.toasts = self.toasts[-3:]

    # ================================================================== header
    def _draw_head(self):
        if not self.built:
            return
        cv = self.head
        cv.delete("all")
        self.hits[cv] = []
        w = cv.winfo_width()
        cv.create_line(0, HEAD_H - 1, w, HEAD_H - 1, fill=P["edge"])
        cv.create_text(26, 28, text=_spaced("Mod Explorer"), anchor="w", fill=P["gold"], font=ui.F(13, 700))
        src = self.sources[self.src_index]["title"] if self.sources else ""
        cv.create_text(26, 52, text=f"Legends: Arceus   ·   target v{self.target}" + (f"   ·   {src}" if src else ""),
                       anchor="w", fill=P["muted"], font=ui.F(10))
        # right to left: settings, compatible / all versions, sort, then the search field
        x = w - 22
        comp = "Compatible only" if self.only else "All versions"
        for label, act, kind in (("Settings", self.open_settings, "ghost"),
                                 (comp, self._toggle_only, "gold" if self.only else "ghost"),
                                 (mbc.SORTS[self.sort], self._next_sort, "ghost")):
            x -= int(cv.tk.call("font", "measure", ui.F(11, 600), label)) + 30
            self._pill(cv, x, 21, label, act, kind)
            x -= 10
        sw = max(160, min(380, x - 420))
        cv.create_window(x - sw, 21, window=self.search, anchor="nw", width=sw, height=34)
        if not self.search_var.get():
            cv.create_text(x - sw - 12, 38, text="Search", anchor="e", fill=P["faint"], font=ui.F(10))

    def _next_sort(self):
        self.sort = (self.sort + 1) % len(mbc.SORTS)
        self._draw_head()
        self.do_search()

    def _toggle_only(self):
        self.only = not self.only
        st = mbc.load_settings(self.cli.settings)
        st["only_compatible"] = self.only
        mbc.save_settings(self.cli.settings, self._merged_settings(st))
        self._draw_head()
        self._draw_grid()

    # ================================================================== rail (sources)
    def _draw_rail(self):
        if not self.built:
            return
        cv = self.rail
        cv.delete("all")
        self.hits[cv] = []
        cv.create_line(RAIL_W - 1, 0, RAIL_W - 1, cv.winfo_height(), fill=P["edge"])
        cv.create_text(26, 28, text=_spaced("Sources"), anchor="w", fill=P["faint"], font=ui.F(9, 700))
        y = 48
        for i, s in enumerate(self.sources):
            on = i == self.src_index
            tag = f"src{i}"
            cv.create_rectangle(0, y, RAIL_W - 1, y + 40, fill=P["panel"] if on else P["rail"], outline="", tags=(tag + "bg",))
            if on:
                cv.create_rectangle(0, y + 9, 3, y + 31, fill=P["gold"], outline="")
            cv.create_oval(26, y + 16, 34, y + 24, fill=P["good"] if s["available"] else P["faint"], outline="")
            cv.create_text(46, y + 20, text=s["title"], anchor="w", fill=P["ink"] if on else P["muted"],
                           font=ui.F(11, 600 if on else 400), tags=(tag,))
            if not s.get("verified") and s["id"] != "mock":
                cv.create_text(RAIL_W - 18, y + 20, text="beta", anchor="e", fill=P["faint"], font=ui.F(8))
            self._hit(cv, (0, y, RAIL_W, y + 40), lambda ii=i: self.select_source(ii), None if on else (tag, "text"))
            y += 42
        y += 20
        cv.create_text(26, y, text=_spaced("Add a mod"), anchor="w", fill=P["faint"], font=ui.F(9, 700))
        y += 16
        for k, (label, act) in enumerate((("From a file...", self._choose_file), ("From a link...", self._ask_link))):
            tag = f"add{k}"
            cv.create_text(26, y + 16, text=label, anchor="w", fill=P["muted"], font=ui.F(11), tags=(tag,))
            self._hit(cv, (0, y, RAIL_W, y + 32), act, (tag, "text"))
            y += 34
        cv.create_text(26, cv.winfo_height() - 26, anchor="w", fill=P["faint"], font=ui.F(9),
                       text=f"{len(self.installed)} mod(s) installed here")

    def select_source(self, i: int):
        if not (0 <= i < len(self.sources)):
            return
        self.src_index = i
        self.focus = 0
        self.close_detail()
        self._draw_rail()
        self._draw_head()
        s = self.sources[i]
        if not s["available"]:
            self.results, self.loading, self.error = [], False, s["why"]
            self._draw_grid()
            return
        if not s["verified"] and s["id"] != "mock":
            self.toast(f"{s['title']} is in beta: not verified against its live API yet.", "warn")
        self.do_search()

    # ================================================================== grid
    def _shown(self) -> list[dict]:
        return [m for m in self.results if not (self.only and m.get("compat", {}).get("compat") == "mismatch")]

    def _draw_grid(self):
        if not self.built or self.notice_up:
            return
        cv = self.grid
        cv.delete("all")
        self.hits[cv] = []
        self.cards = []
        w, h = max(cv.winfo_width(), 300), max(cv.winfo_height(), 200)
        cols = max(1, (w - 56 + GAP) // (CARD_W + GAP))
        self.cols = cols
        x0 = (w - (cols * CARD_W + (cols - 1) * GAP)) // 2
        shown = self._shown()
        card_h = IMG_H + 126
        if not shown:
            if self.loading:
                for k in range(cols * 2):                    # skeleton cards, shimmering (_anim_tick)
                    cx, cy = x0 + (k % cols) * (CARD_W + GAP), 24 + (k // cols) * (card_h + GAP)
                    ui.round_rect(cv, cx, cy, cx + CARD_W, cy + card_h, 14, fill=P["panel"], outline=P["edge"])
                    ui.round_rect(cv, cx + 8, cy + 8, cx + CARD_W - 8, cy + 8 + IMG_H, 10, fill=P["panel2"], outline="", tags=("skel",))
                    cv.create_rectangle(cx + 16, cy + IMG_H + 28, cx + 210, cy + IMG_H + 40, fill=P["panel2"], outline="", tags=("skel",))
                    cv.create_rectangle(cx + 16, cy + IMG_H + 52, cx + 130, cy + IMG_H + 60, fill=P["panel2"], outline="", tags=("skel",))
                cv.configure(scrollregion=(0, 0, w, h))
                return
            msg = self.error or ("Nothing compatible with v%s here - switch to 'All versions' (top right) to see the rest."
                                 % self.target if self.results else "No mods found.")
            cv.create_text(w / 2, h / 2 - 30, text=msg, fill=P["muted"], font=ui.F(12), width=min(620, w - 60), justify="center")
            if self.sources and not self.sources[self.src_index]["available"]:
                self._pill(cv, w / 2 - 66, h / 2 + 10, "Open settings", self.open_settings, "gold", h=40)
            elif self.error and self.sources:
                self._pill(cv, w / 2 - 52, h / 2 + 10, "Try again", self.do_search, "gold", h=40)
            cv.configure(scrollregion=(0, 0, w, h))
            return
        self.focus = max(0, min(self.focus, len(shown) - 1))
        for k, m in enumerate(shown):
            cx, cy = x0 + (k % cols) * (CARD_W + GAP), 24 + (k // cols) * (card_h + GAP)
            self._card(cv, m, k, cx, cy, card_h)
        bottom = 24 + ((len(shown) + cols - 1) // cols) * (card_h + GAP)
        if self.more:
            self._pill(cv, w / 2 - 56, bottom, "Load more", lambda: self.do_search(append=True), "ghost", h=40)
            bottom += 64
        cv.configure(scrollregion=(0, 0, w, max(h, bottom + 20)))

    def _card(self, cv, m: dict, k: int, x: int, y: int, h: int):
        tag = f"card{k}"
        focused = self.zone == "grid" and k == self.focus
        ui.round_rect(cv, x, y, x + CARD_W, y + h, 14, fill=P["panel"], outline=P["gold2"] if focused else P["edge"],
                      width=2 if focused else 1, tags=(tag, tag + "bg"))
        img = self._image(m["source"], m.get("thumb_url", ""), IMG_W, self._draw_grid)
        ix, iy = x + (CARD_W - IMG_W) // 2, y + 8
        if img is not None:
            cv.create_image(ix, iy, image=img, anchor="nw", tags=(tag,))
        else:
            ui.round_rect(cv, ix, iy, ix + IMG_W, iy + IMG_H, 10, fill=P["panel2"], outline="", tags=(tag, "skel"))
        ty = iy + IMG_H + 14
        cv.create_text(x + 16, ty, text=m["name"], anchor="nw", fill=P["ink"], font=ui.F(12, 600), width=CARD_W - 32, tags=(tag,))
        cv.create_text(x + 16, ty + 24, text=f"by {m.get('author') or 'unknown'}", anchor="nw", fill=P["muted"], font=ui.F(10), tags=(tag,))
        meta = f"{mbc.fmt_count(m.get('downloads'))} downloads   ·   {mbc.fmt_count(m.get('likes'))} likes"
        if m.get("version"):
            meta += f"   ·   v{m['version']}"
        cv.create_text(x + 16, ty + 46, text=meta, anchor="nw", fill=P["faint"], font=ui.F(9), tags=(tag,))
        text, col = self._badge(m.get("compat", {}))
        bx = self._chip(cv, x + 16, ty + 70, text, col)
        inst, upd = self._inst_state(m)
        if inst:
            self._chip(cv, bx, ty + 70, "Update available" if upd else "Installed", P["gold2"] if upd else P["gold"])
        self.cards.append({"m": m, "bbox": (x, y, x + CARD_W, y + h), "tag": tag})
        self._hit(cv, (x, y, x + CARD_W, y + h), lambda mm=m, kk=k: (setattr(self, "focus", kk), self.open_details(mm)), (tag, "card"))

    def _scroll_to_focus(self):
        if not self.cards or self.zone != "grid":
            return
        _x0, y0, _x1, y1 = self.cards[max(0, min(self.focus, len(self.cards) - 1))]["bbox"]
        cv = self.grid
        sr = str(cv.cget("scrollregion")).split()
        total = float(sr[3]) if len(sr) == 4 and float(sr[3]) > 0 else 1.0
        top, vis = cv.canvasy(0), cv.winfo_height()
        if y0 - 16 < top:
            cv.yview_moveto(max(0.0, (y0 - 16) / total))
        elif y1 + 16 > top + vis:
            cv.yview_moveto(min(1.0, (y1 + 16 - vis) / total))

    def _wheel(self, cv, e):
        cv.yview_scroll(int(-e.delta / 40), "units")

    # ================================================================== hover / click
    def _motion(self, cv, e):
        prev, cur = self._hover_key, None
        if e is not None:
            x, y = cv.canvasx(e.x), cv.canvasy(e.y)
            for (x0, y0, x1, y1), _act, key in reversed(self.hits.get(cv, [])):
                if x0 <= x <= x1 and y0 <= y <= y1:
                    cur = (cv, key) if key else (cv, None)
                    break
        if cur == prev:
            return
        if prev and prev[1]:
            self._hover_paint(prev, False)
        if cur and cur[1]:
            self._hover_paint(cur, True)
        cv.configure(cursor="hand2" if cur else "")
        self._hover_key = cur

    def _hover_paint(self, hk, on: bool):
        cv, (tag, kind) = hk
        try:
            if kind == "card":
                focused = self.zone == "grid" and int(tag[4:]) == self.focus
                cv.itemconfigure(tag + "bg", outline=P["gold2"] if (on or focused) else P["edge"],
                                 fill=P["lift"] if on else P["panel"], width=2 if (on or focused) else 1)
                cv.move(tag, 0, -3 if on else 3)              # a small lift
            elif kind == "text":
                cv.itemconfigure(tag, fill=P["gold2"] if on else P["muted"])
            elif kind in ("ghost", "bad"):
                cv.itemconfigure(tag + "bg", fill=P["lift"] if on else P["panel2"], outline=P["goldDim"] if on else P["edge2"])
            elif kind == "gold":
                cv.itemconfigure(tag + "bg", fill=P["gold2"] if on else P["gold"])
            elif kind == "quiet":
                cv.itemconfigure(tag + "fg", fill=P["gold2"] if on else P["muted"])
        except tk.TclError:
            pass

    def _click(self, cv, e):
        if cv is not self.head:
            cv.focus_set()
        x, y = cv.canvasx(e.x), cv.canvasy(e.y)
        for (x0, y0, x1, y1), act, _key in reversed(self.hits.get(cv, [])):
            if x0 <= x <= x1 and y0 <= y <= y1:
                self._hover_key = None
                act()
                return

    # ================================================================== keyboard / controller
    def _visible(self) -> bool:
        try:
            return self.built and bool(self.frame.winfo_viewable())
        except tk.TclError:
            return False

    def _key(self, e):
        if not self._visible():
            return None
        if self.frame.focus_get() is self.search:
            return None                                       # typing in the search field
        if isinstance(self.frame.focus_get(), (tk.Entry, tk.Text)):
            return None
        act = {"Left": "left", "Right": "right", "Up": "up", "Down": "down", "Return": "a", "space": "a",
               "Escape": "b", "BackSpace": "b", "Prior": "lb", "Next": "rb", "slash": "y"}.get(e.keysym)
        if act:
            self._nav(act)
            return "break"
        return None

    def _pad_tick(self):
        try:
            if self.pad and self.pad.fn and self._visible() and self._front():
                b = self.pad.read()
                if b is not None:
                    now = time.time()
                    pressed = b & ~self.pad_prev
                    dirs = b & (_Pad.UP | _Pad.DOWN | _Pad.LEFT | _Pad.RIGHT)
                    for bit, act in ((_Pad.A, "a"), (_Pad.B, "b"), (_Pad.X, "x"), (_Pad.Y, "y"), (_Pad.LB, "lb"), (_Pad.RB, "rb")):
                        if pressed & bit:
                            self._nav(act)
                    if dirs and (pressed & dirs or now >= self.pad_next):
                        for bit, act in ((_Pad.UP, "up"), (_Pad.DOWN, "down"), (_Pad.LEFT, "left"), (_Pad.RIGHT, "right")):
                            if dirs & bit:
                                self._nav(act)
                                break
                        self.pad_next = now + (0.32 if pressed & dirs else 0.11)   # hold: repeat
                    self.pad_prev = b
            self.frame.after(50 if self._visible() else 300, self._pad_tick)
        except tk.TclError:
            pass

    def _front(self) -> bool:
        try:
            u = ctypes.windll.user32
            fg = u.GetForegroundWindow()
            me = int(self.frame.winfo_toplevel().wm_frame(), 16)
            return fg == me or u.GetAncestor(fg, 2) == me
        except (AttributeError, OSError, ValueError, tk.TclError):
            return True

    def _nav(self, act: str):
        if self.notice_up:
            if act == "a":
                self._accept_notice()
            return
        if act == "y":
            self.search.focus_set()
            return
        if act in ("lb", "rb") and self.sources:
            self.select_source((self.src_index + (1 if act == "rb" else -1)) % len(self.sources))
            return
        if self.zone == "detail":
            acts = self._detail_actions
            if act == "b":
                self.close_detail()
            elif act in ("left", "up") and acts:
                self.dfocus = (self.dfocus - 1) % len(acts)
                self._draw_detail()
            elif act in ("right", "down") and acts:
                self.dfocus = (self.dfocus + 1) % len(acts)
                self._draw_detail()
            elif act in ("a", "x") and acts:
                acts[min(self.dfocus, len(acts) - 1)][1]()
            return
        n = len(self.cards)
        if not n:
            return
        f = self.focus
        if act in ("left", "right", "up", "down"):
            f = {"left": f - 1, "right": f + 1, "up": f - self.cols, "down": f + self.cols}[act]
            self.focus = max(0, min(n - 1, f))
            self._draw_grid()
            self._scroll_to_focus()
        elif act == "a":
            self.open_details(self.cards[self.focus]["m"])
        elif act == "x":
            self.open_details(self.cards[self.focus]["m"], then_install=True)

    # ================================================================== sources / search
    def reload_sources(self):
        def done(res, err):
            if err:
                self.error = f"The mod explorer could not start: {err}"
                self._draw_grid()
                return
            self.sources = res["sources"]
            self.target = res["target"].get("version") or "1.0.0"
            self.src_index = max(0, min(self.src_index, len(self.sources) - 1))
            self.select_source(self.src_index)
            self.refresh_installed()
            st = mbc.load_settings(self.cli.settings)
            if st.get("check_updates"):
                self.bg(lambda: self.cli.call("check-updates", timeout=300),
                        lambda r, e: (not e and r.get("mods")) and self.toast(f"{len(r['mods'])} installed mod(s) have an update."))
        self.bg(lambda: self.cli.call("sources"), done)

    def do_search(self, append: bool = False):
        if not self.sources:
            return
        s = self.sources[self.src_index]
        if not s["available"]:
            return
        page = self.page + 1 if append else 1
        src_id, sort = s["id"], self.sort
        q = self.search_var.get().strip()
        self.search_seq += 1
        seq = self.search_seq
        self.loading, self.error = True, ""
        if not append:
            self.results = []
            self.grid.yview_moveto(0)
            self._draw_grid()

        def done(res, err):
            if seq != self.search_seq:
                return                                       # a newer search replaced this one
            self.loading = False
            if err:
                self.error = f"{s['title']}: {err}"
                if not append:
                    self.results = []
                self._draw_grid()
                return
            self.page = page
            self.more = res.get("more", False)
            self.results = (self.results if append else []) + res["mods"]
            self._draw_grid()
        self.bg(lambda: self.cli.call("search", source=src_id, q=q or None, sort=sort, page=page, per_page=24), done)

    # ================================================================== detail panel
    def open_details(self, m: dict, then_install: bool = False):
        src = m["source"]

        def done(res, err):
            if err:
                self.toast(f"{m['name']}: {err}", "bad")
                return
            self.detail = res
            self.detail_tab = 0
            self.detail_img = 0
            self.dfocus = 0
            self.zone = "detail"
            self.detail_cv.yview_moveto(0)
            self._draw_detail()
            self.detail_tw.to(1.0)
            if then_install and res.get("files"):
                self.queue_install(res, res["files"][0])
        self.bg(lambda: self.cli.call("details", source=src, id=m["id"]), done)

    def close_detail(self):
        if self.detail is None:
            return
        self.zone = "grid"
        self.detail = None
        self.detail_tw.to(0.0)
        self._draw_grid()

    def _draw_detail(self):
        d = self.detail
        cv = self.detail_cv
        if d is None:
            return
        cv.delete("all")
        self.hits[cv] = []
        for wdg in self._detail_widgets:
            wdg.destroy()
        self._detail_widgets = []
        W = max(cv.winfo_width(), 600)
        pad = 36
        self._pill(cv, pad - 12, 18, "<   Back", self.close_detail, "quiet")
        imgs = d.get("images") or ([d["thumb_url"]] if d.get("thumb_url") else [])
        side = W > 1150
        hero_w = 720 if side else min(720, W - 2 * pad)
        y = 66
        img = self._image(d["source"], imgs[self.detail_img % len(imgs)], 720, self._draw_detail) if imgs else None
        hero_h = int(hero_w * 9 / 16)
        if img is not None:
            hero_w, hero_h = img.width(), img.height()
            cv.create_image(pad, y, image=img, anchor="nw")
        else:
            ui.round_rect(cv, pad, y, pad + hero_w, y + hero_h, 12, fill=P["panel2"], outline="")
        below = y + hero_h
        if len(imgs) > 1:
            ny = below + 10
            self._pill(cv, pad, ny, "<", lambda: self._img_step(-1), "ghost", h=28)
            cv.create_text(pad + 62, ny + 14, text=f"{self.detail_img % len(imgs) + 1} / {len(imgs)}", fill=P["muted"], font=ui.F(10), anchor="w")
            self._pill(cv, pad + 112, ny, ">", lambda: self._img_step(1), "ghost", h=28)
            below = ny + 28
        # the info column (beside the picture on wide windows, under it otherwise)
        ix = pad + hero_w + 34 if side else pad
        iy = y if side else below + 24
        cw = (W - ix - pad) if side else (W - 2 * pad)
        iid = cv.create_text(ix, iy, text=d["name"], anchor="nw", fill=P["ink"], font=ui.F(20, 700), width=cw)
        iy = cv.bbox(iid)[3] + 8
        cv.create_text(ix, iy, text=f"by {d.get('author') or 'unknown'}   ·   all credit to the author", anchor="nw",
                       fill=P["gold"], font=ui.F(11))
        iy += 26
        meta = f"{mbc.fmt_count(d.get('downloads'))} downloads   ·   {mbc.fmt_count(d.get('likes'))} likes"
        if d.get("version"):
            meta += f"   ·   version {d['version']}"
        cv.create_text(ix, iy, text=meta, anchor="nw", fill=P["muted"], font=ui.F(10))
        iy += 28
        text, col = self._badge(d.get("compat", {}))
        bx = self._chip(cv, ix, iy, text, col)
        inst_row = next((i for i in self.installed if i["source"] == d["source"] and i["mod_id"] == d["id"]), None)
        if inst_row:
            self._chip(cv, bx, iy, f"Installed  v{inst_row.get('version') or '?'}", P["gold"])
        iy += 28
        why = (d.get("compat") or {}).get("why")
        if why:
            wid = cv.create_text(ix, iy, text=why, anchor="nw", fill=P["faint"], font=ui.F(9), width=cw)
            iy = cv.bbox(wid)[3] + 10
        files = d.get("files") or []
        first = files[0] if files else None
        acts = []
        if first:
            upd = bool(inst_row) and mbc.version_newer(first.get("version", ""), inst_row.get("version", ""))
            acts.append(("Update" if upd else "Reinstall" if inst_row else "Install", lambda: self.queue_install(d, first), "gold"))
        if inst_row:
            acts.append(("Uninstall", lambda: self.uninstall(inst_row), "bad"))
        if (d.get("page_url") or "").startswith("https://"):
            acts.append(("Open the mod's page", lambda: webbrowser.open(d["page_url"]), "ghost"))
        self._detail_actions = [(a[0], a[1]) for a in acts]
        self.dfocus = min(self.dfocus, max(0, len(acts) - 1))
        ax, iy = ix, iy + 6
        for k, (label, fn, kind) in enumerate(acts):
            ax = self._pill(cv, ax, iy, label, fn, kind, size=12 if k == 0 else 11, h=46 if k == 0 else 38,
                            focused=self.zone == "detail" and k == self.dfocus) + 10
        iy += 66
        # tabs: description / files / changelog
        ty = max(iy, below + 30) if side else iy
        tabs = ["Description", f"Files ({len(files)})"] + (["Changelog"] if d.get("changelog") else [])
        self.detail_tab = min(self.detail_tab, len(tabs) - 1)
        tx = pad
        for k, name in enumerate(tabs):
            on = k == self.detail_tab
            tag = f"tab{k}"
            tid = cv.create_text(tx, ty, text=name, anchor="nw", fill=P["ink"] if on else P["muted"], font=ui.F(12, 600 if on else 400), tags=(tag,))
            x0, y0, x1, y1 = cv.bbox(tid)
            if on:
                cv.create_rectangle(x0, y1 + 7, x1, y1 + 9, fill=P["gold"], outline="")
            self._hit(cv, (x0 - 8, y0 - 6, x1 + 8, y1 + 12), lambda kk=k: self._set_tab(kk), None if on else (tag, "text"))
            tx = x1 + 30
        ty += 40
        cv.create_line(pad, ty - 8, W - pad, ty - 8, fill=P["edge"])
        if self.detail_tab == 1:
            for k, fl in enumerate(files):
                ry = ty + 6 + k * 54
                ui.round_rect(cv, pad, ry, W - pad, ry + 46, 10, fill=P["panel2"], outline=P["edge"])
                cv.create_text(pad + 18, ry + 23, text=fl["name"], anchor="w", fill=P["ink"], font=ui.F(11, 600))
                sub = mbc.fmt_size(fl.get("size")) + (f"   ·   v{fl['version']}" if fl.get("version") else "")
                if not fl.get("has_hash"):
                    sub += "   ·   no checksum from the source"
                cv.create_text(pad + 360, ry + 23, text=sub, anchor="w", fill=P["muted"], font=ui.F(10))
                self._pill(cv, W - pad - 10, ry + 7, "Install", lambda ff=fl: self.queue_install(d, ff), "gold", h=32, anchor="e")
            end = ty + 6 + len(files) * 54 + 24
        else:
            body = (d.get("description") or "No description.").strip() if self.detail_tab == 0 else d.get("changelog", "")
            lines = max(4, min(60, body.count("\n") + len(body) // 95 + 2))
            txt = tk.Text(cv, wrap="word", bg=P["panel"], fg=P["ink"], relief="flat", highlightthickness=0, font=ui.F(11),
                          padx=0, pady=0, insertbackground=P["panel"], selectbackground=P["goldDim"], height=lines,
                          spacing1=2, spacing3=4, cursor="arrow")
            txt.insert("end", body)
            txt.configure(state="disabled")
            txt.bind("<MouseWheel>", lambda e: (self._wheel(cv, e), "break")[1])
            cv.create_window(pad, ty, window=txt, anchor="nw", width=W - 2 * pad)
            self._detail_widgets.append(txt)
            end = ty + lines * 24 + 30
        cv.configure(scrollregion=(0, 0, W, max(cv.winfo_height(), end)))

    def _set_tab(self, k: int):
        self.detail_tab = k
        self._draw_detail()

    def _img_step(self, k: int):
        self.detail_img += k
        self._draw_detail()

    # ================================================================== installed
    def _inst_state(self, m: dict) -> tuple[bool, bool]:
        for i in self.installed:
            if i["source"] == m["source"] and i["mod_id"] == m["id"]:
                return True, mbc.version_newer(m.get("version", ""), i.get("version", ""))
        return False, False

    def refresh_installed(self):
        def done(res, err):
            if err:
                return
            self.installed = res["mods"]
            self._draw_rail()
            if self.detail is not None:
                self._draw_detail()
            else:
                self._draw_grid()
        self.bg(lambda: self.cli.call("installed"), done)

    def uninstall(self, inst: dict):
        if not self._confirm("Uninstall", f"Remove “{inst['name']}”? Only its folder is deleted."):
            return

        def done(res, err):
            self.toast(f"{inst['name']}: {err}" if err else f"{inst['name']} removed", "bad" if err else "info")
            self.refresh_installed()
        self.bg(lambda: self.cli.call("uninstall", folder=inst["folder"]), done)

    def _confirm(self, title: str, text: str) -> bool:
        from tkinter import messagebox
        return messagebox.askyesno(title, text, parent=self.frame)

    # ================================================================== downloads tray
    def queue_install(self, d: dict | None, f: dict | None, *, path: str | None = None, allow: bool = False):
        title = d["name"] if d else Path(path or "mod").name
        job = {"title": title, "d": d, "f": f, "path": path, "allow": allow, "state": "queued", "proc": None,
               "done": 0, "total": (f or {}).get("size", 0), "result": None, "cancel": False}
        with self.job_lock:
            self.jobs.append(job)
        self.toast(f"Queued: {title}")
        self._draw_tray()
        self.job_wake.set()

    def _job_worker(self):
        while True:
            self.job_wake.wait()
            self.job_wake.clear()
            while True:
                with self.job_lock:
                    job = next((j for j in self.jobs if j["state"] == "queued"), None)
                if not job:
                    break
                self._run_job(job)

    def _run_job(self, job: dict):
        try:
            if job["path"] is not None:
                proc = self.cli.start_install(path=job["path"], allow=job["allow"])
            else:
                proc = self.cli.start_install(source=job["d"]["source"], mod_id=job["d"]["id"],
                                              file_id=(job["f"] or {}).get("id"), allow=job["allow"])
        except OSError as e:
            job.update(state="failed", result={"message": str(e)})
            self.q.put((lambda *_: self._job_finished(job), None, None))
            return
        job["proc"] = proc
        job["state"] = "download"
        for msg in mbc.read_lines(proc):
            if msg.get("type") == "progress":
                job["state"] = msg.get("stage", job["state"])
                job["done"], job["total"] = msg.get("done", job["done"]), msg.get("total", job["total"])
            elif msg.get("type") in ("result", "error"):
                job["result"] = msg
            self.q.put((lambda *_: self._draw_tray(), None, None))
        res = job["result"] or {}
        if job["cancel"]:
            job["state"] = "cancelled"
        elif res.get("type") == "error":
            job["state"] = "failed"
            res["message"] = res.get("error", "failed")
        else:
            job["state"] = res.get("state", "failed")
        self.q.put((lambda *_: self._job_finished(job), None, None))

    def _job_finished(self, job: dict):
        self._draw_tray()
        res = job["result"] or {}
        st = job["state"]
        if st == "done":
            extra = f"  ·  {len(res['refused'])} program / script file(s) refused" if res.get("refused") else ""
            self.toast(f"{job['title']} installed{extra}", "good")
            self.refresh_installed()
        elif st == "blocked":
            if self._confirm("Another game version",
                             f"{job['title']}: {res.get('message', '')}\n\nIts exefs patches or cheats name another "
                             "build of the game. The mod loader skips those (with a note in mods.log), so usually only "
                             "its romfs files take effect - and they may not fit this version either.\n\nInstall anyway?"):
                self.queue_install(job["d"], job["f"], path=job["path"], allow=True)
        elif st == "browser" and (res.get("browser_url") or "").startswith("https://"):
            webbrowser.open(res["browser_url"])
            self.toast("Opened the download page - then add it with 'From a file...'", "warn")
        elif st == "failed":
            self.toast(f"{job['title']}: {res.get('message') or 'failed'}", "bad")

    def _draw_tray(self):
        if not self.built:
            return
        cv = self.tray
        cv.delete("all")
        self.hits[cv] = []
        w = cv.winfo_width()
        with self.job_lock:
            jobs = list(self.jobs)
        active = [j for j in jobs if j["state"] in ("queued", "download", "verify", "install")]
        cv.create_line(0, 0, w, 0, fill=P["edge"])
        label = (f"DOWNLOADS   ·   {len(active)} active" if active else
                 f"DOWNLOADS   ·   {len(jobs)} finished" if jobs else "DOWNLOADS")
        cv.create_text(22, 22, text=label, anchor="w", fill=P["muted"], font=ui.F(9, 700), tags=("traylbl",))
        self._hit(cv, (0, 0, 250, TRAY_MIN), self._toggle_tray, ("traylbl", "text"))
        if jobs:
            self._pill(cv, w - 14, 8, "Clear finished", self._clear_jobs, "quiet", h=28, size=9, anchor="e")
        if active:                                            # the current job as a slim gold line
            j = active[0]
            frac = (j["done"] / j["total"]) if j["total"] and j["state"] == "download" else None
            x0, x1 = 270, max(300, w - 170)
            cv.create_rectangle(x0, 18, x1, 21, fill=P["edge"], outline="")
            if frac is not None:
                cv.create_rectangle(x0, 18, x0 + (x1 - x0) * min(1.0, frac), 21, fill=P["gold"], outline="")
            else:
                t = (time.time() * 0.6) % 1
                bx = x0 + (x1 - x0 - 90) * t
                cv.create_rectangle(bx, 18, bx + 90, 21, fill=P["gold"], outline="")
            cv.create_text(x0, 32, text=f"{j['title']}   ·   {j['state']}", anchor="w", fill=P["faint"], font=ui.F(8))
        if self.tray_h > TRAY_MIN + 10:
            y = TRAY_MIN + 2
            for j in jobs[-6:][::-1]:
                if y + 30 > self.tray_h:
                    break
                res = j["result"] or {}
                col = {"done": P["good"], "blocked": P["warn"], "cancelled": P["faint"], "failed": P["bad"],
                       "browser": P["warn"]}.get(j["state"], P["ink"])
                cv.create_text(22, y + 12, text=j["title"], anchor="w", fill=P["ink"], font=ui.F(10, 600), width=240)
                msg = res.get("message") or j["state"]
                if res.get("refused"):
                    msg += "   ·   refused: " + ", ".join(Path(r).name for r in res["refused"])
                cv.create_text(270, y + 12, text=msg, anchor="w", fill=col, font=ui.F(9), width=max(200, w - 440))
                if j in active:
                    self._pill(cv, w - 14, y, "Cancel", lambda jj=j: self._cancel(jj), "quiet", h=24, size=9, anchor="e")
                y += 32

    def _toggle_tray(self):
        self.tray_open = not self.tray_open
        n = min(6, len(self.jobs))
        self.tray_tw.to(float(TRAY_MIN + (n * 32 + 10 if self.tray_open and n else 0)))

    def _cancel(self, job: dict):
        job["cancel"] = True
        if job["proc"] is not None:
            try:
                job["proc"].kill()
            except OSError:
                pass
        elif job["state"] == "queued":
            job["state"] = "cancelled"
        self._draw_tray()

    def _clear_jobs(self):
        with self.job_lock:
            self.jobs = [j for j in self.jobs if j["state"] in ("queued", "download", "verify", "install")]
        if self.tray_open:
            self._toggle_tray()
        self._draw_tray()

    # ================================================================== animation: skeleton shimmer, toasts, tray band
    def _anim_tick(self):
        try:
            if self._visible():
                ph = (time.time() * 0.8) % 1
                self.grid.itemconfigure("skel", fill=ui.mix(P["panel2"], P["lift"], abs(ph * 2 - 1)))
                self._draw_toasts()
                if any(j["state"] in ("queued", "verify", "install") or (j["state"] == "download" and not j["total"])
                       for j in self.jobs):
                    self._draw_tray()
            self.frame.after(50, self._anim_tick)
        except tk.TclError:
            pass

    def _draw_toasts(self):
        cv = self.toast_cv
        now = time.time()
        self.toasts = [t for t in self.toasts if now - t[2] < 4.0]
        if not self.toasts:
            cv.place_forget()
            return
        self._place()
        cv.delete("all")
        text, kind, t0 = self.toasts[-1]
        age = now - t0
        a = min(1.0, age / 0.18) * (1.0 if age < 3.4 else max(0.0, (4.0 - age) / 0.6))
        col = {"good": P["good"], "bad": P["bad"], "warn": P["warn"]}.get(kind, P["gold"])
        cv.configure(bg=P["bg"])
        ui.round_rect(cv, 2, 2, 438, 38, 12, fill=ui.mix(P["bg"], P["panel2"], a), outline=ui.mix(P["bg"], col, a * 0.8))
        cv.create_text(18, 20, text=text, anchor="w", fill=ui.mix(P["bg"], P["ink"], a), font=ui.F(10), width=410)

    # ================================================================== notice
    def _show_notice(self):
        self.notice_up = True
        cv = self.grid
        cv.delete("all")
        self.hits[cv] = []
        w, h = max(cv.winfo_width(), 500), max(cv.winfo_height(), 320)
        bw = min(640, w - 60)
        x0, y0 = (w - bw) / 2, max(30, h / 2 - 150)
        ui.round_rect(cv, x0, y0, x0 + bw, y0 + 290, 18, fill=P["panel"], outline=P["goldDim"])
        cv.create_text(x0 + 34, y0 + 36, text=_spaced("Before you explore"), anchor="w", fill=P["gold"], font=ui.F(11, 700))
        cv.create_text(x0 + 34, y0 + 64, text=NOTICE, anchor="nw", fill=P["ink"], font=ui.F(11), width=bw - 68)
        self._pill(cv, x0 + 34, y0 + 226, "I understand", self._accept_notice, "gold", h=42, focused=True)
        self._draw_head()
        self._draw_rail()

    def _accept_notice(self):
        self.notice_up = False
        st = mbc.load_settings(self.cli.settings)
        st["notice_shown"] = True
        mbc.save_settings(self.cli.settings, self._merged_settings(st))
        self.reload_sources()

    # ================================================================== add a mod: file / link
    def _choose_file(self):
        p = filedialog.askopenfilename(parent=self.frame, title="A mod archive",
                                       filetypes=[("Mod archives", "*.zip *.7z *.rar"), ("All files", "*.*")])
        if p:
            self.queue_install(None, None, path=p)

    def _ask_link(self):
        link = simpledialog.askstring("Install from a link", "An https:// link to a mod archive, or a Nexus nxm:// link:",
                                      parent=self.frame)
        if not link:
            return
        low = link.strip().lower()
        if low.startswith("http://"):
            self.toast("Only https:// links are allowed.", "bad")
        elif low.startswith("https://") or low.startswith("nxm://"):
            self.queue_install(None, None, path=link.strip())
        else:
            self.toast("Not an https:// or nxm:// link.", "bad")

    # ================================================================== settings (a dialog; rarely used)
    def _merged_settings(self, st: dict) -> dict:
        d = {"sources": {"gamebanana": True, "nexus": False, "modio": False, "github": True}, "check_updates": False,
             "notice_shown": False, "only_compatible": True, "target_version": "1.0.0", "github_repos": []}
        d.update({k: v for k, v in st.items() if k != "sources"})
        d["sources"].update(st.get("sources", {}))
        return d

    def _nexus_login(self, parent, key_var, on_var, status_var):
        """Log in to Nexus Mods: with a registered application, the browser sign-in hands the key back by itself;
        until then the API-key page opens and the player pastes the key once.  Either way the key is checked with
        Nexus and the source switched on."""
        def finish(key: str):
            status_var.set("Checking with Nexus...")

            def work():
                res = mbc.nexus_validate(key)

                def done():
                    if res["ok"]:
                        key_var.set(key)
                        on_var.set(True)
                        status_var.set(f"Logged in as {res['name']} ({'Premium' if res['premium'] else 'free account'})"
                                       " - press Save")
                    else:
                        status_var.set("Not logged in: " + res["error"])
                parent.after(0, done)
            threading.Thread(target=work, daemon=True).start()

        if mbc.NEXUS_APP_SLUG:
            status_var.set("Confirm in your browser...")

            def sso():
                try:
                    key = mbc.nexus_sso(webbrowser.open)
                except (OSError, RuntimeError, ValueError) as e:
                    key = ""
                    parent.after(0, lambda: status_var.set(f"Log-in failed: {e}"))
                if key:
                    parent.after(0, lambda: finish(key))
                elif not status_var.get().startswith("Log-in failed"):
                    parent.after(0, lambda: status_var.set("Log-in cancelled or timed out"))
            threading.Thread(target=sso, daemon=True).start()
            return
        webbrowser.open(mbc.NEXUS_KEY_PAGE)
        key = simpledialog.askstring(
            "Log in to Nexus Mods",
            "Your browser opened your Nexus Mods API keys page (log in there if asked).\n\n"
            "Copy the 'Personal API Key' at the bottom and paste it here.\nIt is stored only on this PC.",
            parent=parent)
        if key and key.strip():
            finish(key.strip())

    def open_repos(self, parent, rows_box: dict, builtin: list, on_change=None):
        """The GitHub repositories: built-in ones (the shipped index, they list at once) and the player's own. Paste a
        link and press Add; Rename / Delete per row. Changes go into the settings when the settings are saved."""
        w = tk.Toplevel(parent)
        w.title("GitHub repositories")
        w.configure(bg=P["panel"])
        w.transient(parent)
        w.geometry("640x520")
        top = tk.Frame(w, bg=P["panel"], padx=20, pady=16)
        top.pack(fill="x")
        tk.Label(top, text="Paste a GitHub link (or owner/repo) of a mod collection:", bg=P["panel"], fg=P["muted"],
                 font=ui.F(10)).pack(anchor="w")
        bar = tk.Frame(top, bg=P["panel"])
        bar.pack(fill="x", pady=(6, 0))
        link = tk.StringVar()
        ent = tk.Entry(bar, textvariable=link, bg=P["panel2"], fg=P["ink"], insertbackground=P["gold"], relief="flat",
                       highlightthickness=1, highlightbackground=P["edge"], highlightcolor=P["goldDim"], font=ui.F(11))
        ent.pack(side="left", fill="x", expand=True, ipady=5)
        msg = tk.StringVar()
        tk.Label(top, textvariable=msg, bg=P["panel"], fg=P["muted"], font=ui.F(9)).pack(anchor="w", pady=(4, 0))
        # the list (scrolls)
        holder = tk.Frame(w, bg=P["panel"])
        holder.pack(fill="both", expand=True, padx=20)
        cv = tk.Canvas(holder, bg=P["panel"], highlightthickness=0)
        sb = tk.Scrollbar(holder, orient="vertical", command=cv.yview)
        lst = tk.Frame(cv, bg=P["panel"])
        lst.bind("<Configure>", lambda _e: cv.configure(scrollregion=cv.bbox("all")))
        win_id = cv.create_window((0, 0), window=lst, anchor="nw")
        cv.bind("<Configure>", lambda e: cv.itemconfigure(win_id, width=e.width))
        cv.configure(yscrollcommand=sb.set)
        cv.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        w.bind("<MouseWheel>", lambda e: cv.yview_scroll(int(-e.delta / 120) or (-1 if e.delta > 0 else 1), "units"))

        def changed():
            draw()
            if on_change:
                on_change()

        def rename(row):
            name = simpledialog.askstring("Rename", f"Name for {row['repo']}:", initialvalue=row["name"], parent=w)
            if name is not None and name.strip():
                row["name"] = name.strip()
                changed()

        def delete(row):
            rows_box["rows"] = [r for r in rows_box["rows"] if r is not row]
            msg.set(f"Removed {row['name']}." + ("  (Restore built-in brings it back.)" if row.get("builtin") else ""))
            changed()

        def draw():
            for c in lst.winfo_children():
                c.destroy()
            if not rows_box["rows"]:
                tk.Label(lst, text="No repositories. Paste a link above, or Restore built-in.", bg=P["panel"],
                         fg=P["muted"], font=ui.F(10)).pack(anchor="w", pady=12)
            counts = {b["repo"].lower(): b["mods"] for b in builtin}
            for row in rows_box["rows"]:
                line = tk.Frame(lst, bg=P["panel2"], padx=12, pady=8)
                line.pack(fill="x", pady=3)
                txt = tk.Frame(line, bg=P["panel2"])
                txt.pack(side="left", fill="x", expand=True)
                tk.Label(txt, text=row["name"], bg=P["panel2"], fg=P["ink"], font=ui.F(11, 600), anchor="w").pack(anchor="w")
                n = counts.get(row["repo"].lower())
                sub = row["repo"] + ("   ·   built-in" if row.get("builtin") else "   ·   added by you") + \
                    (f"   ·   {n} mod{'s' if n != 1 else ''}" if n else "")
                tk.Label(txt, text=sub, bg=P["panel2"], fg=P["muted"], font=ui.F(9), anchor="w").pack(anchor="w")
                ui.PillButton(line, "Delete", lambda r=row: delete(r), kind="danger", size="sm", bg=P["panel2"]).pack(side="right")
                ui.PillButton(line, "Rename", lambda r=row: rename(r), kind="ghost", size="sm", bg=P["panel2"]).pack(side="right", padx=6)

        def add(*_):
            repo = mbc.norm_repo(link.get())
            if not repo:
                msg.set("That is not a GitHub repository link (like https://github.com/owner/repo).")
                return
            if any(r["repo"].lower() == repo.lower() for r in rows_box["rows"]):
                msg.set(f"{repo} is already in the list.")
                return
            b = next((x for x in builtin if x["repo"].lower() == repo.lower()), None)
            rows_box["rows"].insert(0, {"repo": b["repo"] if b else repo, "name": b["name"] if b else repo, "builtin": bool(b)})
            link.set("")
            msg.set(f"Added {repo}. Its mods show up in the next GitHub search.")
            changed()

        def restore():
            have = {r["repo"].lower() for r in rows_box["rows"]}
            back = [b for b in builtin if b["repo"].lower() not in have]
            rows_box["rows"] += [{"repo": b["repo"], "name": b["name"], "builtin": True} for b in back]
            msg.set(f"Restored {len(back)} built-in repositor{'y' if len(back) == 1 else 'ies'}." if back else
                    "All built-in repositories are in the list.")
            changed()

        ui.PillButton(bar, "Add", add, kind="primary", size="sm", bg=P["panel"]).pack(side="left", padx=(8, 0))
        ent.bind("<Return>", add)
        foot = tk.Frame(w, bg=P["panel"], padx=20, pady=12)
        foot.pack(fill="x")
        ui.PillButton(foot, "Done", w.destroy, kind="primary", bg=P["panel"]).pack(side="right")
        ui.PillButton(foot, "Restore built-in", restore, kind="secondary", bg=P["panel"]).pack(side="left")
        tk.Label(foot, text="Saved with the settings (Save).", bg=P["panel"], fg=P["muted"], font=ui.F(9)).pack(side="left", padx=10)
        draw()
        ent.focus_set()

    def open_settings(self):
        st = self._merged_settings(mbc.load_settings(self.cli.settings))
        w = tk.Toplevel(self.frame)
        w.title("Mod Explorer settings")
        w.configure(bg=P["panel"])
        w.transient(self.frame.winfo_toplevel())
        fr = tk.Frame(w, bg=P["panel"], padx=24, pady=20)
        fr.pack(fill="both", expand=True)

        def lab(t, r, fg=P["ink"], font=None, span=1):
            tk.Label(fr, text=t, bg=P["panel"], fg=fg, font=font or ui.F(10), wraplength=560, justify="left").grid(
                row=r, column=0, columnspan=span, sticky="w")

        def entry(var, secret):
            return tk.Entry(fr, textvariable=var, width=40, show="*" if secret else "", bg=P["panel2"], fg=P["ink"],
                            insertbackground=P["gold"], relief="flat", highlightthickness=1, highlightbackground=P["edge"],
                            highlightcolor=P["goldDim"])

        def check(text, var, r):
            tk.Checkbutton(fr, text=text, variable=var, bg=P["panel"], fg=P["ink"], selectcolor=P["panel2"],
                           activebackground=P["panel"], activeforeground=P["gold2"], font=ui.F(11, 600)).grid(
                row=r, column=0, columnspan=2, sticky="w", pady=(12, 2))
        lab(_spaced("Sources"), 0, P["gold"], ui.F(10, 700), 2)
        lab(f"Saved in {self.cli.settings} on this PC only. Keys are sent only to their own site and never logged.",
            1, P["muted"], ui.F(9), 2)
        on = {k: tk.BooleanVar(value=bool(st["sources"].get(k))) for k in ("gamebanana", "nexus", "modio", "github")}
        val = {k: tk.StringVar(value=str(st.get(k, ""))) for k in SETTING_KEYS}
        rows = [("gamebanana", "GameBanana", [("gamebanana_game_id", "Game id (the number in gamebanana.com/games/<id>)", False)]),
                ("nexus", "Nexus Mods", [("nexus_api_key", "Personal API key (nexusmods.com > Site preferences > API)", True),
                                         ("nexus_game_domain", "Game domain (the name in nexusmods.com/<domain>/mods)", False)]),
                ("modio", "mod.io (optional)", [("modio_api_key", "API key", True), ("modio_game_id", "Game id", False)]),
                ("github", "GitHub (mod collections)", [])]
        r = 2
        for key, label, fields in rows:
            check(label, on[key], r)
            if key == "nexus":   # Log in: the browser sign-in (registered app) or the key page + one paste
                nx = tk.Frame(fr, bg=P["panel"])
                nx.grid(row=r, column=1, sticky="w", padx=10, pady=(12, 2))
                nx_status = tk.StringVar(value="Logged in (key saved)" if st.get("nexus_api_key") else "Not logged in")
                ui.PillButton(nx, "Log in to Nexus", lambda: self._nexus_login(w, val["nexus_api_key"], on["nexus"], nx_status),
                              kind="primary", size="sm", bg=P["panel"]).pack(side="left")
                tk.Label(nx, textvariable=nx_status, bg=P["panel"], fg=P["muted"], font=ui.F(9)).pack(side="left", padx=8)
            r += 1
            for fk, flabel, secret in fields:
                lab(flabel, r, P["muted"])
                entry(val[fk], secret).grid(row=r, column=1, sticky="w", padx=10, pady=2)
                r += 1
        builtin = mbc.builtin_repos()
        rows_box = {"rows": mbc.repo_rows(st, builtin)}
        lab("Repositories with mods for this game", r, P["muted"])
        repo_btn_holder = tk.Frame(fr, bg=P["panel"])
        repo_btn_holder.grid(row=r, column=1, sticky="w", padx=10)

        def repo_btn_text():
            return f"Repos ({len(rows_box['rows'])})"

        repo_btn = ui.PillButton(repo_btn_holder, repo_btn_text(), lambda: self.open_repos(w, rows_box, builtin, refresh),
                                 kind="secondary", size="sm", bg=P["panel"])
        repo_btn.pack(side="left")

        def refresh():
            repo_btn.configure(text=repo_btn_text())
        r += 1
        upd = tk.BooleanVar(value=bool(st.get("check_updates")))
        check("Check installed mods for updates when the explorer opens", upd, r)
        r += 1
        tv = tk.StringVar(value=st.get("target_version", "1.0.0"))
        lab("Target game version (the version you dumped)", r, P["muted"])
        entry(tv, False).grid(row=r, column=1, sticky="w", padx=10)
        r += 1

        def save():
            st["sources"] = {k: bool(v.get()) for k, v in on.items()}
            for k in SETTING_KEYS:
                st[k] = val[k].get().strip()
            nums = re.findall(r"\d+", st["gamebanana_game_id"])       # a pasted address: its number
            st["gamebanana_game_id"] = nums[-1] if nums else ""
            mbc.apply_repo_rows(st, rows_box["rows"], builtin)
            st["check_updates"] = bool(upd.get())
            st["target_version"] = tv.get().strip() or "1.0.0"
            mbc.save_settings(self.cli.settings, st)
            w.destroy()
            self.sources = []
            self.reload_sources()
        btn = tk.Frame(fr, bg=P["panel"])
        btn.grid(row=r, column=0, columnspan=2, sticky="e", pady=(18, 0))
        ui.PillButton(btn, "Save", save, kind="primary", bg=P["panel"]).pack(side="right")
        ui.PillButton(btn, "Cancel", w.destroy, kind="secondary", bg=P["panel"]).pack(side="right", padx=8)
