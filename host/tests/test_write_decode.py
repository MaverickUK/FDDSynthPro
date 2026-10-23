"""The drive's write decoding against Greaseweazle's encoder, with no
hardware: Greaseweazle builds a track the way `gw write` sends it, the test
turns it into flux with timing errors a real write could have, and
test/tools/decode_write (the drive's data separator, field scanner and write
interpreter) must recover every sector.
"""
from __future__ import annotations

import os
import random
import re
import struct
import subprocess

import pytest

from fddproto.paths import REPO

pytestmark = pytest.mark.sim

TOOL = REPO / ".pio/native-tests/decode_write"
TICKS = 16  # capture ticks per cell
FORMATS = {"1440": 18, "720": 9}


@pytest.fixture(scope="module")
def codec():
    if not TOOL.exists():
        message = "decode_write not built: run test/run_native_tests.sh"
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)
    from greaseweazle.codec import codec
    return codec


def to_flux(bits, speed: float, jitter: float, rng: random.Random) -> bytes:
    """16-bit little-endian intervals, in ticks, from a cell bitarray. Edge
    times are rounded to ticks, as a capture clock does."""
    out, last = bytearray(), 0
    for cell in bits.search(1):
        edge = (cell + 1) * speed + jitter * rng.uniform(-1, 1)
        tick = int(edge * TICKS + 0.5)
        out += struct.pack("<H", max(1, tick - last))
        last = tick
    return bytes(out)


@pytest.mark.parametrize("fmt", FORMATS)
@pytest.mark.parametrize("speed,jitter", [(1.0, 0.0), (1.02, 0.1), (0.97, 0.15)])
def test_greaseweazle_track_write(codec, fmt, speed, jitter):
    rng = random.Random(f"{fmt}{speed}{jitter}")
    disk = codec.get_diskdef(f"ibm.{fmt}")
    for cyl, head in ((0, 0), (41, 1), (79, 1)):
        data = rng.randbytes(FORMATS[fmt] * 512)
        track = disk.mk_track(cyl, head)
        track.set_img_track(data)
        flux = to_flux(track.master_track().bits, speed, jitter, rng)
        r = subprocess.run([str(TOOL), fmt, str(cyl), str(head), "0"], input=flux, capture_output=True, check=True)
        summary = r.stderr.decode()
        assert f"updates={FORMATS[fmt]} " in summary, summary
        assert r.stdout == data, f"{cyl}.{head}: {summary}"


def test_track_for_another_cylinder_is_refused(codec):
    disk = codec.get_diskdef("ibm.1440")
    track = disk.mk_track(5, 0)
    track.set_img_track(bytes(18 * 512))
    flux = to_flux(track.master_track().bits, 1.0, 0.0, random.Random(1))
    r = subprocess.run([str(TOOL), "1440", "6", "0", "0"], input=flux, capture_output=True, check=True)
    assert re.search(r"updates=0 .*unplaced=18", r.stderr.decode())
