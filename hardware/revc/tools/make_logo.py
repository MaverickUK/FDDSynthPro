"""Turns media/FDDSynthDuo.png into the board's silkscreen logo: FDDSynthProRevC.pretty/FDDSynthDuo_Logo.kicad_mod.

Run with KiCad 10's own Python (it has Pillow), or any Python with Pillow:
    <KiCad python> hardware/revc/tools/make_logo.py [--png preview.png]

The logo is cropped to its ink, scaled to LONG_MM across on a PX_MM grid, and thresholded into
silkscreen (white on the board where the logo is dark). Silkscreen prints nothing under about
0.15 mm, so the art is simplified for it, as make_splash.py simplifies it for the OLED:
- the floppy's shutter, a grey gradient, is filled solid (the threshold is light enough to take it);
- "DUO" is drawn in thin vertical bars: they're closed into solid letters, cleaned of the slivers left
  across their counters, and the fading bars beside it are left out (as "PRO" was, in the old logo);
- the floppy's grey shadow, under its outline, is left out, so DUO doesn't merge with the outline;
- every stroke is thickened by a pixel each side, so the thinnest arc and "Synth" print.
It also makes the easter egg on the back, DontCopyThatFloppy.kicad_mod, from media/dont-copy-that-floppy.png:
clean black-on-white art, so only cropped, scaled to SIGN_MM across and thresholded. Its black prints as
silkscreen, so on the board the sign shows in reverse (white, the WARNING band solid).
The footprints are board-only (no symbol, not in the BOM or the placement file): layout_pcb.py places
them, the sign flipped onto the back.
"""
import sys
import uuid
from pathlib import Path

from PIL import Image, ImageFilter, ImageOps

HW = Path(__file__).resolve().parent.parent
SRC = HW.parent.parent / "media" / "FDDSynthDuo.png"
NAME = "FDDSynthDuo_Logo"
OUT = HW / "FDDSynthProRevC.pretty" / f"{NAME}.kicad_mod"
LONG_MM, PX_MM = 11.6, 0.04   # 290 x 211 pixels and a 2-pixel margin: 11.8 x 8.6 mm
THRESHOLD = 240               # luminance at or under this is ink (takes the shutter's gradient)
WORD_THRESHOLD = 150          # in "DUO": its own bars, not the fading ones
WORD_FROM = 0.775             # "DUO" is the rows below this fraction of the height (from just under the
                              # floppy's outline, so its grey shadow, light enough for THRESHOLD, is left out)...
WORD_TO_X = 0.52              # ...and left of this fraction of the width
WORD_GAP = 3                  # pixels between its bars, and breaks in them, closed
MARGIN = 2                    # pixels round the crop
WORD_OPEN = 5                 # the opening that cleans up DUO, in pixels
SIGN_SRC = HW.parent.parent / "media" / "dont-copy-that-floppy.png"
SIGN_NAME = "DontCopyThatFloppy"
SIGN_MM, SIGN_PX_MM = 26.0, 0.025   # its narrowest gaps, between letters, come out about 0.25 mm


def bitmap(src=SRC):
    """The logo as rows of booleans, True for silkscreen."""
    rgba = Image.open(src).convert("RGBA")
    white = Image.new("RGBA", rgba.size, "white")
    white.alpha_composite(rgba)
    grey = white.convert("L")
    # Cropped to the dark ink: the frame's faint anti-aliased edge outside it would print as a dotted line.
    grey = grey.crop(grey.point(lambda v: 255 if v <= WORD_THRESHOLD else 0).getbbox())
    scale = LONG_MM / PX_MM / grey.width
    grey = grey.resize((round(grey.width * scale), round(grey.height * scale)), Image.LANCZOS)
    w, h = grey.size
    px = grey.load()
    word_y = int(h * WORD_FROM)
    ink = [[(px[x, y] <= WORD_THRESHOLD and x < w * WORD_TO_X) if y >= word_y else px[x, y] <= THRESHOLD
            for x in range(w)] for y in range(h)]
    def close(cells):
        """Fills runs of up to WORD_GAP empty cells between ink, along one row or column."""
        last = None
        for i, (y, x) in enumerate(cells):
            if ink[y][x]:
                if last is not None and 0 < i - last - 1 <= WORD_GAP:
                    for k in range(last + 1, i):
                        ink[cells[k][0]][cells[k][1]] = True
                last = i

    for y in range(word_y, h):  # the gaps between DUO's bars...
        close([(y, x) for x in range(w)])
    for x in range(w):         # ...and the breaks in them, which line up across each letter
        close([(y, x) for y in range(word_y, h)])
    img = Image.new("L", (w, h), 0)
    img.putdata([255 if v else 0 for row in ink for v in row])
    # DUO: an opening (shrink, then grow back) drops what's thinner than its strokes: slivers across the
    # counters and single pixels under the bars. Its letters' strokes are 9 or more pixels.
    word = img.crop((0, word_y, w, h))
    word = word.filter(ImageFilter.MinFilter(WORD_OPEN)).filter(ImageFilter.MaxFilter(WORD_OPEN))
    img.paste(word, (0, word_y))
    # A margin first, so the strokes on the crop's edge (the floppy's frame) thicken outwards too.
    framed = Image.new("L", (w + 2 * MARGIN, h + 2 * MARGIN), 0)
    framed.paste(img, (MARGIN, MARGIN))
    img = framed.filter(ImageFilter.MaxFilter(3))
    w, h = img.size
    pix = img.load()
    return [[pix[x, y] > 0 for x in range(w)] for y in range(h)]


def sign_bitmap(src=SIGN_SRC):
    """The sign as rows of booleans, True for silkscreen (the art's black)."""
    rgba = Image.open(src).convert("RGBA")
    white = Image.new("RGBA", rgba.size, "white")
    white.alpha_composite(rgba)
    grey = white.convert("L")
    grey = grey.crop(grey.point(lambda v: 255 if v <= 128 else 0).getbbox())
    w = round(SIGN_MM / SIGN_PX_MM)
    grey = grey.resize((w, round(grey.height * w / grey.width)), Image.LANCZOS)
    px = grey.load()
    return [[px[x, y] <= 128 for x in range(grey.width)] for y in range(grey.height)]


def rectangles(ink):
    """Runs of ink along each row, merged with the same run on the rows below: (x0, y0, x1, y1) in pixels."""
    open_runs, done = {}, []
    for y, row in enumerate(ink + [[False] * len(ink[0])]):
        runs, x = set(), 0
        while x < len(row):
            if row[x]:
                x0 = x
                while x < len(row) and row[x]:
                    x += 1
                runs.add((x0, x))
            x += 1
        for run in list(open_runs):
            if run not in runs:
                done.append((run[0], open_runs.pop(run), run[1], y))
        for run in runs:
            open_runs.setdefault(run, y)
    return done


def footprint(ink, name=NAME, px_mm=PX_MM, what="The FDD Synth Duo logo", src="media/FDDSynthDuo.png", ref="LOGO1"):
    h, w = len(ink), len(ink[0])
    ox, oy = w * px_mm / 2, h * px_mm / 2
    key = "logo" if name == NAME else name  # the logo's uuids as before

    def u(*k):
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"fddsynthpro_revc/{key}/" + "/".join(map(str, k))))

    def mm(v):
        return f"{v:.3f}".rstrip("0").rstrip(".")

    font = '(effects (font (size 1 1) (thickness 0.15)))'
    out = [f'(footprint "{name}"', '\t(version 20241229)', '\t(generator "fddsynthpro_revc_logo")',
           '\t(generator_version "1.0")', '\t(layer "F.Cu")',
           f'\t(descr "{what} in silkscreen, {mm(w * px_mm)} x {mm(h * px_mm)} mm, '
           f'from {src} by tools/make_logo.py")',
           '\t(tags "logo")',
           f'\t(property "Reference" "{ref}" (at 0 {mm(-oy - 1)} 0) (layer "F.SilkS") (hide yes) (uuid "{u("ref")}") {font})',
           f'\t(property "Value" "{name}" (at 0 {mm(oy + 1)} 0) (layer "F.Fab") (hide yes) (uuid "{u("val")}") {font})',
           '\t(attr board_only exclude_from_pos_files exclude_from_bom)']
    for i, (x0, y0, x1, y1) in enumerate(rectangles(ink)):
        a, b, c, d = mm(x0 * px_mm - ox), mm(y0 * px_mm - oy), mm(x1 * px_mm - ox), mm(y1 * px_mm - oy)
        out.append(f'\t(fp_poly (pts (xy {a} {b}) (xy {c} {b}) (xy {c} {d}) (xy {a} {d})) '
                   f'(stroke (width 0) (type solid)) (fill yes) (layer "F.SilkS") (uuid "{u(i)}"))')
    out.append(')')
    return "\n".join(out) + "\n"


def main():
    ink = bitmap()
    OUT.write_text(footprint(ink), encoding="utf8")
    print(f"wrote {OUT.name}: {len(ink[0]) * PX_MM:.1f} x {len(ink) * PX_MM:.1f} mm, {len(rectangles(ink))} rectangles")
    sign = sign_bitmap()
    out = OUT.with_name(f"{SIGN_NAME}.kicad_mod")
    out.write_text(footprint(sign, SIGN_NAME, SIGN_PX_MM, "Don't copy that floppy (the easter egg on the back)",
                             "media/dont-copy-that-floppy.png", "LOGO2"), encoding="utf8")
    print(f"wrote {out.name}: {len(sign[0]) * SIGN_PX_MM:.1f} x {len(sign) * SIGN_PX_MM:.1f} mm, "
          f"{len(rectangles(sign))} rectangles")
    if len(sys.argv) > 2 and sys.argv[1] == "--png":
        img = Image.new("L", (len(ink[0]), len(ink)))
        img.putdata([0 if v else 255 for row in ink for v in row])
        img.resize((img.width * 4, img.height * 4), Image.NEAREST).save(sys.argv[2])


if __name__ == "__main__":
    main()
