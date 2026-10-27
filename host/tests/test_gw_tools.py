"""The Greaseweazle tools without a Greaseweazle: the wrapper's command lines,
and `gw convert` as an independent MFM encoder and decoder. Slice 2 checks
the drive's own track encoder against it. No hardware needed."""
from __future__ import annotations

import os

import pytest

from fddproto.greaseweazle import Greaseweazle

pytestmark = pytest.mark.sim

IMAGE_BYTES = 1_474_560  # ibm.1440: 80 cylinders x 2 heads x 18 sectors x 512


@pytest.fixture(scope="module")
def gw():
    try:
        return Greaseweazle(drive="B")
    except FileNotFoundError as e:
        if os.environ.get("CI"):
            pytest.fail(str(e))
        pytest.skip(str(e))


def test_drive_only_on_drive_actions(gw):
    gw.log.clear()
    gw.run("convert", "--help")
    assert "--drive" not in gw.log[-1].command
    gw.run("seek", "--help")
    assert gw.log[-1].command[-3:] == ["--drive", "B", "--help"]
    assert gw.log[-1].ok


def test_reset_is_not_gw_reset(tmp_path):
    """The reset sends only the reset command (gw_reset.py), never `gw reset`,
    which hangs on the Feather. With no host there, it fails at once."""
    g = Greaseweazle(device=str(tmp_path / "no-such-port"), gw="gw")
    r = g.reset(timeout=20)
    assert r.command[1:3] == ["-m", "fddproto.gw_reset"]
    assert r.command[-2:] == ["--device", str(tmp_path / "no-such-port")]
    assert not r.ok


def test_convert_round_trip(gw, tmp_path):
    """IMG -> flux (.scp) -> IMG gives back the same bytes, for one track."""
    data = os.urandom(IMAGE_BYTES)
    src, flux, back = tmp_path / "in.img", tmp_path / "track.scp", tmp_path / "out.img"
    src.write_bytes(data)
    tracks = "c=0:h=0"
    r = gw.run("convert", "--format", "ibm.1440", "--tracks", tracks, str(src), str(flux))
    assert r.ok, r.output
    r = gw.run("convert", "--format", "ibm.1440", "--tracks", tracks, str(flux), str(back))
    assert r.ok, r.output
    assert "Found 18 sectors of 18" in r.output, r.output
    assert back.read_bytes()[:18 * 512] == data[:18 * 512]
