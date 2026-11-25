"""Hisui-paper widgets for the launcher: the look of Legends: Arceus' own menus (warm cream paper,
sumi-ink text, Galaxy-navy actions, thin gold trim, serif headings) - original drawing, no game assets.
Pill actions, hairlines instead of shadows, quiet 150-240 ms ease-out motion.

Plain tkinter (Canvas-drawn), no extra packages.  Every widget keeps the small API the launcher
logic uses on the ttk widgets it replaces: PillButton.configure(state=, text=), PillBar
.configure(mode=, value=) / cget("mode") / start() / stop().  The option widgets (TabBar, Stepper,
Toggle, Slider, Section, SettingRow) mirror the game's option screens: a label on the left, the value
on the right with arrows, a gold rule between groups."""
from __future__ import annotations

import time
import tkinter as tk
import tkinter.font as tkfont

# ---- tokens -------------------------------------------------------------------------------------
T = {   # Hisui paper (the key names are kept from the earlier Apple-style tokens, so every window follows)
    "page": "#ece1c9",                                   # window background: aged paper
    "canvas": "#fbf6ea", "pearl": "#f6efdf",             # cards / log background: fresh paper
    "parchment": "#ebdfc3", "track": "#e0d1ae", "knob": "#fffbf2",   # filled controls, tracks
    "hairline": "#d5c299", "dividerSoft": "#e7dabb",
    "ink": "#2b2721", "inkMuted80": "#564c3d", "inkMuted48": "#8b7d63",
    "primary": "#2b4470", "primaryHover": "#375684", "primarySoft": "#dbe2ee", "onPrimary": "#fbf6ea",
    "gold": "#b0862f", "goldHover": "#c49a41", "goldSoft": "#f1e3bf", "goldText": "#87631c",
    "danger": "#a8402f", "dangerText": "#9a3423", "dangerSoft": "#f3dcd2", "dangerHover": "#ebcdbf",
    "success": "#557a35", "successText": "#466b2a", "successSoft": "#e2ead2",
    "warning": "#b97b1f", "warningText": "#93611a", "warningSoft": "#f4e4c4",
}
PAPER = dict(T)
DARK = {   # the launcher's original dark look (blue accent on blue-grey surfaces), gold tokens added
    "page": "#17191e",
    "canvas": "#22252c", "pearl": "#1b1d22",
    "parchment": "#2e323b", "track": "#383d48", "knob": "#4a5060",
    "hairline": "#30343d", "dividerSoft": "#2a2d35",
    "ink": "#f2f3f5", "inkMuted80": "#c9ccd3", "inkMuted48": "#8d93a0",
    "primary": "#2f8cff", "primaryHover": "#4a9dff", "primarySoft": "#1f3350", "onPrimary": "#ffffff",
    "gold": "#c9a45c", "goldHover": "#e3c27a", "goldSoft": "#3a3222", "goldText": "#e3c27a",
    "danger": "#ff453a", "dangerText": "#ff6961", "dangerSoft": "#3a2427", "dangerHover": "#4a2a2e",
    "success": "#30d158", "successText": "#5fe08a", "successSoft": "#1d3626",
    "warning": "#ff9f0a", "warningText": "#ffb340", "warningSoft": "#3a2e1a",
}


def set_theme(name: str) -> None:
    """'paper' (Hisui paper, default) or 'dark'.  Changes T in place: call before building widgets."""
    T.clear()
    T.update(DARK if name == "dark" else PAPER)


SPACE = {"xs": 4, "sm": 8, "md": 12, "base": 16, "lg": 20, "xl": 24, "xxl": 32}
RADIUS = {"sm": 6, "md": 10, "lg": 14, "xl": 20}
ANIM = {"on": True}               # the launcher's "animations" setting
FRAME_MS = 16


def font_family() -> str:
    try:
        fams = set(tkfont.families())
    except tk.TclError:
        return "Segoe UI"
    for f in ("Segoe UI Variable Text", "Segoe UI", "Inter", "Helvetica"):
        if f in fams:
            return f
    return "TkDefaultFont"


_FAM: str | None = None


def F(size: int, weight: int = 400) -> tuple:
    """Font for the 400 / 600 / 700 ladder (500 is not used)."""
    global _FAM
    if _FAM is None:
        _FAM = font_family()
    if weight >= 700:
        return (_FAM, size, "bold")
    if weight >= 600:
        try:
            fams = set(tkfont.families())
        except tk.TclError:
            fams = set()
        for f in ("Segoe UI Variable Text Semibold", "Segoe UI Semibold"):
            if f in fams:
                return (f, size)
        return (_FAM, size, "bold")
    return (_FAM, size)


_SERIF: str | None = None


def H(size: int, bold: bool = True) -> tuple:
    """Serif heading font (the journal / Pokédex feel of the game's titles); falls back to F()."""
    global _SERIF
    if _SERIF is None:
        try:
            fams = set(tkfont.families())
        except tk.TclError:
            fams = set()
        _SERIF = next((f for f in ("Palatino Linotype", "Book Antiqua", "Georgia", "Cambria") if f in fams), "")
    if not _SERIF:
        return F(size, 700 if bold else 400)
    return (_SERIF, size, "bold") if bold else (_SERIF, size)


def mix(a: str, b: str, t: float) -> str:
    t = max(0.0, min(1.0, t))
    pa = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    pb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(pa, pb))


def ease_out(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return 1 - (1 - t) ** 3


def round_rect(cv: tk.Canvas, x0, y0, x1, y1, r, **kw) -> int:
    """Rounded rectangle as one smooth polygon (continuous-looking corners)."""
    r = max(0, min(r, (x1 - x0) / 2, (y1 - y0) / 2))
    pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1,
           x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
    return cv.create_polygon(pts, smooth=True, splinesteps=24, **kw)


class Tween:
    """Eases a value towards a target on the Tk event loop (ease-out, ~180 ms)."""

    def __init__(self, widget: tk.Misc, on_step, value: float = 0.0, ms: int = 180):
        self.w, self.on_step, self.value, self.ms = widget, on_step, value, ms
        self._from = value
        self._to = value
        self._t0 = 0.0
        self._job = None

    def to(self, target: float) -> None:
        if not ANIM["on"]:
            self.value = self._to = target
            self.on_step(self.value)
            return
        if target == self._to and self._job:
            return
        self._from, self._to, self._t0 = self.value, target, time.perf_counter()
        if not self._job:
            self._tick()

    def _tick(self) -> None:
        t = (time.perf_counter() - self._t0) * 1000 / self.ms
        self.value = self._from + (self._to - self._from) * ease_out(t)
        try:
            self.on_step(self.value)
        except tk.TclError:
            self._job = None
            return
        if t < 1:
            self._job = self.w.after(FRAME_MS, self._tick)
        else:
            self.value = self._to
            self._job = None


# ---- buttons -----------------------------------------------------------------------------------
class PillButton(tk.Canvas):
    """Pill-shaped action.  kind: primary (filled blue), secondary (parchment fill, ink text),
    ghost (blue text, fill on hover), danger (red text, for Stop).  Press = slight shrink."""

    KINDS = {
        "primary": ("primary", "primaryHover", "onPrimary"),
        "secondary": ("parchment", "track", "ink"),
        "ghost": (None, "parchment", "primary"),
        "danger": ("dangerSoft", "dangerHover", "dangerText"),
        "dangerFill": ("danger", "dangerText", "onPrimary"),   # filled red, white text (Uninstall)
    }

    def __init__(self, parent, text: str, command=None, kind: str = "secondary", bg: str | None = None,
                 size: str = "md", width: int | None = None):
        self._bg = bg or T["canvas"]
        self.kind, self._text, self._cmd, self._state = kind, text, command, "normal"
        self._font = F(15 if size == "lg" else 12 if size == "md" else 10, 600)
        h = {"lg": 48, "md": 36, "sm": 28}[size]
        pad = {"lg": 34, "md": 20, "sm": 14}[size]
        tw = tkfont.Font(font=self._font).measure(text)
        self._fixed_w = width
        super().__init__(parent, height=h, width=width or tw + 2 * pad, bg=self._bg,
                         highlightthickness=0, bd=0, cursor="hand2")
        self._pad = pad
        self._hover = Tween(self, lambda _v: self._draw(), 0.0, 150)
        self._press = Tween(self, lambda _v: self._draw(), 0.0, 90)
        self.bind("<Enter>", lambda _e: self._state == "normal" and self._hover.to(1.0))
        self.bind("<Leave>", lambda _e: (self._hover.to(0.0), self._press.to(0.0)))
        self.bind("<ButtonPress-1>", lambda _e: self._state == "normal" and self._press.to(1.0))
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<Configure>", lambda _e: self._draw())
        self._draw()

    def _release(self, e) -> None:
        self._press.to(0.0)
        inside = 0 <= e.x <= self.winfo_width() and 0 <= e.y <= self.winfo_height()
        if inside and self._state == "normal" and self._cmd:
            self.after(60, self._cmd)

    def _draw(self) -> None:
        self.delete("all")
        w, h = max(2, self.winfo_width()), max(2, self.winfo_height())
        if w <= 2:
            w, h = self.winfo_reqwidth(), self.winfo_reqheight()
        base_k, hov_k, fg_k = self.KINDS[self.kind]
        if self._state == "disabled":
            fill = T["parchment"] if self.kind != "ghost" else None
            fg = T["inkMuted48"]
        else:
            base = T.get(base_k, base_k) if base_k else self._bg
            hov = T.get(hov_k, hov_k)
            fill = mix(base, hov, self._hover.value)
            if not base_k and self._hover.value < 0.02:
                fill = None
            fg = T.get(fg_k, fg_k)
        s = 1.6 * self._press.value                       # press: shrink a little
        if fill:
            round_rect(self, 1 + s, 1 + s, w - 1 - s, h - 1 - s, h / 2, fill=fill, outline="")
        self.create_text(w / 2, h / 2, text=self._text, fill=fg, font=self._font)

    # the ttk-like API the launcher logic uses
    def configure(self, cnf=None, **kw):
        changed = False
        if "state" in kw:
            self._state = "disabled" if str(kw.pop("state")) == "disabled" else "normal"
            self.configure_cursor()
            changed = True
        if "text" in kw:
            self._text = kw.pop("text")
            if not self._fixed_w:
                tw = tkfont.Font(font=self._font).measure(self._text)
                super().configure(width=tw + 2 * self._pad)
            changed = True
        if "command" in kw:
            self._cmd = kw.pop("command")
        if kw or cnf:
            super().configure(cnf, **kw)
        if changed:
            self._draw()

    config = configure

    def configure_cursor(self) -> None:
        super().configure(cursor="hand2" if self._state == "normal" else "arrow")

    def cget(self, key):
        if key == "state":
            return self._state
        if key == "text":
            return self._text
        return super().cget(key)


# ---- segmented control ---------------------------------------------------------------------
class Segmented(tk.Canvas):
    """Apple segmented control: parchment track, white knob that slides to the chosen segment."""

    def __init__(self, parent, items: list[str], on_change, bg: str | None = None, width: int = 360):
        super().__init__(parent, height=36, width=width, bg=bg or T["page"], highlightthickness=0, bd=0,
                         cursor="hand2")
        self.items, self.on_change, self.index = items, on_change, 0
        self._pos = Tween(self, lambda _v: self._draw(), 0.0, 220)
        self.bind("<Button-1>", self._click)
        self.bind("<Configure>", lambda _e: self._draw())

    def select(self, i: int, notify: bool = True) -> None:
        if i == self.index and notify:
            return
        self.index = i
        self._pos.to(float(i))
        if notify:
            self.on_change(i)

    def _click(self, e) -> None:
        w = max(1, self.winfo_width())
        self.select(min(len(self.items) - 1, int(e.x / (w / len(self.items)))))

    def _draw(self) -> None:
        self.delete("all")
        w, h = max(2, self.winfo_width()), max(2, self.winfo_height())
        n = len(self.items)
        seg = (w - 4) / n
        round_rect(self, 0, 0, w, h, h / 2, fill=T["track"], outline="")
        x = 2 + seg * self._pos.value
        round_rect(self, x, 2, x + seg, h - 2, (h - 4) / 2, fill=T["knob"], outline="")
        for i, name in enumerate(self.items):
            on = abs(self._pos.value - i) < 0.5
            self.create_text(2 + seg * i + seg / 2, h / 2, text=name, font=F(11, 600 if on else 400),
                             fill=T["ink"] if on else T["inkMuted48"])


# ---- surfaces ------------------------------------------------------------------------------
class Card(tk.Canvas):
    """White rounded tile on the parchment page (surface change, not a shadow).  Put widgets in .body.
    flash(colour) tints the hairline for a moment (the run/build result)."""

    def __init__(self, parent, bg: str | None = None, pad: int = SPACE["lg"], radius: int = RADIUS["lg"],
                 fill: str | None = None):
        super().__init__(parent, bg=bg or T["page"], highlightthickness=0, bd=0, height=10)
        self._h = 10
        self._fill = fill or T["canvas"]
        self._pad, self._r = pad, radius
        self._edge = T["hairline"]
        self.body = tk.Frame(self, bg=self._fill)
        self._win = self.create_window(pad, pad, window=self.body, anchor="nw")
        self.body.bind("<Configure>", self._fit)
        self.bind("<Configure>", self._layout)
        self._flash_t0 = 0.0
        self._flash_c = ""
        self._auto = True
        self.after_idle(self._watch)

    def _watch(self) -> None:
        """A canvas window outside the visible area gets no <Configure>: re-check the content height."""
        if not self._auto:
            return
        try:
            self._fit()
            self.after(150, self._watch)
        except tk.TclError:
            pass

    def _fit(self, _e=None) -> None:
        need = self.body.winfo_reqheight() + 2 * self._pad
        if abs(self._h - need) > 1:
            self._h = need
            super().configure(height=need)

    def _layout(self, _e=None) -> None:
        w, h = self.winfo_width(), self.winfo_height()
        self.itemconfigure(self._win, width=max(1, w - 2 * self._pad))
        self.delete("bgshape")
        round_rect(self, 1, 1, w - 1, h - 1, self._r, fill=self._fill, outline=self._edge, tags="bgshape")
        self.tag_lower("bgshape")

    def flash(self, colour: str) -> None:
        self._flash_t0, self._flash_c = time.perf_counter(), colour
        self._flash_step()

    def _flash_step(self) -> None:
        t = (time.perf_counter() - self._flash_t0) / 1.6
        if t >= 1 or not ANIM["on"]:
            self._edge = T["hairline"]
            self._layout()
            return
        self._edge = mix(self._flash_c, T["hairline"], ease_out(t))
        self._layout()
        self.after(33, self._flash_step)


class Tile(tk.Frame):
    """Clickable quick-action tile: icon, title, one muted line.  Hover eases the fill to pearl."""

    def __init__(self, parent, icon: str, title: str, subtitle: str, command, bg: str | None = None):
        super().__init__(parent, bg=T["canvas"], highlightthickness=1, highlightbackground=T["hairline"],
                         cursor="hand2", padx=SPACE["base"], pady=SPACE["md"])
        self._cmd = command
        # the content sits in the middle of the tile at any window size (expand without fill = centred)
        self._inner = tk.Frame(self, bg=T["canvas"])
        self._inner.pack(expand=True)
        self._labels = [tk.Label(self._inner, text=icon, font=F(18), bg=T["canvas"], fg=T["primary"]),
                        tk.Label(self._inner, text=title, font=F(12, 600), bg=T["canvas"], fg=T["ink"],
                                 justify="center"),
                        tk.Label(self._inner, text=subtitle, font=F(9), bg=T["canvas"], fg=T["inkMuted48"],
                                 wraplength=180, justify="center")]
        for lb in self._labels:
            lb.pack(anchor="center")
        self._labels[0].pack_configure(pady=(0, 2))
        self.bind("<Configure>", self._rewrap)
        self._hover = Tween(self, self._paint, 0.0, 150)
        for wdg in (self, self._inner, *self._labels):
            wdg.bind("<Enter>", lambda _e: self._hover.to(1.0))
            wdg.bind("<Leave>", lambda _e: self._hover.to(0.0))
            wdg.bind("<ButtonRelease-1>", lambda _e: self._cmd and self.after(40, self._cmd))

    def _rewrap(self, e) -> None:
        """The subtitle wraps to the tile's width, so it stays centred and on as few lines as fit."""
        wrap = max(80, e.width - 2 * SPACE["base"] - 6)
        if int(self._labels[2].cget("wraplength")) != wrap:
            self._labels[2].configure(wraplength=wrap)

    def _paint(self, v: float) -> None:
        c = mix(T["canvas"], T["parchment"], v)
        self.configure(bg=c, highlightbackground=mix(T["hairline"], T["primary"], v * 0.6))
        self._inner.configure(bg=c)
        for lb in self._labels:
            lb.configure(bg=c)


# ---- progress ------------------------------------------------------------------------------
class PillBar(tk.Canvas):
    """Thin pill progress bar.  determinate: value 0..maximum; indeterminate: a soft segment glides."""

    def __init__(self, parent, maximum: float = 1000, bg: str | None = None, height: int = 6):
        super().__init__(parent, height=height, bg=bg or T["canvas"], highlightthickness=0, bd=0)
        self._mode, self._value, self._max = "determinate", 0.0, maximum
        self._job = None
        self._phase = 0.0
        self.bind("<Configure>", lambda _e: self._draw())

    def configure(self, cnf=None, **kw):
        redraw = False
        if "mode" in kw:
            self._mode = kw.pop("mode")
            redraw = True
        if "value" in kw:
            self._value = float(kw.pop("value"))
            redraw = True
        if "maximum" in kw:
            self._max = float(kw.pop("maximum"))
        if kw or cnf:
            super().configure(cnf, **kw)
        if redraw:
            self._draw()

    config = configure

    def cget(self, key):
        if key == "mode":
            return self._mode
        if key == "value":
            return self._value
        return super().cget(key)

    def start(self, _interval: int = 15) -> None:
        if not self._job:
            self._spin()

    def stop(self) -> None:
        if self._job:
            self.after_cancel(self._job)
            self._job = None
        self._draw()

    def _spin(self) -> None:
        self._phase = (self._phase + 0.012) % 1.0
        self._draw()
        self._job = self.after(FRAME_MS, self._spin)

    def _draw(self) -> None:
        self.delete("all")
        w, h = max(2, self.winfo_width()), max(2, self.winfo_height())
        round_rect(self, 0, 0, w, h, h / 2, fill=T["track"], outline="")
        if self._mode == "indeterminate" and self._job:
            seg = w * 0.28
            x = -seg + (w + seg) * ease_out(self._phase) if ANIM["on"] else 0
            x0, x1 = max(0, x), min(w, x + seg)
            if x1 - x0 > h:
                round_rect(self, x0, 0, x1, h, h / 2, fill=T["primary"], outline="")
            return
        fw = w * max(0.0, min(1.0, self._value / self._max if self._max else 0))
        if fw >= h:
            round_rect(self, 0, 0, fw, h, h / 2, fill=T["primary"], outline="")
        elif fw > 0:
            self.create_oval(0, 0, h, h, fill=T["primary"], outline="")


class StatusPill(tk.Canvas):
    """Small parchment pill with a coloured dot and a label; the dot breathes while busy."""

    COLOURS = {"accent": "primary", "good": "success", "warn": "warning", "error": "danger", "": "inkMuted48"}

    def __init__(self, parent, bg: str | None = None):
        super().__init__(parent, height=30, width=180, bg=bg or T["page"], highlightthickness=0, bd=0)
        self._text, self._colour, self._busy, self._t0 = "Ready", T["success"], False, time.perf_counter()
        self._job = None
        self._wpx = 180
        self.bind("<Configure>", lambda _e: self._draw())

    def set(self, text: str, colour: str | None = None, busy: bool = False) -> None:
        self._text = text
        key = self.COLOURS.get(colour or "", colour or "inkMuted48")
        self._colour = T.get(key, key) if key else T["inkMuted48"]
        self._busy = busy
        self._draw()
        if busy and not self._job:
            self._pulse()

    def _pulse(self) -> None:
        if not self._busy:
            self._job = None
            self._draw()
            return
        self._draw()
        self._job = self.after(50, self._pulse)

    def _draw(self) -> None:
        import math
        self.delete("all")
        f = F(10, 600)
        tw = tkfont.Font(font=f).measure(self._text)
        w = tw + 44
        if abs(self._wpx - w) > 2:
            self._wpx = w
            super().configure(width=w)
        h = 30
        round_rect(self, 0, 0, w, h, h / 2, fill=T["canvas"], outline=T["hairline"])
        c = self._colour
        if self._busy and ANIM["on"]:
            t = (math.sin((time.perf_counter() - self._t0) * 4.5) + 1) / 2
            c = mix(c, T["canvas"], 0.55 * t)
        self.create_oval(14, h / 2 - 4, 22, h / 2 + 4, fill=c, outline="")
        self.create_text(30, h / 2, text=self._text, anchor="w", font=f, fill=T["ink"])


class ChipRow(tk.Canvas):
    """Boot stages as small pills that wrap; state per chip: idle / done / now / stopped."""

    STYLE = {"idle": ("parchment", "inkMuted48"), "done": ("successSoft", "successText"),
             "now": ("primary", "onPrimary"), "stopped": ("dangerSoft", "dangerText")}

    def __init__(self, parent, names: list[str], bg: str | None = None):
        super().__init__(parent, height=34, bg=bg or T["canvas"], highlightthickness=0, bd=0)
        self.names = names
        self.states = ["idle"] * len(names)
        self._h = 34
        self.pulse = 0.0
        self.bind("<Configure>", lambda _e: self.draw())

    def draw(self) -> None:
        self.delete("all")
        w = max(50, self.winfo_width())
        f = F(9, 600)
        meas = tkfont.Font(font=f)
        x, y, h, gap = 0, 2, 24, 6
        for name, st in zip(self.names, self.states):
            cw = meas.measure(name) + 20
            if x + cw > w and x > 0:
                x, y = 0, y + h + gap
            fill_k, fg_k = self.STYLE[st]
            fill = T[fill_k]
            if st == "now":
                fill = mix(T["primary"], T["primaryHover"], self.pulse)
            round_rect(self, x, y, x + cw, y + h, h / 2, fill=fill, outline="")
            mark = "✓ " if st == "done" else ""
            self.create_text(x + cw / 2, y + h / 2, text=mark + name, font=f, fill=T[fg_k])
            x += cw + gap
        need = y + h + 4
        if abs(self._h - need) > 1:
            self._h = need
            self.configure(height=need)


# ---- game-style navigation and options ------------------------------------------------------
class TabBar(tk.Canvas):
    """The tab strip at the top, like the game's menu headers: serif names on paper, a gold underline that
    glides to the chosen tab, small gold diamonds at both ends.  Same API as Segmented: select(i, notify),
    .index, .items.  Ctrl+Tab style cycling is left to the caller."""

    def __init__(self, parent, items: list[str], on_change, bg: str | None = None, height: int = 46):
        super().__init__(parent, height=height, bg=bg or T["page"], highlightthickness=0, bd=0, cursor="hand2")
        self.items, self.on_change, self.index = items, on_change, 0
        self._pos = Tween(self, lambda _v: self._draw(), 0.0, 240)
        self._hover_i = -1
        self._xs: list[tuple[float, float]] = []
        self.bind("<Button-1>", self._click)
        self.bind("<Motion>", self._motion)
        self.bind("<Leave>", lambda _e: self._set_hover(-1))
        self.bind("<Configure>", lambda _e: self._draw())

    def select(self, i: int, notify: bool = True) -> None:
        i = max(0, min(len(self.items) - 1, i))
        if i == self.index and notify:
            return
        self.index = i
        self._pos.to(float(i))
        if notify:
            self.on_change(i)

    def _hit(self, x: float) -> int:
        for i, (a, b) in enumerate(self._xs):
            if a - 10 <= x <= b + 10:
                return i
        return -1

    def _click(self, e) -> None:
        i = self._hit(e.x)
        if i >= 0:
            self.select(i)

    def _motion(self, e) -> None:
        self._set_hover(self._hit(e.x))

    def _set_hover(self, i: int) -> None:
        if i != self._hover_i:
            self._hover_i = i
            self._draw()

    def _draw(self) -> None:
        self.delete("all")
        w, h = max(2, self.winfo_width()), max(2, self.winfo_height())
        f_on, f_off = H(13), H(13, bold=False)
        meas = [tkfont.Font(font=f_on).measure(n) for n in self.items]
        gap = 34
        total = sum(meas) + gap * (len(self.items) - 1)
        x = (w - total) / 2
        base = h - 10
        # the rule under the strip, with a diamond at each end
        self.create_line(14, base, w - 14, base, fill=T["hairline"])
        for dx in (14, w - 14):
            self.create_polygon(dx, base - 4, dx + 4, base, dx, base + 4, dx - 4, base, fill=T["gold"], outline="")
        self._xs = []
        for i, (name, mw) in enumerate(zip(self.items, meas)):
            self._xs.append((x, x + mw))
            on = abs(self._pos.value - i) < 0.5
            fg = T["ink"] if on else (T["inkMuted80"] if i == self._hover_i else T["inkMuted48"])
            self.create_text(x + mw / 2, base - 16, text=name, font=f_on if on else f_off, fill=fg)
            x += mw + gap
        # the gold underline glides between tabs (interpolated position and width)
        if self._xs:
            p = max(0.0, min(len(self._xs) - 1.0, self._pos.value))
            i0 = int(p)
            i1 = min(i0 + 1, len(self._xs) - 1)
            t = p - i0
            a = self._xs[i0][0] + (self._xs[i1][0] - self._xs[i0][0]) * t
            b = self._xs[i0][1] + (self._xs[i1][1] - self._xs[i0][1]) * t
            round_rect(self, a - 6, base - 2, b + 6, base + 2, 2, fill=T["gold"], outline="")


class Section(tk.Frame):
    """A group heading in a settings panel: serif title, a muted line, then a thin gold rule."""

    def __init__(self, parent, title: str, subtitle: str = "", bg: str | None = None):
        bg = bg or T["canvas"]
        super().__init__(parent, bg=bg)
        top = tk.Frame(self, bg=bg)
        top.pack(fill="x")
        tk.Label(top, text="◆", font=F(8), bg=bg, fg=T["gold"]).pack(side="left", padx=(0, 6))
        tk.Label(top, text=title, font=H(14), bg=bg, fg=T["ink"]).pack(side="left")
        if subtitle:
            tk.Label(self, text=subtitle, font=F(9), bg=bg, fg=T["inkMuted48"], justify="left",
                     anchor="w", wraplength=760).pack(fill="x", pady=(2, 0))
        tk.Frame(self, bg=T["gold"], height=1).pack(fill="x", pady=(6, 2))


class SettingRow(tk.Frame):
    """One option line, as in the game's settings: name (and a muted explanation) left, control right,
    a soft divider under it.  make(parent) builds the control on the row's background."""

    def __init__(self, parent, label: str, make, help_text: str = "", bg: str | None = None,
                 divider: bool = True):
        bg = bg or T["canvas"]
        super().__init__(parent, bg=bg)
        row = tk.Frame(self, bg=bg)
        row.pack(fill="x", pady=(8, 6))
        left = tk.Frame(row, bg=bg)
        left.pack(side="left", fill="x", expand=True)
        tk.Label(left, text=label, font=F(11, 600), bg=bg, fg=T["ink"], anchor="w").pack(anchor="w")
        if help_text:
            tk.Label(left, text=help_text, font=F(9), bg=bg, fg=T["inkMuted48"], justify="left", anchor="w",
                     wraplength=520).pack(anchor="w", pady=(1, 0))
        self.control = make(row)
        self.control.pack(side="right", padx=(16, 2))
        if divider:
            tk.Frame(self, bg=T["dividerSoft"], height=1).pack(fill="x")


class Stepper(tk.Canvas):
    """◀ value ▶ chooser like the game's option values.  Click an arrow (or the left / right half), or use
    the mouse wheel.  var: a tk.StringVar holding one of values; on_change(value) after a change."""

    def __init__(self, parent, values, var: tk.StringVar, on_change=None, width: int = 210,
                 bg: str | None = None, labels: dict | None = None):
        super().__init__(parent, height=34, width=width, bg=bg or T["canvas"], highlightthickness=0, bd=0,
                         cursor="hand2")
        self.values, self.var, self.on_change = list(values), var, on_change
        self.labels = labels or {}
        self._hover = Tween(self, lambda _v: self._draw(), 0.0, 150)
        self._bump = Tween(self, lambda _v: self._draw(), 0.0, 160)
        self._dir = 0
        self.bind("<Button-1>", lambda e: self.step(-1 if e.x < self.winfo_width() / 2 else 1))
        self.bind("<MouseWheel>", lambda e: self.step(-1 if e.delta > 0 else 1))
        self.bind("<Enter>", lambda _e: self._hover.to(1.0))
        self.bind("<Leave>", lambda _e: self._hover.to(0.0))
        self.bind("<Configure>", lambda _e: self._draw())
        var.trace_add("write", lambda *_a: self._draw())

    def step(self, d: int) -> None:
        cur = self.var.get()
        i = self.values.index(cur) if cur in self.values else 0
        i = (i + d) % len(self.values)
        self.var.set(self.values[i])
        self._dir = d
        self._bump.value = 1.0
        self._bump.to(0.0)
        if self.on_change:
            self.on_change(self.values[i])

    def _draw(self) -> None:
        self.delete("all")
        w, h = max(2, self.winfo_width()), max(2, self.winfo_height())
        fill = mix(T["parchment"], T["track"], self._hover.value)
        round_rect(self, 1, 1, w - 1, h - 1, h / 2, fill=fill, outline=T["hairline"])
        for x, d in ((16, -1), (w - 16, 1)):        # gold arrow heads
            s = 5
            pts = (x - s * d, h / 2 - s, x + s * d, h / 2, x - s * d, h / 2 + s)
            self.create_polygon(pts, fill=T["gold"] if self._hover.value > 0.3 else T["goldText"], outline="")
        v = self.var.get()
        text = self.labels.get(v, v)
        off = 8 * self._bump.value * self._dir
        self.create_text(w / 2 + off, h / 2, text=text, font=F(11, 600), fill=T["ink"])


class Toggle(tk.Canvas):
    """On / off switch: navy pill with a paper knob when on, parchment when off; the knob slides."""

    def __init__(self, parent, var: tk.BooleanVar, on_change=None, bg: str | None = None):
        super().__init__(parent, height=28, width=96, bg=bg or T["canvas"], highlightthickness=0, bd=0,
                         cursor="hand2")
        self.var, self.on_change = var, on_change
        self._pos = Tween(self, lambda _v: self._draw(), 1.0 if var.get() else 0.0, 180)
        self.bind("<Button-1>", lambda _e: self.flip())
        self.bind("<Configure>", lambda _e: self._draw())
        var.trace_add("write", lambda *_a: self._pos.to(1.0 if self.var.get() else 0.0))

    def flip(self) -> None:
        self.var.set(not self.var.get())
        if self.on_change:
            self.on_change(self.var.get())

    def _draw(self) -> None:
        self.delete("all")
        v = self._pos.value
        sw, h = 48, 26
        x0 = 96 - sw - 1
        round_rect(self, x0, 1, x0 + sw, h + 1, h / 2, fill=mix(T["track"], T["primary"], v), outline="")
        kx = x0 + 3 + (sw - h) * v
        self.create_oval(kx, 4, kx + h - 6, h - 2, fill=T["knob"], outline=T["hairline"])
        self.create_text(x0 - 8, h / 2 + 1, text="On" if v >= 0.5 else "Off", anchor="e", font=F(10, 600),
                         fill=T["ink"] if v >= 0.5 else T["inkMuted48"])


class Slider(tk.Canvas):
    """0..max slider: parchment track, gold fill, paper knob; drag or click; shows the value on the right.
    var: tk.IntVar; on_change(value) on release (live=True: while dragging too)."""

    def __init__(self, parent, var: tk.IntVar, to: int = 100, on_change=None, width: int = 260,
                 bg: str | None = None, unit: str = "%", live: bool = False):
        super().__init__(parent, height=30, width=width, bg=bg or T["canvas"], highlightthickness=0, bd=0,
                         cursor="hand2")
        self.var, self.to, self.on_change, self.unit, self.live = var, max(1, to), on_change, unit, live
        self.bind("<Button-1>", self._drag)
        self.bind("<B1-Motion>", self._drag)
        self.bind("<ButtonRelease-1>", lambda _e: self.on_change and self.on_change(self.var.get()))
        self.bind("<MouseWheel>", self._wheel)
        self.bind("<Configure>", lambda _e: self._draw())
        var.trace_add("write", lambda *_a: self._draw())

    def _span(self) -> tuple[float, float]:
        return 12.0, max(40.0, self.winfo_width() - 58.0)

    def _drag(self, e) -> None:
        a, b = self._span()
        v = round((min(max(e.x, a), b) - a) / (b - a) * self.to)
        if v != self.var.get():
            self.var.set(v)
            if self.live and self.on_change:
                self.on_change(v)

    def _wheel(self, e) -> None:
        self.var.set(max(0, min(self.to, self.var.get() + (5 if e.delta > 0 else -5))))
        if self.on_change:
            self.on_change(self.var.get())

    def _draw(self) -> None:
        self.delete("all")
        a, b = self._span()
        h = max(2, self.winfo_height())
        y = h / 2
        t = max(0.0, min(1.0, self.var.get() / self.to))
        round_rect(self, a, y - 3, b, y + 3, 3, fill=T["track"], outline="")
        if t > 0:
            round_rect(self, a, y - 3, a + (b - a) * t, y + 3, 3, fill=T["gold"], outline="")
        kx = a + (b - a) * t
        self.create_oval(kx - 9, y - 9, kx + 9, y + 9, fill=T["knob"], outline=T["gold"], width=2)
        self.create_text(b + 46, y, text=f"{self.var.get()}{self.unit}", anchor="e", font=F(10, 600),
                         fill=T["ink"])


_ICONS: dict = {}        # loaded icon handles: {"path": str, "big": HICON, "small": HICON}


def set_app_icon(ico_path: str) -> None:
    """Use this .ico for every window from now on: the main window (call win_icon on it) and every Toplevel the
    launcher opens (Controls, Options, Saves, History, dialogs) - set straight through Windows (WM_SETICON), so
    the title bars, the taskbar button and Alt+Tab all show it."""
    import sys
    if sys.platform != "win32" or _ICONS.get("path") == ico_path:
        return
    try:
        import ctypes
        u = ctypes.windll.user32
        u.LoadImageW.restype = ctypes.c_void_p
        u.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_uint]
        load = lambda size: u.LoadImageW(None, ico_path, 1, size, size, 0x10)   # IMAGE_ICON, LR_LOADFROMFILE
        _ICONS.update(path=ico_path, big=load(u.GetSystemMetrics(11) * 2 if u.GetSystemMetrics(11) < 48 else
                                              u.GetSystemMetrics(11)), small=load(u.GetSystemMetrics(49)))
    except (OSError, AttributeError):
        return
    orig = tk.Toplevel.__init__

    def init(self, *a, **k):              # every new window gets the icon once it exists
        orig(self, *a, **k)
        self.after(0, lambda: win_icon(self))
    tk.Toplevel.__init__ = init


def _load_ico(path: str) -> tuple:
    """(big, small) icon handles of an .ico file (cached)."""
    key = ("ico", path)
    if key not in _ICONS:
        import ctypes
        u = ctypes.windll.user32
        u.LoadImageW.restype = ctypes.c_void_p
        u.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                 ctypes.c_uint]
        big = max(32, u.GetSystemMetrics(11))
        _ICONS[key] = (u.LoadImageW(None, path, 1, big, big, 0x10), u.LoadImageW(None, path, 1,
                                                                                  u.GetSystemMetrics(49),
                                                                                  u.GetSystemMetrics(49), 0x10))
    return _ICONS[key]


def _send_icon(win: tk.Misc, big, small) -> None:
    import ctypes
    u = ctypes.windll.user32
    u.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
    win.update_idletasks()
    hwnd = u.GetParent(win.winfo_id()) or win.winfo_id()
    u.SendMessageW(hwnd, 0x80, 1, big)            # WM_SETICON, ICON_BIG: taskbar, Alt+Tab
    u.SendMessageW(hwnd, 0x80, 0, small)          # ICON_SMALL: title bar


def win_icon(win: tk.Misc) -> None:
    """Put the app icon (set_app_icon) on this window (unless window_icon gave it its own)."""
    import sys
    if sys.platform != "win32" or not _ICONS.get("big") or getattr(win, "_own_icon", False):
        return
    try:
        _send_icon(win, _ICONS["big"], _ICONS["small"])
    except (OSError, AttributeError, tk.TclError):
        pass


def window_icon(win: tk.Misc, kind: str) -> None:
    """A window opened from an Overview tile gets that tile's symbol as its icon (launcher/assets/icons/
    <kind>_<theme>.ico: saves, controls, options, history; drawn by Engine/Tools/make_logos.py)."""
    import sys
    from pathlib import Path
    if sys.platform != "win32" or not _ICONS.get("path"):
        return
    theme = "dark" if T.get("page", "#ffffff").lower() < "#808080" else "paper"
    ico = Path(_ICONS["path"]).parent / "icons" / f"{kind}_{theme}.ico"
    if not ico.is_file():
        return
    win._own_icon = True

    def apply():
        try:
            _send_icon(win, *_load_ico(str(ico)))
        except (OSError, AttributeError, tk.TclError):
            pass
        title_bar(win)                            # and the title bar in the theme (dark in the dark theme)
    win.after(20, apply)


def title_bar(win: tk.Misc) -> None:
    """Give a window (the launcher's dialogs) a dark Windows title bar when the dark theme is on."""
    import sys
    if sys.platform != "win32":
        return
    try:
        import ctypes
        win.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(win.winfo_id()) or win.winfo_id()
        on = ctypes.c_int(1 if T.get("page", "#ffffff").lower() < "#808080" else 0)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), ctypes.sizeof(on))
    except (OSError, AttributeError, tk.TclError):
        pass


def snapshot(widget: tk.Misc) -> tk.PhotoImage | None:
    """A picture of the widget's window as it is on screen (Windows: PrintWindow of this window only, never the
    desktop), as a Tk image (PPM, no extra packages).  None where that is not possible."""
    import sys
    if sys.platform != "win32":
        return None
    try:
        import ctypes
        import ctypes.wintypes as wt
        user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
        hwnd = widget.winfo_id()
        r = wt.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(r))
        w, h = r.right - r.left, r.bottom - r.top
        if w < 2 or h < 2:
            return None
        hdc = user32.GetDC(hwnd)
        mdc = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
        old = gdi32.SelectObject(mdc, bmp)
        user32.PrintWindow(hwnd, mdc, 2)                   # PW_RENDERFULLCONTENT

        class BIH(ctypes.Structure):
            _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                        ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                        ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                        ("biClrImportant", wt.DWORD)]
        bih = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(w * h * 4)
        got = gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
        gdi32.SelectObject(mdc, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mdc)
        user32.ReleaseDC(hwnd, hdc)
        if got != h:
            return None
        src = buf.raw
        rgb = bytearray(w * h * 3)                         # BGRA -> RGB with slice copies (fast)
        rgb[0::3], rgb[1::3], rgb[2::3] = src[2::4], src[1::4], src[0::4]
        return tk.PhotoImage(master=widget, data=b"P6\n%d %d\n255\n" % (w, h) + bytes(rgb), format="ppm")
    except (OSError, AttributeError, ValueError, tk.TclError):
        return None


def fade_in(root: tk.Tk, ms: int = 220) -> None:
    """Window fades in on start (Windows / most desktops support -alpha)."""
    if not ANIM["on"]:
        return
    try:
        root.attributes("-alpha", 0.0)
    except tk.TclError:
        return
    t0 = time.perf_counter()

    def step():
        t = (time.perf_counter() - t0) * 1000 / ms
        try:
            root.attributes("-alpha", min(1.0, ease_out(t)))
        except tk.TclError:
            return
        if t < 1:
            root.after(FRAME_MS, step)

    root.after(30, step)


def slide_in(frame: tk.Widget, px: int = 14, ms: int = 200) -> None:
    """A panel that was just shown eases up into place (place()-managed inside its holder)."""
    if not ANIM["on"]:
        frame.place_configure(y=0)
        return
    t0 = time.perf_counter()
    # One animation per frame, and only while the frame is shown: place_configure() on a panel that was hidden
    # (place_forget, another tab clicked mid-slide) puts it back on screen, so two tabs overlapped half-drawn.
    gen = getattr(frame, "_slide_gen", 0) + 1
    frame._slide_gen = gen

    def step():
        if getattr(frame, "_slide_gen", 0) != gen:
            return
        t = (time.perf_counter() - t0) * 1000 / ms
        try:
            if frame.winfo_manager() != "place":
                return
            frame.place_configure(y=round(px * (1 - ease_out(t))))
        except tk.TclError:
            return
        if t < 1:
            frame.after(FRAME_MS, step)

    step()
