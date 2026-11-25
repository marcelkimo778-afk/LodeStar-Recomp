"""The controller picture of the Controls window: a Pro-style controller (own drawing from simple shapes; the
layout of the Switch emulators' controller page).  Pressed buttons light up, two dotted circles under it show
where each stick is.

With Pillow the picture is rendered 3x and scaled down (smooth, antialiased edges); without it the same shapes
are drawn straight on the Tk canvas.  One shape list serves both and the click test (a click on a button maps it).
"""
from __future__ import annotations

import tkinter as tk

W, H = 470, 336          # canvas size
CX = W / 2


def chaikin(pts: list[tuple[float, float]], rounds: int = 4, closed: bool = True) -> list[tuple[float, float]]:
    """Corner cutting: a smooth curve through a coarse outline (the same kind of curve as Tk's smooth=True)."""
    for _ in range(rounds):
        out = []
        n = len(pts)
        for i in range(n if closed else n - 1):
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % n]
            out += [(0.75 * x0 + 0.25 * x1, 0.75 * y0 + 0.25 * y1), (0.25 * x0 + 0.75 * x1, 0.25 * y0 + 0.75 * y1)]
        pts = out
    return pts


def mirror(pts):
    return [(W - x, y) for x, y in pts]


def round_rect_pts(x0, y0, x1, y1, r):
    """A rounded rectangle as a polygon."""
    import math
    pts = []
    for cx, cy, a0 in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
        for k in range(7):
            a = math.radians(a0 + k * 15)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


# The shell: traced from Nintendo's front view of the Pro Controller (its outline only, automatically: dark pixels,
# boundary following, left/right averaged), scaled to the canvas.  Top centre -> left side -> centre of the dip.
SHELL_HALF = [(235.0, 44.9), (218.3, 44.9), (201.6, 44.9), (185.0, 45.9), (168.4, 45.7), (152.6, 40.3), (136.1, 41.1), (119.8, 43.4), (104.0, 48.0), (91.1, 54.3), (79.3, 65.7), (69.4, 78.9), (61.7, 93.3), (57.0, 109.1), (53.3, 125.2), (50.1, 141.5), (46.8, 157.7), (43.9, 174.0), (41.2, 190.3), (38.5, 206.7), (35.9, 223.0), (33.0, 239.3), (31.0, 255.7), (30.0, 272.3), (30.8, 288.8), (36.6, 304.4), (47.2, 317.1), (62.1, 324.2), (78.5, 322.8), (92.2, 313.6), (101.3, 299.8), (109.4, 285.2), (116.6, 270.3), (124.4, 255.6), (135.3, 243.3), (151.5, 240.6), (168.2, 240.6), (184.9, 240.6), (201.6, 240.6), (218.3, 240.6), (235.0, 240.6)]
PARTS = {"LSTICK": (120.9, 124.0), "RSTICK": (288.8, 182.3), "DPAD": (172.3, 182.3), "MINUS": (185.1, 93.3), "PLUS": (286.9, 93.3), "CAPTURE": (207.8, 125.0), "HOME": (264.1, 125.0), "X": (344.2, 97.3), "Y": (311.6, 126.0), "A": (375.8, 126.0), "B": (344.2, 153.6)}


def _shapes(theme_dark: bool):
    """[(name or None, kind, geometry, fill, outline, label)] in drawing order.  kind: poly / oval / line; label:
    (text, size[, x, y]) or a 'house' / 'dot' glyph.  name = the Switch input the shape maps (clickable, lights up)."""
    body = "#d0d0d0" if theme_dark else "#d9d1bf"
    edge = "#4d4d4d" if theme_dark else "#7f735d"
    seam = "#b3b3b3" if theme_dark else "#bfb49c"
    base = "#c2c2c2" if theme_dark else "#cbc2ae"
    ink, cap, rim, shoulder = "#1f1f1f", "#161616", "#5a5a5a", "#333333"
    s = []

    def circle(name, xy, r, fill, line, label=None):
        x, y = xy
        s.append((name, "oval", (x - r, y - r, x + r, y + r), fill, line, label))

    outline = chaikin(SHELL_HALF + mirror(SHELL_HALF)[::-1][1:-1], rounds=2)
    s.append((None, "poly", outline, body, edge, None))
    # L / R: crescents along the top corners (the traced edge, and the same edge moved in), labelled
    top = SHELL_HALF[0][1]
    corner = [p for p in SHELL_HALF if p[1] < top + 46 and p[0] < CX - 62]
    outer_edge = [(x - 4, y - 7) for x, y in corner]          # raised above the shell, like a real shoulder button
    inner_edge = [(x + 7, y + 6) for x, y in corner[::-1]]
    lshape = chaikin(outer_edge + inner_edge, rounds=2)
    mid = len(corner) // 2                                    # the label sits in the middle of the band
    lx = (outer_edge[mid][0] + inner_edge[len(corner) - 1 - mid][0]) / 2
    ly = (outer_edge[mid][1] + inner_edge[len(corner) - 1 - mid][1]) / 2
    s.append(("L", "poly", lshape, shoulder, edge, ("L", 11, lx, ly)))
    s.append(("R", "poly", mirror(lshape), shoulder, edge, ("R", 11, W - lx, ly)))
    # seams between the face plate and the grips
    seam_l = chaikin([(70, 70), (58, 130), (66, 190), (92, 226), (128, 240)], rounds=3, closed=False)
    s.append((None, "line", seam_l, seam, seam, None))
    s.append((None, "line", mirror(seam_l), seam, seam, None))
    # ZL / ZR: tags above the shoulders
    s.append(("ZL", "poly", round_rect_pts(10, 4, 60, 28, 7), "#2a2a2a", "#d8d8d8", ("ZL", 12)))
    s.append(("ZR", "poly", round_rect_pts(W - 60, 4, W - 10, 28, 7), "#2a2a2a", "#d8d8d8", ("ZR", 12)))
    # the NFC panel at the top centre
    s.append((None, "poly", round_rect_pts(CX - 9, top + 12, CX + 9, top + 38, 2), body, edge, None))
    # minus, plus, capture, home
    circle("MINUS", PARTS["MINUS"], 11, ink, edge, ("−", 13))
    circle("PLUS", PARTS["PLUS"], 11, ink, edge, ("+", 13))
    x, y = PARTS["CAPTURE"]
    s.append(("CAPTURE", "poly", round_rect_pts(x - 10, y - 10, x + 10, y + 10, 4), ink, edge, "dot"))
    circle("HOME", PARTS["HOME"], 11, ink, edge, "house")
    # sticks: a lighter base (the stick press), the dark cap with a grey rim
    for name in ("LSTICK", "RSTICK"):
        circle(name, PARTS[name], 32, base, edge)
        circle(None, PARTS[name], 21, cap, rim, ("cap", name))           # the caps are drawn as moving sprites
        circle(None, PARTS[name], 15, cap, "#2c2c2c", ("cap", name))
    # d-pad: a rounded cross, every arm its own button
    dx, dy = PARTS["DPAD"]
    a = 12
    arms = {"DPAD_UP": (dx - a, dy - 3 * a, dx + a, dy), "DPAD_DOWN": (dx - a, dy, dx + a, dy + 3 * a),
            "DPAD_LEFT": (dx - 3 * a, dy - a, dx, dy + a), "DPAD_RIGHT": (dx, dy - a, dx + 3 * a, dy + a)}
    for name, (x0, y0, x1, y1) in arms.items():
        s.append((name, "poly", round_rect_pts(x0, y0, x1, y1, 4), ink, edge, None))
    s.append((None, "poly", round_rect_pts(dx - a + 1, dy - a + 1, dx + a - 1, dy + a - 1, 2), ink, ink, None))
    for (tx, ty, ox, oy) in ((dx, dy - 2.5 * a, 0, 1), (dx, dy + 2.5 * a, 0, -1), (dx - 2.5 * a, dy, 1, 0),
                             (dx + 2.5 * a, dy, -1, 0)):
        tri = [(tx, ty), (tx + 6 * ox - 5 * oy, ty + 6 * oy - 5 * ox), (tx + 6 * ox + 5 * oy, ty + 6 * oy + 5 * ox)]
        s.append((None, "poly", tri, "#f2f2f2", "#f2f2f2", None))
    # face buttons: X top, Y left, A right, B bottom (measured centres)
    for name in ("X", "Y", "A", "B"):
        circle(name, PARTS[name], 15, ink, edge, (name, 15))
    return s, edge


STICK_C = {"l": (CX - 52, 286), "r": (CX + 52, 286)}   # the stick circles, in the gap between the grips
STICK_R = 27


def _inside(kind, geo, x, y) -> bool:
    if kind == "oval":
        x0, y0, x1, y1 = geo
        rx, ry = (x1 - x0) / 2, (y1 - y0) / 2
        return ((x - (x0 + rx)) / rx) ** 2 + ((y - (y0 + ry)) / ry) ** 2 <= 1.0
    inside, n = False, len(geo)
    for i in range(n):
        (x0, y0), (x1, y1) = geo[i], geo[(i + 1) % n]
        if (y0 > y) != (y1 > y) and x < (x1 - x0) * (y - y0) / (y1 - y0) + x0:
            inside = not inside
    return inside


class PadArt:
    """Draws the controller on `canvas` (W x H); on_click(name) when a button of the picture is clicked."""

    def __init__(self, canvas: tk.Canvas, colors: dict, on_click):
        self.cv, self.c, self.on_click = canvas, colors, on_click
        self.dark = str(colors.get("bg", "#000000")).lower() < "#808080"
        self.shapes, self.edge = _shapes(self.dark)
        self.lit: set[str] | None = None
        self.img = None
        self.knob: dict[str, int] = {}
        self.caps: dict[str, int] = {}
        self._cap_off: dict[str, tuple] = {}
        self._sprites: dict = {}
        self.items: dict[str, list[int]] = {}
        try:
            from PIL import Image, ImageDraw, ImageFont  # noqa: F401
            self.pil = True
        except ImportError:
            self.pil = False
        canvas.configure(width=W, height=H)
        canvas.bind("<Button-1>", self._click)
        self.draw(set())

    # --- the picture -------------------------------------------------------------------------------------
    def draw(self, lit: set[str]) -> None:
        """(Re)draw with the `lit` Switch inputs highlighted (only when that set changed)."""
        if lit == self.lit:
            return
        self.lit = set(lit)
        cv = self.cv
        cv.delete("pad")
        if self.pil:
            self._draw_pil()
        else:
            self._draw_tk()
        self._draw_caps()
        for side, (x, y) in STICK_C.items():          # stick circles + the moving dots (cheap Tk items)
            cv.create_oval(x - STICK_R, y - STICK_R, x + STICK_R, y + STICK_R, outline=self.edge, dash=(2, 3),
                           width=1, tags="pad")
            self.knob[side] = cv.create_oval(x - 4, y - 4, x + 4, y + 4, fill="#2fbf71", outline="", tags="pad")

    def _fill(self, name, fill):
        return self.c.get("accent", "#2f8cff") if name and name in self.lit else fill

    def _draw_pil(self) -> None:
        from PIL import Image, ImageDraw, ImageFont
        k = 3
        bg = self.c.get("card", "#ffffff")
        im = Image.new("RGB", (W * k, H * k), bg)
        d = ImageDraw.Draw(im)

        def font(size):
            for f in ("segoeuib.ttf", "arialbd.ttf"):
                try:
                    return ImageFont.truetype(f, size * k)
                except OSError:
                    continue
            return ImageFont.load_default()
        for name, kind, geo, fill, outline, label in self.shapes:
            if isinstance(label, tuple) and label[0] == "cap":
                continue
            fill = self._fill(name, fill)
            if kind == "line":
                d.line([(x * k, y * k) for x, y in geo], fill=outline, width=k, joint="curve")
                continue
            if kind == "oval":
                d.ellipse([v * k for v in geo], fill=fill, outline=outline, width=k)
                x0, y0, x1, y1 = geo
            else:
                pts = [(x * k, y * k) for x, y in geo]
                d.polygon(pts, fill=fill, outline=outline, width=2 * k if name is None and len(geo) > 60 else k)
                xs, ys = [p[0] for p in geo], [p[1] for p in geo]
                x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
            mx, my = (x0 + x1) / 2 * k, (y0 + y1) / 2 * k
            if label == "dot":
                d.ellipse([mx - 4 * k, my - 4 * k, mx + 4 * k, my + 4 * k], fill="#f2f2f2")
            elif label == "house":
                d.polygon([(mx, my - 6 * k), (mx - 6 * k, my), (mx - 4 * k, my), (mx - 4 * k, my + 5 * k),
                           (mx + 4 * k, my + 5 * k), (mx + 4 * k, my), (mx + 6 * k, my)], fill="#f2f2f2")
            elif label:
                if len(label) == 4:
                    mx, my = label[2] * k, label[3] * k
                d.text((mx, my), label[0], fill="#ffffff", font=font(label[1]), anchor="mm")
        im = im.resize((W, H), Image.LANCZOS)
        rgb = im.tobytes()
        self.img = tk.PhotoImage(master=self.cv, data=b"P6\n%d %d\n255\n" % (W, H) + rgb, format="ppm")
        self.cv.create_image(0, 0, image=self.img, anchor="nw", tags="pad")

    def _draw_tk(self) -> None:
        cv = self.cv
        for name, kind, geo, fill, outline, label in self.shapes:
            if isinstance(label, tuple) and label[0] == "cap":
                continue
            fill = self._fill(name, fill)
            if kind == "line":
                cv.create_line([v for p in geo for v in p], fill=outline, tags="pad")
                continue
            if kind == "oval":
                cv.create_oval(*geo, fill=fill, outline=outline, tags="pad")
                x0, y0, x1, y1 = geo
            else:
                cv.create_polygon([v for p in geo for v in p], fill=fill, outline=outline, tags="pad")
                xs, ys = [p[0] for p in geo], [p[1] for p in geo]
                x0, y0, x1, y1 = min(xs), min(ys), max(xs), max(ys)
            mx, my = (x0 + x1) / 2, (y0 + y1) / 2
            if label == "dot":
                cv.create_oval(mx - 4, my - 4, mx + 4, my + 4, fill="#f2f2f2", outline="", tags="pad")
            elif label == "house":
                cv.create_text(mx, my, text="⌂", fill="#f2f2f2", font=("Segoe UI", 9, "bold"), tags="pad")
            elif label:
                if len(label) == 4:
                    mx, my = label[2], label[3]
                cv.create_text(mx, my, text=label[0], fill="white", font=("Segoe UI", label[1] - 3, "bold"),
                               tags="pad")

    def _cap_sprite(self, pressed: bool):
        """A stick cap as a small smooth picture with a transparent background (PNG for Tk), cached."""
        key = ("cap", pressed)
        if key in self._sprites:
            return self._sprites[key]
        import base64
        import io
        from PIL import Image, ImageDraw
        k, r = 4, 22
        n = (2 * r + 2) * k
        im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        c = n / 2
        ring = self.c.get("accent", "#2f8cff") if pressed else "#5a5a5a"
        d.ellipse((c - 21 * k, c - 21 * k, c + 21 * k, c + 21 * k), fill="#161616", outline=ring, width=2 * k)
        d.ellipse((c - 15 * k, c - 15 * k, c + 15 * k, c + 15 * k), fill="#161616", outline="#2c2c2c", width=k)
        d.ellipse((c - 9 * k, c - 12 * k, c + 3 * k, c - 6 * k), fill=(255, 255, 255, 18))   # a soft highlight
        im = im.resize((n // k, n // k), Image.LANCZOS)
        buf = io.BytesIO()
        im.save(buf, "PNG")
        img = tk.PhotoImage(master=self.cv, data=base64.b64encode(buf.getvalue()), format="png")
        self._sprites[key] = img
        return img

    def _draw_caps(self) -> None:
        """The stick caps on top of the picture: they move with the sticks (set_sticks)."""
        self.caps = {}
        for side, name in (("l", "LSTICK"), ("r", "RSTICK")):
            x, y = PARTS[name]
            dx, dy = self._cap_off.get(side, (0, 0))
            if self.pil:
                it = self.cv.create_image(x + dx, y + dy, image=self._cap_sprite(name in self.lit), tags="pad")
            else:
                it = self.cv.create_oval(x + dx - 21, y + dy - 21, x + dx + 21, y + dy + 21, fill="#161616",
                                         outline=self.c.get("accent", "#2f8cff") if name in self.lit else "#5a5a5a",
                                         width=2, tags="pad")
            self.caps[side] = it                          # (clicks: the canvas handler's hit test, base = press)

    # --- live sticks, clicks ---------------------------------------------------------------------------
    def set_sticks(self, ls, rs) -> None:
        """Move the stick caps (up to 9 px inside their base ring) and the dots in the circles below."""
        for side, (x, y), name in (("l", ls, "LSTICK"), ("r", rs, "RSTICK")):
            cx, cy = STICK_C[side]
            dx, dy = x * (STICK_R - 5), -y * (STICK_R - 5)
            self.cv.coords(self.knob[side], cx + dx - 4, cy + dy - 4, cx + dx + 4, cy + dy + 4)
            off = (round(x * 9), round(-y * 9))
            if off != self._cap_off.get(side, (0, 0)) and side in self.caps:
                self._cap_off[side] = off
                px, py = PARTS[name]
                if self.pil:
                    self.cv.coords(self.caps[side], px + off[0], py + off[1])
                else:
                    self.cv.coords(self.caps[side], px + off[0] - 21, py + off[1] - 21, px + off[0] + 21,
                                   py + off[1] + 21)

    def hit(self, x, y) -> str | None:
        for name, kind, geo, _f, _o, _l in reversed(self.shapes):
            if name and _inside(kind, geo, x, y):          # the topmost button there (a stick cap = its press)
                return name
        return None

    def _click(self, e) -> None:
        name = self.hit(e.x, e.y)
        if name:
            self.on_click(name)
