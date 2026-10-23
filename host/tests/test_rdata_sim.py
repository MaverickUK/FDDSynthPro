"""Runs the real src/fdd_rdata.pio in the simulator, fed the words the
firmware's DMA sends (test/tools/build_track --cylinder). No hardware needed.

Checks that every 1 cell becomes one pulse of the right width at the right
moment, for either head; that a head switch keeps the rotation's phase; that
Greaseweazle's decoder reads sectors back from the pulse timings alone; and
that a starved FIFO never leaves RDATA asserted.
"""
from __future__ import annotations

import os
import subprocess

import pytest

from fddproto import gen_disk
from fddproto.paths import REPO
from fddproto.pio_sim import GPIO_OFFSET, PioSim

pytestmark = pytest.mark.sim

TOOL = REPO / ".pio/native-tests/build_track"
RDATA = 17 - GPIO_OFFSET   # sample bits (src/config.h)
SIDE1 = 13 - GPIO_OFFSET
IDLE = 0x3FF               # every input released
CYLINDER = 3


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("fdd_rdata")


@pytest.fixture(scope="module")
def slots() -> list[int]:
    """Cylinder 3 of the generated 1.44 MB disk, as slot words."""
    if not TOOL.exists():
        message = "build_track not built: run test/run_native_tests.sh"
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)
    raw = subprocess.run([str(TOOL), "1440", "--cylinder", str(CYLINDER)], check=True,
                         capture_output=True).stdout
    return [int.from_bytes(raw[i:i + 4], "big") for i in range(0, len(raw), 4)]


def cell(slots: list[int], slot: int, head: int) -> int:
    word = slots[slot // 16]
    return (word >> (31 - 2 * (slot % 16) - head)) & 1


def stream(program, words, n_slots: int, head_at=lambda slot: 0, feed: int | None = None):
    """Runs the program for n_slots cell slots. head_at(slot) is the head the
    host selects during that slot. `feed` limits how many words reach the TX
    FIFO (None: as many as it takes). Returns (pulses, sim): each pulse is
    (start cycle, width in cycles)."""
    c = program.defines["CELL_CYCLES"]
    sim = PioSim(program, sideset_base=RDATA, jmp_pin=SIDE1, out_shift_right=False, autopull=True)
    sim.pin_out = 1 << RDATA  # the firmware sets the pin high before starting
    words = iter(words if feed is None else words[:feed])
    pulses, low_since = [], None
    seen = [IDLE, IDLE]  # the input synchroniser: pins arrive two cycles late
    for k in range(n_slots * c):
        while len(sim.tx_fifo) < 8:
            w = next(words, None)
            if w is None:
                break
            sim.tx_fifo.append(w)
        pins = IDLE & ~(1 << SIDE1) if head_at(k // c) else IDLE
        seen.append(pins)
        sim.step(seen.pop(0))
        low = not (sim.pin_out >> RDATA) & 1
        if low and low_since is None:
            low_since = k
        elif not low and low_since is not None:
            pulses.append((low_since, k - low_since))
            low_since = None
    return pulses, sim


def expected(program, slots, n_slots: int, head_at=lambda slot: 0) -> list[tuple[int, int]]:
    c, start, width = (program.defines[k] for k in ("CELL_CYCLES", "PULSE_START", "PULSE_CYCLES"))
    return [(i * c + start, width) for i in range(n_slots) if cell(slots, i, head_at(i))]


def test_program_constants(program):
    d = program.defines
    assert d["CELL_CYCLES"] == 16  # rdata::CELL_CYCLES
    # A pulse ends with time to spare before the next can start: 1 cells are
    # always at least two slots apart in MFM.
    assert d["PULSE_START"] + d["PULSE_CYCLES"] < d["CELL_CYCLES"]


@pytest.mark.parametrize("head", [0, 1])
def test_pulses_match_cells(program, slots, head):
    n = 4000  # gap 4a, the index mark and the first ID field
    pulses, sim = stream(program, slots, n, head_at=lambda slot: head)
    assert pulses == expected(program, slots, n, head_at=lambda slot: head)
    assert not sim.tx_stall


def test_head_switch_keeps_phase(program, slots):
    """The host switches heads mid-track; the stream carries on from the same
    point of the rotation on the other head, from the very next slot."""
    def head_at(slot):
        return 1 if 1000 <= slot < 2500 else 0
    n = 3500
    pulses, _ = stream(program, slots, n, head_at)
    assert pulses == expected(program, slots, n, head_at)


@pytest.mark.parametrize("head", [0, 1])
def test_greaseweazle_decodes_the_pulses(program, slots, head):
    """The first two sectors, decoded by Greaseweazle from the pulse times
    alone, as `gw read` would see them on the cable."""
    from greaseweazle.codec import codec
    from greaseweazle.flux import Flux

    n = 1500 * 16  # past the end of sector 2's data field
    pulses, _ = stream(program, slots, n, head_at=lambda slot: head)
    starts = [s for s, _ in pulses]
    intervals = [b - a for a, b in zip([0] + starts, starts)]
    c = program.defines["CELL_CYCLES"]
    # Sample ticks are PIO cycles: 16 MHz for 1 us cells (HD). The index list
    # is one whole revolution in ticks, though only its start was streamed.
    flux = Flux([200_000 * c], intervals, c * 1_000_000)

    track = codec.get_diskdef("ibm.1440").decode_flux(CYLINDER, head, flux)
    first_lba = (CYLINDER * 2 + head) * 18
    for s in range(2):
        assert track.has_sec(s), f"sector {s + 1} not found"
        assert bytes(track.sectors[s].dam.data) == gen_disk.sector(first_lba + s)
    assert track.nr_missing() == 16  # the rest weren't streamed


def test_starved_fifo_leaves_rdata_released(program, slots):
    """If the DMA falls behind, the program waits on its next OUT with RDATA
    released: no stray pulse, and never stuck low."""
    pulses, sim = stream(program, slots, 200, feed=2)  # 32 slots of data, then nothing
    assert sim.tx_stall
    assert (sim.pin_out >> RDATA) & 1
    assert all(start < 32 * 16 for start, _ in pulses)
