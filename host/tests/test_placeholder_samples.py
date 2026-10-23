"""The stand-in sample pack (scripts/make_placeholder_samples.py). No hardware needed."""
from __future__ import annotations

import importlib.util
import wave

import pytest

from fddproto.paths import REPO

pytestmark = pytest.mark.sim

EXPECTED = {"spinup.wav", "spin.wav", "spindown.wav", "step.wav", "seek.wav", "bump.wav", "insert.wav", "eject.wav"}


@pytest.fixture(scope="module")
def maker():
    spec = importlib.util.spec_from_file_location("make_placeholder_samples",
                                                  REPO / "scripts/make_placeholder_samples.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_pack(maker, tmp_path):
    maker.make_pack(tmp_path)
    assert {p.name for p in tmp_path.iterdir()} == EXPECTED
    for p in tmp_path.iterdir():
        with wave.open(str(p)) as w:
            # The HDD Synth packs' format: mono, 16-bit, one of the allowed rates.
            assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (1, 2, 22050), p.name
            assert w.getnframes() > 0


def test_loops_are_whole_cycles(maker):
    rate = maker.RATE
    # spin: whole revolutions at 300 RPM; seek: whole 6 ms steps.
    assert len(maker.spin()) % (rate // 5) == 0
    assert len(maker.seek()) % int(0.006 * rate) == 0


def test_same_pack_every_time(maker, tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    maker.rng.seed(1987)
    maker.make_pack(a)
    maker.rng.seed(1987)
    maker.make_pack(b)
    for name in EXPECTED:
        assert (a / name).read_bytes() == (b / name).read_bytes(), name
