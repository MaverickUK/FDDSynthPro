"""Adds the parts the schematic has gained to the routed board, or moves parts on it, so only they need routing.

Run with KiCad 10's own Python, after gen_schematic.py:
    <KiCad python> hardware/revc/tools/add_parts.py prepare [board] [--move REF ...] [--rip NET ...]
    FREEROUTING=... JAVA=... <KiCad python> hardware/revc/tools/autoroute.py [board]
    <KiCad python> hardware/revc/tools/add_parts.py finish [board]

`prepare` places each part missing from the board at its position in layout_pcb.PLACE, with its nets,
and moves each --move part to its position there. It sets every pad's net from the schematic, as "Update
PCB from Schematic" does, and takes up the tracks and vias of each net that a new, moved or changed pad
is on (bar the planes'). It locks the routing that's left (Freerouting routes around locked copper and leaves it be), removes
the outer ground pours (Freerouting would take them as copper covering the board; autoroute.py pours
them again). --rip takes up whole nets by name, for one the router left unconnected. `finish` unlocks what `prepare` locked. layout_pcb.py rebuilds the whole board instead;
this keeps the routing that's been checked.
"""
import json
import sys
from pathlib import Path

import pcbnew

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import layout_pcb as L  # noqa: E402


def crosses(track, box):
    """Whether a track or via touches the box. A diagonal track's bounding box covers far more than the
    track, and took up nets (Raspberry Pi's QSPI routing, +5V) that only passed near a new part."""
    half = track.GetWidth(pcbnew.F_Cu) / 2 if track.GetClass() == "PCB_VIA" else track.GetWidth() / 2
    x0, y0 = box.GetLeft() - half, box.GetTop() - half
    x1, y1 = box.GetRight() + half, box.GetBottom() + half
    a, b = track.GetStart(), track.GetEnd()
    # Liang-Barsky: clip the segment a-b to the box grown by half the track's width.
    t0, t1 = 0.0, 1.0
    dx, dy = b.x - a.x, b.y - a.y
    for p, q in ((-dx, a.x - x0), (dx, x1 - a.x), (-dy, a.y - y0), (dy, y1 - a.y)):
        if p == 0:
            if q < 0:
                return False
        else:
            t = q / p
            if p < 0:
                t0 = max(t0, t)
            else:
                t1 = min(t1, t)
            if t0 > t1:
                return False
    return True


def prepare(path, move=(), rip=()):
    comps, nets = L.netlist()
    board = pcbnew.LoadBoard(str(path))
    have = {fp.GetReference() for fp in board.GetFootprints()}
    new = sorted(set(comps) - have)
    pad_net = {key: name for name, nodes in nets.items() for key in nodes}
    for ref in new:
        if ref not in L.PLACE:
            raise KeyError(f"no position for {ref} in layout_pcb.PLACE")
        c = comps[ref]
        fp = L.load_footprint(c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH(c["path"]))
        for name, text in c["fields"].items():
            fp.SetField(name, text)
            fp.GetField(name).SetVisible(False)
        fp.SetExcludedFromBOM(not c["in_bom"])
        fp.SetDNP(c["dnp"])
        x, y, r = L.PLACE[ref]
        fp.SetPosition(L.vec(x, y))
        fp.SetOrientationDegrees(r)
        fp.Reference().SetVisible(ref not in L.REF_HIDDEN)
        board.Add(fp)
        for pad in fp.Pads():
            name = pad_net.get((ref, pad.GetNumber()))
            if not name:
                continue
            item = board.FindNet(name)
            if item is None:
                item = pcbnew.NETINFO_ITEM(board, name)
                board.Add(item)
            pad.SetNet(item)
    ripped = set()
    for fp in board.GetFootprints():  # pads whose net the schematic changed
        for pad in fp.Pads():
            name = pad_net.get((fp.GetReference(), pad.GetNumber()))
            if name is None or not pad.GetNumber() or name == pad.GetNetname():
                continue
            ripped |= {pad.GetNetname(), name}
            item = board.FindNet(name)
            if item is None:
                item = pcbnew.NETINFO_ITEM(board, name)
                board.Add(item)
            pad.SetNet(item)
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in new:
            ripped |= {pad.GetNetname() for pad in fp.Pads()}
        if ref in move:
            x, y, r = L.PLACE[ref]
            fp.SetPosition(L.vec(x, y))
            fp.SetOrientationDegrees(r)
            ripped |= {pad.GetNetname() for pad in fp.Pads()}
    ripped -= {"", "GND", "+3V3"}
    # So do the nets of tracks under a new or moved part (its courtyard, with a margin), whole: cutting
    # just the pieces underneath left locked ends the router didn't rejoin.
    margin = pcbnew.FromMM(0.3)
    areas = []
    for fp in board.GetFootprints():
        if fp.GetReference() in set(new) | set(move):
            box = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
            box.Inflate(margin)
            areas.append(box)
    ripped |= {t.GetNetname() for t in board.GetTracks()
               if not t.IsLocked() and any(crosses(t, box) for box in areas)}
    ripped -= {"", "GND", "+3V3"}
    ripped |= set(rip)
    rip = [t for t in board.GetTracks() if t.GetNetname() in ripped and not t.IsLocked()]
    locked = []
    for t in board.GetTracks():
        if not t.IsLocked() and t.GetNetname() not in ripped:
            t.SetLocked(True)
            locked.append(t.m_Uuid.AsString())
    Path(str(path) + ".locked.json").write_text(json.dumps(locked), encoding="utf8")
    # Removing items last: KiCad 10's bindings misbehave after a Remove, but saving still works.
    for item in rip + [z for z in board.Zones() if z.GetZoneName() == "GND pour"]:
        board.Remove(item)
    board.Save(str(path))
    print(f"added {', '.join(new) or 'nothing'}; moved {', '.join(move) or 'nothing'}, "
          f"taking up {len(rip)} pieces: {', '.join(sorted(ripped)) or 'no net'}; locked {len(locked)} tracks and vias")


def finish(path):
    listing = Path(str(path) + ".locked.json")
    locked = set(json.loads(listing.read_text(encoding="utf8")))
    board = pcbnew.LoadBoard(str(path))
    n = 0
    for t in board.GetTracks():
        if t.m_Uuid.AsString() in locked:
            t.SetLocked(False)
            n += 1
    board.Save(str(path))
    listing.unlink()
    print(f"unlocked {n} of {len(locked)}")


if __name__ == "__main__":
    args = sys.argv[1:]
    opts = {"--move": [], "--rip": []}
    for flag in opts:  # each takes the words after it, up to the next flag
        if flag in args:
            i = args.index(flag)
            j = next((k for k in range(i + 1, len(args)) if args[k] in opts), len(args))
            opts[flag], args = args[i + 1:j], args[:i] + args[j:]
    board_path = Path(args[1]) if len(args) > 1 else L.OUT
    if args[0] == "prepare":
        prepare(board_path, opts["--move"], opts["--rip"])
    else:
        finish(board_path)
