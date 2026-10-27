"""Builds the carrier board's KiCad schematic, its BB48R symbol and footprint.

    python3 hardware/make_schematic.py      # needs KiCad's libraries (KICAD_SYMBOL_DIR)

Writes, in hardware/:
    fddsynthpro_carrier.kicad_sch   the schematic
    FDDSynthPro.kicad_sym           the Olimex RP2350-PICO2-BB48R symbol
    FDDSynthPro.pretty/             its footprint (measured from Olimex's own PCB)

The pin map is the one in src/config.h and docs/wiring.md; keep them in step
(host/tests/test_carrier_schematic.py checks the netlist against config.h).
Every pin gets a short wire and a net label, so the connections are exact and
the sheet reads as a table of who goes where. Stock parts are copied from
KiCad's libraries into the schematic, as KiCad does itself.
"""
from __future__ import annotations

import os
import sys
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from sexp import Sym, dump, find, first, parse  # noqa: E402

SYMBOLS = Path(os.environ.get("KICAD_SYMBOL_DIR",
                              "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"))
NAME = "fddsynthpro_carrier"
GRID = 2.54
STUB = 2.54  # wire from each pin to its label

# A fixed namespace keeps the UUIDs stable from run to run, so a rebuild
# only changes what really changed.
NS = uuid.UUID("6f1d3c2e-2b51-4c8e-9a43-fdd5f0a0c0de")
_count = 0


def uid(tag: str = "") -> str:
    global _count
    _count += 1
    return str(uuid.uuid5(NS, f"{tag}/{_count}"))


# ---- The BB48R --------------------------------------------------------------

# EXT1 (left, GPIO 0-23) and EXT2 (right, GPIO 24-47), pin 1 at the USB end,
# as on Olimex's pinout. Pad numbers: EXT1 pin n is n, EXT2 pin n is 27 + n.
EXT1 = ["3V3", "GND", "3V3_EN"] + [f"GP{i}" for i in range(24)]
EXT2 = ["VBUS", "VSYS", "GND"] + [f"GP{i}" for i in range(24, 48)]
BOARD_OWN = {8: "PSRAM CS", 9: "SD CS", 10: "SD SCK", 11: "SD MOSI", 24: "SD MISO", 25: "LED"}


def bb48_pin_name(name: str) -> str:
    if name.startswith("GP") and int(name[2:]) in BOARD_OWN:
        return f"{name} ({BOARD_OWN[int(name[2:])]})"
    return name


def bb48_pin_type(name: str) -> str:
    return {"3V3": "power_out", "VBUS": "power_out", "GND": "power_in"}.get(
        name, "passive" if name in ("VSYS", "3V3_EN") else "bidirectional")


def bb48_symbol(lib_name: str) -> list:
    """The BB48R as a 54-pin symbol: EXT1 down the left, EXT2 down the right."""
    pins = []
    for side, names, x, angle, base in ((1, EXT1, -20.32, 0, 0), (2, EXT2, 20.32, 180, 27)):
        for n, name in enumerate(names, 1):
            y = 33.02 - (n - 1) * GRID
            pins.append([Sym("pin"), Sym(bb48_pin_type(name)), Sym("line"),
                         [Sym("at"), x, y, angle], [Sym("length"), 2.54],
                         [Sym("name"), bb48_pin_name(name), effects()],
                         [Sym("number"), str(base + n), effects()]])
    body = [Sym("symbol"), f"{lib_name}_0_1",
            [Sym("rectangle"), [Sym("start"), -17.78, 35.56], [Sym("end"), 17.78, -35.56],
             [Sym("stroke"), [Sym("width"), 0.254], [Sym("type"), Sym("default")]],
             [Sym("fill"), [Sym("type"), Sym("background")]]],
            [Sym("text"), "USB", [Sym("at"), 0, 33.02, 0], effects()],
            [Sym("text"), "EXT1", [Sym("at"), -13.97, 36.83, 0], effects()],
            [Sym("text"), "EXT2", [Sym("at"), 13.97, 36.83, 0], effects()],
            [Sym("text"), "microSD (underneath)", [Sym("at"), 0, -33.02, 0], effects()]]
    units = [Sym("symbol"), f"{lib_name}_1_1", *pins]
    return [Sym("symbol"), lib_name,
            [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes")], [Sym("on_board"), Sym("yes")],
            prop("Reference", "U", 0, 39.37),
            prop("Value", "Olimex RP2350-PICO2-BB48R", 0, -39.37),
            prop("Footprint", "FDDSynthPro:Olimex_RP2350-PICO2-BB48", 0, -41.91, hide=True),
            prop("Datasheet", "https://github.com/OLIMEX/RP2350-PICO2-BB48", 0, -44.45, hide=True),
            prop("Description", "Olimex RP2350B board, 0.6 in DIL, 2 x 27 pins. The R variant has 8 MB PSRAM "
                 "(GP8) and a microSD slot on SPI1 (GP9-11, GP24) on its underside.", 0, -46.99, hide=True),
            body, units]


def bb48_footprint() -> list:
    """The BB48R's two 27-pin rows, 15.24 mm apart, measured from Olimex's
    PICO2-BB48_Rev_A.kicad_pcb: origin at EXT1 pin 1, rows running down
    from the USB end. The board outline and its microSD keep-clear area go
    on the courtyard, so DRC flags anything placed under the card slot."""
    items = [Sym("footprint"), "Olimex_RP2350-PICO2-BB48",
             [Sym("version"), 20240108], [Sym("generator"), "make_schematic.py"],
             [Sym("layer"), "F.Cu"],
             [Sym("descr"), "Olimex RP2350-PICO2-BB48(R), soldered flat or in 1x27 sockets. "
                            "Rows 15.24 mm apart; microSD slot underneath at the far end from USB."],
             [Sym("tags"), "RP2350 Olimex BB48"],
             fp_text("reference", "REF**", 7.62, -3.5, "F.SilkS"),
             fp_text("value", "Olimex_RP2350-PICO2-BB48", 7.62, 70.5, "F.Fab")]
    left, right, top, bottom = -1.565, 16.805, -1.413, 67.587  # the board edge, from Olimex's PCB
    # The silkscreen outline starts 0.4 mm in from the top: the board sits
    # flush with the carrier's edge, so its USB-C socket can be reached.
    items.append(fp_rect(left, top + 0.4, right, bottom, "F.SilkS", 0.12))
    items.append(fp_rect(left, top, right, bottom, "F.Fab", 0.1))
    # Card slot underneath (TFC-9P at x 7.64, y 55.59 from pin 1) and the card
    # sticking out past the edge: nothing on the carrier there, and room for a finger.
    items.append(fp_rect(1.0, 46.0, 14.3, bottom + 8.0, "F.CrtYd", 0.05))
    items.append(fp_rect(left - 0.5, top - 0.5, right + 0.5, 46.0, "F.CrtYd", 0.05))
    items.append(fp_text("user", "microSD: keep clear", 7.62, bottom + 3.0, "F.SilkS"))  # past the edge
    items.append(fp_text("user", "USB", 7.62, 1.5, "F.SilkS"))
    items.append(fp_text("user", "EXT1", -0.1, -2.6, "F.Fab"))
    items.append(fp_text("user", "EXT2", 15.3, -2.6, "F.Fab"))
    for row, x, base in ((1, 0.0, 0), (2, 15.24, 27)):
        for n in range(1, 28):
            shape = "rect" if n == 1 else "circle"
            items.append([Sym("pad"), str(base + n), Sym("thru_hole"), Sym(shape),
                          [Sym("at"), x, (n - 1) * GRID], [Sym("size"), 1.778, 1.778], [Sym("drill"), 1.0],
                          [Sym("layers"), "*.Cu", "*.Mask"]])
    return items


def fp_text(kind, text, x, y, layer):
    return [Sym("fp_text"), Sym(kind), text, [Sym("at"), x, y], [Sym("layer"), layer],
            [Sym("effects"), [Sym("font"), [Sym("size"), 1, 1], [Sym("thickness"), 0.15]]]]


def fp_rect(x1, y1, x2, y2, layer, width):
    return [Sym("fp_rect"), [Sym("start"), x1, y1], [Sym("end"), x2, y2],
            [Sym("stroke"), [Sym("width"), width], [Sym("type"), Sym("solid")]],
            [Sym("fill"), Sym("no")], [Sym("layer"), layer]]


# ---- Helpers for the schematic ----------------------------------------------

def effects(size=1.27, justify=None, hide=False):
    e = [Sym("effects"), [Sym("font"), [Sym("size"), size, size]]]
    if justify:
        e.append([Sym("justify"), *[Sym(j) for j in justify]])
    if hide:
        e.append([Sym("hide"), Sym("yes")])
    return e


def prop(name, value, x, y, hide=False, justify=None, angle=0):
    return [Sym("property"), name, value, [Sym("at"), x, y, angle], effects(hide=hide, justify=justify)]


def load_lib(name):
    lib = parse((SYMBOLS / f"{name}.kicad_sym").read_text())
    return {s[1]: s for s in find(lib, "symbol")}


LIBS: dict[str, dict] = {}


def lib_symbol(lib_id: str) -> list:
    """A symbol from KiCad's libraries, flattened (`extends` resolved) and
    named lib:name, as the schematic embeds it."""
    lib, name = lib_id.split(":")
    if lib not in LIBS:
        LIBS[lib] = load_lib(lib)
    sym = LIBS[lib][name]
    base = first(sym, "extends")
    if base:
        parent = LIBS[lib][base[1]]
        props = {p[1]: p for p in find(sym, "property")}
        out = [Sym("symbol"), lib_id]
        for item in parent[2:]:
            if isinstance(item, list) and item[0] == "property":
                out.append(props.get(item[1], item))
            elif isinstance(item, list) and item[0] == "symbol":
                out.append([Sym("symbol"), item[1].replace(base[1], name, 1), *item[2:]])
            else:
                out.append(item)
        return out
    return [Sym("symbol"), lib_id, *sym[2:]]


def field_at(sym: list, name: str, x: float, y: float, angle: int) -> tuple[float, float]:
    """Where the library puts a symbol's field, for an instance at (x, y)
    turned by `angle` (counter-clockwise; library y is up, the sheet's down)."""
    for p in find(sym, "property"):
        if p[1] == name:
            at = first(p, "at")
            px, py = float(at[1]), float(at[2])
            break
    else:
        px, py = 0.0, 0.0
    c, s = {0: (1, 0), 90: (0, 1), 180: (-1, 0), 270: (0, -1)}[angle]
    return x + px * c - py * s, y - (px * s + py * c)


def unit_pins(sym: list, unit: int) -> list[dict]:
    """The pins a unit has: its own, and those common to every unit (unit 0)."""
    pins = []
    for sub in find(sym, "symbol"):
        parts = sub[1].rsplit("_", 2)
        u = int(parts[1])
        if u not in (0, unit):
            continue
        for p in find(sub, "pin"):
            at = first(p, "at")
            pins.append({"number": first(p, "number")[1], "name": first(p, "name")[1],
                         "x": float(at[1]), "y": float(at[2]), "angle": int(float(at[3]))})
    return pins


class Sheet:
    def __init__(self):
        self.root = uid("root")
        self.items: list = []
        self.lib_symbols: dict[str, list] = {}
        self.pwr = 0

    def use(self, lib_id: str, sym: list | None = None) -> list:
        if lib_id not in self.lib_symbols:
            self.lib_symbols[lib_id] = sym if sym is not None else lib_symbol(lib_id)
        return self.lib_symbols[lib_id]

    def instance(self, lib_id, ref, value, x, y, unit=1, footprint="", fields=(), hidden_ref=False, angle=0,
                 in_bom=True, dnp=False):
        sym = self.lib_symbols[lib_id]
        pins = [p for u in ([unit] if unit else [1]) for p in unit_pins(sym, u)]
        ref_at = field_at(sym, "Reference", x, y, angle)
        value_at = field_at(sym, "Value", x, y, angle)
        node = [Sym("symbol"), [Sym("lib_id"), lib_id], [Sym("at"), x, y, angle], [Sym("unit"), unit],
                [Sym("exclude_from_sim"), Sym("no")], [Sym("in_bom"), Sym("yes" if in_bom and not ref.startswith("#") else "no")],
                [Sym("on_board"), Sym("no" if ref.startswith("#") else "yes")], [Sym("dnp"), Sym("yes" if dnp else "no")],
                [Sym("uuid"), uid(ref)],
                prop("Reference", ref, *ref_at, hide=hidden_ref),
                prop("Value", value, *value_at, angle=angle),  # horizontal whichever way the symbol turns
                prop("Footprint", footprint, x, y, hide=True),
                prop("Datasheet", "", x, y, hide=True)]
        for name, val in fields:
            node.append(prop(name, val, x, y, hide=True))
        for p in pins:
            node.append([Sym("pin"), p["number"], [Sym("uuid"), uid(ref + p["number"])]])
        node.append([Sym("instances"), [Sym("project"), NAME,
                     [Sym("path"), f"/{self.root}", [Sym("reference"), ref], [Sym("unit"), unit]]]])
        self.items.append(node)
        return pins

    def wire(self, x1, y1, x2, y2):
        self.items.append([Sym("wire"), [Sym("pts"), [Sym("xy"), x1, y1], [Sym("xy"), x2, y2]],
                           [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("default")]],
                           [Sym("uuid"), uid("w")]])

    def label(self, net, x, y, angle):
        justify = ["left", "bottom"] if angle in (0, 90) else ["right", "bottom"]
        self.items.append([Sym("label"), net, [Sym("at"), x, y, angle], effects(justify=justify),
                           [Sym("uuid"), uid("l" + net)]])

    def no_connect(self, x, y):
        self.items.append([Sym("no_connect"), [Sym("at"), x, y], [Sym("uuid"), uid("nc")]])

    def power(self, lib_id, x, y, toward):
        """A power symbol at a wire's end, turned to point along the wire
        (`toward`: 0 right, 90 up, 180 left, 270 down), so neighbours don't
        overlap. GND points down as drawn; +3V3, VBUS and PWR_FLAG point up."""
        self.use(lib_id)
        self.pwr += 1
        name = lib_id.split(":")[1]
        natural = 270 if name == "GND" else 90
        self.instance(lib_id, f"#PWR{self.pwr:02d}", name, x, y, hidden_ref=True, angle=(toward - natural) % 360)

    def text(self, s, x, y, size=1.27):
        self.items.append([Sym("text"), s, [Sym("exclude_from_sim"), Sym("no")], [Sym("at"), x, y, 0],
                           effects(size=size, justify=["left", "top"]), [Sym("uuid"), uid("t")]])

    def part(self, lib_id, ref, value, x, y, nets: dict, unit=1, footprint="", fields=(), sym=None, in_bom=True,
             dnp=False):
        """Places a symbol and connects each pin: a net name (a wire and a
        label), '+3V3' / 'VBUS' / 'GND' (a power symbol), or None (no-connect).
        Every pin must be listed, so nothing is left floating by accident."""
        self.use(lib_id, sym)
        pins = self.instance(lib_id, ref, value, x, y, unit, footprint, fields, in_bom=in_bom, dnp=dnp)
        numbers = {p["number"] for p in pins}
        missing = numbers - set(nets)
        extra = set(nets) - numbers
        assert not missing and not extra, f"{ref}: pins not listed {sorted(missing)}, unknown {sorted(extra)}"
        for p in pins:
            ex, ey = x + p["x"], y - p["y"]  # symbol y is up, the sheet's is down
            net = nets[p["number"]]
            if net is None:
                self.no_connect(ex, ey)
                continue
            dx, dy = {0: (-STUB, 0), 180: (STUB, 0), 90: (0, STUB), 270: (0, -STUB)}[p["angle"]]
            sx, sy = ex + dx, ey + dy
            self.wire(ex, ey, sx, sy)
            if net in ("+3V3", "VBUS", "GND"):
                self.power(f"power:{net}", sx, sy, {0: 180, 180: 0, 90: 270, 270: 90}[p["angle"]])
            else:
                self.label(net, sx, sy, {0: 180, 180: 0, 90: 270, 270: 90}[p["angle"]])

    def write(self, path: Path, title_block: list):
        doc = [Sym("kicad_sch"), [Sym("version"), 20251024], [Sym("generator"), "make_schematic.py"],
               [Sym("generator_version"), "1.0"], [Sym("uuid"), self.root], [Sym("paper"), "A3"],
               title_block, [Sym("lib_symbols"), *self.lib_symbols.values()], *self.items,
               [Sym("sheet_instances"), [Sym("path"), "/", [Sym("page"), "1"]]]]
        path.write_text(dump(doc) + "\n")


# ---- The circuit --------------------------------------------------------------

# Floppy inputs: header pin, BB48R GPIO (src/config.h), signal. Each also
# goes to the SIP network, pins 2-11 in this order.
INPUTS = [(2, 12, "DENSEL"), (10, 13, "MOTEA"), (12, 14, "DRVSB"), (14, 15, "DRVSA"), (16, 16, "MOTEB"),
          (18, 17, "DIR"), (20, 18, "STEP"), (22, 19, "WDATA"), (24, 20, "WGATE"), (32, 21, "SIDE1")]
# Floppy outputs: header pin, GPIO, signal, SN7407 unit, its A and Y pins.
OUTPUTS = [(8, 4, "INDEX", 1, "1", "2"), (26, 5, "TRK00", 2, "3", "4"), (28, 6, "WPT", 3, "5", "6"),
           (30, 7, "RDATA", 4, "9", "8"), (34, 22, "DSKCHG", 5, "11", "10")]
# Everything else on the BB48R (config.h): GPIO -> net.
OTHER = {0: "UART_TX", 1: "UART_RX", 2: "OLED_SDA", 3: "OLED_SCL", 26: "I2S_DIN", 27: "I2S_BCLK",
         28: "I2S_LRC", 32: "ENC_A", 33: "ENC_B", 34: "ENC_SW", 35: "ACTION"}


# Parts JLCPCB fits, by LCSC number (checked on jlcpcb.com, 2026-10).
LCSC = {
    "sn7407": "C40481",       # TI SN7407DR, SOIC-14 (extended)
    "10k": "C25744",          # 0402 10k, as on the HDD Synth MK5
    "1k": "C11702",           # 0402 1k, as on the HDD Synth MK5
    "100n": "C14663",         # YAGEO CC0603KRX7R9BB104 (basic; the HDD Synth uses it too)
    "box_header": "C9042",    # BOOMELE 2.54-2*17P straight box header (extended, through-hole)
    # Straight 2.54 mm pin headers (extended, through-hole): Ckmtw's
    # B-2100S0xP-A110, as on the HDD Synth MK5, and XFCN's 1x7 (Ckmtw has none).
    "1x2": "C124375",
    "1x3": "C124376",
    "1x4": "C124378",
    "1x5": "C124379",
    "1x7": "C492406",
}
# The field name the HDD Synth's symbols use, which its BOM exporter (KiCad's
# Fabrication Toolkit) reads.
LCSC_FIELD = "LCSC Part Number"


def cable(sig):
    return "~{" + sig + "}"  # active low: drawn with an overbar


def build() -> Sheet:
    s = Sheet()
    gpio_net = {g: cable(sig) for _, g, sig in INPUTS}
    gpio_net.update({g: f"{sig}_GPIO" for _, g, sig, *_ in OUTPUTS})
    gpio_net.update(OTHER)

    # The BB48R.
    bb = bb48_symbol("Olimex_RP2350-PICO2-BB48R")
    nets = {}
    for base, names in ((0, EXT1), (27, EXT2)):
        for n, name in enumerate(names, 1):
            num = str(base + n)
            if name in ("3V3",):
                nets[num] = "+3V3"
            elif name in ("VBUS", "GND"):
                nets[num] = name
            elif name.startswith("GP"):
                nets[num] = gpio_net.get(int(name[2:]))
            else:
                nets[num] = None
    s.part("FDDSynthPro:Olimex_RP2350-PICO2-BB48R", "U1", "Olimex RP2350-PICO2-BB48R", 254.0, 139.7, nets,
           footprint="FDDSynthPro:Olimex_RP2350-PICO2-BB48", sym=[Sym("symbol"), "FDDSynthPro:Olimex_RP2350-PICO2-BB48R", *bb[2:]],
           dnp=True)  # plugged in by hand, never by the assembler

    # The floppy header: odd pins ground, 4 and 6 unused on a 3.5-inch PC drive.
    fdd = {str(p): "GND" for p in range(1, 35, 2)}
    fdd.update({"4": None, "6": None})
    fdd.update({str(h): cable(sig) for h, _, sig in INPUTS})
    fdd.update({str(h): cable(sig) for h, _, sig, *_ in OUTPUTS})
    s.part("Connector_Generic:Conn_02x17_Odd_Even", "J1", "Floppy 34-pin", 63.5, 127.0, fdd,
           footprint="Connector_IDC:IDC-Header_2x17_P2.54mm_Vertical", fields=[(LCSC_FIELD, LCSC["box_header"])])

    # Pull-ups on the ten inputs, fitted by JLCPCB: 10k always (R1-R10,
    # right for the FeatherWing host), and 1k (R11-R20) to PC_TERM, which J7's
    # jumper joins to 3.3 V for a real PC (1k || 10k = 909 ohms).
    for i, (_, _, sig) in enumerate(INPUTS):
        s.part("Device:R", f"R{i + 1}", "10k", 50.8 + i * 7.62, 205.74, {"1": cable(sig), "2": "+3V3"},
               footprint="Resistor_SMD:R_0402_1005Metric", fields=[(LCSC_FIELD, LCSC["10k"])])
        s.part("Device:R", f"R{i + 11}", "1k", 50.8 + i * 7.62, 228.6, {"1": cable(sig), "2": "PC_TERM"},
               footprint="Resistor_SMD:R_0402_1005Metric", fields=[(LCSC_FIELD, LCSC["1k"])])
    s.part("Connector_Generic:Conn_01x02", "J7", "PC termination (jumper)", 132.08, 228.6,
           {"1": "PC_TERM", "2": "+3V3"}, footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical", fields=[(LCSC_FIELD, LCSC["1x2"])])

    # The SN7407, fitted by JLCPCB: five buffers (GPIO -> A, Y -> cable), the
    # sixth tied off.
    y = 50.8
    for h, g, sig, unit, a, ypin in OUTPUTS:
        s.part("74xx:74LS07", "U2", "SN7407DR", 152.4, y, {a: f"{sig}_GPIO", ypin: cable(sig)}, unit=unit,
               footprint="Package_SO:SOIC-14_3.9x8.7mm_P1.27mm", fields=[(LCSC_FIELD, LCSC["sn7407"])])
        y += 20.32
    s.part("74xx:74LS07", "U2", "SN7407DR", 152.4, y, {"13": "GND", "12": None}, unit=6,
           footprint="Package_SO:SOIC-14_3.9x8.7mm_P1.27mm", fields=[(LCSC_FIELD, LCSC["sn7407"])])
    s.part("74xx:74LS07", "U2", "SN7407DR", 152.4, 177.8, {"14": "VBUS", "7": "GND"}, unit=7,
           footprint="Package_SO:SOIC-14_3.9x8.7mm_P1.27mm", fields=[(LCSC_FIELD, LCSC["sn7407"])])
    s.part("Device:C", "C1", "100n", 175.26, 177.8, {"1": "VBUS", "2": "GND"},
           footprint="Capacitor_SMD:C_0603_1608Metric", fields=[(LCSC_FIELD, LCSC["100n"])])
    s.use("power:PWR_FLAG")
    s.part("power:PWR_FLAG", "#FLG01", "PWR_FLAG", 190.5, 190.5, {"1": "GND"})

    # Headers for the rest.
    x = 345.44
    s.part("Connector_Generic:Conn_01x04", "J2", "OLED (SSD1306, I2C)", x, 55.88,
           {"1": "GND", "2": "+3V3", "3": "OLED_SCL", "4": "OLED_SDA"},
           footprint="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical", fields=[(LCSC_FIELD, LCSC["1x4"])])
    s.part("Connector_Generic:Conn_01x05", "J3", "Rotary encoder (CLK DT SW + GND)", x, 86.36,
           {"1": "ENC_A", "2": "ENC_B", "3": "ENC_SW", "4": "+3V3", "5": "GND"},
           footprint="Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical", fields=[(LCSC_FIELD, LCSC["1x5"])])
    s.part("Connector_Generic:Conn_01x02", "J4", "Action button", x, 114.3, {"1": "ACTION", "2": "GND"},
           footprint="Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical", fields=[(LCSC_FIELD, LCSC["1x2"])])
    s.part("Connector_Generic:Conn_01x07", "J5", "MAX98357A amp", x, 144.78,
           {"1": "I2S_LRC", "2": "I2S_BCLK", "3": "I2S_DIN", "4": None, "5": None, "6": "GND", "7": "VBUS"},
           footprint="Connector_PinHeader_2.54mm:PinHeader_1x07_P2.54mm_Vertical", fields=[(LCSC_FIELD, LCSC["1x7"])])
    s.part("Connector_Generic:Conn_01x03", "J6", "Debug UART", x, 182.88,
           {"1": "UART_TX", "2": "UART_RX", "3": "GND"},
           footprint="Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical", fields=[(LCSC_FIELD, LCSC["1x3"])])

    # M3 mounting holes where the HDD Synth MK5 has them, plated and grounded
    # as on that board.
    for k in range(4):
        s.part("Mechanical:MountingHole_Pad", f"H{k + 1}", "M3", 205.74 + k * 12.7, 248.92, {"1": "GND"},
               footprint="MountingHole:MountingHole_3.2mm_M3_Pad", in_bom=False)

    s.text("FDDSynthPro carrier: an Olimex RP2350-PICO2-BB48R as a 3.5-inch PC floppy drive.\n"
           "Pin map: src/config.h and docs/wiring.md. Every floppy signal is active low (overbar).\n"
           "\n"
           "U1  Solder the BB48R flat or in 1x27 sockets. Its microSD slot is underneath, at the end\n"
           "    away from USB: keep that area and the space past the edge clear (courtyard).\n"
           "    GP8 (PSRAM) and GP9-11, GP24 (SD card) are the board's own: never connect them.\n"
           "U2  SN7407DR (SOIC-14), from VBUS (5 V). It pulls the cable low; the GPIOs drive its inputs.\n"
           "R1-R10  10k pull-ups (the FeatherWing host). R11-R20  1k pull-ups to PC_TERM: fit a jumper on J7\n"
           "    for a real PC (909 ohms in all). U2, C1, R1-R20 and J1 are fitted by JLCPCB (the LCSC field).\n"
           "J2  SSD1306 OLED (or use the BB48R's own Qwiic socket: same I2C1 pins).\n"
           "J3  Encoder: a KY-040 module plugs in as it is; a bare encoder needs only CLK, DT, SW and GND\n"
           "    (the firmware uses internal pull-ups). J4: a push button to GND.\n"
           "J5  Adafruit MAX98357A in its own pin order; GAIN and SD left open.",
           25.4, 236.22)
    return s


def title_block():
    return [Sym("title_block"), [Sym("title"), "FDDSynthPro carrier: BB48R floppy drive"],
            [Sym("date"), "2026-10-26"], [Sym("rev"), "A"], [Sym("company"), "FDDSynthPro"],
            [Sym("comment"), 1, "Generated by hardware/make_schematic.py; pin map from src/config.h"]]


def main():
    sheet = build()
    sheet.write(HERE / f"{NAME}.kicad_sch", title_block())
    sym = bb48_symbol("Olimex_RP2350-PICO2-BB48R")
    lib = [Sym("kicad_symbol_lib"), [Sym("version"), 20251024], [Sym("generator"), "make_schematic.py"], sym]
    (HERE / "FDDSynthPro.kicad_sym").write_text(dump(lib) + "\n")
    (HERE / "FDDSynthPro.pretty").mkdir(exist_ok=True)
    (HERE / "FDDSynthPro.pretty/Olimex_RP2350-PICO2-BB48.kicad_mod").write_text(dump(bb48_footprint()) + "\n")
    print(f"wrote {NAME}.kicad_sch, FDDSynthPro.kicad_sym, FDDSynthPro.pretty/")


if __name__ == "__main__":
    main()
