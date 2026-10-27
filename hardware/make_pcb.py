"""Lays out and routes the carrier board from the schematic's netlist.

    kicad-cli sch export netlist --format kicadsexpr -o /tmp/carrier.net hardware/fddsynthpro_carrier.kicad_sch
    <KiCad's python3> hardware/make_pcb.py /tmp/carrier.net

KiCad's own Python is needed for pcbnew (on macOS:
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3).
Writes hardware/fddsynthpro_carrier.kicad_pcb; check it with
`kicad-cli pcb drc --schematic-parity`.

The board has the HDD Synth MK5's outline, mounting holes and speaker grille
(mk5_outline.py), so it fits the same case. The BB48R stands upright and
turned round, flush with the top and bottom edges (USB-C at the bottom, the
microSD slot at the top), so its EXT1 row, with every floppy signal, faces
the floppy header (J1) on the right; the pull-ups and the SN7407 sit
between them. The headers for the panel and the amp form a column on the
left, clear of the speaker (a 40 mm square, 20 mm deep, on the MK5's holes). Routing is by a small maze router on a 0.635 mm grid (a quarter
of 2.54 mm, so every header pin sits on it), two layers, with vias; ground
is routed like any net and then poured on both layers.
"""
from __future__ import annotations

import heapq
import math
import struct
import sys
import zlib
from pathlib import Path

import pcbnew

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import mk5_outline  # noqa: E402
from sexp import find, first, parse  # noqa: E402

FOOTPRINTS = Path("/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
LOCAL = {"FDDSynthPro": HERE / "FDDSynthPro.pretty"}

# The board: mm, origin at its top-left corner; placed on the sheet at ORIGIN.
W, H = 100.33, 69.914  # the HDD Synth MK5's board
ORIGIN = (100.0, 50.0)

# Design rules (JLCPCB's standard two-layer service is looser than these).
CLEARANCE = 0.2
SIGNAL_W = 0.3
POWER_W = 0.4
VIA_D, VIA_DRILL = 0.6, 0.3
POWER_NETS = {"+3V3", "VBUS", "GND"}

GRID = 0.635
# Everything is placed relative to the BB48R's EXT1 pin 1, so every 2.54 mm
# part lands on the routing grid. The BB48R is turned round: pin 1 is at the
# bottom (its USB-C end, flush with the board's edge) and EXT1 is its right
# row, EXT2 (15.24 mm to the left) its left.
BX, BY = 74.0, 68.5
ROW = 2.54


def row(n):
    """y of BB48R EXT1/EXT2 pin n (pin 1 at the bottom)."""
    return BY - (n - 1) * ROW


def grid_x(mm_from_bx):
    """An x on the routing grid, about mm_from_bx from EXT1."""
    return BX + round(mm_from_bx / GRID) * GRID


# ref -> (x, y, rotation in degrees). Rotation is KiCad's: counter-clockwise.
PLACEMENT = {
    "U1": (BX, BY, 180),
    # Turned round too, so its signal pins (the even row) face the BB48R and
    # its rows run upwards in the same order as GP12-21. Between the
    # right-hand mounting holes, its key towards the board's edge.
    "J1": (grid_x(20.95), BY - 21 * GRID, 180),
    "U2": (82.0, row(10), 0),                # SN7407 beside GP4-7, its inputs
    "C1": (82.0, row(10) + 7.6, 90),         # beside U2's pin 14
}
# The pull-ups, each on the row of the GPIO it serves (GP12-21: EXT1 rows
# 16-25), upright, with pin 1 on the row: the input runs straight along the
# row through both, and each pin 2 (just above the row) drops through a via
# to its bus on the underside. The 10k (R1-R10) nearer the BB48R, then the
# 1k (R11-R20). build() nudges each so its pin 1 sits exactly on the row.
UPRIGHT = {f"R{k}" for k in range(1, 21)}
for k in range(10):
    PLACEMENT[f"R{k + 1}"] = (grid_x(5.08), row(16 + k), 90)
    PLACEMENT[f"R{k + 11}"] = (grid_x(8.89), row(16 + k), 90)
# The headers: one column on the left, between the speaker and the BB48R,
# top to bottom, each group 1.27 mm below the last. The amp's at the bottom,
# beside GP26-28; the encoder and button above, beside GP32-35.
HEADER_COLUMN_X = grid_x(-28.575)
HEADER_ORDER = ["J7", "J2", "J6", "J4", "J3", "J5"]
_y = BY - 103 * GRID  # the first pin, about 3 mm from the top edge
for ref, pins in zip(HEADER_ORDER, (2, 4, 3, 2, 5, 7)):
    PLACEMENT[ref] = (HEADER_COLUMN_X, _y, 0)
    _y += (pins - 1) * ROW + ROW + 2 * GRID
for k, (x, y) in enumerate(mk5_outline.MOUNTING_HOLES, 1):
    PLACEMENT[f"H{k}"] = (x, y, 0)
HOLES = mk5_outline.MOUNTING_HOLES
# No parts or copper over the speaker: its grille and mounting holes, plus a margin.
SPEAKER = (0.0, 20.3, 41.0, 59.95)  # x0, y0, x1, y1

# Silkscreen: the pin names beside each header, and notes.
HEADER_PINS = {
    "J2": ["GND", "3V3", "SCL", "SDA"],
    "J3": ["CLK", "DT", "SW", "+", "GND"],
    "J4": ["BTN", "GND"],
    "J5": ["LRC", "BCLK", "DIN", "GAIN", "SD", "GND", "VIN"],
    "J6": ["TX", "RX", "GND"],
    "J7": ["1k", "3V3"],
}
HEADER_TITLES = {"J2": "OLED", "J3": "ENCODER", "J4": "ACTION", "J5": "AMP", "J6": "UART", "J7": "TERM"}


def mm(v):
    return pcbnew.FromMM(v)


def vec(x, y):
    return pcbnew.VECTOR2I(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y))


def to_board(p):
    return pcbnew.ToMM(p.x) - ORIGIN[0], pcbnew.ToMM(p.y) - ORIGIN[1]


# ---- The netlist ----------------------------------------------------------------

def read_netlist(path):
    tree = parse(Path(path).read_text())
    comps = {}
    for c in find(first(tree, "components"), "comp"):
        ref = first(c, "ref")[1]
        stamps = first(c, "tstamps")
        fields = {f[1][1]: (f[2] if len(f) > 2 else "") for f in find(first(c, "fields") or [], "field")}
        dnp = any(first(prop, "name")[1] == "dnp" for prop in find(c, "property"))
        comps[ref] = {"value": first(c, "value")[1], "footprint": first(c, "footprint")[1],
                      "uuid": stamps[1] if stamps else "", "fields": fields, "dnp": dnp}
    nets = {}
    for n in find(first(tree, "nets"), "net"):
        nets[first(n, "name")[1]] = [(first(x, "ref")[1], first(x, "pin")[1]) for x in find(n, "node")]
    return comps, nets


def load_footprint(lib_id):
    lib, name = lib_id.split(":")
    path = LOCAL.get(lib, FOOTPRINTS / f"{lib}.pretty")
    fp = pcbnew.FootprintLoad(str(path), name)
    assert fp, f"no footprint {lib_id}"
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


# ---- The router -------------------------------------------------------------------

class Grid:
    """Two layers of cells on GRID. owner: 0 free, -1 blocked, else a net
    number. Pads also reserve a margin of cells around them for their own net."""

    def __init__(self):
        self.nx = int(W / GRID) + 1
        self.ny = int(H / GRID) + 1
        self.x0 = BX % GRID   # the grid passes through EXT1 pin 1
        self.y0 = BY % GRID
        self.owner = [[[0] * self.nx for _ in range(self.ny)] for _ in range(2)]
        # Cells close enough to another net's pad that only a signal-width
        # track fits (between two header pins, for instance): the pad's net.
        self.narrow = [[[0] * self.nx for _ in range(self.ny)] for _ in range(2)]
        self.via_ok = [[True] * self.nx for _ in range(self.ny)]

    def xy(self, i, j):
        return self.x0 + i * GRID, self.y0 + j * GRID

    def cell(self, x, y):
        return round((x - self.x0) / GRID), round((y - self.y0) / GRID)

    def inside(self, i, j):
        return 0 <= i < self.nx and 0 <= j < self.ny

    def block_edges(self, margin):
        for j in range(self.ny):
            for i in range(self.nx):
                x, y = self.xy(i, j)
                if x < margin or y < margin or x > W - margin or y > H - margin:
                    for L in (0, 1):
                        self.owner[L][j][i] = -1

    def claim_region(self, layers, test, net, radius_cells):
        """Cells (on `layers`) where test(x, y) holds go to `net`; a cell
        claimed by two nets is blocked."""
        cx0, cy0, cx1, cy1 = radius_cells
        for j in range(max(0, cy0), min(self.ny, cy1 + 1)):
            for i in range(max(0, cx0), min(self.nx, cx1 + 1)):
                x, y = self.xy(i, j)
                if test(x, y):
                    for L in layers:
                        o = self.owner[L][j][i]
                        if o == 0:
                            self.owner[L][j][i] = net
                        elif o != net:
                            self.owner[L][j][i] = -1


class Pad:
    def __init__(self, ref, number, net, x0, y0, x1, y1, round_, layers):
        self.ref, self.number, self.net = ref, number, net
        self.x0, self.y0, self.x1, self.y1 = x0, y0, x1, y1
        self.round = round_
        self.layers = layers
        self.cx, self.cy = (x0 + x1) / 2, (y0 + y1) / 2
        self.r = (x1 - x0) / 2
        self.stub_cells = set()  # for a pad no grid cell falls inside: the cells beside it

    def distance(self, x, y):
        """From (x, y) to the pad's copper (0 inside)."""
        if self.round:
            return max(0.0, math.hypot(x - self.cx, y - self.cy) - self.r)
        dx = max(self.x0 - x, 0, x - self.x1)
        dy = max(self.y0 - y, 0, y - self.y1)
        return math.hypot(dx, dy)


WIDE: set = set()  # nets routed at POWER_W
DEBUG = bool(__import__("os").environ.get("ROUTER_DEBUG"))
DIRS = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]


class Router:
    """Routes nets one at a time on a Grid, each as a tree: from the cells
    already in it, the cheapest path to its next pad. When a net can't get
    through, a second search may cross other nets' tracks at a high cost;
    the nets it crossed are ripped up and queued again (rip-up and reroute),
    and the cells fought over cost more from then on, so the next attempts
    spread out. The grid holds what never moves (pads, edges, keep-outs);
    tracks live here, so they can be taken up again."""

    VIA_COST, BEND_COST, CROSS_COST = 6.0, 0.6, 25.0

    def __init__(self, grid, pads_by_net):
        self.g = grid
        self.pads = pads_by_net
        layer = lambda v: [[v] * grid.nx for _ in range(grid.ny)]  # noqa: E731
        self.track = [layer(0), layer(0)]
        self.history = [layer(0.0), layer(0.0)]
        self.claims = {}   # net -> cells its tracks took
        self.paths = {}    # net -> its paths
        self.targets = {n: self._targets(ps) for n, ps in pads_by_net.items()}

    def _targets(self, pads):
        """Each pad's own cells: those inside its copper, or its stub cell."""
        out = []
        for p in pads:
            cells = set()
            i0, j0 = self.g.cell(p.x0, p.y0)
            i1, j1 = self.g.cell(p.x1, p.y1)
            for j in range(j0 - 1, j1 + 2):
                for i in range(i0 - 1, i1 + 2):
                    if self.g.inside(i, j) and p.distance(*self.g.xy(i, j)) <= 1e-6:
                        cells.update((L, i, j) for L in p.layers)
            cells = cells or p.stub_cells
            assert cells, f"{p.ref}.{p.number}: no grid cell for the pad"
            out.append(cells)
        return out

    # -- What a cell costs a net: None if it can't go there at all.
    def cost(self, net, L, i, j, soft):
        g = self.g
        if not g.inside(i, j):
            return None
        o = g.owner[L][j][i]
        if o != 0 and o != net:
            return None
        if net in WIDE:
            n = g.narrow[L][j][i]
            if n != 0 and n != net:
                return None
        t = self.track[L][j][i]
        if t != 0 and t != net:
            return self.CROSS_COST + 4.0 * self.history[L][j][i] if soft else None
        return 0.3 * self.history[L][j][i]

    def via_cost(self, net, i, j, soft):
        if not self.g.via_ok[j][i]:
            return None
        total = 0.0
        for L in (0, 1):
            for dj in (-1, 0, 1):
                for di in (-1, 0, 1):
                    c = self.cost(net, L, i + di, j + dj, soft)
                    if c is None:
                        return None
                    total += c
        return total

    def astar(self, net, sources, goal, soft):
        """The cheapest path from any source cell to any goal cell. Searched
        first within the two's bounding box plus a margin (most paths stay
        near), then over the whole board if that finds nothing."""
        cells = list(sources) + list(goal)
        lo_i = min(c[1] for c in cells)
        hi_i = max(c[1] for c in cells)
        lo_j = min(c[2] for c in cells)
        hi_j = max(c[2] for c in cells)
        m = 25  # cells: about 16 mm
        box = (lo_i - m, lo_j - m, hi_i + m, hi_j + m)
        return (self._astar(net, sources, goal, soft, box)
                or self._astar(net, sources, goal, soft, (0, 0, self.g.nx, self.g.ny)))

    def _astar(self, net, sources, goal, soft, box):
        bi0, bj0, bi1, bj1 = box
        gi = [c[1] for c in goal]
        gj = [c[2] for c in goal]
        tx, ty = sum(gi) / len(gi), sum(gj) / len(gj)

        def h(i, j):
            dx, dy = abs(i - tx), abs(j - ty)
            return (max(dx, dy) - min(dx, dy)) + 1.414 * min(dx, dy)

        # The state is the cell alone; the bend penalty uses the direction
        # the cell was last reached from (close enough, and 9 times fewer states).
        openq, best, parent, came = [], {}, {}, {}
        for s in sources:
            best[s] = 0.0
            came[s] = -1
            heapq.heappush(openq, (h(s[1], s[2]), 0.0, s))
        while openq:
            f, g, s = heapq.heappop(openq)
            if best.get(s, 1e18) < g:
                continue
            if s in goal:
                path = [s]
                while s in parent:
                    s = parent[s]
                    path.append(s)
                return path[::-1]
            L, i, j = s
            d = came[s]
            for k, (di, dj) in enumerate(DIRS):
                ni, nj = i + di, j + dj
                if ni < bi0 or nj < bj0 or ni > bi1 or nj > bj1:
                    continue
                c = self.cost(net, L, ni, nj, soft)
                if c is None:
                    continue
                if di and dj:  # a diagonal passes close to the two cells beside it
                    a = self.cost(net, L, i + di, j, soft)
                    b = self.cost(net, L, i, j + dj, soft)
                    if a is None or b is None:
                        continue
                    c += a + b
                step = (1.414 if di and dj else 1.0) + (self.BEND_COST if d not in (-1, k) else 0.0)
                if (L == 1 and dj and not di) or (L == 0 and di and not dj):
                    step += 0.15  # a mild preference: B.Cu runs across, F.Cu runs down
                t = (L, ni, nj)
                cost = g + step + c
                if cost < best.get(t, 1e18):
                    best[t] = cost
                    parent[t] = s
                    came[t] = k
                    heapq.heappush(openq, (cost + h(ni, nj), cost, t))
            v = self.via_cost(net, i, j, soft)
            if v is not None:
                t = (1 - L, i, j)
                cost = g + self.VIA_COST + v
                if cost < best.get(t, 1e18):
                    best[t] = cost
                    parent[t] = s
                    came[t] = -1
                    heapq.heappush(openq, (cost + h(i, j), cost, t))
        return None

    def route_net(self, net, soft=False):
        """The net's paths, or None. Soft: may cross other nets' tracks."""
        targets = self.targets[net]
        tree = set(targets[0])
        remaining = list(targets[1:])
        paths = []
        while remaining:
            sample = list(tree)[:200]
            remaining.sort(key=lambda cells: min(abs(a[1] - b[1]) + abs(a[2] - b[2]) for a in cells for b in sample))
            goal = remaining.pop(0)
            if goal & tree:
                continue
            path = self.astar(net, tree, goal, soft)
            if path is None:
                return None
            paths.append(path)
            tree.update(path)
        return paths

    def footprint(self, path):
        """Every cell a path takes: its own, the two beside each diagonal
        step, and the neighbours of each via (a via is wider than a track)."""
        cells = set(path)
        for k in range(1, len(path)):
            (L0, i0, j0), (L, i, j) = path[k - 1], path[k]
            if L0 == L and i0 != i and j0 != j:
                cells.update(((L, i0, j), (L, i, j0)))
            if L0 != L:
                cells.update((LL, i + di, j + dj) for LL in (0, 1) for di in (-1, 0, 1) for dj in (-1, 0, 1))
        return {c for c in cells if self.g.inside(c[1], c[2])}

    def commit(self, net, paths):
        claimed = []
        for path in paths:
            for (L, i, j) in self.footprint(path):
                if self.track[L][j][i] == 0:
                    self.track[L][j][i] = net
                    claimed.append((L, i, j))
        self.claims[net] = claimed
        self.paths[net] = paths

    def ripup(self, net):
        for (L, i, j) in self.claims.pop(net, []):
            if self.track[L][j][i] == net:
                self.track[L][j][i] = 0
        self.paths.pop(net, None)

    def blockers(self, net, paths):
        found = set()
        for path in paths:
            for (L, i, j) in self.footprint(path):
                t = self.track[L][j][i]
                if t and t != net:
                    found.add(t)
                    self.history[L][j][i] += 1.0
        return found

    def run(self, order, names, max_steps=600):
        queue = list(order)
        steps = 0
        while queue and steps < max_steps:
            steps += 1
            net = queue.pop(0)
            paths = self.route_net(net)
            if paths is not None:
                self.commit(net, paths)
                continue
            if net in WIDE:
                # A power net that can't fit at POWER_W (it must pass between
                # the BB48R's pins to reach both sides) goes at SIGNAL_W:
                # 0.3 mm still carries an amp.
                WIDE.discard(net)
                print(f"  {names[net]} at {SIGNAL_W} mm")
                queue.insert(0, net)
                continue
            soft = self.route_net(net, soft=True)
            if soft is None:
                print(f"  {names[net]}: no way through at all")
                continue
            ripped = self.blockers(net, soft)
            if DEBUG:
                print(f"  {names[net]} rips up {[names[b] for b in ripped]}")
            for b in ripped:
                self.ripup(b)
                queue.append(b)
            queue.insert(0, net)
        unrouted = [n for n in order if n not in self.paths]
        print(f"  router: {steps} steps, {len(self.paths)}/{len(order)} nets routed")
        return unrouted


# ---- The board ----------------------------------------------------------------------

def build(netlist_path):
    comps, nets = read_netlist(netlist_path)
    board = pcbnew.NewBoard(str(HERE / "fddsynthpro_carrier.kicad_pcb"))

    ds = board.GetDesignSettings()
    nc = ds.m_NetSettings.GetDefaultNetclass()
    nc.SetClearance(mm(CLEARANCE))
    nc.SetTrackWidth(mm(SIGNAL_W))
    nc.SetViaDiameter(mm(VIA_D))
    nc.SetViaDrill(mm(VIA_DRILL))
    ds.m_TrackMinWidth = mm(0.2)
    ds.m_ViasMinSize = mm(0.5)
    ds.m_MinThroughDrill = mm(0.3)
    ds.m_CopperEdgeClearance = mm(0.4)

    netinfo = {}
    for i, name in enumerate(sorted(nets), 1):
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        netinfo[name] = n
    net_of = {node: name for name, nodes in nets.items() for node in nodes}
    net_num = {name: k for k, name in enumerate(sorted(nets), 1)}

    # Footprints.
    footprints = {}
    for ref, c in sorted(comps.items()):
        fp = load_footprint(c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH("/" + c["uuid"]))
        fp.SetDNP(c["dnp"])  # U1, the BB48R: plugged in by hand
        for name, value in c["fields"].items():  # LCSC and the like, as the schematic has them
            if name not in ("Footprint", "Datasheet", "Description") and value:
                fp.SetField(name, value)
                fp.GetField(name).SetVisible(False)
        x, y, rot = PLACEMENT[ref]
        fp.SetPosition(vec(x, y))
        fp.SetOrientationDegrees(rot)
        if ref in UPRIGHT:
            # Pin 1 on (x, y), pin 2 above it.
            pads = {pd.GetNumber(): to_board(pd.GetPosition()) for pd in fp.Pads()}
            if pads["2"][1] > pads["1"][1]:
                fp.SetOrientationDegrees(rot + 180)
                pads = {pd.GetNumber(): to_board(pd.GetPosition()) for pd in fp.Pads()}
            fp.SetPosition(vec(x + (x - pads["1"][0]), y + (y - pads["1"][1])))
        board.Add(fp)
        for pad in fp.Pads():
            name = net_of.get((ref, pad.GetNumber()))
            if name:
                pad.SetNet(netinfo[name])
        footprints[ref] = fp
    for k in range(1, 5):
        footprints[f"H{k}"].Reference().SetVisible(False)

    outline(board)
    silkscreen(board, footprints)
    logo(board, LOGO, LOGO_AT, LOGO_WIDTH)

    # Route.
    pads_by_net = {}
    for ref, fp in footprints.items():
        for pad in fp.Pads():
            name = net_of.get((ref, pad.GetNumber()))
            bb = pad.GetBoundingBox()
            x0, y0 = to_board(pcbnew.VECTOR2I(bb.GetLeft(), bb.GetTop()))
            x1, y1 = to_board(pcbnew.VECTOR2I(bb.GetRight(), bb.GetBottom()))
            layers = (0, 1) if pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else (0,)
            p = Pad(ref, pad.GetNumber(), net_num.get(name, -1), x0, y0, x1, y1,
                    pad.GetShape(pcbnew.F_Cu) == pcbnew.PAD_SHAPE_CIRCLE, layers)
            if name:
                pads_by_net.setdefault(net_num[name], []).append(p)
            else:
                ORPHANS.append(p)
    grid = fresh_grid(footprints, net_of, net_num, pads_by_net)

    names = {v: k for k, v in net_num.items()}
    multi = [n for n, ps in pads_by_net.items() if len(ps) > 1]
    # Power first (wide and many-legged), then the rest shortest first, and
    # ground last (the pours back it up).
    def span(n):
        ps = pads_by_net[n]
        return max(p.cx for p in ps) - min(p.cx for p in ps) + max(p.cy for p in ps) - min(p.cy for p in ps)
    order = sorted(multi, key=lambda n: (names[n] not in POWER_NETS, names[n] == "GND", span(n)))
    gnd = net_num["GND"]
    order = [n for n in order if n != gnd] + [gnd]
    WIDE.update(n for n in multi if names[n] in POWER_NETS)
    router = Router(grid, pads_by_net)
    failed = router.run(order, names)
    if failed:
        print("UNROUTED:", [names[n] for n in failed])

    for n, plist in router.paths.items():
        width = POWER_W if n in WIDE else SIGNAL_W
        for path in plist:
            emit(board, grid, netinfo[names[n]], path, width)

    pours(board, netinfo["GND"])
    board.BuildConnectivity()
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(HERE / "fddsynthpro_carrier.kicad_pcb"))
    return failed


def fresh_grid(footprints, net_of, net_num, pads_by_net):
    """The empty grid: board edge, mounting holes, and every pad reserving
    its margin for its own net (other nets keep a power track's half-width
    plus the clearance away)."""
    grid = Grid()
    grid.block_edges(0.6)
    sx0, sy0, sx1, sy1 = SPEAKER
    i0, j0 = grid.cell(sx0, sy0)
    i1, j1 = grid.cell(sx1, sy1)
    grid.claim_region((0, 1), lambda x, y: sx0 <= x <= sx1 and sy0 <= y <= sy1, -1, (i0, j0, i1, j1))
    for (cx, cy) in ((0, 0), (W, 0), (0, H), (W, H)):  # the rounded corners
        i0, j0 = grid.cell(cx - 3.2, cy - 3.2)
        i1, j1 = grid.cell(cx + 3.2, cy + 3.2)
        grid.claim_region((0, 1), lambda x, y, cx=cx, cy=cy: abs(x - cx) < 3.2 and abs(y - cy) < 3.2, -1,
                          (i0, j0, i1, j1))
    # The mounting holes are plated pads on GND (as on the HDD Synth): their
    # pads keep other nets clear like any other, and the screw heads sit on ground.
    for ps in list(pads_by_net.values()) + [ORPHANS]:
        for p in ps:
            # Within a signal track's reach of the pad: its net only. A
            # little further, out to a power track's reach: signal tracks
            # of other nets may pass, power tracks may not.
            narrow_m = SIGNAL_W / 2 + CLEARANCE
            margin = POWER_W / 2 + CLEARANCE
            i0, j0 = grid.cell(p.x0 - margin - GRID, p.y0 - margin - GRID)
            i1, j1 = grid.cell(p.x1 + margin + GRID, p.y1 + margin + GRID)
            grid.claim_region(p.layers, lambda x, y, p=p, m=narrow_m: p.distance(x, y) < m,
                              p.net if p.net > 0 else -1, (i0, j0, i1, j1))
            for j in range(max(0, j0), min(grid.ny, j1 + 1)):
                for i in range(max(0, i0), min(grid.nx, i1 + 1)):
                    x, y = grid.xy(i, j)
                    if p.distance(x, y) < margin + 0.01:
                        for L in p.layers:
                            if grid.narrow[L][j][i] == 0:
                                grid.narrow[L][j][i] = p.net if p.net > 0 else -1
            for j in range(max(0, j0), min(grid.ny, j1 + 1)):
                for i in range(max(0, i0), min(grid.nx, i1 + 1)):
                    x, y = grid.xy(i, j)
                    if p.distance(x, y) < VIA_D / 2 + CLEARANCE + 0.05:
                        grid.via_ok[j][i] = False
    # A pad's own copper is always its net's, even where a close neighbour's
    # margin reaches over it (an 0402's pads are 0.5 mm apart). A pad too small
    # or too far off the grid to hold a cell gets its nearest cell instead,
    # and emit() joins that cell to the pad with a short stub.
    STUBS.clear()
    for ps in pads_by_net.values():
        for p in ps:
            ci, cj = grid.cell(p.cx, p.cy)
            inside = any(p.distance(*grid.xy(i, j)) <= 0.0
                         for j in range(cj - 1, cj + 2) for i in range(ci - 1, ci + 2))
            if not inside:
                best = min(((i, j) for j in range(cj - 1, cj + 2) for i in range(ci - 1, ci + 2)),
                           key=lambda c: math.hypot(grid.xy(*c)[0] - p.cx, grid.xy(*c)[1] - p.cy))
                p.stub_cells = {(L, best[0], best[1]) for L in p.layers}
                for L in p.layers:
                    grid.owner[L][best[1]][best[0]] = p.net
                    grid.narrow[L][best[1]][best[0]] = p.net
                    STUBS[(L, best[0], best[1])] = p
            i0, j0 = grid.cell(p.x0 - GRID, p.y0 - GRID)
            i1, j1 = grid.cell(p.x1 + GRID, p.y1 + GRID)
            for j in range(max(0, j0), min(grid.ny, j1 + 1)):
                for i in range(max(0, i0), min(grid.nx, i1 + 1)):
                    x, y = grid.xy(i, j)
                    if p.distance(x, y) <= 0.0:
                        for L in p.layers:
                            grid.owner[L][j][i] = p.net
                            grid.narrow[L][j][i] = p.net
    return grid


ORPHANS: list = []  # pads with no net (none on this board, but kept as obstacles if there were)
STUBS: dict = {}    # (layer, i, j) -> the off-grid pad that cell stands in for


def emit(board, grid, net, path, width):
    """Turns a cell path into tracks (one per straight run) and vias, and a
    stub from either end to an off-grid pad it stands in for."""
    def point(c):
        x, y = grid.xy(c[1], c[2])
        return vec(x, y)
    for end in (path[0], path[-1]):
        pad = STUBS.get(end)
        if pad is not None:
            track(board, net, point(end), vec(pad.cx, pad.cy), end[0], width)
    start = path[0]
    prev = path[0]
    direction = None
    for c in path[1:]:
        if c[0] != prev[0]:  # a via at prev's cell
            if prev != start:
                track(board, net, point(start), point(prev), prev[0], width)
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(point(prev))
            v.SetDrill(mm(VIA_DRILL))
            v.SetWidth(mm(VIA_D))
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            v.SetNet(net)
            board.Add(v)
            start, prev, direction = c, c, None
            continue
        d = (c[1] - prev[1], c[2] - prev[2])
        if direction is not None and d != direction:
            track(board, net, point(start), point(prev), prev[0], width)
            start = prev
        direction = d
        prev = c
    if prev != start:
        track(board, net, point(start), point(prev), prev[0], width)


def track(board, net, a, b, layer, width):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(a)
    t.SetEnd(b)
    t.SetWidth(mm(width))
    t.SetLayer(pcbnew.F_Cu if layer == 0 else pcbnew.B_Cu)
    t.SetNet(net)
    board.Add(t)


def outline(board):
    """The HDD Synth MK5's outline, speaker holes and grille (mk5_outline.py)."""
    def edge(shape):
        shape.SetLayer(pcbnew.Edge_Cuts)
        shape.SetWidth(mm(0.1))
        board.Add(shape)
    for (a, b) in mk5_outline.LINES:
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(vec(*a))
        s.SetEnd(vec(*b))
        edge(s)
    for (a, m, b) in mk5_outline.ARCS:
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_ARC)
        s.SetArcGeometry(vec(*a), vec(*m), vec(*b))
        edge(s)
    for (x, y, r) in mk5_outline.HOLES:
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(pcbnew.SHAPE_T_CIRCLE)
        s.SetCenter(vec(x, y))
        s.SetEnd(vec(x + r, y))
        edge(s)


def text(board, s, x, y, size=1.0, layer=pcbnew.F_SilkS, justify=None, angle=0):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(vec(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(size * 0.15))
    if angle:
        t.SetTextAngleDegrees(angle)
    if justify == "left":
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    elif justify == "right":
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT)
    board.Add(t)


def silkscreen(board, fps):
    for ref, names in HEADER_PINS.items():
        x, y, _ = PLACEMENT[ref]
        for k, n in enumerate(names):
            text(board, n, x + 1.9, y + k * ROW, 0.8, justify="left")
        # "J2 OLED" beside the group, reading up, in place of the bare reference.
        fps[ref].Reference().SetVisible(False)
        mid = y + (len(names) - 1) * ROW / 2
        text(board, f"{ref} {HEADER_TITLES[ref]}", x + 7.3, mid, 0.8, angle=90)
    jx, jy, _ = PLACEMENT["J1"]
    fps["J1"].Reference().SetVisible(False)
    text(board, "J1 FLOPPY", jx - 7.6, jy - 20.32, 1.0, angle=90)
    # The pull-ups: one label per column, not twenty references.
    for k in range(1, 21):
        fps[f"R{k}"].Reference().SetVisible(False)
    text(board, "10k", PLACEMENT["R1"][0], row(16) + 2.0, 0.8)
    text(board, "1k", PLACEMENT["R11"][0], row(16) + 2.0, 0.8)
    for ref in ("U2", "C1"):
        fps[ref].Reference().SetVisible(False)
    ux, uy, _ = PLACEMENT["U2"]
    text(board, "U2 SN7407", ux, uy - 5.6, 0.8)
    text(board, "FDDSynthPro carrier rev B", 16.5, H - 3.6, 1.0, justify="left")


# ---- The logo ---------------------------------------------------------------------

LOGO = HERE.parent / "media/FDDSynthPro.png"
LOGO_WIDTH = 21.0                  # mm; the height follows the image
LOGO_AT = (16.0, 1.6)              # top-left: above the speaker, right of the first mounting hole


def read_png(path):
    """An 8-bit RGBA PNG (non-interlaced), as rows of (r, g, b, a). KiCad's
    Python has no imaging library, and this is all the logo needs."""
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat = 8, b""
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and colour == 6 and not interlace, "the logo must be 8-bit RGBA, not interlaced"
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride, bpp = w * 4, 4
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 0xFF
            elif f == 2:
                line[x] = (line[x] + b) & 0xFF
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append([tuple(line[i:i + 4]) for i in range(0, stride, 4)])
        prev = line
    return w, h, rows


def logo_mask(path):
    """Ink where the logo is dark and opaque: silkscreen has no grey, so the
    shutter's gradient and the faded lines behind PRO keep only their darker
    parts. Then a pixel of thickening all round, so the finest lines (3 px,
    0.12 mm at this size) clear the fab's 0.15 mm minimum."""
    w, h, rows = read_png(path)
    ink = [[a >= 128 and (299 * r + 587 * g + 114 * b) // 1000 < 140 for (r, g, b, a) in row] for row in rows]
    thick = [[ink[y][x] or any(0 <= y + dy < h and 0 <= x + dx < w and ink[y + dy][x + dx]
                               for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
              for x in range(w)] for y in range(h)]
    return w, h, thick


def logo(board, path, at, width):
    """The logo on the front silkscreen, as filled polygons: each row's runs
    of ink become rectangles (stacked where rows repeat), and KiCad merges
    them into outlines."""
    w, h, mask = logo_mask(path)
    px = width / w
    rects = []
    open_runs = {}
    for y in range(h + 1):
        runs = set()
        if y < h:
            x = 0
            while x < w:
                if mask[y][x]:
                    x0 = x
                    while x < w and mask[y][x]:
                        x += 1
                    runs.add((x0, x))
                else:
                    x += 1
        for run in list(open_runs):
            if run not in runs:
                rects.append((run[0], open_runs.pop(run), run[1], y))
        for run in runs:
            open_runs.setdefault(run, y)
    polys = pcbnew.SHAPE_POLY_SET()
    for x0, y0, x1, y1 in rects:
        polys.NewOutline()
        for (x, y) in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
            polys.Append(mm(ORIGIN[0] + at[0] + x * px), mm(ORIGIN[1] + at[1] + y * px))
    polys.Simplify()
    polys.Fracture()
    for k in range(polys.OutlineCount()):
        one = pcbnew.SHAPE_POLY_SET()
        one.AddOutline(polys.COutline(k))
        shape = pcbnew.PCB_SHAPE(board)
        shape.SetShape(pcbnew.SHAPE_T_POLY)
        shape.SetPolyShape(one)
        shape.SetFilled(True)
        shape.SetWidth(0)
        shape.SetLayer(pcbnew.F_SilkS)
        board.Add(shape)
    print(f"logo: {len(rects)} rectangles -> {polys.OutlineCount()} outlines, {width:.0f} x {h * px:.1f} mm")


def pours(board, gnd):
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(gnd)
        o = z.Outline()
        o.NewOutline()
        m = 0.3
        for x, y in ((m, m), (W - m, m), (W - m, H - m), (m, H - m)):
            o.Append(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y))
        z.SetLocalClearance(mm(0.3))
        z.SetMinThickness(mm(0.25))
        z.SetThermalReliefGap(mm(0.4))
        z.SetThermalReliefSpokeWidth(mm(0.4))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        board.Add(z)


if __name__ == "__main__":
    failed = build(sys.argv[1] if len(sys.argv) > 1 else "/tmp/carrier.net")
    print("routed" if not failed else f"{len(failed)} nets unrouted")
