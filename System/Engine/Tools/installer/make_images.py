"""The installer's pictures (Inno Setup wants BMPs): the side panel of the first and last page and the small corner
logo, in 100 % and 200 % sizes, drawn from launcher/assets/launcher.png (our own artwork).  Needs Pillow."""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[3]   # System/
LOGO = ROOT / "launcher" / "assets" / "launcher.png"
NAVY_TOP, NAVY_BOTTOM, GOLD = (43, 68, 112), (16, 24, 44), (192, 145, 58)


def side(w: int, h: int) -> Image.Image:
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(NAVY_TOP, NAVY_BOTTOM)))
    logo = Image.open(LOGO).convert("RGBA")
    s = int(w * 0.72)
    logo = logo.resize((s, s), Image.LANCZOS)
    img.paste(logo, ((w - s) // 2, int(h * 0.16)), logo)
    d.line([(int(w * 0.2), int(h * 0.62)), (int(w * 0.8), int(h * 0.62))], fill=GOLD, width=max(1, w // 80))
    return img


def small(n: int) -> Image.Image:
    img = Image.new("RGB", (n, n), (255, 255, 255))
    logo = Image.open(LOGO).convert("RGBA").resize((n, n), Image.LANCZOS)
    img.paste(logo, (0, 0), logo)
    return img


def main(out: Path) -> int:
    out.mkdir(parents=True, exist_ok=True)
    for scale in (1, 2):
        side(164 * scale, 314 * scale).save(out / f"wizard_large_{scale}x.bmp")
        small(55 * scale).save(out / f"wizard_small_{scale}x.bmp")
    print(f"wrote installer images to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent / "Developer" / "Release" / "build"))
