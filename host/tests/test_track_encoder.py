"""The drive's track encoder (src/ibm_track.*) against Greaseweazle's, with
no hardware: for every track of a disk,
  - the cells match Greaseweazle's own encoding of the same data, cell for
    cell (the same format definitions, ibm.1440 and ibm.720), and
  - Greaseweazle's decoder, the one `gw read` uses, reads the drive's track
    back to the original sector data.

The tracks come from test/tools/build_track, built by test/run_native_tests.sh.
"""
from __future__ import annotations

import os
import random
import subprocess
import zlib

import pytest

from fddproto import gen_disk
from fddproto.paths import REPO

pytestmark = pytest.mark.sim

TOOL = REPO / ".pio/native-tests/build_track"
CYLINDERS, HEADS = 80, 2
FORMATS = {  # name: (sectors per track, track bytes)
    "1440": (18, 12500),
    "720": (9, 6250),
}


def need(condition: bool, message: str):
    """Skips locally, but fails in CI, where a missing tool must not hide tests."""
    if not condition:
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)


@pytest.fixture(scope="module")
def gw_codec():
    try:
        from greaseweazle.codec import codec
        from greaseweazle.track import MasterTrack
    except ImportError:
        need(False, "greaseweazle not installed: pip install -r host/requirements.txt")
    return codec, MasterTrack


def drive_tracks(fmt: str, image_path=None) -> list[bytes]:
    """The drive's encoding of every track, in IMG order."""
    need(TOOL.exists(), "build_track not built: run test/run_native_tests.sh")
    args = [str(TOOL), fmt] + ([str(image_path)] if image_path else [])
    out = subprocess.run(args, check=True, capture_output=True).stdout
    track_bytes = FORMATS[fmt][1] * 2
    assert len(out) == CYLINDERS * HEADS * track_bytes
    return [out[i:i + track_bytes] for i in range(0, len(out), track_bytes)]


def first_difference(a: bytes, b: bytes) -> str:
    i = next(k for k in range(min(len(a), len(b))) if a[k] != b[k])
    return f"first difference at cell byte {i} (data byte {i // 2}): drive {a[i:i+4].hex()} vs gw {b[i:i+4].hex()}"


def check_against_greaseweazle(gw_codec, fmt: str, image: bytes, tracks: list[bytes]):
    codec, MasterTrack = gw_codec
    disk = codec.get_diskdef(f"ibm.{fmt}")
    per_track = FORMATS[fmt][0] * 512
    encode_mismatches, decode_failures = [], []
    for n, ours in enumerate(tracks):
        cyl, head = divmod(n, HEADS)
        data = image[n * per_track:(n + 1) * per_track]

        reference = disk.mk_track(cyl, head)
        reference.set_img_track(data)
        theirs = reference.master_track().bits.tobytes()
        if ours != theirs:
            encode_mismatches.append(f"{cyl}.{head}: {first_difference(ours, theirs)}")

        decoded = disk.decode_flux(cyl, head, MasterTrack(bits=ours, time_per_rev=0.2))
        if decoded.nr_missing() or bytes(decoded.get_img_track()) != data:
            decode_failures.append(f"{cyl}.{head}: {decoded.nr_missing()} sectors missing")

    assert not encode_mismatches, "\n".join(encode_mismatches[:5])
    assert not decode_failures, "\n".join(decode_failures[:5])


def test_gen_disk_known_answers():
    # The same values as test/native/test_track.cpp and HDDSynthPro.
    known = {0: 0xE1249ABB, 1: 0xE9018A87, 12345: 0xD29A1FF2, 131071: 0x69680A43, 0x0FFFFFFF: 0x907ED2FE}
    for lba, crc in known.items():
        assert zlib.crc32(gen_disk.sector(lba)) == crc, lba
    assert gen_disk.first_mismatch(gen_disk.image(18)[:1024]) is None


@pytest.mark.parametrize("fmt", FORMATS)
def test_generated_disk(gw_codec, fmt):
    image = gen_disk.image(FORMATS[fmt][0])
    check_against_greaseweazle(gw_codec, fmt, image, drive_tracks(fmt))


@pytest.mark.parametrize("fmt", FORMATS)
def test_random_image(gw_codec, fmt, tmp_path):
    """Random bytes include every byte value next to every other, and many
    A1-like patterns inside sector data."""
    size = CYLINDERS * HEADS * FORMATS[fmt][0] * 512
    image = random.Random(fmt).randbytes(size)
    path = tmp_path / "random.img"
    path.write_bytes(image)
    check_against_greaseweazle(gw_codec, fmt, image, drive_tracks(fmt, path))


def test_wrong_image_size_is_refused(tmp_path):
    need(TOOL.exists(), "build_track not built: run test/run_native_tests.sh")
    path = tmp_path / "short.img"
    path.write_bytes(bytes(1000))
    r = subprocess.run([str(TOOL), "1440", str(path)], capture_output=True, text=True)
    assert r.returncode != 0
    assert "expected 1474560 bytes" in r.stderr
