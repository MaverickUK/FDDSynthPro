"""JLCPCB's assembly files for the carrier: the BOM and the placement (CPL).

    python3 hardware/make_jlc.py <netlist> <kicad-cli pos .csv> <out dir>

Only parts with an "LCSC Part Number" field, and not marked DNP, are listed
(the BB48R is DNP: it's plugged in by hand; the mounting holes have no part). Columns follow JLCPCB's
templates. Check each part's rotation in JLCPCB's placement preview before
ordering: their footprints don't always share KiCad's zero angle (SOICs
especially), and the preview is where a pin 1 the wrong way round shows.
"""
from __future__ import annotations

import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexp import find, first, parse  # noqa: E402


def lcsc_parts(netlist: Path) -> dict[str, dict]:
    tree = parse(netlist.read_text())
    parts = {}
    for c in find(first(tree, "components"), "comp"):
        fields = {f[1][1]: (f[2] if len(f) > 2 else "") for f in find(first(c, "fields") or [], "field")}
        dnp = any(first(prop, "name")[1] == "dnp" for prop in find(c, "property"))
        if fields.get("LCSC Part Number") and not dnp:
            parts[first(c, "ref")[1]] = {"value": first(c, "value")[1],
                                         "footprint": first(c, "footprint")[1].split(":")[-1],
                                         "lcsc": fields["LCSC Part Number"]}
    return parts


def main(netlist, pos, out):
    parts = lcsc_parts(Path(netlist))
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)

    # One line per part (J4 and J7 are both 1x2 headers, say), its values joined.
    groups: dict[tuple, list[str]] = {}
    for ref, p in parts.items():
        groups.setdefault((p["footprint"], p["lcsc"]), []).append(ref)
    order = lambda r: (r.rstrip("0123456789"), int(r[len(r.rstrip("0123456789")):] or 0))  # noqa: E731
    with open(out / "bom.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (footprint, lcsc), refs in sorted(groups.items(), key=lambda g: order(min(g[1], key=order))):
            refs.sort(key=order)
            values = list(dict.fromkeys(parts[r]["value"] for r in refs))
            w.writerow([" / ".join(values), ",".join(refs), footprint, lcsc])

    with open(pos, newline="") as f, open(out / "positions.csv", "w", newline="") as g:
        w = csv.writer(g)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        placed = set()
        for row in csv.DictReader(f):
            if row["Ref"] in parts:
                side = "Top" if row["Side"].lower().startswith("top") else "Bottom"
                w.writerow([row["Ref"], f'{float(row["PosX"]):.4f}mm', f'{float(row["PosY"]):.4f}mm', side,
                            f'{float(row["Rot"]) % 360:g}'])
                placed.add(row["Ref"])
    missing = set(parts) - placed
    assert not missing, f"no position for {sorted(missing)}"
    print(f"jlc: {len(parts)} parts in {len(groups)} BOM lines -> {out}/bom.csv, positions.csv")


if __name__ == "__main__":
    main(*sys.argv[1:4])
