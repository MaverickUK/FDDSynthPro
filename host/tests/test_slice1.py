"""Slice 1: head, index and status lines, with the Greaseweazle as host.

Exit criterion: `gw seek` to every cylinder and back, with the drive agreeing
on the position each time; `gw rpm` reports 300 RPM.
"""
from __future__ import annotations

import re

import pytest

from conftest import require_build

pytestmark = pytest.mark.slice1

CYLINDERS = 80
SWEEPS = 20


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def stats(drive) -> dict[str, str]:
    """The drive's `s` line as a dict: {"cyl": "12", "disk": "1", ...}."""
    line = drive.command("s", "STATS")
    return dict(item.split("=", 1) for item in line.split()[1:])


def pin_level(gw, pin: int) -> str | None:
    """`gw pin get`: "Low" or "High", or None if this Greaseweazle can't read the pin."""
    r = gw.run("pin", "get", str(pin))
    m = re.search(r"Pin \d+ is (Low|High)", r.output)
    return m.group(1) if m else None


@pytest.fixture
def disk_in(drive):
    """Makes sure a disk is in (console `d` toggles), and leaves one in."""
    if stats(drive)["disk"] != "1":
        drive.command("d", "# disk")
    yield
    if stats(drive)["disk"] != "1":
        drive.command("d", "# disk")


def test_seek_every_cylinder(drive, gw, run_dir):
    wrong = []
    for cyl in list(range(CYLINDERS)) + list(range(CYLINDERS - 1, -1, -1)):
        r = gw.seek(cyl)
        assert r.ok, f"gw seek {cyl} failed:\n{r.output}"
        got = int(stats(drive)["cyl"])
        if got != cyl:
            wrong.append((cyl, got))
    (run_dir / "gw.log").write_text(gw.transcript())
    assert not wrong, f"(asked, drive's cylinder): {wrong}"


def test_full_sweeps(drive, gw):
    for _ in range(SWEEPS):
        assert gw.seek(CYLINDERS - 1).ok
        assert int(stats(drive)["cyl"]) == CYLINDERS - 1
        assert gw.seek(0).ok
        assert int(stats(drive)["cyl"]) == 0
    assert int(stats(drive)["outer_knocks"]) == 0, "the Greaseweazle never steps past track 0"


def test_track0_line(gw):
    assert gw.seek(0).ok
    assert pin_level(gw, 26) == "Low"
    assert gw.seek(1).ok
    assert pin_level(gw, 26) == "High"


def test_write_protect_line(gw, disk_in):
    assert pin_level(gw, 28) == "Low", "the disk is read-only until slice 5"


def test_rpm(gw, disk_in):
    r = gw.rpm()
    assert r.ok, r.output
    m = re.search(r"Rate: ([\d.]+) rpm", r.output)
    assert m, r.output
    assert abs(float(m.group(1)) - 300.0) < 0.1, r.output


def test_no_index_without_disk(drive, gw, disk_in):
    drive.command("d", "# disk ejected")
    r = gw.rpm()
    assert not r.ok or "Rate:" not in r.output, f"index pulses with no disk:\n{r.output}"


def test_disk_change_line(drive, gw, disk_in):
    """Pin 34: set by an eject, cleared by a step with a disk in. Skipped if
    this Greaseweazle can't read pin 34."""
    drive.command("d", "# disk ejected")
    level = pin_level(gw, 34)
    if level is None:
        pytest.skip("this Greaseweazle can't read pin 34")
    assert level == "Low"
    drive.command("d", "# disk inserted")
    assert pin_level(gw, 34) == "Low", "DSKCHG must stay set until a step"
    assert gw.seek(1).ok and gw.seek(0).ok
    assert pin_level(gw, 34) == "High"
