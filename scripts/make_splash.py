#!/usr/bin/env python3
"""Turns media/FDDSynthPro.png into src/splash_logo.h: the logo the OLED
shows for a second at power-on (ui::Screen::Splash), 128 x 64, one bit a
pixel.

    scripts/make_splash.py            # writes src/splash_logo.h
    scripts/make_splash.py --png out.png   # and a preview

The logo is cropped to its ink, scaled to fit the screen (Lanczos), and
thresholded: its dark strokes, and the floppy's shutter, become lit pixels
on the OLED's black. Dithering lost the detail at this size. Needs Pillow.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageOps

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "media" / "FDDSynthPro.png"
OUT = REPO / "src" / "splash_logo.h"
W, H = 128, 64
THRESHOLD = 150  # luminance at or under this is ink


def bitmap(src: Path = SRC) -> Image.Image:
    rgba = Image.open(src).convert("RGBA")
    white = Image.new("RGBA", rgba.size, "white")
    white.alpha_composite(rgba)
    grey = white.convert("L")
    grey = grey.crop(ImageOps.invert(grey).getbbox())
    scale = min(W / grey.width, H / grey.height)
    grey = grey.resize((max(1, round(grey.width * scale)), max(1, round(grey.height * scale))), Image.LANCZOS)
    ink = grey.point(lambda v: 255 if v <= THRESHOLD else 0).convert("1")  # lit where the logo is dark
    screen = Image.new("1", (W, H), 0)
    screen.paste(ink, ((W - ink.width) // 2, (H - ink.height) // 2))
    return screen


def header(screen: Image.Image) -> str:
    rows = []
    for y in range(H):
        row = []
        for x0 in range(0, W, 8):
            byte = 0
            for b in range(8):
                byte = byte << 1 | (1 if screen.getpixel((x0 + b, y)) else 0)
            row.append(f"0x{byte:02X}")
        rows.append("    {" + ", ".join(row) + "},")
    return (
        "// The logo the OLED shows at power-on: 128 x 64, a row of 16 bytes per\n"
        "// line, the leftmost pixel in the top bit, 1 = lit. Generated from\n"
        "// media/FDDSynthPro.png by scripts/make_splash.py: run that, don't edit this.\n"
        "#pragma once\n\n#include <cstdint>\n\nnamespace splash {\n\n"
        f"constexpr int WIDTH = {W}, HEIGHT = {H};\n"
        f"constexpr uint8_t LOGO[{H}][{W // 8}] = {{\n" + "\n".join(rows) + "\n};\n\n}  // namespace splash\n"
    )


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--png", type=Path, help="also write a preview, 4x")
    args = p.parse_args()
    screen = bitmap()
    OUT.write_text(header(screen))
    print(f"wrote {OUT.relative_to(REPO)}")
    if args.png:
        screen.convert("L").resize((W * 4, H * 4), Image.NEAREST).save(args.png)
        print(f"wrote {args.png}")


if __name__ == "__main__":
    main()
