"""Slice 6: swapping images, GoTek style (console `[` and `]`).

Needs two or more images on the card, with copies in FDDPRO_IMAGES. Make
them with `python -m fddproto.make_images`; GEN1440.IMG, RAND1440.IMG and
RAND720.IMG swap between both formats.
"""
from __future__ import annotations

import os
import time
from pathlib import Path

import pytest

from conftest import require_build
from fddproto import trace

pytestmark = pytest.mark.slice6

FORMATS = {1474560: "ibm.1440", 737280: "ibm.720"}
SWAP_S = 2.0  # half a second out, then a load


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def image_line(drive) -> dict[str, str]:
    """`i` as a dict: name, kb, contiguous, writable, index ("2/3")."""
    parts = drive.command("i", "IMAGE").split()
    info = {"name": parts[1], "kb": parts[2] if len(parts) > 2 else ""}
    info.update(item.split("=", 1) for item in parts[3:])
    return info


@pytest.fixture
def folder(drive) -> Path:
    info = image_line(drive)
    if info["name"] == "generated" or info.get("index", "1/1").endswith("/1"):
        pytest.skip("needs two or more images on the card")
    path = os.environ.get("FDDPRO_IMAGES")
    if not path:
        pytest.skip("set FDDPRO_IMAGES to the folder holding copies of the card's images")
    return Path(path).expanduser()


def swap(drive, key: str) -> dict[str, str]:
    mark = drive.mark()
    drive.send(key)
    time.sleep(SWAP_S)
    t = trace.parse(drive.lines_since(mark))
    assert t.sounds()[-2:] == ["EJECT", "INSERT"], t.sounds()
    return image_line(drive)


def test_every_image_in_turn(drive, gw, folder, tmp_path):
    """`]` round the whole list and back to the first: each image reads as
    its local copy, whatever its format."""
    first = image_line(drive)
    count = int(first["index"].split("/")[1])
    seen = []
    for _ in range(count):
        info = swap(drive, "]")
        seen.append(info["name"])
        data = (folder / info["name"]).read_bytes()
        out = tmp_path / f"{info['name']}.read"
        r = gw.read(out, fmt=FORMATS[len(data)], tracks="c=0-2:h=0-1")
        assert r.ok, r.output
        n = 3 * 2 * (len(data) // (80 * 2))
        assert out.read_bytes()[:n] == data[:n], info["name"]
    assert seen[-1] == first["name"] and len(set(seen)) == count


def test_previous_undoes_next(drive, folder):
    start = image_line(drive)["name"]
    swap(drive, "]")
    assert swap(drive, "[")["name"] == start
