#!/usr/bin/env python3
"""Draw the project's logos (original artwork, no game assets): "the Hisui compass".

    python Engine/Tools/make_logos.py

Writes launcher/assets/launcher.png + launcher.ico, launcher/assets/icons/*.ico (the Overview tile symbols
as window icons) (the launcher: navy disc, gold ring, faceted gold / cream
eight-point star) and Engine/Runtime/res/game.png + game.ico (the game exe: the same compass on a night-sky disc
with a paper ring).  Needs Pillow (only to regenerate them; the launcher and the build use the files)."""
from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
S = 2048                     # drawn large, scaled down (antialiasing)
C = S / 2


def hexc(h: str, a: int = 255) -> tuple:
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (a,)


def polar(r: float, deg: float) -> tuple[float, float]:
    a = math.radians(deg - 90)          # 0 deg = up
    return C + r * math.cos(a), C + r * math.sin(a)


def radial_disc(img: Image.Image, r: float, inner: str, outer: str) -> None:
    d = ImageDraw.Draw(img)
    ci, co = hexc(inner), hexc(outer)
    steps = 220
    for k in range(steps, 0, -1):
        t = k / steps
        col = tuple(round(co[i] * t + ci[i] * (1 - t)) for i in range(3)) + (255,)
        rr = r * t
        d.ellipse((C - rr, C - rr, C + rr, C + rr), fill=col)


def point(d: ImageDraw.ImageDraw, deg: float, length: float, width: float, light: str, dark: str,
          base: float = 0.0) -> None:
    """One star point: a long diamond from the centre to `length`, split along its axis into a light and a
    dark facet (the cut-metal look)."""
    tip = polar(length, deg)
    root = polar(base, deg)
    mid_r = base + (length - base) * 0.28
    left = polar(math.hypot(mid_r, width), deg - math.degrees(math.atan2(width, mid_r)))
    right = polar(math.hypot(mid_r, width), deg + math.degrees(math.atan2(width, mid_r)))
    d.polygon([root, left, tip], fill=hexc(light))
    d.polygon([root, right, tip], fill=hexc(dark))


def emblem(disc_in: str, disc_out: str, ring: str, ring_hi: str, long_l: str, long_d: str,
           short_l: str, short_d: str, stone: str, stone_ring: str) -> Image.Image:
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # soft shadow under the disc
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse((C - 0.47 * S, C - 0.45 * S, C + 0.47 * S, C + 0.49 * S), fill=(0, 0, 0, 110))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(S * 0.012)))
    radial_disc(img, 0.47 * S, disc_in, disc_out)
    d = ImageDraw.Draw(img)
    # outer ring: a thick band with a bright inner edge
    for r, w, col in ((0.452 * S, 0.040 * S, ring), (0.433 * S, 0.006 * S, ring_hi)):
        d.ellipse((C - r, C - r, C + r, C + r), outline=hexc(col), width=round(w))
    # compass ticks: 64 fine, 8 long
    for k in range(64):
        deg = k * 360 / 64
        long_ = k % 8 == 0
        r0, r1 = (0.355 * S, 0.420 * S) if long_ else (0.392 * S, 0.418 * S)
        d.line([polar(r0, deg), polar(r1, deg)], fill=hexc(ring_hi if long_ else ring, 230),
               width=round(S * (0.009 if long_ else 0.004)))
    # thin inner circle
    r = 0.300 * S
    d.ellipse((C - r, C - r, C + r, C + r), outline=hexc(ring, 150), width=round(S * 0.004))
    # the star: four short cream diagonals under four long gold points
    for k in range(4):
        point(d, 45 + 90 * k, 0.255 * S, 0.050 * S, short_l, short_d)
    for k in range(4):
        point(d, 90 * k, 0.405 * S, 0.072 * S, long_l, long_d)
    # centre stone
    for rr, col in ((0.075 * S, stone_ring), (0.060 * S, stone), (0.022 * S, ring)):
        d.ellipse((C - rr, C - rr, C + rr, C + rr), fill=hexc(col))
    return img


def save(img: Image.Image, png: Path, ico: Path) -> None:
    png.parent.mkdir(parents=True, exist_ok=True)
    big = img.resize((512, 512), Image.LANCZOS)
    big.save(png)
    sizes = [(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)]
    img.resize((256, 256), Image.LANCZOS).save(ico, sizes=sizes)
    print("wrote", png, ico)


# the Overview tiles' symbols (launcher.py) as window icons of the windows they open, per launcher theme
GLYPHS = {"saves": "\U0001F4BE", "controls": "\U0001F3AE", "options": "\u2699", "history": "\U0001F558"}
THEME_COLOURS = {   # (tile, edge, glyph) = ui_kit canvas, hairline, primary of each theme
    "paper": ("#fbf6ea", "#d5c299", "#2b4470"),
    "dark": ("#22252c", "#30343d", "#2f8cff"),
}


def glyph_icons() -> None:
    """launcher/assets/icons/<name>_<theme>.ico: the tile symbol in the theme's accent on a rounded tile."""
    from PIL import ImageFont
    fonts = Path(r"C:\Windows\Fonts")
    out = ROOT / "launcher" / "assets" / "icons"
    out.mkdir(parents=True, exist_ok=True)
    N = 512
    for name, ch in GLYPHS.items():
        for theme, (tile, edge, ink) in THEME_COLOURS.items():
            img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
            d.rounded_rectangle((8, 8, N - 8, N - 8), radius=110, fill=hexc(tile), outline=hexc(edge), width=14)
            font = None
            for f in ("seguiemj.ttf", "seguisym.ttf"):
                try:
                    font = ImageFont.truetype(str(fonts / f), 330)
                    if font.getbbox(ch)[2] > 0:
                        break
                except OSError:
                    font = None
            d.text((N / 2, N / 2 + 6), ch, font=font, fill=hexc(ink), anchor="mm")   # single colour, like the tile
            img.save(out / f"{name}_{theme}.ico", sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
            if theme == "paper":
                img.resize((128, 128), Image.LANCZOS).save(out / f"{name}_preview.png")
    print("wrote", out)


def main() -> None:
    launcher = emblem(disc_in="#3a5a92", disc_out="#1b2c4f", ring="#c0913a", ring_hi="#f2d488",
                      long_l="#f6dc93", long_d="#b6862c", short_l="#fbf3e2", short_d="#cdbb97",
                      stone="#fbf6ea", stone_ring="#b6862c")
    save(launcher, ROOT / "launcher" / "assets" / "launcher.png", ROOT / "launcher" / "assets" / "launcher.ico")
    # the game: the same compass in the other colours (crimson disc, silver ring and star, gold diagonals), so
    # the running game and the launcher are easy to tell apart in the taskbar / Task Manager (2026-11-25)
    game = emblem(disc_in="#c23a3a", disc_out="#5a1018", ring="#e9edf2", ring_hi="#ffffff",
                  long_l="#ffffff", long_d="#b9c3cf", short_l="#f6dc93", short_d="#b6862c",
                  stone="#fbf6ea", stone_ring="#8a1c24")
    save(game, ROOT / "Engine" / "Runtime" / "res" / "game.png", ROOT / "Engine" / "Runtime" / "res" / "game.ico")
    glyph_icons()


if __name__ == "__main__":
    main()
