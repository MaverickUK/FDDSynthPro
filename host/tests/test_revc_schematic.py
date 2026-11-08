"""The rev C board's schematic (hardware/revc/) against the firmware's pin map
(src/config.h): KiCad exports the netlist, and every connection is checked.
The RP2350B's pins are found by name (GPIO12 and so on), so the test reads
the same map the firmware does. Skips without kicad-cli (CI has none); set
KICAD_CLI if it isn't on the PATH.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys

import pytest

from fddproto.paths import REPO

sys.path.insert(0, str(REPO / "hardware/revc/tools"))
import sexpr  # noqa: E402  (the generator's reader)

pytestmark = pytest.mark.sim

SCHEMATIC = REPO / "hardware/revc/fddsynthpro_revc.kicad_sch"
FLOPPY_PIN = {"DENSEL": 2, "INDEX": 8, "MOTEA": 10, "DRVSB": 12, "DRVSA": 14, "MOTEB": 16, "DIR": 18,
              "STEP": 20, "WDATA": 22, "WGATE": 24, "TRK00": 26, "WPT": 28, "RDATA": 30, "SIDE1": 32,
              "DSKCHG": 34, "CHNG": 2}
INPUTS = ["DENSEL", "MOTEA", "DRVSB", "DRVSA", "MOTEB", "DIR", "STEP", "WDATA", "WGATE", "SIDE1"]
# Floppy pins 4 and 6: read only, on GPIOs rev C adds (not in config.h until the firmware plans them).
# (floppy pin, GPIO, 10k pull-up, 1k series resistor)
SPARE_INPUTS = [(4, 36, "R57", "R59"), (6, 42, "R58", "R60")]
# The SN7407's A and Y pins for each output (gates 1-6); its pull-up on the GPIO side.
BUFFER = {"INDEX": ("1", "2", "R21"), "TRK00": ("3", "4", "R22"), "WPT": ("5", "6", "R23"),
          "RDATA": ("9", "8", "R24"), "DSKCHG": ("11", "10", "R25"), "CHNG": ("13", "12", "R26")}


def config_pins() -> dict[str, int]:
    text = (REPO / "src/config.h").read_text(encoding="utf8")
    return {m[1]: int(m[2]) for m in re.finditer(r"constexpr unsigned PIN_(\w+) = (\d+);", text)}


@pytest.fixture(scope="module")
def netlist(tmp_path_factory):
    cli = os.environ.get("KICAD_CLI") or shutil.which("kicad-cli")
    if not cli:
        pytest.skip("kicad-cli not installed")
    out = tmp_path_factory.mktemp("netlist") / "revc.net"
    subprocess.run([cli, "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out), str(SCHEMATIC)],
                   check=True, capture_output=True)
    tree = sexpr.parse(out.read_text(encoding="utf8"))
    nets, gpio_pad = {}, {}
    for net in sexpr.find(sexpr.first(tree, "nets"), "net"):
        name = sexpr.first(net, "name")[1]
        nodes = set()
        for n in sexpr.find(net, "node"):
            ref, pin = sexpr.first(n, "ref")[1], sexpr.first(n, "pin")[1]
            nodes.add((ref, pin))
            func = sexpr.first(n, "pinfunction")
            # KiCad 10 names a pin "GPIO12_11" (name, then pad), so stop at the first non-digit.
            m = re.match(r"GPIO(\d+)(?!\d)", func[1]) if (ref == "U1" and func) else None
            if m:
                gpio_pad[int(m[1])] = pin
        nets[name] = nodes
    return nets, gpio_pad


def net_of(nets, ref: str, pin: str) -> set[tuple[str, str]]:
    for nodes in nets.values():
        if (ref, pin) in nodes:
            return nodes
    return set()


def gp(netlist, gpio: int) -> tuple[str, str]:
    _, gpio_pad = netlist
    assert gpio in gpio_pad, f"GPIO{gpio} is not connected"
    return ("U1", gpio_pad[gpio])


def test_inputs(netlist):
    """Each input: the floppy pin with its 10k and 1k pull-ups, then a 1k in series (R47-R56) to its GPIO,
    which limits what a host can push into the RP2350 while it's unpowered. Pin 2 is also gate 6's output."""
    nets, _ = netlist
    pins = config_pins()
    for i, sig in enumerate(INPUTS):
        want = {("J1", str(FLOPPY_PIN[sig])), (f"R{i + 1}", "2"), (f"R{i + 11}", "2"), (f"R{i + 47}", "1")}
        if sig == "DENSEL":
            want.add(("U2", BUFFER["CHNG"][1]))
        assert net_of(nets, "J1", str(FLOPPY_PIN[sig])) == want, sig
        assert net_of(nets, *gp(netlist, pins[sig])) == {gp(netlist, pins[sig]), (f"R{i + 47}", "2")}, sig


def test_pins_4_and_6_are_read_only(netlist):
    """Floppy pins 4 and 6: a 10k pull-up and a 1k in series to a GPIO; no 1k termination, nothing driving them."""
    nets, _ = netlist
    for pin, gpio, pull, series in SPARE_INPUTS:
        assert net_of(nets, "J1", str(pin)) == {("J1", str(pin)), (pull, "2"), (series, "1")}, pin
        assert net_of(nets, *gp(netlist, gpio)) == {gp(netlist, gpio), (series, "2")}, pin
        assert (pull, "1") in nets["+3V3"], pin
        assert gpio not in config_pins().values(), f"GPIO{gpio} is taken in config.h"


def test_board_id_strap(netlist):
    """GPIO 37 tied low through 4.7k (under the 8.2k erratum E9 asks for), so the firmware can tell rev C
    from the BB48R, which leaves it open: with the pull-up on, it reads low here and high there."""
    nets, _ = netlist
    assert net_of(nets, *gp(netlist, 37)) == {gp(netlist, 37), ("R62", "1")}
    assert ("R62", "2") in nets["GND"]
    assert 37 not in config_pins().values(), "GPIO37 is taken in config.h"


def test_amp_gain_pad(netlist):
    """The amp's GAIN pin is open (9 dB); R61, not fitted, would take it to GND (12 dB, or 15 dB with 100k)."""
    nets, _ = netlist
    assert net_of(nets, "U5", "2") == {("U5", "2"), ("R61", "1")}
    assert ("R61", "2") in nets["GND"]


def test_spare_gpio_header(netlist):
    """J14 (not fitted): 3V3, GND and GPIO 38-41, nothing else on them."""
    nets, _ = netlist
    assert ("J14", "1") in nets["+3V3"] and ("J14", "2") in nets["GND"]
    for pin, gpio in zip(("3", "4", "5", "6"), (38, 39, 40, 41)):
        assert net_of(nets, *gp(netlist, gpio)) == {gp(netlist, gpio), ("J14", pin)}, gpio
        assert gpio not in config_pins().values(), f"GPIO{gpio} is taken in config.h"


def test_inputs_are_consecutive():
    """One read captures all ten inputs (CLAUDE.md)."""
    pins = config_pins()
    assert [pins[s] for s in INPUTS] == list(range(pins["DENSEL"], pins["DENSEL"] + 10))


def test_pull_ups(netlist):
    """10k to 3.3 V always; 1k to PC_TERM, which J7's jumper joins to 3.3 V. Each pull-up's
    supply end is its pin 1."""
    nets, _ = netlist
    v33 = nets["+3V3"]
    assert {(f"R{i}", "1") for i in range(1, 11)} | {("J7", "2")} <= v33
    assert net_of(nets, "J7", "1") == {(f"R{i}", "1") for i in range(11, 21)} | {("J7", "1")}


def test_outputs_through_the_sn7407(netlist):
    """GPIO to the buffer's A (held high by a 10k until the firmware drives it), its Y to the floppy pin."""
    nets, _ = netlist
    pins = config_pins()
    for sig, (a, y, pull) in BUFFER.items():
        assert net_of(nets, *gp(netlist, pins[sig])) == {gp(netlist, pins[sig]), ("U2", a), (pull, "2")}, sig
        assert (pull, "1") in nets["+3V3"], sig
        if sig != "CHNG":  # pin 2's net is DENSEL's, checked in test_inputs
            assert net_of(nets, "J1", str(FLOPPY_PIN[sig])) == {("J1", str(FLOPPY_PIN[sig])), ("U2", y)}, sig


def test_power_and_ground(netlist):
    nets, _ = netlist
    gnd, v5, v33 = nets["GND"], nets["+5V"], nets["+3V3"]
    assert {("J1", str(p)) for p in range(1, 35, 2)} <= gnd
    assert {("U2", "7"), ("J8", "2"), ("J8", "3"), ("U6", "4")} <= gnd  # U6's ~ON tied low: always on
    assert {(f"H{k}", "1") for k in range(1, 5)} <= gnd  # the mounting holes, as on the HDD Synth
    assert {("U2", "14"), ("U5", "7"), ("U6", "2"), ("U7", "3")} <= v5  # SN7407, amp, mux out, LDO in
    assert ("U7", "2") in v33
    assert net_of(nets, "J8", "4") <= {("J8", "4")}  # +12V unused
    fdd = net_of(nets, "U6", "3")  # VIN1: the floppy connector, through the polyfuse
    assert ("F1", "2") in fdd and net_of(nets, "F1", "1") == {("F1", "1"), ("J8", "1")}
    usb = net_of(nets, "U6", "6")  # VIN2: USB-C's VBUS
    assert {("J9", "A4"), ("J9", "B9")} <= usb


def test_psram_and_sd_on_the_bb48r_pins(netlist):
    """The firmware's PSRAM and SD set-up assume the BB48R's pins."""
    nets, _ = netlist
    pins = config_pins()
    assert ("U4", "1") in net_of(nets, *gp(netlist, pins["PSRAM_CS"]))
    sd = {"SD_CS": "2", "SD_SCK": "5", "SD_MOSI": "3", "SD_MISO": "7"}  # J10 pins: DAT3, CLK, CMD, DAT0
    for name, pad in sd.items():
        assert ("J10", pad) in net_of(nets, *gp(netlist, pins[name])), name
    for qspi in ("1", "2", "3", "5", "6", "7"):  # PSRAM shares the flash's bus, except chip select
        if qspi != "1":
            assert net_of(nets, "U4", qspi) == net_of(nets, "U3", qspi), qspi


def test_panel_amp_and_oled(netlist):
    nets, _ = netlist
    pins = config_pins()
    expect = {"ENC_A": ("J3", "1"), "ENC_B": ("J3", "2"), "ENC_BUTTON": ("J3", "3"), "ACTION_BUTTON": ("J4", "1"),
              "OLED_SCL": ("J2", "3"), "OLED_SDA": ("J2", "4"), "I2S_DATA": ("U5", "1"), "I2S_BCLK": ("U5", "16"),
              "BOARD_LED": ("R38", "1")}
    for name, node in expect.items():
        assert node in net_of(nets, *gp(netlist, pins[name])), name
    lrc = pins["I2S_BCLK"] + 1  # LRCLK follows BCLK
    assert ("U5", "14") in net_of(nets, *gp(netlist, lrc))
    assert ("R36", "2") in net_of(nets, *gp(netlist, pins["OLED_SDA"]))  # 2.2k, as the BB48R's R17/R18


def test_every_configured_pin_is_wired(netlist):
    """Every PIN_ in config.h reaches something on the board (the direct builds' pins are the same GPIOs)."""
    nets, _ = netlist
    for name, gpio in config_pins().items():
        assert len(net_of(nets, *gp(netlist, gpio))) > 1, f"PIN_{name} (GPIO{gpio}) goes nowhere"


def test_gpio_0_to_3_stay_off_the_interface(netlist):
    """GPIO 0-3: the debug UART and the OLED's I2C, never a floppy line (CLAUDE.md)."""
    nets, _ = netlist
    for gpio in range(4):
        assert not any(ref in ("J1", "U2") for ref, _ in net_of(nets, *gp(netlist, gpio))), gpio
