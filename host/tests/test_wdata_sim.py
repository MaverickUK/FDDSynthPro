"""Runs the real src/fdd_wdata.pio in the simulator against the WDATA pulses
a host writes. No hardware needed.

Checks that the captured intervals are exact (after adding back the edge
ticks the program doesn't count); that a sector write captured this way is
decoded and placed by test/tools/decode_write, as the firmware will; and
that a full FIFO stalls rather than losing intervals silently.
"""
from __future__ import annotations

import os
import random
import struct
import subprocess

import pytest

from fddproto import gen_disk
from fddproto.paths import REPO
from fddproto.pio_sim import GPIO_OFFSET, PioSim

pytestmark = pytest.mark.sim

WDATA = 11 - GPIO_OFFSET  # sample bit (src/config.h)
IDLE = 0x3FF
BUILD = REPO / ".pio/native-tests/build_track"
DECODE = REPO / ".pio/native-tests/decode_write"
CYCLES_PER_TICK = 2
PULSE_CYCLES = 8          # a 250 ns write pulse at HD (32 cycles a cell)


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("fdd_wdata")


@pytest.fixture(scope="module")
def tools():
    for tool in (BUILD, DECODE):
        if not tool.exists():
            message = f"{tool.name} not built: run test/run_native_tests.sh"
            if os.environ.get("CI"):
                pytest.fail(message)
            pytest.skip(message)


def capture(program, edges: list[int], total_cycles: int, drain: bool = True):
    """Runs the program with WDATA pulsing low for PULSE_CYCLES at each edge
    (in cycles). Returns (intervals in ticks, as the program measured them,
    the simulator)."""
    sim = PioSim(program, jmp_pin=WDATA, in_shift_right=True, autopush=True)
    sim.pc = program.defines.get("start", 0)
    starts = set(edges)
    low_until = -1
    seen = [IDLE, IDLE]  # the input synchroniser
    words = []
    for k in range(total_cycles):
        if k in starts:
            low_until = k + PULSE_CYCLES
        pins = IDLE & ~(1 << WDATA) if k < low_until else IDLE
        seen.append(pins)
        sim.step(seen.pop(0))
        if drain and sim.rx_fifo:
            words.append(sim.rx_fifo.pop(0))
    intervals = [(w >> (8 * i)) & 0xFF for w in words for i in range(4)]
    return intervals, sim


def test_constants(program):
    assert program.defines["TICKS_PER_CELL"] * CYCLES_PER_TICK == 32  # rdata_stream clock divider


def test_intervals_are_exact(program):
    """Edges two to four cells apart, at random phases within a cycle: each
    interval, plus EDGE_TICKS, is the true one to within a tick."""
    rng = random.Random(4)
    tpc = program.defines["TICKS_PER_CELL"]
    cell_cycles = tpc * CYCLES_PER_TICK
    edges, t = [], 100
    for _ in range(400):
        t += rng.choice((2, 3, 4)) * cell_cycles + rng.randint(-3, 3)
        edges.append(t)
    got, _ = capture(program, edges, edges[-1] + 200)
    got = got[1:len(edges)]  # the first interval runs from the start; complete words only
    true = [(b - a) / CYCLES_PER_TICK for a, b in zip(edges, edges[1:])]
    edge = program.defines["EDGE_TICKS"]
    errors = [g + edge - t_ for g, t_ in zip(got, true)]
    assert len(errors) >= 390
    assert max(abs(e) for e in errors) <= 1, sorted(set(errors))
    assert abs(sum(errors) / len(errors)) < 0.1  # no bias


def test_sector_write_round_trip(program, tools):
    """A host writes sector 3 of cylinder 3, head 0: the data field as a
    controller writes it. The capture, decoded, puts it in the right place."""
    cyl, head, sector = 3, 0, 2  # sector index 2 = ID 3
    raw = subprocess.run([str(BUILD), "1440"], capture_output=True, check=True).stdout
    track_bytes = 12500 * 2
    words = raw[(cyl * 2 + head) * track_bytes:(cyl * 2 + head + 1) * track_bytes]
    start = 146 + 44 + sector * 682            # ibm::HD_1440.dataFieldStart(sector)
    burst = words[start * 2:(start + 12 + 4 + 512 + 2 + 4) * 2]
    bits = "".join(f"{b:08b}" for b in burst)
    cell_cycles = program.defines["TICKS_PER_CELL"] * CYCLES_PER_TICK
    edges = [40 + i * cell_cycles for i, c in enumerate(bits) if c == "1"]
    intervals, sim = capture(program, edges, edges[-1] + 4 * cell_cycles)
    assert not sim.rx_stall

    edge = program.defines["EDGE_TICKS"]
    # The firmware adds EDGE_TICKS back; partial last word's zeros dropped.
    flux = b"".join(struct.pack("<H", i + edge) for i in intervals[:len(edges)])
    r = subprocess.run([str(DECODE), "1440", str(cyl), str(head), str(start)], input=flux,
                       capture_output=True, check=True)
    assert "updates=1 " in r.stderr.decode(), r.stderr.decode()
    lba = (cyl * 2 + head) * 18 + sector
    assert r.stdout[sector * 512:(sector + 1) * 512] == gen_disk.sector(lba)


def test_full_fifo_stalls(program):
    cell_cycles = 32
    edges = [40 + i * 3 * cell_cycles for i in range(60)]  # 15 words: more than the 8-deep FIFO
    _, sim = capture(program, edges, edges[-1] + 200, drain=False)
    assert len(sim.rx_fifo) == 8
    assert sim.rx_stall
