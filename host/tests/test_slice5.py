"""Slice 5: writing, with the host writing to the image on the SD card.

Needs a writable image on the card (not marked read-only), its copy in
FDDPRO_IMAGES as for slice 3, and the FeatherWing's write switch on WR. The
tests put back every track they change, so the copy stays valid.

Exit criterion: written tracks read back byte for byte, cycle after cycle;
the card keeps them (power-cycle check in bringup.md).
"""
from __future__ import annotations

import os
import random
from pathlib import Path

import pytest

from conftest import require_build

pytestmark = pytest.mark.slice5

FORMATS = {1474560: ("ibm.1440", 18), 737280: ("ibm.720", 9)}
CYCLES = int(os.environ.get("FDDPRO_WRITE_CYCLES", "5"))


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def stats(drive) -> dict[str, str]:
    line = drive.command("s", "STATS")
    return dict(item.split("=", 1) for item in line.split()[1:])


@pytest.fixture
def image(drive):
    """(name, original bytes, format, sectors per track) of a writable image."""
    line = drive.command("i", "IMAGE")
    parts = line.split()
    if parts[1] in ("welcome", "generated"):
        pytest.skip(f"the drive serves the {parts[1]} disk, not an image on the SD card")
    if "writable=1" not in line:
        pytest.skip(f"{parts[1]} is read-only on the card")
    folder = os.environ.get("FDDPRO_IMAGES")
    local = Path(folder).expanduser() / parts[1] if folder else None
    if not local or not local.exists():
        pytest.skip("set FDDPRO_IMAGES to the folder holding a copy of the card's image")
    data = local.read_bytes()
    fmt, sectors = FORMATS[len(data)]
    return parts[1], data, fmt, sectors


def with_track(data: bytes, sectors: int, cyl: int, head: int, track: bytes) -> bytes:
    n = (cyl * 2 + head) * sectors * 512
    return data[:n] + track + data[n + len(track):]


def write_and_read(gw, tmp_path, data, fmt, sectors, cyl, head, track) -> bytes:
    img = tmp_path / "w.img"
    img.write_bytes(with_track(data, sectors, cyl, head, track))
    tracks = f"c={cyl}:h={head}"
    r = gw.write(img, fmt=fmt, tracks=tracks)
    assert r.ok, r.output
    out = tmp_path / "r.img"
    r = gw.read(out, fmt=fmt, tracks=tracks)
    assert r.ok, r.output
    n = (cyl * 2 + head) * sectors * 512
    return out.read_bytes()[n:n + sectors * 512]


def test_write_tracks_and_read_back(drive, gw, image, tmp_path):
    name, data, fmt, sectors = image
    rng = random.Random(5)
    before = int(stats(drive)["written"])
    changed = []
    try:
        for _ in range(CYCLES):
            cyl, head = rng.randrange(80), rng.randrange(2)
            changed.append((cyl, head))
            track = rng.randbytes(sectors * 512)
            assert write_and_read(gw, tmp_path, data, fmt, sectors, cyl, head, track) == track, f"{cyl}.{head}"
        s = stats(drive)
        assert int(s["written"]) - before >= CYCLES * sectors
        assert s["write_bad_crc"] == "0" and s["bursts_dropped"] == "0", s
    finally:
        for cyl, head in changed:  # put the originals back
            n = (cyl * 2 + head) * sectors * 512
            write_and_read(gw, tmp_path, data, fmt, sectors, cyl, head, data[n:n + sectors * 512])


def test_written_track_survives_a_seek(drive, gw, image, tmp_path):
    """After the head moves away, the track comes back from the card, not
    from the buffer."""
    name, data, fmt, sectors = image
    cyl, head = 33, 1
    track = random.Random(6).randbytes(sectors * 512)
    n = (cyl * 2 + head) * sectors * 512
    try:
        write_and_read(gw, tmp_path, data, fmt, sectors, cyl, head, track)
        assert gw.seek(70).ok and gw.seek(cyl).ok
        out = tmp_path / "again.img"
        assert gw.read(out, fmt=fmt, tracks=f"c={cyl}:h={head}").ok
        assert out.read_bytes()[n:n + sectors * 512] == track
    finally:
        write_and_read(gw, tmp_path, data, fmt, sectors, cyl, head, data[n:n + sectors * 512])
