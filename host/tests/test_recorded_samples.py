"""The sample pack cut from a real drive's recording
(scripts/make_recorded_pack.py, samples/Recorded). No hardware needed."""
from __future__ import annotations

import re
import wave

import pytest

from fddproto.paths import REPO

pytestmark = pytest.mark.sim

PACK = REPO / "samples/Recorded"
RATE = 22050


def frames(name: str) -> int:
    with wave.open(str(PACK / f"{name}.wav")) as w:
        return w.getnframes()


def test_the_firmwares_format():
    # Everything but insert and eject, which the recording doesn't have (both optional).
    assert {p.name for p in PACK.iterdir()} == {f"{n}.wav" for n in
                                                ("spinup", "spin", "spindown", "step", "seek", "bump")}
    for p in PACK.iterdir():
        with wave.open(str(p)) as w:
            assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (1, 2, RATE), p.name
            assert w.getnframes() > 0


def test_fits_the_sample_pool():
    config = (REPO / "src/config.h").read_text()
    kb = int(re.search(r"SAMPLE_POOL_MAX_BYTES = (\d+) \* 1024", config).group(1))
    total = sum(p.stat().st_size - 44 for p in PACK.iterdir())  # less each WAV's header
    assert total <= kb * 1024


def test_loops_are_whole_cycles():
    # spin: 4 revolutions (199.8 ms each, as measured); seek: 20 steps 10 ms apart.
    assert abs(frames("spin") - 4 * 0.1998 * RATE) <= 1
    assert frames("seek") == 20 * RATE // 100
