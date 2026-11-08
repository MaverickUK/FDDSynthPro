"""Places the rev C board: footprints, nets, fixed copper and planes (docs/pcb-revc.md).

Run with KiCad 10's own Python (it has pcbnew), after gen_schematic.py:
    <KiCad python> hardware/revc/tools/layout_pcb.py [out.kicad_pcb]
then route with autoroute.py, and check:
    kicad-cli pcb drc --refill-zones --save-board --schematic-parity hardware/revc/fddsynthpro_revc.kicad_pcb

It reads the netlist from the schematic (via kicad-cli), so every footprint is linked to its
symbol and "Update PCB from Schematic" works afterwards. Like gen_schematic.py it only
bootstraps: rerunning it overwrites edits made in KiCad. Traces aren't routed here.
`layout_pcb.py --silkscreen <board>` redoes only the labels on a routed board.

The board keeps rev B's outline: the HDD Synth MK5's, with its M3 holes, speaker holes and
grille (hardware/mk5_outline.py). In a 3.5-inch bay it runs front to back: the left edge
(the speaker's) is the front, the right edge the rear. So:
- J1 (right-angle) is on the rear edge, between the rear M3 holes, and the floppy power header
  (J8) in the corner beyond the lower one, plugged from above (a right-angle one doesn't fit
  beside J1 and the hole);
- the speaker sits on top at the front, on the MK5's holes: nothing on top, and no copper,
  inside its 40 mm square;
- the microSD socket is above the speaker, its card in from the top edge, and USB-C below it,
  in from the bottom edge (the MK5's front holes and the speaker leave no room on the front
  edge itself); the activity LED is at the front edge, for a light pipe in the bezel;
- the RP2350B is turned 180 degrees so its floppy pins (GPIO 4-23) face J1, with Raspberry Pi's
  regulator and flash layout (HDDSynthPro's) turned with it.
Four layers: signals, GND plane, 3V3 plane, signals.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

HERE = Path(__file__).resolve().parent
HW = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HW.parent))  # hardware/, for mk5_outline
import mk5_outline  # noqa: E402
from sexpr import find, first, parse  # noqa: E402

KICAD_SHARE = Path(os.environ.get("KICAD_SHARE", "/Applications/KiCad/KiCad.app/Contents/SharedSupport"))
KICAD_CLI = os.environ.get("KICAD_CLI", "kicad-cli")
PROJECT = "fddsynthpro_revc"
LIB = "FDDSynthProRevC"
OUT = HW / f"{PROJECT}.kicad_pcb"

# Board coordinates: mm from the board's top-left corner, placed on the sheet at ORIGIN.
ORIGIN = (100.0, 50.0)
W, H = 100.33, 69.914
SPEAKER = (0.0, 20.3, 41.0, 59.95)  # x0, y0, x1, y1: the speaker's square on the MK5's holes, as rev B

# The RP2350B, turned 180 degrees: GPIO 4-20 face right (J1), GPIO 21-32 up, QSPI and USB down.
U1 = (52.0, 22.0, 180)

# J1's pin 1, and the input rows (floppy pin -> y): pins 1/2 on the first row, 33/34 on the last.
J1_X, J1_Y = 87.0, 13.2


def j1_row(pin):
    return J1_Y + ((pin - 1) // 2) * 2.54


INPUTS = [("DENSEL", 2), ("MOTEA", 10), ("DRVSB", 12), ("DRVSA", 14), ("MOTEB", 16), ("DIR", 18),
          ("STEP", 20), ("WDATA", 22), ("WGATE", 24), ("SIDE1", 32)]

# ref -> (x, y, rotation in degrees, counterclockwise), board coordinates.
PLACE = {
    "U1": U1,
    # The rear edge: J1 between the M3 holes, mating face on the edge; the power header beyond.
    "J1": (J1_X, J1_Y, 0),
    "J8": (91.4, 64.5, 90),
    # Pull-ups on each input's row: 10k beside J1, 1k beside them, then the 1k in series to the GPIO
    # (pin 1 towards J1); J7 below. Pins 4 and 6 have a 10k and a series 1k only.
    **{f"R{i + 1}": (83.6, j1_row(pin), 0) for i, (_, pin) in enumerate(INPUTS)},
    **{f"R{i + 11}": (81.2, j1_row(pin), 0) for i, (_, pin) in enumerate(INPUTS)},
    **{f"R{i + 47}": (78.6, j1_row(pin), 180) for i, (_, pin) in enumerate(INPUTS)},
    "R57": (83.6, j1_row(4), 0), "R58": (83.6, j1_row(6), 0),
    "R59": (78.6, j1_row(4), 180), "R60": (78.6, j1_row(6), 180),
    "J7": (77.5, 45.9, 0),
    # The spare GPIO header (not fitted) in the clear patch between the PSRAM and J7; the board ID strap
    # left of the core regulator, near GPIO 37.
    "J14": (68.6, 43.96, 0),
    "R62": (45.0, 32.6, 90),
    # The SN7407 below the floppy pins, its inputs held high by R21-R26 on its left.
    "U2": (72.0, 36.0, 0),
    "C1": (72.0, 30.6, 0),
    **{f"R{21 + k}": (66.4, 32.2 + 1.3 * k, 0) for k in range(6)},
    # The RP2350B's crystal above it (its XIN/XOUT pins face up).
    "Y1": (52.0, 10.5, 180),
    "R30": (50.4, 14.4, 270),
    "C28": (55.4, 9.65, 180),
    "C29": (48.6, 11.35, 180),
    # PSRAM beside the flash, on the same QSPI lines; its chip select pull-up and capacitor.
    "U4": (59.5, 47.0, 180),        # below the flash, turned the same way: the QSPI lines run pin to pin
    "R39": (64.8, 45.6, 90),
    "C27": (64.8, 48.4, 90),
    "C30": (64.0, 33.0, 90),       # 3V3 bulk
    "R36": (59.6, 29.0, 90),       # I2C pull-ups, by GPIO 2/3
    "R37": (60.8, 29.0, 90),
    # Front panel and debug headers: along the top edge, and beside the SD socket.
    "J3": (43.5, 2.6, 90),         # encoder, 1x5, pins along the edge
    "J2": (57.6, 2.6, 90),         # OLED, 1x4
    "J4": (68.9, 2.6, 90),         # action button
    "SW1": (65.0, 10.0, 0),        # BOOTSEL
    "SW2": (73.0, 10.0, 0),        # RESET
    "R33": (77.6, 10.0, 90),       # RUN to RESET, 1k
    "J11": (38.6, 3.0, 0),         # SWD, 1x3, pins down
    "J6": (38.6, 12.0, 0),         # UART, 1x3
    # The microSD socket above the speaker, card in from the top edge.
    "J10": (27.0, 9.69, 180),
    **{r: (15.4, 8.0 + 1.4 * k, 0) for k, r in enumerate(["R41", "R42", "R43", "R44", "R45"])},
    "C35": (15.4, 15.6, 0),
    "C36": (15.4, 17.6, 0),
    # Activity LED at the front edge, above the speaker, for a light pipe.
    "D2": (1.6, 13.0, 90),
    "R38": (1.6, 17.0, 90),
    # A header for an external LED (a case's HDD LED lead) beside it, pin 1 (+) nearer the edge.
    "J13": (5.0, 12.0, 90),
    "R46": (4.4, 15.6, 0),
    # USB-C below the speaker, in from the bottom edge; ESD and CC beside it.
    "J9": (27.0, 66.26, 0),
    "R27": (19.6, 64.2, 90),
    "R28": (34.4, 64.2, 90),
    "U8": (37.6, 66.0, 90),
    # The amp and the speaker connectors, beside the speaker's bottom-right corner.
    "J5": (45.3, 57.8, 90),        # JST-PH, 2 mm clear of the speaker's square
    "J12": (44.4, 64.6, 0),        # 3.5 mm terminal, not fitted
    "U5": (51.6, 56.4, 0),
    "FB1": (48.6, 51.2, 90),
    "FB2": (52.2, 51.2, 90),
    "C33": (50.4, 51.2, 90),
    "C34": (54.0, 51.2, 90),
    "C31": (55.6, 55.2, 90),
    "C32": (55.6, 59.2, 90),
    "R40": (51.6, 60.4, 0),
    "R61": (51.6, 61.8, 0),        # GAIN to GND, not fitted: under R40
    # Power: the floppy input's fuse and TVS by J8, the ideal-diode mux, and the 3V3 LDO.
    "F1": (80.0, 59.6, 0),
    "D1": (78.4, 66.4, 180),
    "U6": (72.0, 60.0, 0),
    "C2": (69.4, 61.8, 90),        # 5V_FDD in
    "C3": (74.6, 61.8, 90),        # VBUS in
    "C4": (72.0, 56.4, 0),         # +5V out
    "C5": (69.4, 56.6, 90),
    "C6": (74.6, 56.6, 90),
    "U7": (62.0, 64.0, 0),
    "C7": (66.4, 57.0, 0),
    "C8": (58.6, 58.0, 90),
}
# Raspberry Pi's core regulator and flash corner (HDDSynthPro's layout_pcb.py, from their RP2350
# minimal design), relative to the VREG_LX pad with the chip unrotated: ref -> (dx, dy, rotation).
# Our references for their parts: U3 the flash, R29 the AVDD 33R, R31/R32 the flash CS 10k and
# BOOTSEL 1k, R34/R35 the USB 27R.
CORE = {
    "C9": (0.0, -1.16, 0), "C10": (0.0, -2.1, 0), "L1": (0.0, -3.75, 0),
    "C11": (2.2, -1.6, 270), "R29": (2.2, -3.45, 270),
    "R34": (-3.1, -3.95, 270), "R35": (-2.1, -3.95, 270),
    "U3": (-8.95, -9.27, 0), "R31": (-9.0, -14.35, 270), "R32": (-13.1, -15.25, 0), "C26": (-5.81, -13.72, 180),
}
CORE_ZONES = [
    ("+3V3", 7, [(-0.5, 0.75), (-0.5, -0.6), (-0.75, -0.85), (-0.75, -1.4), (-0.3, -1.4), (-0.3, 0.75)]),
    ("GND", 4, [(0.3, 0.3), (0.3, -1.4), (0.45, -1.4), (0.45, -1.85), (0.3, -1.85), (0.3, -2.35), (1.1, -2.35),
                (1.4, -2.05), (1.4, -1.3), (0.5, -0.4), (0.5, 0.3)]),
    ("VREG_LX", 5, [(-0.1, 0.3), (-0.1, -0.5), (-0.15, -0.55), (-0.15, -2.7), (0.35, -3.2), (0.35, -4.6), (1.05, -4.6),
                    (1.05, -2.9), (0.75, -2.6), (0.3, -2.6), (0.15, -2.45), (0.15, -0.55), (0.1, -0.5), (0.1, 0.3)]),
    ("+1V1", 6, [(-0.35, -1.85), (-0.35, -4.6), (-1.05, -4.6), (-1.05, -2.75), (-0.75, -2.45), (-0.75, -2.3),
                 (-0.95, -2.3), (-1.3, -2.65), (-1.5, -2.65), (-1.8, -2.35), (-1.8, -1.75), (-1.5, -1.45), (-1.0, -1.45),
                 (-1.0, -1.65), (-0.45, -1.65), (-0.45, -1.85)]),
]
CORE_TRACKS = [
    ("VREG_AVDD", 0.15, [(0.8, 0.0), (0.8, -0.15), (1.64, -0.99), (1.64, -1.52), (2.2, -2.08), (2.2, -2.97)]),
    ("+1V1", 0.15, [(-0.8, 0.0), (-0.825, -0.025), (-0.825, -0.675), (-1.075, -0.925), (-1.075, -1.675)]),
    ("USB_DP_MCU", 0.15, [(-1.6, 0.0), (-1.6, -0.55), (-3.1, -2.05), (-3.1, -3.47)]),
    ("USB_DM_MCU", 0.15, [(-1.2, 0.0), (-1.225, -0.025), (-1.225, -0.651), (-2.1, -1.526), (-2.1, -3.47)]),
    ("QSPI_SS", 0.15, [(-13.61, -13.85), (-13.61, -15.25)]),
    ("QSPI_SS", 0.15, [(-12.55, -11.18), (-12.59, -11.22), (-12.59, -13.85), (-10.71, -13.85), (-10.7, -13.84),
                       (-9.0, -13.84)]),
    ("QSPI_SS", 0.15, [(-13.61, -13.85), (-12.59, -13.85)]),
    ("QSPI_SD1", 0.15, [(-4.4, 0.0), (-4.4, -0.41), (-5.94, -1.95), (-9.15, -1.95), (-14.0, -6.8), (-14.0, -8.95),
                        (-13.04, -9.91), (-12.55, -9.91)]),
    ("QSPI_SD2", 0.15, [(-4.0, 0.0), (-4.0, -0.45), (-10.4, -6.85), (-10.4, -7.95), (-11.09, -8.64), (-12.55, -8.64)]),
    ("QSPI_SD0", 0.15, [(-3.6, 0.0), (-3.6, -0.55), (-6.0, -2.95), (-6.0, -6.72), (-5.35, -7.37)]),
    ("QSPI_SCLK", 0.15, [(-3.2, 0.0), (-3.2, -0.65), (-5.6, -3.05), (-5.6, -5.35), (-4.2, -6.75), (-4.2, -8.05),
                         (-4.79, -8.64), (-5.35, -8.64)]),
    ("QSPI_SD3", 0.15, [(-2.8, 0.0), (-2.8, -0.75), (-5.2, -3.15), (-5.2, -5.05), (-3.8, -6.45), (-3.8, -8.95),
                        (-4.76, -9.91), (-5.35, -9.91)]),
    ("GND", 0.2, [(2.2, -1.12), (2.85, -1.12)]),
    ("+3V3", 0.2, [(2.2, -3.93), (2.2, -4.7)]),
    ("GND", 0.25, [(-12.55, -7.37), (-11.7, -6.45)]),
]
CORE_VIAS = [("+1V1", -1.5, -2.35), ("+1V1", -1.5, -1.75), ("GND", 1.1, -1.45), ("GND", 1.1, -2.05),
             ("GND", 2.85, -1.12), ("+3V3", 2.2, -4.7), ("GND", -11.7, -6.45)]
# The RP2350B's supply pins (pad centres, chip-relative, unrotated) and the via under the chip each
# joins (HDDSynthPro's): (net, pads, via).
SUPPLY_VIAS = [
    ("+3V3", [(-4.95, -2.2)], (-3.7, -2.2)),               # IOVDD 5
    ("+1V1", [(-4.95, -0.2)], (-3.7, -0.2)),               # DVDD 10
    ("+3V3", [(-4.95, 1.8)], (-3.7, 1.8)),                 # IOVDD 15
    ("+3V3", [(-2.6, 4.95)], (-2.6, 3.7)),                 # IOVDD 24
    ("+3V3", [(-0.6, 4.95)], (-0.6, 3.7)),                 # IOVDD 29
    ("+1V1", [(0.6, 4.95)], (0.6, 3.7)),                   # DVDD 32
    ("+3V3", [(4.95, 3.8)], (3.7, 3.6)),                   # IOVDD 41
    ("+3V3", [(4.95, 0.2)], (3.7, 0.7)),                   # IOVDD 50
    ("+1V1", [(4.95, -0.2)], (3.7, -0.7)),                 # DVDD 51
    ("+3V3", [(4.95, -3.4), (4.95, -3.8)], (3.7, -3.6)),   # ADC_AVDD 59, IOVDD 60
    ("+3V3", [(2.6, -4.95)], (2.6, -3.7)),                 # VREG_VIN 64
    ("+3V3", [(1.0, -4.95), (0.6, -4.95)], (0.8, -3.7)),   # USB_OTP_VDD 68, QSPI_IOVDD 69
    ("+3V3", [(-2.2, -4.95)], (-2.2, -3.7)),               # IOVDD 76
]
CORE_1V1_BOTTOM = [
    [(1.5, -7.3), (1.5, -6.7), (1.7, -6.5), (1.7, -3.0), (3.7, -1.0), (3.7, -0.7)],   # to DVDD 51
    [(1.7, -3.0), (-2.0, -3.0), (-3.0, -2.0), (-3.0, -0.2), (-3.7, -0.2)],            # to DVDD 10
    [(-3.0, -0.2), (-3.0, 2.6), (-0.2, 2.6), (0.6, 3.4), (0.6, 3.7)],                 # to DVDD 32
]
# The 100 nF by each supply pin, chip-relative and unrotated, as HDDSynthPro places them: the chip's
# corners and the unused GPIO 36-47 side; the ones for the busy left side sit beyond its top corner.
DECOUPLING = {
    "C24": (-16.8, -7.0, 90), "C25": (-15.2, -7.0, 90), "C19": (-13.6, -7.0, 90), "C20": (-12.0, -7.0, 90),
    "C12": (-10.4, -7.0, 90),
    "C13": (-6.4, 6.4, 0), "C14": (-6.4, 7.6, 0), "C15": (-2.6, 9.2, 0),
    "C16": (6.4, 6.4, 0), "C21": (6.4, 7.6, 0),
    "C17": (6.8, -2.0, 90), "C22": (8.0, -2.0, 90),
    "C18": (6.6, -5.6, 90), "C23": (7.8, -5.6, 90),
}
REF_HIDDEN = ([f"C{n}" for n in range(1, 37)] + [f"R{n}" for n in range(1, 63)]
              + ["L1", "FB1", "FB2", "H1", "H2", "H3", "H4", "J2", "J3", "J4", "J6", "J11", "J13", "SW1", "SW2",
                 "D2", "J5", "J12", "J14", "R46", "U2", "U6", "U8", "LOGO1"])
SILK = [  # text, x, y, size, rotation (board coordinates)
    ("FDDSynthPro rev C", 66.0, 52.4, 1.2, 0),
    ("10k", 83.6, 10.6, 0.8, 0), ("1k", 81.2, 8.8, 0.8, 0),
    ("J1 FLOPPY", 85.1, 34.0, 0.8, 90),
    ("PC/AMIGA", 75.0, 47.8, 0.8, 90),
    ("ENC", 48.6, 5.2, 0.8, 0), ("OLED", 61.4, 5.2, 0.8, 0), ("BTN", 70.2, 5.2, 0.8, 0),
    ("BOOT", 65.0, 14.6, 0.8, 0), ("RST", 73.0, 14.6, 0.8, 0),
    ("SWD", 36.4, 5.5, 0.8, 90), ("UART", 36.4, 14.5, 0.8, 90),
    ("+5V", 91.4, 61.0, 0.8, 0),
    ("SPK", 45.3, 52.8, 0.8, 0),
    ("J12", 51.0, 64.6, 1.0, 90),
    ("+ LED -", 6.27, 9.6, 0.8, 0),
    ("GPIO 38-41", 69.6, 41.4, 0.8, 0),
]
# The logo (tools/make_logo.py, a board-only footprint) between the speaker and the flash, turned to read
# along the board: x, y, rotation. 1.5 mm clear of the speaker's square (x 41.26), 0.3 mm of the PSRAM_CS via.
LOGO = ("FDDSynthPro_Logo", 47.1, 41.25, 90)
POWER_PATTERNS = ["+5V", "*VBUS_USB", "*5V_FDD", "*5V_FDD_IN"]  # GND and +3V3 are planes
# Power tracks are 0.4 mm, which still enter the LM66200's and the amp's 0.5 mm-pitch pads. The speaker
# lines: about 1 A peak, from neighbouring amp pads, so 0.3 mm (two fit side by side).
AUDIO_PATTERNS = ["*SPK_P*", "*SPK_N*"]
# Where copper can't go: the grille's holes, with a margin, on every layer. The rest of the speaker's
# square only keeps parts off the top; tracks may pass under the speaker.
GRILLE = (4.8, 24.5, 37.7, 55.2)
# USB D+ and D- leave the 27R resistors (R34, R35, inside Raspberry Pi's cluster, which the QSPI fan-out
# hems in once the chip is turned) by a fixed stub and via each, so the router takes them on underneath.
FIXED_TRACKS = [("USB_DP", 0.2, [(52.1, 31.41), (52.1, 32.8)]), ("USB_DM", 0.2, [(51.1, 31.41), (51.1, 32.8)])]
FIXED_VIAS = [("USB_DP", 52.1, 32.8), ("USB_DM", 51.1, 32.8)]



def mm(v):
    return pcbnew.FromMM(v)


def vec(x, y):
    """Board coordinates to KiCad's, on the sheet."""
    return pcbnew.VECTOR2I(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y))


def rot(dx, dy, deg):
    """Turn a (y-down) offset as KiCad turns a footprint by deg (counterclockwise on screen)."""
    for _ in range(int(deg) // 90 % 4):
        dx, dy = dy, -dx
    return dx, dy


def netlist():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "board.net"
        subprocess.run([KICAD_CLI, "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out),
                        str(HW / f"{PROJECT}.kicad_sch")], check=True, capture_output=True)
        root = parse(out.read_text(encoding="utf8"))
    comps = {}
    for c in find(first(root, "components"), "comp"):
        ref = first(c, "ref")[1]
        sheet = first(first(c, "sheetpath"), "tstamps")[1]
        tstamps = first(c, "tstamps")
        fields = {f[1][1]: (f[2] if len(f) > 2 else "") for f in find(first(c, "fields"), "field")}
        fields.pop("Footprint", None)
        props = {first(p, "name")[1]: (first(p, "value") or [None, None])[1] for p in find(c, "property")}
        comps[ref] = {"value": first(c, "value")[1], "footprint": first(c, "footprint")[1],
                      "path": sheet + tstamps[1], "fields": fields, "in_bom": "exclude_from_bom" not in props,
                      "dnp": "dnp" in props}
    nets = {}
    for n in find(first(root, "nets"), "net"):
        name = first(n, "name")[1]
        nets[name] = [(first(x, "ref")[1], first(x, "pin")[1]) for x in find(n, "node")]
    return comps, nets


def load_footprint(fpid):
    lib, name = fpid.split(":")
    path = HW / f"{lib}.pretty" if lib == LIB else KICAD_SHARE / "footprints" / f"{lib}.pretty"
    fp = pcbnew.FootprintLoad(str(path), name)
    if fp is None:
        raise FileNotFoundError(fpid)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def outline(board):
    """The HDD Synth MK5's outline, speaker holes and grille (hardware/mk5_outline.py), as rev B."""
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


def rule_area(board, name, pts, layers, **allow):
    """A keep-out on the given layers; allow=True for anything it lets through."""
    area = pcbnew.ZONE(board)
    area.SetIsRuleArea(True)
    lset = pcbnew.LSET()
    for layer in layers:
        lset.AddLayer(layer)
    area.SetLayerSet(lset)
    for flag in ("Tracks", "Vias", "Pads", "Footprints", "ZoneFills"):
        getattr(area, f"SetDoNotAllow{flag}")(not allow.get(flag.lower(), False))
    area.SetZoneName(name)
    ol = area.Outline()
    ol.NewOutline()
    for x, y in pts:
        ol.Append(vec(x, y))
    board.Add(area)


def plane(board, net, layer, name):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net)
    z.SetZoneName(name)
    z.SetLocalClearance(mm(0.3))
    z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    ol = z.Outline()
    ol.NewOutline()
    m = 0.4
    for x, y in [(m, m), (W - m, m), (W - m, H - m), (m, H - m)]:
        ol.Append(vec(x, y))
    board.Add(z)


def design_rules(project_file):
    """Net classes and limits for a 4-layer board at JLCPCB-class fabs, with margin (HDDSynthPro's).
    KiCad keeps these in the project file, not the board, so they're written there."""
    pro = json.loads(project_file.read_text(encoding="utf8")) if project_file.exists() else {}
    base = {"bus_width": 12, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2,
            "line_style": 0, "microvia_diameter": 0.3, "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)",
            "schematic_color": "rgba(0, 0, 0, 0.000)", "tuning_profile": "", "wire_width": 6}
    pro["net_settings"] = {
        "classes": [
            {**base, "name": "Default", "priority": 2147483647, "clearance": 0.15, "track_width": 0.2,
             "via_diameter": 0.6, "via_drill": 0.3},
            {**base, "name": "Power", "priority": 0, "clearance": 0.15, "track_width": 0.4,
             "via_diameter": 0.8, "via_drill": 0.4},
            {**base, "name": "Audio", "priority": 1, "clearance": 0.15, "track_width": 0.3,
             "via_diameter": 0.6, "via_drill": 0.3},
        ],
        "meta": {"version": 5}, "net_colors": None, "netclass_assignments": None,
        "netclass_patterns": [{"netclass": "Power", "pattern": p} for p in POWER_PATTERNS]
                             + [{"netclass": "Audio", "pattern": p} for p in AUDIO_PATTERNS],
    }
    rules = pro.setdefault("board", {}).setdefault("design_settings", {}).setdefault("rules", {})
    rules.update({"min_clearance": 0.12, "min_track_width": 0.1, "min_via_diameter": 0.5,
                  "min_via_annular_width": 0.1, "min_through_hole_diameter": 0.2, "min_hole_to_hole": 0.25,
                  "min_copper_edge_clearance": 0.3, "min_resolved_spokes": 1})
    project_file.write_text(json.dumps(pro, indent=2) + "\n", encoding="utf8")


def lx_pad():
    """The RP2350B's VREG_LX pin (63): (3.0, -4.95) from the QFN-80's centre, unrotated."""
    x, y, r = U1
    dx, dy = rot(3.0, -4.95, r)
    return x + dx, y + dy


def core_copper(board):
    """Raspberry Pi's regulator copper and the RP2350B's supply stubs (HDDSynthPro's), turned with the chip."""
    def net(name):
        for n, item in board.GetNetsByName().items():
            if str(n) == name or str(n).endswith("/" + name):
                return item
        raise KeyError(name)

    lx, ly = lx_pad()
    cx, cy, r = U1

    def at_lx(dx, dy):
        dx, dy = rot(dx, dy, r)
        return lx + dx, ly + dy

    def at_chip(dx, dy):
        dx, dy = rot(dx, dy, r)
        return cx + dx, cy + dy

    def track(netname, pts, width, layer=pcbnew.F_Cu):
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            tr = pcbnew.PCB_TRACK(board)
            tr.SetStart(vec(x0, y0))
            tr.SetEnd(vec(x1, y1))
            tr.SetWidth(mm(width))
            tr.SetLayer(layer)
            tr.SetNet(net(netname))
            tr.SetLocked(True)
            board.Add(tr)

    def via(netname, x, y):
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(vec(x, y))
        v.SetWidth(mm(0.5))
        v.SetDrill(mm(0.3))
        v.SetNet(net(netname))
        v.SetLocked(True)
        board.Add(v)

    # Raspberry Pi's cluster keeps 0.125 mm in places; fddsynthpro_revc.kicad_dru allows that here only.
    area = pcbnew.ZONE(board)
    area.SetIsRuleArea(True)
    area.SetLayer(pcbnew.F_Cu)
    for flag in ("Tracks", "Vias", "Pads", "Footprints", "ZoneFills"):
        getattr(area, f"SetDoNotAllow{flag}")(False)
    area.SetZoneName("RP2350 core")
    ol = area.Outline()
    ol.NewOutline()
    for dx, dy in [(-3.8, -4.8), (2.8, -4.8), (2.8, 0.5), (-3.8, 0.5)]:
        ol.Append(vec(*at_lx(dx, dy)))
    board.Add(area)

    for netname, prio, pts in CORE_ZONES:
        z = pcbnew.ZONE(board)
        z.SetLayer(pcbnew.F_Cu)
        z.SetNet(net(netname))
        z.SetAssignedPriority(prio)
        z.SetLocalClearance(mm(0.15))
        z.SetMinThickness(mm(0.12))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
        z.SetLocked(True)
        ol = z.Outline()
        ol.NewOutline()
        for dx, dy in pts:
            ol.Append(vec(*at_lx(dx, dy)))
        board.Add(z)
    for netname, width, pts in CORE_TRACKS:
        track(netname, [at_lx(*p) for p in pts], width)
    for netname, dx, dy in CORE_VIAS:
        via(netname, *at_lx(dx, dy))
    for netname, width, pts in FIXED_TRACKS:
        track(netname, pts, width)
    for netname, x, y in FIXED_VIAS:
        via(netname, x, y)
    for pts in CORE_1V1_BOTTOM:
        track("+1V1", [at_chip(*p) for p in pts], 0.4, pcbnew.B_Cu)
    for netname, pads, (vx, vy) in SUPPLY_VIAS:
        for px, py in pads:
            k = 4.3 / 4.95  # straight in past the pad's inner end, then across to the via
            mid = (px * k if abs(px) > 4 else px, py * k if abs(py) > 4 else py)
            track(netname, [at_chip(px, py), at_chip(*mid), at_chip(vx, vy)], 0.2)
        via(netname, *at_chip(vx, vy))


def place_logo(board):
    """Adds the logo from the library at LOGO. Returns the copies already on the board, for the caller to
    remove last (KiCad 10's bindings misbehave after a Remove)."""
    name, x, y, r = LOGO
    old = [f for f in board.GetFootprints() if f.GetFPID().GetLibItemName() == name]
    fp = load_footprint(f"{LIB}:{name}")
    fp.SetReference("LOGO1")
    fp.Reference().SetVisible(False)
    fp.SetPosition(vec(x, y))
    fp.SetOrientationDegrees(r)
    board.Add(fp)
    return old


def silkscreen(board):
    for text, x, y, size, angle in SILK:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetPosition(vec(x, y))
        t.SetTextAngleDegrees(angle)
        t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(size * 0.15))
        board.Add(t)


def redo_silkscreen(path):
    """Redo only the labels and which references show, on a board that's already routed. Each label is
    found by its text and moved (KiCad 10's Python bindings crash after removing items); new ones are added."""
    board = pcbnew.LoadBoard(str(path))
    texts = {d.GetText(): d for d in board.GetDrawings()
             if d.GetClass() == "PCB_TEXT" and d.GetLayer() == pcbnew.F_SilkS}
    for text, x, y, size, angle in SILK:
        t = texts.get(text)
        if t is None:
            t = pcbnew.PCB_TEXT(board)
            t.SetText(text)
            t.SetLayer(pcbnew.F_SilkS)
            board.Add(t)
        t.SetPosition(vec(x, y))
        t.SetTextAngleDegrees(angle)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(size * 0.15))
    old_logos = place_logo(board)
    for fp in board.GetFootprints():
        fp.Reference().SetVisible(fp.GetReference() not in REF_HIDDEN)
    # The old logo and labels no longer in SILK go last: KiCad 10's bindings misbehave after a Remove,
    # but saving works.
    labels = {s[0] for s in SILK}
    for item in old_logos + [t for text, t in texts.items() if text not in labels]:
        board.Remove(item)
    board.Save(str(path))
    print(f"silkscreen redone on {path}")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--silkscreen":
        redo_silkscreen(Path(sys.argv[2]))
        return
    comps, nets = netlist()
    lx, ly = lx_pad()
    for ref, (dx, dy, r) in CORE.items():
        ox, oy = rot(dx, dy, U1[2])
        PLACE[ref] = (lx + ox, ly + oy, (r + U1[2]) % 360)
    for ref, (dx, dy, r) in DECOUPLING.items():
        ox, oy = rot(dx, dy, U1[2])
        PLACE[ref] = (U1[0] + ox, U1[1] + oy, (r + U1[2]) % 360)
    for i, (hx, hy) in enumerate(mk5_outline.MOUNTING_HOLES, start=1):
        PLACE[f"H{i}"] = (hx, hy, 0)
    missing = set(comps) - set(PLACE)
    if missing:
        raise KeyError(f"no position for {sorted(missing)}")

    board = pcbnew.CreateEmptyBoard()
    board.SetCopperLayerCount(4)
    board.SetLayerName(pcbnew.In1_Cu, "GND")
    board.SetLayerName(pcbnew.In2_Cu, "3V3")
    outline(board)

    net_items = {}
    for name in nets:
        item = pcbnew.NETINFO_ITEM(board, name)
        board.Add(item)
        net_items[name] = item
    pad_net = {key: name for name, nodes in nets.items() for key in nodes}

    for ref, c in sorted(comps.items()):
        fp = load_footprint(c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH(c["path"]))
        for name, text in c["fields"].items():  # as "Update PCB from Schematic" would
            fp.SetField(name, text)
            fp.GetField(name).SetVisible(False)
        fp.SetExcludedFromBOM(not c["in_bom"])
        fp.SetDNP(c["dnp"])
        x, y, r = PLACE[ref]
        fp.SetPosition(vec(x, y))
        fp.SetOrientationDegrees(r)
        if ref in REF_HIDDEN:
            fp.Reference().SetVisible(False)
        board.Add(fp)
        for pad in fp.Pads():
            name = pad_net.get((ref, pad.GetNumber()))
            if name:
                pad.SetNet(net_items[name])

    silkscreen(board)
    place_logo(board)
    core_copper(board)
    # The speaker's square: no parts on top. No copper on any layer over the grille, nor tracks or vias
    # round the speaker's screws (their nuts sit underneath).
    sx0, sy0, sx1, sy1 = SPEAKER
    rule_area(board, "Speaker", [(sx0, sy0), (sx1, sy0), (sx1, sy1), (sx0, sy1)], [pcbnew.F_Cu],
              tracks=True, vias=True, zonefills=True)
    gx0, gy0, gx1, gy1 = GRILLE
    every = [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]
    rule_area(board, "Grille", [(gx0, gy0), (gx1, gy0), (gx1, gy1), (gx0, gy1)], every, footprints=True, pads=True)
    for k, (hx, hy, r) in enumerate(h for h in mk5_outline.HOLES if h[2] > 1.5):
        d = r + 1.6
        rule_area(board, f"Speaker screw {k + 1}", [(hx - d, hy - d), (hx + d, hy - d), (hx + d, hy + d), (hx - d, hy + d)],
                  every, footprints=True, pads=True, zonefills=True)
    plane(board, net_items["GND"], pcbnew.In1_Cu, "GND plane")
    plane(board, net_items["+3V3"], pcbnew.In2_Cu, "3V3 plane")

    out = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    board.Save(str(out))
    design_rules(out.with_suffix(".kicad_pro"))
    print(f"wrote {out}: {len(comps)} parts, {len(nets)} nets")


if __name__ == "__main__":
    main()
