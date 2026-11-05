"""Other disk formats on the rig: each image read back whole by the Feather
with Greaseweazle's own definition of its format.

The drive picks a PC format by an image's size, and others from IMG.CFG
(FlashFloppy's file). These are random images of each size, on the card and
in FDDPRO_IMAGES; the FM one needs the card's IMG.CFG section `[dfs::102400]`.
Each test serves its image with the console's `e device0=...` and `!`, and
the last puts device0 back as it found it.

    PIO_ENV=bb48_drive scripts/rig.sh -m formats
"""
from __future__ import annotations

import os
from pathlib import Path

import pytest

from conftest import require_build

pytestmark = pytest.mark.formats

# Image, Greaseweazle format, what it shows.
CASES = [
    ("FMT160.IMG", "ibm.160", "single-sided, 40 cylinders"),
    ("FMT360.IMG", "ibm.360", "double-sided, 40 cylinders"),
    ("FMT800.IMG", "ibm.800", "10 sectors a track at DD"),
    ("FMT1200.IMG", "ibm.1200", "5.25-inch HD at 360 RPM"),
    ("FMT1680.IMG", "ibm.1680", "DMF: 21 sectors, interleaved"),
    ("BBC.DFS.IMG", "acorn.dfs.ss", "FM, from an IMG.CFG section"),
]


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


@pytest.fixture(scope="module")
def device0(drive):
    """device0 as the file has it, put back after the module."""
    volume = dict(i.split("=", 1) for i in drive.command("s", "STATS").split()[1:] if "=" in i)["volume"]
    before = drive.set_setting(f"volume={volume}").get("device0", "")
    yield
    drive.set_setting(f"device0={before}")
    drive.restart()


@pytest.mark.parametrize("name,fmt,what", CASES, ids=[c[0] for c in CASES])
def test_format_reads_back(drive, gw, device0, tmp_path, name, fmt, what):
    folder = os.environ.get("FDDPRO_IMAGES")
    local = Path(folder).expanduser() / name if folder else None
    if not local or not local.exists():
        pytest.skip(f"no {name} in FDDPRO_IMAGES")
    drive.set_setting(f"device0={name}")
    drive.restart()
    served = drive.command("i", "IMAGE").split()[1]
    if served != name:
        pytest.skip(f"{name} isn't on the card (the drive serves {served})")
    assert gw.seek(0).ok  # the Feather finds track 0 again after the restart
    out = tmp_path / "read.img"
    r = gw.read(out, fmt=fmt)
    assert r.ok, r.output
    got, want = out.read_bytes(), local.read_bytes()
    assert len(got) == len(want), (len(got), len(want))
    assert got == want, f"{name} ({what}) differs from its copy"
