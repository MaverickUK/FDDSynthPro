"""The Amiga mode on the rig, with the Feather standing in for an Amiga.

The Feather drives pins 12 and 16: DF1's select and the Amiga's motor line.
So with `interface=amiga` and `drive=df1` in /FDDSYNTH.CFG the drive answers
it as an Amiga's DF1, and `gw` reads and writes AmigaDOS tracks with
Greaseweazle's own codec (amiga.amigados). That proves the track format,
the motor latched at select and the write decoder on hardware, before a
real Amiga. Needs an ADF on the card served by the drive (`device0=...`),
its copy in FDDPRO_IMAGES, and the FeatherWing's write switch on WR for the
writes. The writes put back every track they change.

    PIO_ENV=bb48_drive scripts/rig.sh -m amiga
"""
from __future__ import annotations

import os
import random
import re
from pathlib import Path

import pytest

from conftest import require_build, rpm_tolerance

pytestmark = pytest.mark.amiga

FMT = "amiga.amigados"
SECTORS = 11
TRACK_BYTES = SECTORS * 512
CYCLES = int(os.environ.get("FDDPRO_WRITE_CYCLES", "5"))


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def stats(drive) -> dict[str, str]:
    line = drive.command("s", "STATS")
    return dict(item.split("=", 1) for item in line.split()[1:])


@pytest.fixture
def adf(drive, gw):
    """(name, original bytes, writable) of the ADF the drive serves as DF1."""
    s = stats(drive)
    if s.get("interface") != "amiga" or s.get("drive") != "df1":
        pytest.skip("needs interface=amiga and drive=df1 in /FDDSYNTH.CFG (the Feather drives DF1's lines)")
    parts = drive.command("i", "IMAGE").split()
    if not parts[1].upper().endswith(".ADF"):
        pytest.skip(f"the drive serves {parts[1]}, not an ADF")
    folder = os.environ.get("FDDPRO_IMAGES")
    local = Path(folder).expanduser() / parts[1] if folder else None
    if not local or not local.exists():
        pytest.skip("set FDDPRO_IMAGES to the folder holding a copy of the card's ADF")
    # The Feather keeps its idea of the head's cylinder across commands; a
    # flash puts the drive's at 0. Its seek to 0 finds track 0 again.
    assert gw.seek(0).ok
    return parts[1], local.read_bytes(), "writable=1" in " ".join(parts)


def test_index_at_300_rpm(gw, adf):
    r = gw.rpm()
    assert r.ok, r.output
    m = re.search(r"Rate: ([\d.]+) rpm", r.output)  # as slice 1 reads it
    assert m, r.output
    assert abs(float(m.group(1)) - 300) <= rpm_tolerance(gw), r.output


def test_whole_disk_reads_back(gw, adf, tmp_path):
    name, data, _ = adf
    out = tmp_path / "read.adf"
    r = gw.read(out, fmt=FMT)
    assert r.ok, r.output
    got = out.read_bytes()
    assert len(got) == len(data)
    bad = [t for t in range(160) if got[t * TRACK_BYTES:(t + 1) * TRACK_BYTES] != data[t * TRACK_BYTES:(t + 1) * TRACK_BYTES]]
    assert not bad, f"tracks differ: {bad[:10]}"


def write_and_read(gw, tmp_path, data, cyl, head, track) -> bytes:
    n = (cyl * 2 + head) * TRACK_BYTES
    img = tmp_path / "w.adf"
    img.write_bytes(data[:n] + track + data[n + TRACK_BYTES:])
    tracks = f"c={cyl}:h={head}"
    r = gw.write(img, fmt=FMT, tracks=tracks)
    assert r.ok, r.output
    out = tmp_path / "r.adf"
    r = gw.read(out, fmt=FMT, tracks=tracks)
    assert r.ok, r.output
    return out.read_bytes()[n:n + TRACK_BYTES]


def test_written_tracks_read_back(drive, gw, adf, tmp_path):
    """Whole-track writes, as an Amiga makes them: decoded by the drive,
    stored in the ADF, read back. Then the originals go back."""
    name, data, writable = adf
    if not writable:
        pytest.skip(f"{name} is read-only on the card")
    rng = random.Random(7)
    before = stats(drive)
    changed = []
    try:
        for _ in range(CYCLES):
            cyl, head = rng.randrange(80), rng.randrange(2)
            changed.append((cyl, head))
            track = rng.randbytes(TRACK_BYTES)
            assert write_and_read(gw, tmp_path, data, cyl, head, track) == track, f"{cyl}.{head}"
        after = stats(drive)
        assert int(after["written"]) - int(before["written"]) >= CYCLES * SECTORS, after
        assert after["capture_overruns"] == before["capture_overruns"], after
        assert after["write_faults"] == before["write_faults"], after
        print(f"decode {after.get('decode_kbps')} KB/s, slowest finish {after.get('finish_us_max')} us")
    finally:
        for cyl, head in changed:
            n = (cyl * 2 + head) * TRACK_BYTES
            write_and_read(gw, tmp_path, data, cyl, head, data[n:n + TRACK_BYTES])
