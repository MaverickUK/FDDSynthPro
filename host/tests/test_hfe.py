"""HFE flux images (src/hfe.*) against Greaseweazle, which writes them: for
HFE files Greaseweazle makes, v1 and v3, the drive must serve exactly the
cells Greaseweazle recorded. Then copy protection's tricks, which no sector
image can hold: a bad CRC, two sectors with one ID, a long track, weak bits
and a changing bit rate, each served as recorded.

The tracks come from test/tools/build_track hfe:FILE (each the same length,
the longest's; a shorter one carries on from its start).
"""
from __future__ import annotations

import random
import re
import subprocess

import pytest
from bitarray import bitarray

from fddproto.paths import REPO
from test_track_encoder import FULL, TOOL, need

pytestmark = pytest.mark.sim


@pytest.fixture(scope="module")
def gw():
    try:
        from greaseweazle.codec import codec
        from greaseweazle.image.hfe import HFE
        from greaseweazle.track import MasterTrack
    except ImportError:
        need(False, "greaseweazle not installed: pip install -r host/requirements.txt")
    return codec, HFE, MasterTrack


def make_hfe(gw, path, tracks: dict, version: int, bitrate: int | None = None):
    """An HFE of `tracks` ((cyl, head): a codec track or a MasterTrack), as
    Greaseweazle writes one."""
    _, HFE, _ = gw
    hfe = HFE(str(path), None)
    hfe.opts.version = version
    if bitrate:
        hfe.opts.bitrate = bitrate
    for (cyl, head), t in sorted(tracks.items()):
        hfe.emit_track(cyl, head, t)
    path.write_bytes(hfe.get_image())


def served(path) -> tuple[list[bytes], dict]:
    """The drive's tracks for an HFE, and what it says of the file."""
    need(TOOL.exists(), "build_track not built: run test/run_native_tests.sh")
    r = subprocess.run([str(TOOL), f"hfe:{path}"], capture_output=True)
    assert r.returncode == 0, r.stderr.decode()
    info = dict(re.findall(r"(\w+)=(\d+)", r.stderr.decode()))
    n = int(info["words"]) * 2
    return [r.stdout[i:i + n] for i in range(0, len(r.stdout), n)], {k: int(v) for k, v in info.items()}


def same_cells(mine: bytes, bits: bitarray) -> bool:
    """The drive's track holds `bits` from its start, then carries on from
    the start again (the disk loops)."""
    want = bitarray(endian="big")
    while len(want) < len(mine) * 8:
        want += bits
    return mine == want[:len(mine) * 8].tobytes()


def disk_tracks(gw, fmt: str, image: bytes, cyls: int, heads: int, per_track: int):
    codec, _, _ = gw
    disk = codec.get_diskdef(fmt)
    out = {}
    for cyl in range(cyls):
        for head in range(heads):
            t = disk.mk_track(cyl, head)
            n = (cyl * heads + head) * per_track
            t.set_img_track(image[n:n + per_track])
            out[cyl, head] = t
    return disk, out


def chosen(n_tracks: int) -> list[int]:
    if FULL:
        return list(range(n_tracks))
    return sorted({0, 1, n_tracks - 2, n_tracks - 1} | set(random.Random(n_tracks).sample(range(n_tracks), 4)))


@pytest.mark.parametrize("version", [1, 3])
@pytest.mark.parametrize("fmt,size,sectors", [("ibm.1440", 1474560, 18), ("ibm.720", 737280, 9)])
def test_pc_disk(gw, tmp_path, version, fmt, size, sectors):
    image = random.Random(size + version).randbytes(size)
    disk, tracks = disk_tracks(gw, fmt, image, 80, 2, sectors * 512)
    path = tmp_path / "disk.hfe"
    make_hfe(gw, path, tracks, version)
    mine, info = served(path)
    assert info["cyls"] == 80 and info["heads"] == 2 and info["variable"] == 0
    assert info["period_us"] == 200000  # a 300 RPM revolution
    for n in chosen(160):
        cyl, head = divmod(n, 2)
        assert same_cells(mine[n], tracks[cyl, head].master_track().bits), f"{cyl}.{head}"
    # And they're the cells the drive's own encoder makes for that image.
    ours = subprocess.run([str(TOOL), "1440" if sectors == 18 else "720", "/dev/stdin"], input=image,
                          capture_output=True, check=True).stdout
    assert ours == b"".join(mine)


def test_amiga_disk(gw, tmp_path):
    image = random.Random("adf").randbytes(901120)
    disk, tracks = disk_tracks(gw, "amiga.amigados", image, 80, 2, 11 * 512)
    path = tmp_path / "amiga.hfe"
    make_hfe(gw, path, tracks, 1)
    mine, info = served(path)
    for n in chosen(160):
        cyl, head = divmod(n, 2)
        assert same_cells(mine[n], tracks[cyl, head].master_track().bits), f"{cyl}.{head}"
        # Greaseweazle's decoder reads the drive's track back.
        decoded = disk.decode_flux(cyl, head, gw[2](bits=_bits(mine[n]), time_per_rev=0.2))
        assert decoded.nr_missing() == 0


def test_fm_disk_is_double_rate(gw, tmp_path):
    """HFE keeps FM at double rate, each cell doubled: served as it is."""
    codec, _, MasterTrack = gw
    defs = tmp_path / "fm.cfg"
    defs.write_text("disk osborne\n cyls = 40\n heads = 1\n tracks * ibm.fm\n  secs = 10\n  bps = 256\n"
                    "  cskew = 2\n  iam = no\n end\nend\n")
    disk = codec.get_diskdef("osborne", str(defs))
    image = random.Random("fm").randbytes(102400)
    tracks = {}
    for cyl in range(40):
        t = disk.mk_track(cyl, 0)
        t.set_img_track(image[cyl * 2560:(cyl + 1) * 2560])
        tracks[cyl, 0] = t
    path = tmp_path / "fm.hfe"
    make_hfe(gw, path, tracks, 1)
    mine, info = served(path)
    assert info["rate"] == 250000  # FM's 125 kbit/s, doubled
    for cyl in (0, 17, 39):
        decoded = disk.decode_flux(cyl, 0, MasterTrack(bits=_undouble(mine[cyl]), time_per_rev=0.2))
        assert decoded.nr_missing() == 0
        assert bytes(decoded.get_img_track()) == image[cyl * 2560:(cyl + 1) * 2560]


def _bits(b: bytes) -> bitarray:
    x = bitarray(endian="big")
    x.frombytes(b)
    return x


def _undouble(b: bytes) -> bitarray:
    """Greaseweazle doubles each FM cell as an empty cell, then the cell."""
    return _bits(b)[1::2]


# ---- Copy protection: what only a flux image can hold ---------------------------------------


def _protected_track(gw, cyl, head, image):
    """A 1.44 MB track's bits, as Greaseweazle encodes them."""
    codec, _, _ = gw
    t = codec.get_diskdef("ibm.1440").mk_track(cyl, head)
    t.set_img_track(image)
    return t, t.master_track().bits.copy()


def test_protection_tricks(gw, tmp_path):
    codec, _, MasterTrack = gw
    disk = codec.get_diskdef("ibm.1440")
    rng = random.Random("protect")
    image = {(c, h): rng.randbytes(9216) for c in range(80) for h in range(2)}
    tracks = {k: disk.mk_track(*k) for k in image}
    for k, t in tracks.items():
        t.set_img_track(image[k])
    masters = {}

    # 10.0: a sector with a bad CRC (one data cell flipped, CRC left alone).
    t, bits = _protected_track(gw, 10, 0, image[10, 0])
    s5 = t.sectors[5].dam
    flip = s5.start + 64 * 16 + 1  # a data cell well inside sector 6's data
    bits[flip] = not bits[flip]
    masters[10, 0] = MasterTrack(bits=bits, time_per_rev=0.2)

    # 20.1: sector 3's ID field copied over sector 4's: two sectors called 3.
    t, bits = _protected_track(gw, 20, 1, image[20, 1])
    a, b = t.sectors[2].idam, t.sectors[3].idam
    bits[b.start:b.end] = bits[a.start:a.end]
    masters[20, 1] = MasterTrack(bits=bits, time_per_rev=0.2)

    # 30.0: a long track: 4% more gap before the index.
    t, bits = _protected_track(gw, 30, 0, image[30, 0])
    long_bits = bits + bitarray("01" * (len(bits) * 4 // 200), endian="big")
    masters[30, 0] = MasterTrack(bits=long_bits, time_per_rev=0.208)

    for k, m in masters.items():
        tracks[k] = m
    path = tmp_path / "protected.hfe"
    make_hfe(gw, path, tracks, 1, bitrate=500)
    mine, info = served(path)

    # Every track the long one's length; that one's revolution 4% longer.
    assert info["words"] * 16 >= len(long_bits)
    assert info["period_us"] > 200000 * 1.035
    # The tricks are there, cell for cell.
    for (cyl, head), m in masters.items():
        assert mine[cyl * 2 + head][:len(m.bits) // 8] == m.bits.tobytes()[:len(m.bits) // 8], f"{cyl}.{head}"
    # And a controller sees them: Greaseweazle's decoder finds sector 6 bad,
    # and sector 3 twice on 20.1 (so sector 4 missing).
    d = disk.decode_flux(10, 0, MasterTrack(bits=_bits(mine[20]), time_per_rev=0.2))
    assert d.nr_missing() == 1 and not d.has_sec(5)
    d = disk.decode_flux(20, 1, MasterTrack(bits=_bits(mine[41]), time_per_rev=0.2))
    assert not d.has_sec(3) and d.has_sec(2)


def test_weak_bits_and_variable_rate(gw, tmp_path):
    """HFE v3: weak bits come as empty cells (the drive fills them with noise
    every revolution); a rate that changes along a track is counted."""
    codec, _, MasterTrack = gw
    disk = codec.get_diskdef("ibm.720")
    rng = random.Random("v3")
    tracks = {}
    for c in range(80):
        for h in range(2):
            t = disk.mk_track(c, h)
            t.set_img_track(rng.randbytes(4608))
            tracks[c, h] = t
    _, bits = None, tracks[5, 0].master_track().bits.copy()
    weak = (40000, 800)
    tracks[5, 0] = MasterTrack(bits=bits, time_per_rev=0.2, weak=[weak])
    # 6.1: the second half of the track 6% fast, as rate-based protections do.
    bits6 = tracks[6, 1].master_track().bits.copy()
    half = len(bits6) // 2
    ticks = [1.0] * half + [0.94] * (len(bits6) - half)
    tracks[6, 1] = MasterTrack(bits=bits6, time_per_rev=0.2 * (0.5 + 0.47), bit_ticks=ticks)
    path = tmp_path / "v3.hfe"
    make_hfe(gw, path, tracks, 3, bitrate=250)
    mine, info = served(path)
    assert info["variable"] == 1
    track = _bits(mine[10])
    s, n = weak
    assert not track[s + 8:s + n - 8].any(), "weak bits come as empty cells"
    assert track[:s - 8] == bits[:s - 8] and track[s + n + 8:len(bits)] == bits[s + n + 8:]
    # The variable-rate track's cells are all there, served at the disk's rate.
    assert same_cells(mine[13], bits6)
