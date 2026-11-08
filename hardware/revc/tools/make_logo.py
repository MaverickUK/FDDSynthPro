"""Turns media/FDDSynthPro.png into the board's silkscreen logo: FDDSynthProRevC.pretty/FDDSynthPro_Logo.kicad_mod.

Run with KiCad 10's own Python (it has Pillow), or any Python with Pillow:
    <KiCad python> hardware/revc/tools/make_logo.py [--png preview.png]

The logo is cropped to its ink, scaled to LONG_MM across on a PX_MM grid, and thresholded into
silkscreen (white on the board where the logo is dark). Silkscreen prints nothing under about
0.15 mm, so the art is simplified for it, as make_splash.py simplifies it for the OLED:
- the floppy's shutter, a grey gradient, is filled solid (the threshold is light enough to take it);
- "PRO" is drawn in thin vertical bars: they're closed into solid letters, cleaned of the slivers left
  across their counters, and the fading bars beside it are left out;
- every stroke is thickened by a pixel each side, so the thinnest arc and "Synth" print.
The footprint is board-only (no symbol, not in the BOM or the placement file): layout_pcb.py places it.
"""
import sys
import uuid
from pathlib import Path

from PIL import Image, ImageFilter, ImageOps

HW = Path(__file__).resolve().parent.parent
SRC = HW.parent.parent / "media" / "FDDSynthPro.png"
NAME = "FDDSynthPro_Logo"
OUT = HW / "FDDSynthProRevC.pretty" / f"{NAME}.kicad_mod"
LONG_MM, PX_MM = 11.6, 0.04   # 290 x 211 pixels and a 2-pixel margin: 11.8 x 8.6 mm
THRESHOLD = 240               # luminance at or under this is ink (takes the shutter's gradient)
PRO_THRESHOLD = 150           # in "PRO": its own bars, not the fading ones
PRO_FROM = 0.79               # "PRO" is the rows below this fraction of the height...
PRO_TO_X = 0.55               # ...and left of this fraction of the width
PRO_GAP = 3                   # pixels between its bars, and breaks in them, closed
MARGIN = 2                    # pixels round the crop
PRO_OPEN = 5                  # the opening that cleans up PRO, in pixels


def bitmap(src=SRC):
    """The logo as rows of booleans, True for silkscreen."""
    rgba = Image.open(src).convert("RGBA")
    white = Image.new("RGBA", rgba.size, "white")
    white.alpha_composite(rgba)
    grey = white.convert("L")
    # Cropped to the dark ink: the frame's faint anti-aliased edge outside it would print as a dotted line.
    grey = grey.crop(grey.point(lambda v: 255 if v <= PRO_THRESHOLD else 0).getbbox())
    scale = LONG_MM / PX_MM / grey.width
    grey = grey.resize((round(grey.width * scale), round(grey.height * scale)), Image.LANCZOS)
    w, h = grey.size
    px = grey.load()
    pro_y = int(h * PRO_FROM)
    ink = [[(px[x, y] <= PRO_THRESHOLD and x < w * PRO_TO_X) if y >= pro_y else px[x, y] <= THRESHOLD
            for x in range(w)] for y in range(h)]
    def close(cells):
        """Fills runs of up to PRO_GAP empty cells between ink, along one row or column."""
        last = None
        for i, (y, x) in enumerate(cells):
            if ink[y][x]:
                if last is not None and 0 < i - last - 1 <= PRO_GAP:
                    for k in range(last + 1, i):
                        ink[cells[k][0]][cells[k][1]] = True
                last = i

    for y in range(pro_y, h):  # the gaps between PRO's bars...
        close([(y, x) for x in range(w)])
    for x in range(w):         # ...and the breaks in them, which line up across each letter
        close([(y, x) for y in range(pro_y, h)])
    img = Image.new("L", (w, h), 0)
    img.putdata([255 if v else 0 for row in ink for v in row])
    # PRO: an opening (shrink, then grow back) drops what's thinner than its strokes: slivers across the
    # counters and single pixels under the bars. Its letters' strokes are 9 or more pixels.
    pro = img.crop((0, pro_y, w, h))
    pro = pro.filter(ImageFilter.MinFilter(PRO_OPEN)).filter(ImageFilter.MaxFilter(PRO_OPEN))
    img.paste(pro, (0, pro_y))
    # A margin first, so the strokes on the crop's edge (the floppy's frame) thicken outwards too.
    framed = Image.new("L", (w + 2 * MARGIN, h + 2 * MARGIN), 0)
    framed.paste(img, (MARGIN, MARGIN))
    img = framed.filter(ImageFilter.MaxFilter(3))
    w, h = img.size
    pix = img.load()
    return [[pix[x, y] > 0 for x in range(w)] for y in range(h)]


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


def footprint(ink):
    h, w = len(ink), len(ink[0])
    ox, oy = w * PX_MM / 2, h * PX_MM / 2

    def u(*k):
        return str(uuid.uuid5(uuid.NAMESPACE_URL, "fddsynthpro_revc/logo/" + "/".join(map(str, k))))

    def mm(v):
        return f"{v:.3f}".rstrip("0").rstrip(".")

    font = '(effects (font (size 1 1) (thickness 0.15)))'
    out = [f'(footprint "{NAME}"', '\t(version 20241229)', '\t(generator "fddsynthpro_revc_logo")',
           '\t(generator_version "1.0")', '\t(layer "F.Cu")',
           f'\t(descr "The FDD Synth Pro logo in silkscreen, {mm(w * PX_MM)} x {mm(h * PX_MM)} mm, '
           'from media/FDDSynthPro.png by tools/make_logo.py")',
           '\t(tags "logo")',
           f'\t(property "Reference" "LOGO1" (at 0 {mm(-oy - 1)} 0) (layer "F.SilkS") (hide yes) (uuid "{u("ref")}") {font})',
           f'\t(property "Value" "{NAME}" (at 0 {mm(oy + 1)} 0) (layer "F.Fab") (hide yes) (uuid "{u("val")}") {font})',
           '\t(attr board_only exclude_from_pos_files exclude_from_bom)']
    for i, (x0, y0, x1, y1) in enumerate(rectangles(ink)):
        a, b, c, d = mm(x0 * PX_MM - ox), mm(y0 * PX_MM - oy), mm(x1 * PX_MM - ox), mm(y1 * PX_MM - oy)
        out.append(f'\t(fp_poly (pts (xy {a} {b}) (xy {c} {b}) (xy {c} {d}) (xy {a} {d})) '
                   f'(stroke (width 0) (type solid)) (fill yes) (layer "F.SilkS") (uuid "{u(i)}"))')
    out.append(')')
    return "\n".join(out) + "\n"


def main():
    ink = bitmap()
    OUT.write_text(footprint(ink), encoding="utf8")
    print(f"wrote {OUT.name}: {len(ink[0]) * PX_MM:.1f} x {len(ink) * PX_MM:.1f} mm, {len(rectangles(ink))} rectangles")
    if len(sys.argv) > 2 and sys.argv[1] == "--png":
        img = Image.new("L", (len(ink[0]), len(ink)))
        img.putdata([0 if v else 255 for row in ink for v in row])
        img.resize((img.width * 4, img.height * 4), Image.NEAREST).save(sys.argv[2])


if __name__ == "__main__":
    main()
