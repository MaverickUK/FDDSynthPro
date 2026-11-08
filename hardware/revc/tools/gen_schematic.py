#!/usr/bin/env python3
"""Generates the rev C board's KiCad schematic (docs/pcb-revc.md).

Rev C puts everything on one board: the RP2350B and its support circuit
(HDDSynthPro's, after Raspberry Pi's minimal design), 8 MB of PSRAM and a
microSD socket on the BB48R's pins, the floppy interface (rev B's pull-ups and
SN7407, with its sixth gate wired to floppy pin 2), the MAX98357A amp (the HDD
Synth MK5's circuit), and power from the floppy power connector or USB-C.

The GPIOs are the BB48R's, so the bb48_drive firmware runs unchanged; the pin
map is src/config.h's. host/tests/test_revc_schematic.py checks the netlist
against it.

Parts connect by net labels at the end of a short wire on every pin, not by
drawn wires: labels with the same name on a sheet are one net, and global
labels join nets across sheets. Library symbols are copied from KiCad's own
libraries, so pin numbers match KiCad's footprints; the SN7407 and LM66200
are drawn here (FDDSynthProRevC.kicad_sym).

Usage (KiCad 10's libraries; set KICAD_SHARE to its share/kicad folder):
    python hardware/revc/tools/gen_schematic.py
Once the schematic has been edited in KiCad, the .kicad_sch files are the
source of truth: rerunning this overwrites them.
"""
import json
import os
import uuid
from pathlib import Path

from sexpr import Sym as S, dump, find, first, parse

HERE = Path(__file__).resolve().parent
HW = HERE.parent
KICAD_SHARE = Path(os.environ.get("KICAD_SHARE", "/Applications/KiCad/KiCad.app/Contents/SharedSupport"))
PROJECT = "fddsynthpro_revc"
LIB = "FDDSynthProRevC"
SCH_VERSION = 20250114  # KiCad 9 format; KiCad 10 opens it
DATE = "2026-11-08"
REV = "C"
STUB = 2.54
POWER_NETS = {"GND": "power:GND", "+3V3": "power:+3V3", "+5V": "power:+5V"}
BERG_FP = "TE_171825-4_1x04_P2.50mm_Vertical"
LCSC_FIELD = "LCSC Part Number"  # as rev B and the HDD Synth: the Fabrication Toolkit reads it


def uid(*parts):
    """A stable UUID, so regenerating doesn't churn every line of the files."""
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "fddsynthpro-revc/" + "/".join(parts)))


def font(size=1.27):
    return [S("font"), [S("size"), size, size]]


def effects(justify=None, hide=False):
    e = [S("effects"), font()]
    if justify:
        e.append([S("justify")] + [S(j) for j in justify.split()])
    if hide:
        e.append([S("hide"), S("yes")])
    return e


def at(x, y, a=0):
    return [S("at"), round(x, 4), round(y, 4), a]


# ---------------------------------------------------------------- library symbols

_libs = {}


def _lib(lib):
    if lib not in _libs:
        text = (KICAD_SHARE / "symbols" / f"{lib}.kicad_sym").read_text(encoding="utf8")
        _libs[lib] = {s[1]: s for s in find(parse(text), "symbol")}
    return _libs[lib]


def _flatten(lib, name):
    """A library symbol with any `extends` resolved, as KiCad embeds it."""
    s = _lib(lib)[name]
    ext = first(s, "extends")
    if not ext:
        return s
    parent = _flatten(lib, ext[1])
    child_props = {p[1]: p for p in find(s, "property")}
    child_heads = {x[0]: x for x in s[2:] if isinstance(x, list) and x[0] not in ("property", "extends", "symbol")}
    heads, props, units, tail = [], [], [], []
    for item in parent[2:]:
        if not isinstance(item, list):
            heads.append(item)
        elif item[0] == "property":
            props.append(child_props.pop(item[1], item))
        elif item[0] == "symbol":
            units.append([S("symbol"), name + item[1][len(parent[1]):]] + item[2:])
        elif item[0] == "embedded_fonts":
            tail.append(item)
        else:
            heads.append(child_heads.pop(item[0], item))
    props += child_props.values()
    heads += child_heads.values()
    return [S("symbol"), name] + heads + props + units + tail


def _kicad9_property(prop):
    """KiCad 10's libraries put (hide yes) on the property; the KiCad 9 format wants it in effects."""
    hidden = any(isinstance(x, list) and x[:2] == ["hide", "yes"] for x in prop)
    out = [x for x in prop if not (isinstance(x, list) and x and x[0] in ("hide", "show_name", "do_not_autoplace"))]
    if hidden:
        eff = first(out, "effects")
        if not any(isinstance(x, list) and x and x[0] == "hide" for x in eff):
            eff.append([S("hide"), S("yes")])
    return out


def is_hidden(prop):
    eff = first(prop, "effects") or []
    return any(isinstance(x, list) and x[:2] == ["hide", "yes"] for x in list(prop) + list(eff))


# ---------------------------------------------------------------- our own symbols

def _sym_prop(key, value, x, y, hide=False):
    return [S("property"), key, value, at(x, y), effects(hide=hide)]


def box_symbol(name, ref, value, descr, footprint, left=(), right=(), top=(), bottom=(), half_w=7.62):
    """A rectangle with pins: each side a list of (number, name, type) or None for a gap.
    Pins after the first at the same place (stacked) must be marked hidden passive by the caller."""
    rows = max(len(left), len(right), 1)
    half_h = 2.54 * ((rows + 1) // 2 + 1)
    pins = []

    def pin(p, x, y, angle):
        num, pname, kind = p[:3]
        hidden = len(p) > 3 and p[3]
        node = [S("pin"), S(kind), S("line"), at(x, y, angle), [S("length"), 2.54]]
        if hidden:
            node.append([S("hide"), S("yes")])
        node += [[S("name"), pname, effects()], [S("number"), num, effects()]]
        pins.append(node)

    for side, xs, angle in ((left, -half_w - 2.54, 0), (right, half_w + 2.54, 180)):
        y = half_h - 2.54
        for p in side:
            if p:
                pin(p, xs, y, angle)
            y -= 2.54
    for side, ys, angle in ((top, half_h + 2.54, 270), (bottom, -half_h - 2.54, 90)):
        x = -2.54 * (len([p for p in side if p is not None and not (len(p) > 3 and p[3])]) - 1) / 2
        for p in side:
            if p is None:
                x += 2.54
                continue
            pin(p, round(x / 2.54) * 2.54, ys, angle)
            if not (len(p) > 3 and p[3]):
                x += 2.54
    body = [S("symbol"), f"{name}_0_1",
            [S("rectangle"), [S("start"), -half_w, half_h], [S("end"), half_w, -half_h],
             [S("stroke"), [S("width"), 0.254], [S("type"), S("default")]], [S("fill"), [S("type"), S("background")]]]]
    return [S("symbol"), name, [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes")], [S("on_board"), S("yes")],
            _sym_prop("Reference", ref, 0, half_h + 5.08), _sym_prop("Value", value, 0, -half_h - 5.08),
            _sym_prop("Footprint", footprint, 0, -half_h - 7.62, hide=True),
            _sym_prop("Datasheet", "", 0, -half_h - 10.16, hide=True),
            _sym_prop("Description", descr, 0, -half_h - 12.7, hide=True),
            body, [S("symbol"), f"{name}_1_1"] + pins, [S("embedded_fonts"), S("no")]]


def sn7407_symbol():
    """SN7407 (TI SN7407DR), one unit: six open-collector buffers, A in, Y out."""
    gates = [("1", "3", "5", "9", "11", "13"), ("2", "4", "6", "8", "10", "12")]
    return box_symbol("SN7407", "U", "SN7407", "Hex buffer, open-collector outputs (30 V, 40 mA)",
                      "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm",
                      left=[(a, f"{i + 1}A", "input") for i, a in enumerate(gates[0])],
                      right=[(y, f"{i + 1}Y", "open_collector") for i, y in enumerate(gates[1])],
                      top=[("14", "VCC", "power_in")], bottom=[("7", "GND", "power_in")])


def lm66200_symbol():
    """TI LM66200: dual ideal diode, the higher input wins (SLVSG04, pin table 5-1)."""
    return box_symbol("LM66200", "U", "LM66200", "Dual 2.5 A ideal diode (power mux), 1.6-5.5 V",
                      "Package_TO_SOT_SMD:SOT-583-8",
                      left=[("3", "VIN1", "power_in"), ("6", "VIN2", "power_in"), ("4", "~{ON}", "input")],
                      right=[("2", "VOUT", "power_out"), ("7", "VOUT", "passive", True), ("8", "ST", "output")],
                      bottom=[("1", "GND", "power_in"), ("5", "GND", "passive", True)])


LOCAL_SYMBOLS = {f"{LIB}:SN7407": sn7407_symbol, f"{LIB}:LM66200": lm66200_symbol}


def symbol_def(lib_id):
    if lib_id in LOCAL_SYMBOLS:
        return LOCAL_SYMBOLS[lib_id]()
    lib, name = lib_id.split(":")
    sym = _flatten(lib, name)
    sym = [_kicad9_property(x) if isinstance(x, list) and x and x[0] == "property" else x for x in sym]
    for unit in find(sym, "symbol"):
        for p in find(unit, "pin"):
            if (lib_id, first(p, "number")[1]) in PASSIVE_PINS:
                p[1] = S("passive")
    return sym


# Exposed pads KiCad's libraries type "unspecified": they are ground, so ERC sees them as passive.
PASSIVE_PINS = {("Audio:MAX98357A", "17")}


def symbol_pins(sym):
    """[(number, x, y, angle, type)] for unit 1, body style 1."""
    out = []
    for unit in find(sym, "symbol"):
        u, b = unit[1].rsplit("_", 2)[-2:]
        if u not in ("0", "1") or b not in ("0", "1"):
            continue
        for p in find(unit, "pin"):
            a = first(p, "at")
            out.append((first(p, "number")[1], float(a[1]), float(a[2]), int(float(a[3])), str(p[1])))
    return out


# ---------------------------------------------------------------- sheets

def rotate(x, y, rot):
    """Rotate a screen offset (y down) counterclockwise on screen, as KiCad rotates symbols."""
    for _ in range(rot // 90 % 4):
        x, y = y, -x
    return x, y


OUTWARD = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}  # lib pin angle -> schematic direction
LABEL_ANGLE = {(-1, 0): 180, (1, 0): 0, (0, -1): 90, (0, 1): 270}


class Project:
    def __init__(self):
        self.root_uuid = uid("root")
        self.sheets = []
        self.pwr = 0
        self.flg = 0

    def sheet(self, name, file, page):
        sh = Sheet(self, name, file, page)
        self.sheets.append(sh)
        return sh


class Sheet:
    def __init__(self, project, name, file, page):
        self.p = project
        self.name, self.file, self.page = name, file, page
        self.sym_uuid = uid("sheet", file)
        self.path = f"/{project.root_uuid}/{self.sym_uuid}"
        self.items = []
        self.labels = []  # (net, x, y, angle)
        self.points = {}  # connection point -> net
        self.lib_ids = set()

    # --- low level
    def _claim(self, x, y, net):
        key = (round(x, 3), round(y, 3))
        prev = self.points.get(key)
        if prev is not None and prev != net:
            raise ValueError(f"{self.file}: {key} is both {prev} and {net}")
        self.points[key] = net

    def _symbol(self, lib_id, ref, value, x, y, rot=0, footprint=None, in_bom=True, fields=(), dnp=False):
        sym = symbol_def(lib_id)
        self.lib_ids.add(lib_id)
        props = {p[1]: p for p in find(sym, "property")}
        node = [S("symbol"), [S("lib_id"), lib_id], at(x, y, rot), [S("unit"), 1],
                [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes" if in_bom else "no")],
                [S("on_board"), S("yes")], [S("dnp"), S("yes" if dnp else "no")], [S("uuid"), uid(self.file, ref)]]
        values = {"Reference": ref, "Value": value}
        if footprint is not None:
            values["Footprint"] = footprint
        for key in ("Reference", "Value", "Footprint", "Datasheet", "Description"):
            lp = props.get(key)
            text = values.get(key, lp[2] if lp else "")
            if lp:
                la = first(lp, "at")
                px, py = rotate(float(la[1]), -float(la[2]), rot)
                pa = int(float(la[3]))
                eff = [x for x in first(lp, "effects") if not (isinstance(x, list) and x and x[0] == "hide")]
                if is_hidden(lp) or key in ("Footprint", "Datasheet", "Description"):
                    eff.append([S("hide"), S("yes")])
            else:
                px, py, pa, eff = 0, 0, 0, effects(hide=True)
            node.append([S("property"), key, text, at(x + px, y + py, pa), eff])
        for key, text in fields:
            node.append([S("property"), key, text, at(x, y), effects(hide=True)])
        for num in dict.fromkeys(n for n, *_ in symbol_pins(sym)):
            node.append([S("pin"), num, [S("uuid"), uid(self.file, ref, "pin", num)]])
        node.append([S("instances"), [S("project"), PROJECT,
                                      [S("path"), self.path, [S("reference"), ref], [S("unit"), 1]]]])
        self.items.append(node)
        return sym

    def _wire(self, x1, y1, x2, y2):
        self.items.append([S("wire"), [S("pts"), [S("xy"), round(x1, 4), round(y1, 4)], [S("xy"), round(x2, 4), round(y2, 4)]],
                           [S("stroke"), [S("width"), 0], [S("type"), S("default")]],
                           [S("uuid"), uid(self.file, "wire", f"{x1},{y1},{x2},{y2}")]])

    def _power(self, net, x, y, d):
        """A power symbol pointing away from the pin (GND's arrow points down, supplies' up)."""
        lib_id = POWER_NETS[net]
        self.p.pwr += 1
        if net == "GND":
            rot = {(0, 1): 0, (1, 0): 90, (0, -1): 180, (-1, 0): 270}[d]
        else:
            rot = {(0, -1): 0, (-1, 0): 90, (0, 1): 180, (1, 0): 270}[d]
        self._symbol(lib_id, f"#PWR{self.p.pwr:03d}", net, x, y, rot=rot, in_bom=False)

    def attach(self, x, y, d, net):
        """A short wire out of a pin at (x, y) in direction d, ending in a label or power symbol."""
        qx, qy = x + d[0] * STUB, y + d[1] * STUB
        self._claim(qx, qy, net)
        self._wire(x, y, qx, qy)
        if net in POWER_NETS:
            self._power(net, qx, qy, d)
        else:
            self.labels.append((net, qx, qy, LABEL_ANGLE[d]))

    # --- high level
    def part(self, lib_id, ref, value, x, y, nets, footprint="", fields=(), in_bom=True, dnp=False):
        """Place a symbol and give every pin a net (None = not connected)."""
        if ref in LCSC:
            fields = list(fields) + [(LCSC_FIELD, LCSC[ref])]
        fields = list(fields) + FT_FIELDS.get(footprint.split(":")[-1], [])
        sym = self._symbol(lib_id, ref, value, x, y, footprint=footprint, in_bom=in_bom, fields=fields, dnp=dnp)
        done = {}
        for num, px, py, angle, kind in symbol_pins(sym):
            if num not in nets:
                if kind == "no_connect":
                    continue
                raise KeyError(f"{ref} pin {num} has no net")
            net = nets[num]
            ex, ey = x + px, y - py
            key = (round(ex, 3), round(ey, 3))
            if key in done:  # stacked pins share one connection point
                if done[key] != net:
                    raise ValueError(f"{ref} pin {num}: stacked pins on different nets")
                continue
            done[key] = net
            self._claim(ex, ey, net if net else f"NC:{ref}.{num}")
            if net is None:
                self.items.append([S("no_connect"), [S("at"), round(ex, 4), round(ey, 4)],
                                   [S("uuid"), uid(self.file, ref, "nc", num)]])
            else:
                self.attach(ex, ey, OUTWARD[angle], net)
        unknown = set(nets) - {n for n, *_ in symbol_pins(sym)}
        if unknown:
            raise KeyError(f"{ref}: no pins {sorted(unknown)}")

    def r(self, ref, value, x, y, top, bottom, footprint=None):
        self.part("Device:R", ref, value, x, y, {"1": top, "2": bottom}, footprint or R0402)

    def c(self, ref, value, x, y, top="+3V3", bottom="GND", footprint=None):
        self.part("Device:C", ref, value, x, y, {"1": top, "2": bottom}, footprint or C0402)

    def flag(self, net, x, y):
        """A PWR_FLAG: tells ERC the net is powered from a connector."""
        self.p.flg += 1
        self._symbol("power:PWR_FLAG", f"#FLG{self.p.flg:02d}", "PWR_FLAG", x, y, in_bom=False)
        self._claim(x, y, net)
        self.attach(x, y, (0, 1), net)

    def note(self, text, x, y, size=1.27):
        self.items.append([S("text"), text, [S("exclude_from_sim"), S("no")], at(x, y),
                           [S("effects"), [S("font"), [S("size"), size, size]], [S("justify"), S("left"), S("top")]],
                           [S("uuid"), uid(self.file, "text", text[:40])]])


# Corrections for the KiCad Fabrication Toolkit's JLCPCB placement file, by footprint (HDDSynthPro's,
# found by fitting JLCPCB's own footprints onto KiCad's on 2026-10-27). Check new parts in JLCPCB's
# placement preview before paying.
FT_FIELDS = {
    "QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm_ThermalVias": [("FT Rotation Offset", "-90")],
    "SOT-23-6": [("FT Rotation Offset", "90")],
    "SOT-223-3_TabPin2": [("FT Position Offset", "-0.18,0")],
    "USB_C_Receptacle_HRO_TYPE-C-31-M-12": [("FT Position Offset", "0,0.111")],
    "microSD_TF-PUSH": [("FT Position Offset", "0,0.15")],
    "L_Abracon_AOTA-B201610S3R3": [("FT Rotation Offset", "180")],  # JLCPCB marks the dot at the other end
}

# ---------------------------------------------------------------- the design

R0402 = "Resistor_SMD:R_0402_1005Metric"
R0603 = "Resistor_SMD:R_0603_1608Metric"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"


def _refs(prefix, *nums):
    return [f"{prefix}{n}" for n in nums]


# JLCPCB/LCSC part numbers, in each part's "LCSC Part Number" field. Checked at LCSC on
# 2026-11-08 (stock in brackets where it was low). docs/pcb-revc.md lists the choices.
LCSC = {
    **{r: "C25744" for r in _refs("R", *range(1, 11))},         # 10k 0402: input pull-ups
    **{r: "C11702" for r in _refs("R", *range(11, 21))},        # 1k 0402: PC termination
    **{r: "C25744" for r in _refs("R", *range(21, 27))},        # 10k: SN7407 inputs held released
    "R27": "C23186", "R28": "C23186",                           # 5.1k 0603: USB-C CC
    "R29": "C25105",                                            # 33R 0402: VREG_AVDD filter
    "R30": "C11702",                                            # 1k: XOUT
    "R31": "C25744", "R32": "C11702", "R33": "C11702",          # flash CS pull-up, BOOTSEL, RUN
    "R34": "C25100", "R35": "C25100",                           # 27R: USB
    "R36": "C25879", "R37": "C25879",                           # 2.2k: I2C, as the BB48R's R17/R18
    "R38": "C25117",                                            # 470R: LED
    "R46": "C25104",                                            # 330R: external LED (J13)
    "R39": "C25744",                                            # 10k: PSRAM CE#
    "R40": "C25744",                                            # 10k: amp SD_MODE on
    **{r: "C25744" for r in _refs("R", 41, 42, 43, 44, 45)},  # 10k: microSD pull-ups and detect
    **{r: "C11702" for r in _refs("R", *range(47, 57))},        # 1k: floppy inputs in series
    "R57": "C25744", "R58": "C25744",                           # 10k: floppy pins 4 and 6
    "R59": "C11702", "R60": "C11702",                           # 1k: their series resistors
    "R62": "C25900",                                            # 4.7k 0402: the board ID strap
    # Not fitted, so no LCSC number: R61 (the amp's gain), J14 (the spare GPIO header).
    "C1": "C1525",                                               # 100nF: SN7407
    "C2": "C52923", "C3": "C52923", "C4": "C15850",              # 1uF, 1uF, 10uF: LM66200 in/out
    "C5": "C59461", "C6": "C59461",                              # 22uF 0603: +5V bulk (the amp's peaks)
    "C7": "C15850", "C8": "C15850",                              # 10uF: LDO in, out
    "C9": "C23733", "C10": "C23733", "C11": "C23733",            # 4.7uF 0402: core regulator, AVDD. Basic
                                                                 # (Samsung CL05A475M); if it's out of stock,
                                                                 # C368809 (CL05A475K, extended) is the same part
    **{r: "C1525" for r in _refs("C", *range(12, 26))},          # 100nF by each RP2350B supply pin
    "C26": "C1525", "C27": "C1525",                              # flash, PSRAM
    "C28": "C1548", "C29": "C1548",                              # 15pF crystal
    "C30": "C15850",                                             # 10uF +3V3 bulk
    "C31": "C1525", "C32": "C15850",                             # amp 100nF, 10uF
    "C33": "C1588", "C34": "C1588",                              # amp outputs 1nF
    "C35": "C15850", "C36": "C1525",                             # microSD 10uF, 100nF
    "D1": "C113952",                                             # SMAJ5.0A
    "D2": "C2297",                                               # green LED 0805
    "F1": "C2830250",                                            # 1.5A polyfuse 1206, 12V: basic (the 6V one isn't)
    "FB1": "C85840", "FB2": "C85840",
    "J1": "C48603650",                                           # 2x17 right-angle box header [438]
    "J2": "C124378", "J3": "C124379", "J4": "C124375", "J5": "C131337", "J6": "C124376", "J7": "C124375",
    "J8": "C210162",                                             # TE 171825-4, straight
    "J9": "C165948", "J10": "C393941", "J11": "C124376", "J12": "C474892", "J13": "C124375",
    "L1": "C42411119",                                           # [0 at LCSC on 2026-11-08]
    "SW1": "C318884", "SW2": "C318884",
    "U1": "C42415655", "U2": "C40481", "U3": "C97521",           # U3 [20 at LCSC]
    "U4": "C5333729", "U5": "C910544", "U6": "C3235556", "U7": "C6186", "U8": "C2687116",
    "Y1": "C20625731",
}

# The pin map: src/config.h's, which is the BB48R's. New on rev C, and still to be planned in config.h:
# GPIO 29 (the amp's SD_MODE), 30 (microSD card detect), 36 and 42 (floppy pins 4 and 6, read only),
# 37 (the board ID: tied low through 4.7k, where the BB48R leaves it open), and 38-41 (J14, spare).
# The floppy inputs' GPIO-side nets end in _IN: each reaches its pin through a 1k series resistor.
GPIO = {
    0: "UART_TX", 1: "UART_RX", 2: "I2C_SDA", 3: "I2C_SCL",
    4: "INDEX_DRV", 5: "TRK00_DRV", 6: "WPT_DRV", 7: "RDATA_DRV",
    8: "PSRAM_CS", 9: "SD_CS", 10: "SD_SCK", 11: "SD_MOSI",
    12: "DENSEL_IN", 13: "MOTEA_IN", 14: "DRVSB_IN", 15: "DRVSA_IN", 16: "MOTEB_IN",
    17: "DIR_IN", 18: "STEP_IN", 19: "WDATA_IN", 20: "WGATE_IN", 21: "SIDE1_IN",
    22: "DSKCHG_DRV", 23: "CHNG_DRV", 24: "SD_MISO", 25: "LED",
    26: "I2S_DIN", 27: "I2S_BCLK", 28: "I2S_LRCLK", 29: "AMP_SD", 30: "SD_CD",
    32: "ENC_A", 33: "ENC_B", 34: "ENC_SW", 35: "ACTION_BTN", 36: "PIN4_IN",
    37: "BOARD_ID", 38: "GP38", 39: "GP39", 40: "GP40_ADC0", 41: "GP41_ADC1", 42: "PIN6_IN",
}
# Floppy inputs, in GPIO order (12-21), with their pins on the 34-way header.
INPUTS = [("DENSEL", 2), ("MOTEA", 10), ("DRVSB", 12), ("DRVSA", 14), ("MOTEB", 16), ("DIR", 18),
          ("STEP", 20), ("WDATA", 22), ("WGATE", 24), ("SIDE1", 32)]
# Floppy pins 4 and 6, read only: unused on a PC; pin 6 is DS3 on Shugart hosts, and pin 4 means different
# things to different machines. Only a 10k pull-up each (no 1k termination), so they barely load a host
# that uses them some other way. (net, floppy pin, pull-up, series resistor)
SPARE_INPUTS = [("FDD_PIN4", 4, "R57", "R59"), ("FDD_PIN6", 6, "R58", "R60")]
# Floppy outputs through the SN7407: (GPIO net, gate, floppy net, floppy pin). Gate 6 drives pin 2,
# which is also DENSEL's input: the firmware drives it only with pin02=chg (Shugart, Amiga).
OUTPUTS = [("INDEX_DRV", 1, "FDD_INDEX", 8), ("TRK00_DRV", 2, "FDD_TRK00", 26), ("WPT_DRV", 3, "FDD_WPT", 28),
           ("RDATA_DRV", 4, "FDD_RDATA", 30), ("DSKCHG_DRV", 5, "FDD_DSKCHG", 34), ("CHNG_DRV", 6, "DENSEL", 2)]
SN7407_A = {1: "1", 2: "3", 3: "5", 4: "9", 5: "11", 6: "13"}
SN7407_Y = {1: "2", 2: "4", 3: "6", 4: "8", 5: "10", 6: "12"}


def power_sheet(sh):
    sh.note("Power: the floppy power connector (J8) or USB-C (J9), whichever is higher, through the LM66200's\n"
            "ideal diodes: about 40 mOhm, so +5V stays within the SN7407's 4.75 V minimum (Schottky diodes\n"
            "would drop 0.4-0.5 V at the amp's peaks). It also stops USB feeding the PC's 5 V while the PC is off.\n"
            "The AMS1117 makes +3V3 from +5V. +12V on J8 pin 4 is not used.", 20, 20, 1.6)
    sh.part("Connector_Generic:Conn_01x04", "J8", "Floppy power", 40.64, 76.2,
            {"1": "5V_FDD_IN", "2": "GND", "3": "GND", "4": None},
            f"{LIB}:{BERG_FP}", fields=[("MPN", "TE 171825-4")])
    sh.note("J8: 1 +5V, 2-3 GND, 4 +12V (unused).\nThe 3.5-inch drive's mini power plug.", 25.4, 88.9)
    sh.part("Device:Polyfuse", "F1", "1.5A", 76.2, 73.66, {"1": "5V_FDD_IN", "2": "5V_FDD"},
            "Fuse:Fuse_1206_3216Metric")
    sh.part("Device:D_Zener", "D1", "SMAJ5.0A", 99.06, 86.36, {"1": "5V_FDD", "2": "GND"}, "Diode_SMD:D_SMA")
    sh.note("F1 and D1 protect the board from the PC's supply\n(a short, or a spike above 6.4 V).", 66.04, 99.06)
    sh.flag("5V_FDD", 116.84, 63.5)

    usb = {"A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND",
           "A4": "VBUS_USB", "A9": "VBUS_USB", "B4": "VBUS_USB", "B9": "VBUS_USB",
           "A5": "CC1", "B5": "CC2", "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DM", "B7": "USB_DM",
           "A8": None, "B8": None}
    sh.part("Connector:USB_C_Receptacle_USB2.0_16P", "J9", "USB-C", 50.8, 160.02, usb,
            "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12", fields=[("MPN", "HRO TYPE-C-31-M-12")])
    sh.r("R27", "5.1k", 101.6, 147.32, "CC1", "GND", R0603)
    sh.r("R28", "5.1k", 116.84, 147.32, "CC2", "GND", R0603)
    sh.note("CC pull-downs: without them a\nC-to-C cable gives no power.", 96.52, 160.02)
    sh.part("Power_Protection:USBLC6-2SC6", "U8", "USBLC6-2SC6", 160.02, 152.4,
            {"1": "USB_DP", "6": "USB_DP", "3": "USB_DM", "4": "USB_DM", "2": "GND", "5": "VBUS_USB"},
            "Package_TO_SOT_SMD:SOT-23-6")
    sh.note("ESD protection, close to J9. The 27 R series\nresistors sit by the RP2350 (RP2350 sheet).", 144.78, 167.64)
    sh.flag("VBUS_USB", 71.12, 129.54)

    sh.part(f"{LIB}:LM66200", "U6", "LM66200DRLR", 213.36, 99.06,
            {"3": "5V_FDD", "6": "VBUS_USB", "4": "GND", "2": "+5V", "7": "+5V", "8": None, "1": "GND", "5": "GND"},
            "Package_TO_SOT_SMD:SOT-583-8")
    sh.c("C2", "1uF", 180.34, 73.66, "5V_FDD", "GND")
    sh.c("C3", "1uF", 195.58, 73.66, "VBUS_USB", "GND")
    sh.c("C4", "10uF", 246.38, 73.66, "+5V", "GND", C0805)
    sh.c("C5", "22uF", 259.08, 73.66, "+5V", "GND", C0603)
    sh.c("C6", "22uF", 271.78, 73.66, "+5V", "GND", C0603)
    sh.note("ON tied low: always on. VIN1 (floppy) and VIN2 (USB): the higher wins.\n"
            "ST (high on VIN1) is left open: it swings to 5 V.\nC5, C6: bulk for the amp's peaks.", 182.88, 124.46)

    sh.part("Regulator_Linear:AMS1117-3.3", "U7", "AMS1117-3.3", 254.0, 160.02, {"3": "+5V", "2": "+3V3", "1": "GND"},
            "Package_TO_SOT_SMD:SOT-223-3_TabPin2")
    sh.c("C7", "10uF", 228.6, 160.02, "+5V", "GND", C0805)
    sh.c("C8", "10uF", 287.02, 160.02, "+3V3", "GND", C0805)
    sh.note("3V3 for the RP2350B, flash, PSRAM, card and pull-ups: well under 300 mA.", 223.52, 175.26)
    sh.flag("GND", 299.72, 63.5)

    sh.part("Switch:SW_Push", "SW1", "BOOTSEL", 63.5, 218.44, {"1": "BOOTSEL", "2": "GND"},
            "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A")
    sh.part("Switch:SW_Push", "SW2", "RESET", 63.5, 241.3, {"1": "RUN_BTN", "2": "GND"},
            "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A")
    sh.note("Hold BOOTSEL and press RESET for the UF2 drive.\nBoth go through 1k on the RP2350 sheet.", 45.72, 256.54)
    sh.part("Connector_Generic:Conn_01x03", "J11", "SWD", 147.32, 228.6,
            {"1": "SWCLK", "2": "GND", "3": "SWDIO"},
            "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical")
    sh.note("Optional SWD, in the Raspberry Pi debug connector's order.", 129.54, 241.3)

    for i in range(4):
        sh.part("Mechanical:MountingHole_Pad", f"H{i + 1}", "M3", 330.2 + 15.24 * i, 228.6, {"1": "GND"},
                "MountingHole:MountingHole_3.2mm_M3_Pad", in_bom=False)
    sh.note("The HDD Synth MK5's four M3 holes, grounded as on the MK5.", 325.12, 241.3)


def mcu_sheet(sh):
    sh.note("RP2350B with Raspberry Pi's minimal-design support circuit (HDDSynthPro's): core regulator,\n"
            "12 MHz crystal, 16 MB QSPI flash, USB; and 8 MB PSRAM on XIP CS1 (GPIO 8), as on the BB48R.\n"
            "GPIO numbers as in src/config.h. GPIO 29 (amp SD_MODE) and 30 (card detect) are new on rev C.", 20, 20, 1.6)
    power = {"IOVDD": "+3V3", "ADC_AVDD": "+3V3", "USB_OTP_VDD": "+3V3", "QSPI_IOVDD": "+3V3", "VREG_VIN": "+3V3",
             "DVDD": "+1V1", "VREG_FB": "+1V1", "VREG_AVDD": "VREG_AVDD", "VREG_LX": "VREG_LX",
             "VREG_PGND": "GND", "GND": "GND", "XIN": "XIN", "XOUT": "XOUT", "RUN": "RUN",
             "SWCLK": "SWCLK", "SWDIO": "SWDIO", "USB_DP": "USB_DP_MCU", "USB_DM": "USB_DM_MCU",
             "~{QSPI_SS}": "QSPI_SS", "QSPI_SCLK": "QSPI_SCLK", "QSPI_SD0": "QSPI_SD0", "QSPI_SD1": "QSPI_SD1",
             "QSPI_SD2": "QSPI_SD2", "QSPI_SD3": "QSPI_SD3"}
    pins = {}
    sym = symbol_def("MCU_RaspberryPi:RP2350B")
    for unit in find(sym, "symbol"):
        for p in find(unit, "pin"):
            name, num = first(p, "name")[1], first(p, "number")[1]
            pins[num] = GPIO.get(int(name[4:].split("/")[0])) if name.startswith("GPIO") else power[name]
    sh.part("MCU_RaspberryPi:RP2350B", "U1", "RP2350B", 205.74, 152.4, pins,
            "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm_ThermalVias")

    sh.note("Core regulator, as Raspberry Pi require: this inductor (polarity dot), and their layout.", 30.48, 45.72)
    sh.part("Device:L", "L1", "3.3uH", 40.64, 66.04, {"1": "+1V1", "2": "VREG_LX"},
            f"{LIB}:L_Abracon_AOTA-B201610S3R3", fields=[("MPN", "Abracon AOTA-B201610S3R3-101-T")])
    sh.c("C9", "4.7uF", 55.88, 66.04, "+3V3", "GND", f"{LIB}:C_0402_1005Metric_RPi_Wide")
    sh.c("C10", "4.7uF", 71.12, 66.04, "+1V1", "GND", f"{LIB}:C_0402_1005Metric_RPi_Wide")
    sh.r("R29", "33", 86.36, 66.04, "+3V3", "VREG_AVDD")
    sh.c("C11", "4.7uF", 101.6, 66.04, "VREG_AVDD", "GND")
    sh.flag("+1V1", 116.84, 58.42)
    sh.flag("VREG_AVDD", 129.54, 58.42)

    sh.note("100 nF on every supply pin, each by its pin.", 30.48, 254.0)
    for i in range(14):
        sh.c(f"C{12 + i}", "100nF", 40.64 + 12.7 * i, 271.78, "+1V1" if i in (8, 9, 10) else "+3V3", "GND")

    sh.note("12 MHz crystal: Raspberry Pi's part and values (ABM8-272-T3, 15 pF, 1k).", 30.48, 172.72)
    sh.part("Device:Crystal_GND24", "Y1", "12MHz", 63.5, 196.85, {"1": "XIN", "3": "XTAL_OUT", "2": "GND", "4": "GND"},
            "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", fields=[("MPN", "Abracon ABM8-272-T3")])
    sh.c("C28", "15pF", 40.64, 203.2, "XIN", "GND")
    sh.c("C29", "15pF", 86.36, 203.2, "XTAL_OUT", "GND")
    sh.r("R30", "1k", 101.6, 190.5, "XOUT", "XTAL_OUT")

    sh.part("Memory_Flash:W25Q128JVS", "U3", "W25Q128JVSIQ", 76.2, 127.0,
            {"1": "QSPI_SS", "2": "QSPI_SD1", "3": "QSPI_SD2", "4": "GND", "5": "QSPI_SD0", "6": "QSPI_SCLK",
             "7": "QSPI_SD3", "8": "+3V3"}, "Package_SO:SOIC-8_5.3x5.3mm_P1.27mm")
    sh.c("C26", "100nF", 101.6, 116.84)
    sh.r("R31", "10k", 116.84, 116.84, "+3V3", "QSPI_SS")
    sh.r("R32", "1k", 132.08, 116.84, "QSPI_SS", "BOOTSEL")
    sh.r("R33", "1k", 132.08, 142.24, "RUN", "RUN_BTN")
    sh.note("Flash CS pulled up (10k). BOOTSEL pulls QSPI_SS low through 1k;\n"
            "RESET pulls RUN low through 1k (RUN has its own pull-up).", 99.06, 157.48)

    sh.part("Memory_RAM:APS6404L-3SQRx-SN", "U4", "APS6404L-3SQR-SN", 76.2, 233.68,
            {"1": "PSRAM_CS", "2": "QSPI_SD1", "3": "QSPI_SD2", "4": "GND", "5": "QSPI_SD0", "6": "QSPI_SCLK",
             "7": "QSPI_SD3", "8": "+3V3"}, "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm")
    sh.c("C27", "100nF", 101.6, 226.06)
    sh.r("R39", "10k", 116.84, 226.06, "+3V3", "PSRAM_CS")
    sh.note("8 MB PSRAM on the QSPI bus, chip select on GPIO 8 (XIP CS1), as the BB48R:\n"
            "the firmware's PSRAM set-up is unchanged. CE# held high until the core sets it up.", 99.06, 238.76)

    sh.r("R34", "27", 297.18, 203.2, "USB_DP", "USB_DP_MCU")
    sh.r("R35", "27", 312.42, 203.2, "USB_DM", "USB_DM_MCU")
    sh.note("USB: 27 R series, next to the RP2350.", 287.02, 218.44)
    sh.c("C30", "10uF", 330.2, 203.2, "+3V3", "GND", C0805)
    sh.note("Bulk on 3V3.", 322.58, 218.44)

    sh.r("R36", "2.2k", 289.56, 81.28, "+3V3", "I2C_SDA")
    sh.r("R37", "2.2k", 304.8, 81.28, "+3V3", "I2C_SCL")
    sh.note("I2C pull-ups for the OLED, as the BB48R's R17/R18.", 279.4, 96.52)

    sh.note("Activity LED (GPIO 25), at the board's front edge: through a light pipe\nin the bezel it's the drive light.",
            279.4, 116.84)
    sh.r("R38", "470", 289.56, 139.7, "LED", "LED_A")
    sh.part("Device:LED", "D2", "Activity", 289.56, 160.02, {"2": "LED_A", "1": "GND"}, "LED_SMD:LED_0805_2012Metric")
    sh.r("R46", "330", 314.96, 139.7, "LED", "LED_EXT")
    sh.part("Connector_Generic:Conn_01x02", "J13", "Ext LED", 330.2, 157.48, {"1": "LED_EXT", "2": "GND"},
            "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical")
    sh.note("J13: an external activity LED, such as a case's\nHDD LED lead: pin 1 (+) the anode, pin 2 (-) GND.", 297.18, 177.8)

    sh.r("R62", "4.7k", 233.68, 241.3, "BOARD_ID", "GND")
    sh.note("Board ID: GPIO 37 tied low (4.7k: under the 8.2k\nerratum E9 asks for). The BB48R leaves it open,\n"
            "so with the pull-up on it reads high there, low here.", 213.36, 254.0)
    sh.part("Connector_Generic:Conn_02x03_Odd_Even", "J14", "Spare GPIO", 297.18, 236.22,
            {"1": "+3V3", "2": "GND", "3": "GP38", "4": "GP39", "5": "GP40_ADC0", "6": "GP41_ADC1"},
            "Connector_PinHeader_2.54mm:PinHeader_2x03_P2.54mm_Vertical", dnp=True)
    sh.note("J14, not fitted: spare GPIO 38-41 (40 and 41 are\nADC inputs), 3V3 and GND, for what comes later.", 281.94, 246.38)


def floppy_sheet(sh):
    sh.note("The floppy interface, as rev B. Inputs: a 10k pull-up to 3.3 V always, and a 1k to PC_TERM, which J7's\n"
            "jumper joins to 3.3 V for a real PC (909 R); then 1k in series to the GPIO (R47-R56), which limits the\n"
            "current a host driving a line high can push into the RP2350 while it's unpowered (under 5 mA).\n"
            "Outputs: through the SN7407 (open collector); GPIO low asserts.\n"
            "Gate 6 drives pin 2 (/CHNG for Shugart and Amiga hosts, pin02=chg): the rework rev B needed.\n"
            "Pins 4 and 6 are read only (GPIO 36, 42), for hosts that use them: 10k pull-up, 1k series.", 20, 20, 1.6)
    j1 = {str(p): "GND" for p in range(1, 35, 2)}
    for net, pin in INPUTS:
        j1[str(pin)] = net
    for _, _, net, pin in OUTPUTS:
        j1[str(pin)] = net
    for net, pin, _, _ in SPARE_INPUTS:
        j1[str(pin)] = net
    sh.part("Connector_Generic:Conn_02x17_Odd_Even", "J1", "Floppy", 330.2, 147.32, j1,
            "Connector_IDC:IDC-Header_2x17_P2.54mm_Horizontal",
            fields=[("MPN", "XFCN BH254R-34P (right angle, shrouded)")])
    sh.note("J1: odd pins GND; 4 and 6 read only.", 312.42, 193.04)

    for i, (net, _) in enumerate(INPUTS):
        x = 45.72 + 15.24 * i
        sh.r(f"R{i + 1}", "10k", x, 81.28, "+3V3", net)
        sh.r(f"R{i + 11}", "1k", x, 116.84, "PC_TERM", net)
        sh.r(f"R{i + 47}", "1k", x, 134.62, net, f"{net}_IN")
    for k, (net, _, pull, series) in enumerate(SPARE_INPUTS):
        x = 45.72 + 15.24 * (10 + k)
        sh.r(pull, "10k", x, 81.28, "+3V3", net)
        sh.r(series, "1k", x, 134.62, net, net.replace("FDD_", "") + "_IN")
    sh.part("Connector_Generic:Conn_01x02", "J7", "PC term", 50.8, 147.32, {"1": "PC_TERM", "2": "+3V3"},
            "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical")
    sh.note("J7: jumper on for a real PC or an Amiga (1k termination);\noff for the Greaseweazle/FeatherWing host (10k).",
            30.48, 157.48)

    nets = {"14": "+5V", "7": "GND"}
    for drv, gate, fdd, _ in OUTPUTS:
        nets[SN7407_A[gate]] = drv
        nets[SN7407_Y[gate]] = fdd
    sh.part(f"{LIB}:SN7407", "U2", "SN7407DR", 205.74, 157.48, nets, "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm")
    sh.c("C1", "100nF", 241.3, 129.54, "+5V", "GND")
    for i, (drv, *_) in enumerate(OUTPUTS):
        sh.r(f"R{21 + i}", "10k", 116.84 + 12.7 * i, 205.74, "+3V3", drv)
    sh.note("R21-R26 hold the SN7407's inputs high (lines released) from power-on until the firmware\n"
            "drives them: the RP2350's pins start as inputs with a weak pull-down.", 111.76, 220.98)


def audio_sheet(sh):
    sh.note("The amp: a MAX98357A, as on the HDD Synth MK5 (its amplifier sheet), on GPIO 26-28.\n"
            "SD_MODE high (R40, or GPIO 29 high) plays the left channel; the firmware sends the same\n"
            "sample on both. GPIO 29 low shuts it down (silence without hiss).\n"
            "GAIN open: 9 dB. R61, not fitted, takes it to GND: a solder bridge (0R) for 12 dB, 100k for 15 dB.", 20, 20, 1.6)
    sh.part("Audio:MAX98357A", "U5", "MAX98357A", 101.6, 101.6,
            {"1": "I2S_DIN", "2": "AMP_GAIN", "3": "GND", "4": "AMP_SD", "7": "+5V", "8": "+5V", "9": "SPK_P_AMP",
             "10": "SPK_N_AMP", "11": "GND", "14": "I2S_LRCLK", "15": "GND", "16": "I2S_BCLK", "17": "GND"},
            "Package_DFN_QFN:TQFN-16-1EP_3x3mm_P0.5mm_EP1.23x1.23mm_ThermalVias")
    sh.r("R40", "10k", 50.8, 129.54, "+3V3", "AMP_SD")
    sh.part("Device:R", "R61", "DNP", 66.04, 129.54, {"1": "AMP_GAIN", "2": "GND"}, R0402, dnp=True)
    sh.c("C31", "100nF", 50.8, 73.66, "+5V", "GND")
    sh.c("C32", "10uF", 63.5, 73.66, "+5V", "GND", C0805)
    sh.part("Device:FerriteBead_Small", "FB1", "BLM21PG221", 162.56, 93.98, {"1": "SPK_P_AMP", "2": "SPK_P"},
            "Inductor_SMD:L_0805_2012Metric")
    sh.part("Device:FerriteBead_Small", "FB2", "BLM21PG221", 162.56, 111.76, {"1": "SPK_N_AMP", "2": "SPK_N"},
            "Inductor_SMD:L_0805_2012Metric")
    sh.c("C33", "1nF", 180.34, 99.06, "SPK_P", "GND", C0603)
    sh.c("C34", "1nF", 193.04, 99.06, "SPK_N", "GND", C0603)
    sh.part("Connector_Generic:Conn_01x02", "J5", "Speaker", 228.6, 96.52, {"1": "SPK_P", "2": "SPK_N"},
            "Connector_JST:JST_PH_B2B-PH-K_1x02_P2.00mm_Vertical", fields=[("MPN", "JST B2B-PH-K-S")])
    sh.part("Connector_Generic:Conn_01x02", "J12", "Speaker (alt)", 228.6, 119.38, {"1": "SPK_P", "2": "SPK_N"},
            "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-2-3.5-H_1x02_P3.50mm_Horizontal",
            fields=[("MPN", "KF350-3.5-2P")], dnp=True)
    sh.note("J5: JST-PH, for Pimoroni's mini speaker (4 Ohm, 3 W).\nJ12: the HDD Synth's 3.5 mm screw terminal, not fitted.\n"
            "Bridge-tied outputs: neither wire is ground.", 213.36, 132.08)

    sh.note("The front panel and debug headers, as rev B.", 20, 175.26, 1.6)
    sh.part("Connector_Generic:Conn_01x04", "J2", "OLED", 45.72, 203.2,
            {"1": "GND", "2": "+3V3", "3": "I2C_SCL", "4": "I2C_SDA"},
            "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical")
    sh.part("Connector_Generic:Conn_01x05", "J3", "Encoder", 99.06, 203.2,
            {"1": "ENC_A", "2": "ENC_B", "3": "ENC_SW", "4": "+3V3", "5": "GND"},
            "Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical")
    sh.part("Connector_Generic:Conn_01x02", "J4", "Button", 152.4, 203.2, {"1": "ACTION_BTN", "2": "GND"},
            "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical")
    sh.part("Connector_Generic:Conn_01x03", "J6", "UART", 205.74, 203.2, {"1": "UART_TX", "2": "UART_RX", "3": "GND"},
            "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical")
    sh.note("J2: SSD1306 OLED (GND, 3V3, SCL, SDA). J3: KY-040 encoder (CLK, DT, SW, +, GND).\n"
            "J4: action button to GND. J6: debug UART (GPIO 0, 1). The firmware uses internal pull-ups.", 30.48, 228.6)


def sd_sheet(sh):
    sh.note("microSD in SPI mode on SPI1, the BB48R's pins: DAT3 = CS (GPIO 9), CLK (10), CMD = MOSI (11),\n"
            "DAT0 = MISO (24). DAT1 and DAT2 pulled up. Card detect on GPIO 30 (new on rev C).", 20, 20, 1.6)
    sh.part("Connector:Micro_SD_Card_Det1", "J10", "microSD", 228.6, 111.76,
            {"1": "SD_DAT1_2", "2": "SD_CS", "3": "SD_MOSI", "4": "+3V3", "5": "SD_SCK", "6": "GND",
             "7": "SD_MISO", "8": "SD_DAT1_2", "9": "SD_CD", "SH": "GND"},
            f"{LIB}:microSD_TF-PUSH", fields=[("MPN", "TF PUSH (generic push-push)")])
    sh.note("Card detect: pin 9, pulled up by R45. On these sockets it usually meets the\n"
            "grounded shell when a card is in (reads low): check on the first board.", 205.74, 132.08)
    for i, (ref, net) in enumerate([("R41", "SD_MOSI"), ("R42", "SD_MISO"), ("R43", "SD_DAT1_2"),
                                    ("R44", "SD_CS"), ("R45", "SD_CD")]):
        sh.r(ref, "10k", 63.5 + 17.78 * i, 99.06, "+3V3", net)
    sh.note("Pull-ups, close to the socket. CLK needs none. DAT1 and DAT2 share R43.", 58.42, 116.84)
    sh.c("C35", "10uF", 81.28, 149.86, "+3V3", "GND", C0805)
    sh.c("C36", "100nF", 99.06, 149.86)
    sh.note("At the socket's VDD.", 76.2, 162.56)


# ---------------------------------------------------------------- output

def title_block(title, comment=""):
    tb = [S("title_block"), [S("title"), title], [S("date"), DATE], [S("rev"), REV], [S("company"), "FDDSynthPro"],
          [S("comment"), 1, "Generated by hardware/revc/tools/gen_schematic.py from docs/pcb-revc.md"]]
    if comment:
        tb.append([S("comment"), 2, comment])
    return tb


def label_nodes(sheet, global_nets):
    out = []
    for net, x, y, angle in sheet.labels:
        left = angle in (0, 90)
        if net in global_nets:
            out.append([S("global_label"), net, [S("shape"), S("bidirectional")], at(x, y, angle),
                        [S("fields_autoplaced"), S("yes")], effects("left" if left else "right"),
                        [S("uuid"), uid(sheet.file, "label", net, f"{x},{y}")],
                        [S("property"), "Intersheetrefs", "${INTERSHEET_REFS}", at(x, y, angle),
                         effects("left" if left else "right", hide=True)]])
        else:
            out.append([S("label"), net, at(x, y, angle), [S("fields_autoplaced"), S("yes")],
                        effects("left bottom" if left else "right bottom"),
                        [S("uuid"), uid(sheet.file, "label", net, f"{x},{y}")]])
    return out


def lib_symbols(lib_ids):
    out = [S("lib_symbols")]
    for lib_id in sorted(lib_ids):
        sym = symbol_def(lib_id)
        out.append([S("symbol"), lib_id] + sym[2:])
    return out


def write_sheet(path, file_uuid, title, body, lib_ids, extra=(), comment=""):
    doc = [S("kicad_sch"), [S("version"), SCH_VERSION], [S("generator"), "fddsynthpro_revc_gen"],
           [S("generator_version"), "1.0"], [S("uuid"), file_uuid], [S("paper"), "A3"],
           title_block(title, comment), lib_symbols(lib_ids)] + body + list(extra) + [[S("embedded_fonts"), S("no")]]
    path.write_text(dump(doc) + "\n", encoding="utf8")


def root_sheet(project):
    items = []
    x = 30.48
    for sh in project.sheets:
        w, h = 60.96, 25.4
        items.append([S("sheet"), [S("at"), x, 60.96], [S("size"), w, h],
                      [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes")], [S("on_board"), S("yes")],
                      [S("dnp"), S("no")], [S("fields_autoplaced"), S("yes")],
                      [S("stroke"), [S("width"), 0.1524], [S("type"), S("solid")]],
                      [S("fill"), [S("color"), 0, 0, 0, 0.0]],
                      [S("uuid"), sh.sym_uuid],
                      [S("property"), "Sheetname", sh.name, at(x, 60.2484), effects("left bottom")],
                      [S("property"), "Sheetfile", sh.file, at(x, 60.96 + h + 0.5866), effects("left top")],
                      [S("instances"), [S("project"), PROJECT,
                                        [S("path"), f"/{project.root_uuid}", [S("page"), str(sh.page)]]]]])
        x += w + 15.24
    note = ("FDDSynthPro rev C: an RP2350B that acts as a 3.5-inch floppy drive, with its sounds.\n"
            "Pages: power; the RP2350B, flash and PSRAM; the floppy interface; the amp and panel headers; the microSD socket.\n"
            "Nets cross pages through global labels. The GPIO map must agree with src/config.h and docs/wiring.md:\n"
            "host/tests/test_revc_schematic.py checks it.")
    items.append([S("text"), note, [S("exclude_from_sim"), S("no")], at(30.48, 30.48),
                  [S("effects"), [S("font"), [S("size"), 2, 2]], [S("justify"), S("left"), S("top")]],
                  [S("uuid"), uid("root", "note")]])
    write_sheet(HW / f"{PROJECT}.kicad_sch", project.root_uuid, "FDDSynthPro rev C", items, set(),
                extra=[[S("sheet_instances"), [S("path"), "/", [S("page"), "1"]]]])


def write_library():
    lib = [S("kicad_symbol_lib"), [S("version"), 20241209], [S("generator"), "fddsynthpro_revc_gen"],
           [S("generator_version"), "1.0"]] + [make() for make in LOCAL_SYMBOLS.values()]
    (HW / f"{LIB}.kicad_sym").write_text(dump(lib) + "\n", encoding="utf8")


def write_footprints():
    """The floppy power header: TE's EI series 171825-4, straight, 4 pins at 2.5 mm, 0.9 mm holes
    (TE drawing 171825). The body is 10.0 x 4.9 mm round the pin row; the right-angle 171826-4 has
    the same pins but doesn't fit beside J1 and the MK5's rear M3 hole (docs/pcb-revc.md)."""
    def u(*k):
        return [S("uuid"), uid("fp", "berg", *k)]

    fx = [S("effects"), [S("font"), [S("size"), 1, 1], [S("thickness"), 0.15]]]

    def rect(layer, x1, y1, x2, y2, width):
        return [S("fp_rect"), [S("start"), x1, y1], [S("end"), x2, y2],
                [S("stroke"), [S("width"), width], [S("type"), S("solid")]], [S("fill"), S("no")], [S("layer"), layer],
                u("rect", layer)]

    fp = [S("footprint"), BERG_FP, [S("version"), 20241229], [S("generator"), "fddsynthpro_revc_gen"],
          [S("generator_version"), "1.0"], [S("layer"), "F.Cu"],
          [S("descr"), "3.5-inch floppy drive power header (Berg), TE AMP EI series 171825-4, straight. "
                       "4 pins, 2.5 mm pitch, 0.9 mm holes; pin 1 +5V."],
          [S("tags"), "floppy power berg TE 171825"],
          [S("property"), "Reference", "REF**", [S("at"), -3.6, 3.75, 90], [S("layer"), "F.SilkS"], u("ref"), fx],
          [S("property"), "Value", "Floppy power", [S("at"), 3.6, 3.75, 90], [S("layer"), "F.Fab"], u("val"), fx],
          [S("attr"), S("through_hole")],
          rect("F.Fab", -2.45, -1.25, 2.45, 8.75, 0.1),
          rect("F.SilkS", -2.57, -1.37, 2.57, 8.87, 0.12),
          rect("F.CrtYd", -2.85, -1.55, 2.85, 9.05, 0.05),
          [S("fp_text"), S("user"), "${REFERENCE}", [S("at"), 0, 3.75, 90], [S("layer"), "F.Fab"], u("reftext"), fx]]
    for n in range(4):
        fp.append([S("pad"), str(n + 1), S("thru_hole"), S("rect") if n == 0 else S("circle"), [S("at"), 0, 2.5 * n],
                   [S("size"), 1.7, 1.7], [S("drill"), 1.0], [S("layers"), "*.Cu", "*.Mask"],
                   [S("remove_unused_layers"), S("no")], u("pad", str(n))])
    fp.append([S("embedded_fonts"), S("no")])
    d = HW / f"{LIB}.pretty"
    d.mkdir(exist_ok=True)
    for old in d.glob("TE_171826-4*.kicad_mod"):
        old.unlink()
    (d / f"{BERG_FP}.kicad_mod").write_text(dump(fp) + "\n", encoding="utf8")


def write_project(project):
    # Update the project in place: KiCad keeps the board's net classes and rules in it too.
    path = HW / f"{PROJECT}.kicad_pro"
    pro = json.loads(path.read_text(encoding="utf8")) if path.exists() else {"boards": [], "text_variables": {}}
    pro["meta"] = {"filename": f"{PROJECT}.kicad_pro", "version": pro.get("meta", {}).get("version", 3)}
    pro["sheets"] = [[project.root_uuid, "Root"]] + [[sh.sym_uuid, sh.name] for sh in project.sheets]
    path.write_text(json.dumps(pro, indent=2) + "\n", encoding="utf8")
    (HW / "sym-lib-table").write_text(
        f'(sym_lib_table\n\t(version 7)\n\t(lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.kicad_sym")'
        '(options "")(descr "FDDSynthPro rev C parts"))\n)\n', encoding="utf8")
    (HW / "fp-lib-table").write_text(
        f'(fp_lib_table\n\t(version 7)\n\t(lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.pretty")'
        '(options "")(descr "FDDSynthPro rev C footprints"))\n)\n', encoding="utf8")


def main():
    project = Project()
    builders = [("Power", "power.kicad_sch", power_sheet), ("RP2350B", "mcu.kicad_sch", mcu_sheet),
                ("Floppy", "floppy.kicad_sch", floppy_sheet), ("Audio and panel", "audio.kicad_sch", audio_sheet),
                ("microSD", "sd.kicad_sch", sd_sheet)]
    for page, (name, file, build) in enumerate(builders, start=2):
        build(project.sheet(name, file, page))

    sheets_of = {}
    for sh in project.sheets:
        for net, *_ in sh.labels:
            sheets_of.setdefault(net, set()).add(sh.file)
    global_nets = {n for n, s in sheets_of.items() if len(s) > 1}
    for sh in project.sheets:
        counts = {}
        for net, *_ in sh.labels:
            counts[net] = counts.get(net, 0) + 1
        lonely = [n for n, c in counts.items() if c == 1 and n not in global_nets]
        if lonely:
            raise ValueError(f"{sh.file}: nets with one connection: {lonely}")

    for sh in project.sheets:
        write_sheet(HW / sh.file, uid("file", sh.file), f"FDDSynthPro rev C: {sh.name}",
                    sh.items + label_nodes(sh, global_nets), sh.lib_ids)
    root_sheet(project)
    write_library()
    write_footprints()
    write_project(project)
    print(f"wrote {len(project.sheets) + 1} sheets, {len(global_nets)} global nets")


if __name__ == "__main__":
    main()
