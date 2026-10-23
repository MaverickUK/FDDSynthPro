"""Slice 3: images on the SD card, with the host reading them.

Copy an image made by `python -m fddproto.make_images <folder>` (or any
720 KB / 1.44 MB IMG) to the card, and set FDDPRO_IMAGES to the folder that
holds the same file, so the tests can compare. The console's `i` names the
image the drive serves.

Exit criterion: the image reads back byte for byte; seek-heavy random reads
too; every load fits the host's settle time.
"""
from __future__ import annotations

import os
import random
from pathlib import Path

import pytest

from conftest import require_build

pytestmark = pytest.mark.slice3

SETTLE_US = 15000
FORMATS = {1474560: ("ibm.1440", 18), 737280: ("ibm.720", 9)}


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def stats(drive) -> dict[str, str]:
    line = drive.command("s", "STATS")
    return dict(item.split("=", 1) for item in line.split()[1:])


@pytest.fixture
def image(drive) -> tuple[str, bytes]:
    """The served image's name and its bytes, from the local copy."""
    line = drive.command("i", "IMAGE")  # "IMAGE <name> <kb> contiguous=<0|1>" or "IMAGE generated <kb>"
    parts = line.split()
    if parts[1] == "generated":
        pytest.skip("no image on the SD card: the drive serves the generated disk")
    folder = os.environ.get("FDDPRO_IMAGES")
    if not folder:
        pytest.skip("set FDDPRO_IMAGES to the folder holding a copy of the card's image")
    local = Path(folder).expanduser() / parts[1]
    if not local.exists():
        pytest.skip(f"{local} not found: copy the card's image there")
    if "contiguous=0" in line:
        print(f"note: {parts[1]} is fragmented on the card; reads may be slower")
    return parts[1], local.read_bytes()


def test_image_reads_byte_for_byte(gw, image, tmp_path):
    name, data = image
    fmt, sectors = FORMATS[len(data)]
    for n in range(3):
        path = tmp_path / f"read{n}.img"
        r = gw.read(path, fmt=fmt)
        assert r.ok, r.output
        assert f"Found {80 * 2 * sectors} sectors of {80 * 2 * sectors}" in r.output, r.output
        got = path.read_bytes()
        assert got == data, f"{name}: first difference at byte {next(i for i in range(len(data)) if got[i] != data[i])}"


def test_random_tracks(gw, image, tmp_path):
    """Seek-heavy: every read is a different cylinder, so every read waits on
    a load from the card."""
    _, data = image
    fmt, sectors = FORMATS[len(data)]
    track_bytes = sectors * 512
    rng = random.Random(3)
    for _ in range(20):
        cyl, head = rng.randrange(80), rng.randrange(2)
        path = tmp_path / f"t{cyl}.{head}.img"
        r = gw.read(path, fmt=fmt, tracks=f"c={cyl}:h={head}")
        assert r.ok, r.output
        n = cyl * 2 + head
        assert path.read_bytes()[n * track_bytes:(n + 1) * track_bytes] == data[n * track_bytes:(n + 1) * track_bytes]


def test_loads_fit_the_settle_time(drive, gw, image):
    for cyl in (1, 79, 40, 0):
        assert gw.seek(cyl).ok
        assert int(stats(drive)["ready"]) == cyl
    s = stats(drive)
    read, encode = int(s["read_us_max"]), int(s["encode_us_max"])
    # A step in progress when the head arrives, then a whole load
    # (test/native/test_loader.cpp works the same bound out).
    worst = max(read, encode) + 2 * read + 2 * encode
    print(f"slowest read {read} us, slowest encode {encode} us: worst case {worst} us after the last step")
    assert worst < SETTLE_US
