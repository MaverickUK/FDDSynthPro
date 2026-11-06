"""Runs the real src/fdd_edge.pio and src/fdd_gate*.pio in the simulator:
the Amiga's bus at hardware speed (src/amiga_pio.h). No hardware needed.

An A1200 selects its drive for 4 us to read each bit of the drive ID and
for about 20 us around a step, changing DIR with the select. The edge
sampler must catch every falling edge, however short the pulse, with the
inputs as they were at the edge; the gate must put the levels on the cable
only while the select is asserted, within a few cycles of it changing.
"""
from __future__ import annotations

import random

import pytest

from fddproto.pio_sim import GPIO_OFFSET, PioSim

pytestmark = pytest.mark.sim

IN_BASE = 12                 # GPIO 12-21, the ten inputs (src/config.h)
STEP, DIR, SELECT = 18 - IN_BASE, 17 - IN_BASE, 13 - IN_BASE   # bits of the edge sampler's pins
IDLE = 0x3FF                 # every input released
SYNC = 2                     # the input synchroniser's cycles


def run_edge(program, waves, cycles, jmp_pin):
    """`waves(k)` gives the ten inputs at cycle k; returns each sample pushed,
    with the cycle it came out."""
    sim = PioSim(program, jmp_pin=jmp_pin, in_shift_right=False)
    seen = [IDLE] * SYNC
    out = []
    for k in range(cycles):
        seen.append(waves(k))
        sim.step(seen.pop(0))
        while sim.rx_fifo:
            out.append((k, sim.rx_fifo.pop(0) & 0x3FF))
    return out


@pytest.fixture(scope="module")
def edge(assemble):
    return assemble("fdd_edge")


def test_every_step_edge_with_the_inputs_at_it(edge):
    """Steps 3 to 300 cycles long, DIR and the select set before each and
    changed 6 cycles after it falls (well after the edge, well inside a
    microsecond): every edge is sampled once, with the inputs as they were."""
    rng = random.Random(1)
    pulses, t = [], 50
    for _ in range(200):
        low = rng.choice([3, 4, 10, 300])
        state = IDLE & ~((rng.random() < 0.5) << DIR) & ~((rng.random() < 0.7) << SELECT)
        pulses.append((t, low, state))
        t += low + rng.choice([60, 300, 3000])  # steps are ms apart; 60 cycles is 0.4 us

    def wave(k):
        for start, low, state in pulses:
            if start - 20 <= k < start + low + 20:
                pins = state if k < start + 6 else IDLE  # DIR and the select go with the step
                if start <= k < start + low:
                    pins &= ~(1 << STEP)
                return pins
        return IDLE

    got = run_edge(edge, wave, t + 50, STEP)
    assert len(got) == len(pulses)
    for (cycle, sample), (start, _, state) in zip(got, pulses):
        assert cycle - start <= SYNC + 4  # out within a few cycles of the edge
        assert sample == state & ~(1 << STEP)


def test_select_edges_of_an_id_read(edge):
    """Kickstart reads a drive's ID with 32 selects of 4 us (600 cycles at
    150 MHz) 4 us apart: 32 samples, plus the two before them."""
    starts = [100, 1300] + [2500 + 1200 * i for i in range(32)]

    def wave(k):
        return IDLE & ~(1 << SELECT) if any(s <= k < s + 600 for s in starts) else IDLE

    got = run_edge(edge, wave, starts[-1] + 2000, SELECT)
    assert len(got) == len(starts)


def run_gate(program, levels_at, select_at, cycles, out_count=20):
    """The gate with core 1 sending levels (at cycles given) and the select
    changing; returns the pins (or pindirs) it drives each cycle."""
    sel_bit = 13 - GPIO_OFFSET  # the gate's pins start at GPIO 4
    sim = PioSim(program, jmp_pin=sel_bit, out_base=0, out_count=out_count, tx_depth=8)
    sim.tx_fifo.append(levels_at[0][1])
    seen = [0xFFFFF] * SYNC
    history = []
    for k in range(cycles):
        for t, word in levels_at:
            if t == k and k:
                sim.tx_fifo.append(word)
        selected = any(a <= k < b for a, b in select_at)
        seen.append(0xFFFFF & ~((1 if selected else 0) << sel_bit))
        sim.step(seen.pop(0))
        history.append((sim.pin_out, sim.pindirs))
    return history


@pytest.mark.parametrize("name", ["fdd_gate", "fdd_gate_od"])
def test_gate_follows_the_select(assemble, name):
    program = assemble(name)
    od = name.endswith("_od")
    released = 0xFFFFF
    trk0 = released & ~(1 << 1)       # GPIO 5 low: TRK0 asserted
    trk0_rdy = trk0 & ~(1 << 18)      # and GPIO 22: /RDY
    word = (lambda lv: ~lv & 0xFFFFF) if od else (lambda lv: lv)
    levels = [(0, word(trk0)), (2000, word(trk0_rdy))]
    selects = [(300, 900), (1500, 1520), (2100, 2700)]  # the middle one a 20-cycle blip
    hist = run_gate(program, levels, selects, 3000)

    def shown(k):  # what the cable gets at cycle k
        pins, dirs = hist[k]
        return (~dirs & 0xFFFFF) if od else (pins & 0xFFFFF)

    latency = SYNC + 6
    for a, b in selects:
        expect = trk0 if a < 2000 else trk0_rdy
        assert all(shown(k) == expect for k in range(a + latency, b))
        assert all(shown(k) == released for k in range(b + latency, b + 100))
    assert all(shown(k) == released for k in range(latency, 300))  # before any select
