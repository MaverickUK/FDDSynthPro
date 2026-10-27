"""The carrier board's schematic (hardware/) against the firmware's pin map
(src/config.h): KiCad exports the netlist, and every connection is checked.
Skips without kicad-cli (CI has none); run it after changing either.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys

import pytest

from fddproto.paths import REPO

sys.path.insert(0, str(REPO / "hardware"))
import sexp as sexp_lite  # noqa: E402  (hardware/sexp.py, the generator's reader)

pytestmark = pytest.mark.sim

SCHEMATIC = REPO / "hardware/fddsynthpro_carrier.kicad_sch"
FLOPPY_PIN = {"DENSEL": 2, "INDEX": 8, "MOTEA": 10, "DRVSB": 12, "DRVSA": 14, "MOTEB": 16, "DIR": 18,
              "STEP": 20, "WDATA": 22, "WGATE": 24, "TRK00": 26, "WPT": 28, "RDATA": 30, "SIDE1": 32,
              "DSKCHG": 34}
INPUTS = ["DENSEL", "MOTEA", "DRVSB", "DRVSA", "MOTEB", "DIR", "STEP", "WDATA", "WGATE", "SIDE1"]
# The SN7407's A and Y pins for each output (gates 1-5).
BUFFER = {"INDEX": ("1", "2"), "TRK00": ("3", "4"), "WPT": ("5", "6"), "RDATA": ("9", "8"), "DSKCHG": ("11", "10")}


def config_pins() -> dict[str, int]:
    text = (REPO / "src/config.h").read_text()
    return {m[1]: int(m[2]) for m in re.finditer(r"constexpr unsigned PIN_(\w+) = (\d+);", text)}


def bb48_pad(gpio: int) -> str:
    """GPIO 0-23 are EXT1 pins 4-27 (pads 4-27); GPIO 24-47 are EXT2 pins 4-27 (pads 31-54)."""
    return str(gpio + 4) if gpio <= 23 else str(27 + gpio - 20)


@pytest.fixture(scope="module")
def nets(tmp_path_factory) -> dict[str, set[tuple[str, str]]]:
    cli = shutil.which("kicad-cli")
    if not cli:
        pytest.skip("kicad-cli not installed")
    out = tmp_path_factory.mktemp("netlist") / "carrier.net"
    subprocess.run([cli, "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out), str(SCHEMATIC)],
                   check=True, capture_output=True)
    tree = sexp_lite.parse(out.read_text())
    result = {}
    for net in sexp_lite.find(sexp_lite.first(tree, "nets"), "net"):
        name = sexp_lite.first(net, "name")[1]
        result[name] = {(sexp_lite.first(n, "ref")[1], sexp_lite.first(n, "pin")[1])
                        for n in sexp_lite.find(net, "node")}
    return result


def net_of(nets, ref: str, pin: str) -> set[tuple[str, str]]:
    for nodes in nets.values():
        if (ref, pin) in nodes:
            return nodes
    return set()


def test_inputs(nets):
    """Each input: the floppy pin, its GPIO, its 10k pull-up and its 1k
    pull-up (to PC_TERM), and nothing else."""
    pins = config_pins()
    for i, sig in enumerate(INPUTS):
        want = {("J1", str(FLOPPY_PIN[sig])), ("U1", bb48_pad(pins[sig])), (f"R{i + 1}", "1"), (f"R{i + 11}", "1")}
        assert net_of(nets, "J1", str(FLOPPY_PIN[sig])) == want, sig


def test_pull_ups(nets):
    """10k to 3.3 V always; 1k to PC_TERM, which J7's jumper joins to 3.3 V."""
    v33 = net_of(nets, "U1", "1")
    assert {(f"R{i}", "2") for i in range(1, 11)} | {("J7", "2")} <= v33
    term = net_of(nets, "J7", "1")
    assert term == {(f"R{i}", "2") for i in range(11, 21)} | {("J7", "1")}


def test_outputs_through_the_sn7407(nets):
    """GPIO to the buffer's A, its Y to the floppy pin."""
    pins = config_pins()
    for sig, (a, y) in BUFFER.items():
        assert net_of(nets, "U1", bb48_pad(pins[sig])) == {("U1", bb48_pad(pins[sig])), ("U2", a)}, sig
        assert net_of(nets, "J1", str(FLOPPY_PIN[sig])) == {("J1", str(FLOPPY_PIN[sig])), ("U2", y)}, sig


def test_power_and_ground(nets):
    gnd = net_of(nets, "U1", "2")
    assert {("J1", str(p)) for p in range(1, 35, 2)} <= gnd
    assert {("U1", "30"), ("U2", "7"), ("U2", "13"), ("C1", "2")} <= gnd  # EXT2-3, the SN7407, its unused gate
    vbus = net_of(nets, "U1", "28")  # EXT2-1
    assert {("U2", "14"), ("C1", "1"), ("J5", "7")} <= vbus
    v33 = net_of(nets, "U1", "1")  # EXT1-1
    assert ("R1", "2") in v33 and ("U2", "14") not in v33
    assert {(f"H{k}", "1") for k in range(1, 5)} <= gnd  # the mounting holes, as on the HDD Synth


def test_board_own_pins_are_free(nets):
    """GP8-11 and GP24 belong to the BB48R's PSRAM and SD slot."""
    for gpio in (8, 9, 10, 11, 24):
        assert net_of(nets, "U1", bb48_pad(gpio)) in (set(), {("U1", bb48_pad(gpio))}), f"GP{gpio} is wired"


def test_panel_amp_and_oled(nets):
    pins = config_pins()
    expect = {"ENC_A": ("J3", "1"), "ENC_B": ("J3", "2"), "ENC_BUTTON": ("J3", "3"), "ACTION_BUTTON": ("J4", "1"),
              "OLED_SCL": ("J2", "3"), "OLED_SDA": ("J2", "4"), "I2S_DATA": ("J5", "3"), "I2S_BCLK": ("J5", "2")}
    for name, node in expect.items():
        assert net_of(nets, "U1", bb48_pad(pins[name])) == {("U1", bb48_pad(pins[name])), node}, name
    lrc = pins["I2S_BCLK"] + 1  # LRCLK follows BCLK
    assert net_of(nets, "U1", bb48_pad(lrc)) == {("U1", bb48_pad(lrc)), ("J5", "1")}
