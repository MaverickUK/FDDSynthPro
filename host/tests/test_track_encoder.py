"""The drive's track encoder (src/ibm_track.*) against Greaseweazle's, with
no hardware: for every track of a disk,
  - the cells match Greaseweazle's own encoding of the same data, cell for
    cell (the same format definitions, ibm.1440 and ibm.720), and
  - Greaseweazle's decoder, the one `gw read` uses, reads the drive's track
    back to the original sector data.

The tracks come from test/tools/build_track, built by test/run_native_tests.sh.

By default a spread of tracks is compared (the outermost and innermost
cylinders, both heads, the middle, and a fixed random few): a second or so.
FDDPRO_FULL_TRACKS=1 compares all 160, as CI does on release tags.
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
FULL = os.environ.get("FDDPRO_FULL_TRACKS") == "1"


def tracks_to_check() -> list[int]:
    """Track numbers (cylinder * 2 + head) to compare."""
    if FULL:
        return list(range(CYLINDERS * HEADS))
    chosen = {0, 1, 78, 79, 80, 81, 156, 157, 158, 159}  # cylinders 0, 39, 40, 78, 79
    chosen.update(random.Random(160).sample(range(CYLINDERS * HEADS), 6))
    return sorted(chosen)


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
    for n in tracks_to_check():
        ours = tracks[n]
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


# ---- Every other format the drive knows -------------------------------------------------
#
# The PC formats it finds by an image's size, against Greaseweazle's own
# ibm.* definitions; and IMG.CFG sections (from FlashFloppy's examples)
# against the same geometry written as Greaseweazle disk definitions. Each
# track's cells must match Greaseweazle's encoding, and its decoder must
# read them back to the sector data.

SIZES = {  # image bytes: Greaseweazle's definition
    163840: "ibm.160", 184320: "ibm.180", 327680: "ibm.320", 368640: "ibm.360",
    819200: "ibm.800", 1228800: "ibm.1200", 1720320: "ibm.1680",
}


def build(fmt_arg: str, image_path) -> bytes:
    need(TOOL.exists(), "build_track not built: run test/run_native_tests.sh")
    r = subprocess.run([str(TOOL), fmt_arg, str(image_path)], capture_output=True)
    assert r.returncode == 0, r.stderr.decode()
    return r.stdout


def undouble(cells: bytes) -> bytes | None:
    """A half-rate track's cells, as sent at the disk's rate (each cell then
    an empty one), back at its own rate; None if an in-between cell isn't empty."""
    out = bytearray()
    for i in range(0, len(cells) - 1, 2):
        w = cells[i] << 8 | cells[i + 1]
        if w & 0x5555:
            return None
        b = 0
        for k in range(8):
            b = b << 1 | (w >> (15 - 2 * k)) & 1
        out.append(b)
    return bytes(out)


def compare(gw_codec, disk, image: bytes, ours: bytes, cyls: int, heads: int, rpm: int, track_size=None,
            half_rate=lambda cyl, head: False):
    """Every track of `ours` (the drive's, in cylinder-head order) against
    Greaseweazle's `disk`. `image` is in the same (interleaved) order."""
    codec, MasterTrack = gw_codec
    n_tracks = cyls * heads
    track_len = len(ours) // n_tracks
    # The outermost and innermost cylinders and a few between, unless FULL.
    chosen = set(range(n_tracks)) if FULL else {0, 1, heads, n_tracks - 1}
    if not FULL:
        chosen.update(random.Random(n_tracks).sample(range(n_tracks), 4))
    encode_mismatches, decode_failures = [], []
    at = 0
    for n in range(n_tracks):
        cyl, head = divmod(n, heads)
        reference = disk.mk_track(cyl, head)
        size = track_size or sum(len(s.dam.data) for s in reference.sectors)
        data = image[at:at + size]
        at += size
        if n not in chosen:
            continue
        reference.set_img_track(data)
        theirs = reference.master_track().bits.tobytes()
        mine = ours[n * track_len:(n + 1) * track_len]
        if half_rate(cyl, head):
            mine = undouble(mine)
            if mine is None:
                encode_mismatches.append(f"{cyl}.{head}: a doubled cell isn't followed by an empty one")
                continue
        # The drive's track is a revolution of whole 16-cell words. At 360
        # RPM a revolution isn't one, and where a layout nearly fills it
        # Greaseweazle lengthens its track to keep 1% before the index (and
        # an Amiga track is longer at Paula's 1.97 us cells); so theirs may
        # run on, but only with more gap (4E, 0x9254 in cells; or 00, 0xAAAA).
        # And a disk with one track a little too long for a revolution has
        # every track stretched (one revolution time for the whole disk),
        # where Greaseweazle stretches only that one: so either may run on,
        # but only with gap.
        common = min(len(mine), len(theirs)) & ~1
        extra = (theirs if len(theirs) > len(mine) else mine)[common:]
        if mine[:common] != theirs[:common]:
            encode_mismatches.append(f"{cyl}.{head}: {first_difference(mine, theirs)}")
        elif extra.rstrip(b"\x00") and set(extra[:len(extra) & ~1].hex(" ", 2).split()) - {"9254", "aaaa", "ffff"}:
            encode_mismatches.append(f"{cyl}.{head}: one track runs on past the other with more than gap")
        decoded = disk.decode_flux(cyl, head, MasterTrack(bits=mine, time_per_rev=60 / rpm))
        if decoded.nr_missing() or bytes(decoded.get_img_track()) != data:
            decode_failures.append(f"{cyl}.{head}: {decoded.nr_missing()} sectors missing")
    assert not encode_mismatches, "\n".join(encode_mismatches[:5])
    assert not decode_failures, "\n".join(decode_failures[:5])


@pytest.mark.parametrize("size", SIZES)
def test_pc_formats_by_size(gw_codec, size, tmp_path):
    codec, _ = gw_codec
    disk = codec.get_diskdef(SIZES[size])
    image = random.Random(size).randbytes(size)
    path = tmp_path / "disk.img"
    path.write_bytes(image)
    rpm = 360 if size == 1228800 else 300
    heads = 1 if size in (163840, 184320) else 2
    cyls = 40 if size < 400000 else 80
    compare(gw_codec, disk, image, build(f"size:{size}", path), cyls, heads, rpm)


# FlashFloppy's IMG.CFG sections, and the same geometry for Greaseweazle.
IMG_CFG = """
[os::737280]
cyls = 80
heads = 2
secs = 9
bps = 512
tracks = 0-79
  interleave = 2
  hskew = 1
  cskew = 2

[::409600]
cyls = 40
heads = 2
secs = 10
bps = 512
interleave = 3
h = 0
tracks = 0-39.0
  id = 0
tracks = 0-39.1
  id = 10

[::1798144]
cyls = 80
heads = 2
tracks = 0
  secs = 18
  bps = 512
tracks = 1-79
  secs = 11
  bps = 1024
  gap3 = 30

[::1638400]
cyls = 80
heads = 2
secs = 20
bps = 512
gap3 = 40
id = 0
rate = 500

[::102400]
cyls = 40
heads = 1
secs = 10
bps = 256
cskew = 2
mode = fm
iam  = no

[::733184]
cyls = 80
heads = 2
bps = 256
tracks = 0.0
  secs = 10
  mode = fm
  interleave = 4
  id = 1
tracks = 0.1
  secs = 10
  mode = fm
  interleave = 4
  hskew = 1
  id = 11
tracks = 1-79.0
  secs = 18
  mode = mfm
  interleave = 6
  id = 1
tracks = 1-79.1
  secs = 18
  mode = mfm
  interleave = 6
  hskew = 5
  id = 19

[::1256704]
cyls = 77
heads = 2
interleave = 1
rpm = 360
gap3 = 42
tracks = 0.0
  bps = 128
  secs = 26
  mode = fm
  id = 1
tracks = 0.1
  bps = 1024
  secs = 8
  mode = mfm
  id = 1
tracks = 1-76
  bps = 1024
  secs = 8
  mode = mfm
  id = 1
"""

GW_DEFS = """
disk roland_os
    cyls = 80
    heads = 2
    tracks * ibm.mfm
        secs = 9
        bps = 512
        interleave = 2
        hskew = 1
        cskew = 2
    end
end

disk kaypro_400
    cyls = 40
    heads = 2
    tracks 0-39.0 ibm.mfm
        secs = 10
        bps = 512
        interleave = 3
        h = 0
        id = 0
    end
    tracks 0-39.1 ibm.mfm
        secs = 10
        bps = 512
        interleave = 3
        h = 0
        id = 10
    end
end

disk bung_mgd2
    cyls = 80
    heads = 2
    tracks 0 ibm.mfm
        secs = 18
        bps = 512
    end
    tracks 1-79 ibm.mfm
        secs = 11
        bps = 1024
        gap3 = 30
    end
end

disk ensoniq_1600
    cyls = 80
    heads = 2
    tracks * ibm.mfm
        secs = 20
        bps = 512
        gap3 = 40
        id = 0
        rate = 500
    end
end

disk osborne_sssd
    cyls = 40
    heads = 1
    tracks * ibm.fm
        secs = 10
        bps = 256
        cskew = 2
        iam = no
    end
end

disk tsc_flex
    cyls = 80
    heads = 2
    tracks 0.0 ibm.fm
        secs = 10
        bps = 256
        interleave = 4
        id = 1
    end
    tracks 0.1 ibm.fm
        secs = 10
        bps = 256
        interleave = 4
        hskew = 1
        id = 11
    end
    tracks 1-79.0 ibm.mfm
        secs = 18
        bps = 256
        interleave = 6
        id = 1
    end
    tracks 1-79.1 ibm.mfm
        secs = 18
        bps = 256
        interleave = 6
        hskew = 5
        id = 19
    end
end

disk compupro_dsdd
    cyls = 77
    heads = 2
    tracks 0.0 ibm.fm
        secs = 26
        bps = 128
        gap3 = 42
        rpm = 360
        id = 1
    end
    tracks 0.1 ibm.mfm
        secs = 8
        bps = 1024
        gap3 = 42
        rpm = 360
        id = 1
    end
    tracks 1-76 ibm.mfm
        secs = 8
        bps = 1024
        gap3 = 42
        rpm = 360
        id = 1
    end
end
"""

CFG_CASES = {  # image name and size: Greaseweazle's name, cylinders, heads, rpm, half-rate tracks
    ("S330.os.img", 737280): ("roland_os", 80, 2, 300, ()),
    ("CPM.IMG", 409600): ("kaypro_400", 40, 2, 300, ()),
    ("GAME.IMG", 1798144): ("bung_mgd2", 80, 2, 300, ()),
    ("EPS.IMG", 1638400): ("ensoniq_1600", 80, 2, 300, ()),
    ("OS1.IMG", 102400): ("osborne_sssd", 40, 1, 300, ()),
    ("FLEX.IMG", 733184): ("tsc_flex", 80, 2, 300, ((0, 0), (0, 1))),
    ("CPM8.IMG", 1256704): ("compupro_dsdd", 77, 2, 360, ((0, 0),)),
}


@pytest.mark.parametrize("name,size", CFG_CASES)
def test_img_cfg_formats(gw_codec, name, size, tmp_path):
    codec, _ = gw_codec
    (tmp_path / "IMG.CFG").write_text(IMG_CFG)
    (tmp_path / "gw.cfg").write_text(GW_DEFS)
    gw_name, cyls, heads, rpm, half = CFG_CASES[(name, size)]
    disk = codec.get_diskdef(gw_name, str(tmp_path / "gw.cfg"))
    image = random.Random(size).randbytes(size)
    path = tmp_path / "disk.img"
    path.write_bytes(image)
    compare(gw_codec, disk, image, build(f"cfg:{tmp_path / 'IMG.CFG'}:{name}:{size}", path), cyls, heads, rpm,
            half_rate=lambda cyl, head: (cyl, head) in half)


def test_amiga_adf(gw_codec, tmp_path):
    """An ADF: AmigaDOS tracks, against Greaseweazle's amiga.amigados."""
    codec, _ = gw_codec
    disk = codec.get_diskdef("amiga.amigados")
    size = 80 * 2 * 11 * 512
    image = random.Random("adf").randbytes(size)
    path = tmp_path / "disk.adf"
    path.write_bytes(image)
    compare(gw_codec, disk, image, build("adf", path), 80, 2, 300, track_size=11 * 512)
