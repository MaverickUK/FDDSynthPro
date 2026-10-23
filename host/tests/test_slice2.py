"""Slice 2: read a generated disk, with the Greaseweazle as host.

Exit criterion: `gw read --format ibm.1440` gives the generated image byte
for byte, ten times; random single-track reads too.
"""
from __future__ import annotations

import random
import re

import pytest

from conftest import require_build
from fddproto import gen_disk

pytestmark = pytest.mark.slice2

SECTORS = 18
TRACK_BYTES = SECTORS * 512
IMAGE_BYTES = 80 * 2 * TRACK_BYTES
FULL_READS = 10
SETTLE_US = 15000  # the host's settle time: a cylinder must be loaded within it


@pytest.fixture(autouse=True)
def drive_build(firmware_slice, drive):
    require_build(firmware_slice, drive_build=True)
    if not drive.command("i", "IMAGE").startswith("IMAGE generated"):
        pytest.skip("an image is on the SD card: slice 2 needs the generated disk (no card)")


@pytest.fixture(scope="module")
def expected() -> bytes:
    return gen_disk.image(SECTORS)


def stats(drive) -> dict[str, str]:
    line = drive.command("s", "STATS")
    return dict(item.split("=", 1) for item in line.split()[1:])


def test_full_reads(gw, expected, tmp_path, run_dir):
    for n in range(FULL_READS):
        path = tmp_path / f"read{n}.img"
        r = gw.read(path)
        (run_dir / "gw.log").write_text(gw.transcript())
        assert r.ok, r.output
        assert "Found 2880 sectors of 2880" in r.output, r.output
        data = path.read_bytes()
        assert len(data) == IMAGE_BYTES
        assert gen_disk.first_mismatch(data) is None, f"read {n}: first mismatch (lba, offset) {gen_disk.first_mismatch(data)}"


def test_random_tracks(gw, expected, tmp_path):
    rng = random.Random(2)
    for _ in range(20):
        cyl, head = rng.randrange(80), rng.randrange(2)
        path = tmp_path / f"t{cyl}.{head}.img"
        r = gw.read(path, tracks=f"c={cyl}:h={head}")
        assert r.ok, r.output
        assert f"Found {SECTORS} sectors of {SECTORS}" in r.output, r.output
        n = cyl * 2 + head
        assert path.read_bytes()[n * TRACK_BYTES:(n + 1) * TRACK_BYTES] == expected[n * TRACK_BYTES:(n + 1) * TRACK_BYTES]


def test_cylinder_loads_within_settle_time(drive, gw):
    for cyl in (1, 40, 79, 0):
        assert gw.seek(cyl).ok
        assert int(stats(drive)["ready"]) == cyl
    s = stats(drive)
    read, encode = int(s["read_us_max"]), int(s["encode_us_max"])
    worst = max(read, encode) + 2 * read + 2 * encode  # as test_loader.cpp works it out
    print(f"slowest read {read} us, slowest encode {encode} us: worst case {worst} us")
    assert worst < SETTLE_US


def test_rpm_still_exact(gw):
    m = re.search(r"Rate: ([\d.]+) rpm", gw.rpm().output)
    assert m and abs(float(m.group(1)) - 300.0) < 0.1
