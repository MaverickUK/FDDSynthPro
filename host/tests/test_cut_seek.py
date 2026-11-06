"""The seek cutter (scripts/cut_seek.py), on a recording made from the
Placeholder pack's step: room noise, single steps 0.4 s apart, then
full-stroke seeks of 79 steps at a PC's 5 ms. The loop must be whole steps
at that rate, start on a step, and keep the seek's level against a step."""
from __future__ import annotations

import array
import random
import sys
import wave
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import cut_seek  # noqa: E402

RATE = cut_seek.RATE
STEP_S = 0.00505
SINGLES = [1.0 + 0.4 * i for i in range(10)]
SEEKS = [6.0 + 0.8 * i for i in range(6)]
SEEK_STEPS = 79

pytestmark = pytest.mark.sim


def placeholder_step() -> list[float]:
    with wave.open(str(REPO / "samples/Placeholder/step.wav")) as w:
        assert w.getframerate() == RATE
        return [v / 32768 for v in array.array("h", w.readframes(w.getnframes()))]


def recording() -> tuple[list[float], list[float]]:
    rnd = random.Random(7)
    step = placeholder_step()
    x = [rnd.gauss(0, 0.002) for _ in range(int(11 * RATE))]

    def add(t: float, gain: float):
        i0 = int(round(t * RATE))  # each edge rounded to a sample, not each interval
        for i, v in enumerate(step):
            x[i0 + i] += v * gain

    for t in SINGLES:
        add(t, 0.4)
    for t in SEEKS:
        for k in range(SEEK_STEPS):
            add(t + k * STEP_S, 0.4)
    return x, step


@pytest.fixture(scope="module")
def cut():
    x, step = recording()
    pack_step_peak = 0.45
    return cut_seek.cut(x, (5.5, 11.0), (0.5, 5.2), STEP_S, pack_step_peak), x, pack_step_peak


def test_hears_the_steps_and_seeks(cut):
    s, _, _ = cut
    assert s.single_steps == len(SINGLES)
    assert s.seeks == len(SEEKS)


def test_loop_is_whole_steps(cut):
    s, _, _ = cut
    assert len(s.sound) == round(cut_seek.LOOP_STEPS * STEP_S * RATE)


def test_loop_starts_on_a_step(cut):
    s, _, _ = cut
    seek = min(SEEKS, key=lambda t: abs(t - s.source[0]))
    into = (s.start / RATE - seek) % STEP_S
    assert STEP_S - 0.001 < into or into < 0.0005  # just before an edge


def test_keeps_the_seek_against_a_step(cut):
    s, x, pack_step_peak = cut
    loudness = cut_seek.peak(s.sound) / pack_step_peak
    expected = min(10 ** (s.seek_over_step_db / 20), cut_seek.PEAK / pack_step_peak)
    assert loudness == pytest.approx(expected, rel=0.01)
    assert cut_seek.peak(s.sound) <= cut_seek.PEAK + 1e-9


def test_no_seek_is_an_error():
    x, _ = recording()
    with pytest.raises(SystemExit):
        cut_seek.cut(x, (0.5, 5.5), (0.5, 5.5), STEP_S, 0.45)
