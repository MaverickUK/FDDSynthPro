"""A real DOS disk through the whole drive, with no hardware: mtools builds a
FAT12 image, the virtual PC (test/tools/dos_session) mounts it, reads every
file and writes a new one through the drive, then mtools and fsck.fat check
what it left. And two disks, with the drive as A and B: COPY A:*.* B:. They're independent of our code: if the drive or the PC got a
sector, the FAT or a directory entry wrong, they'd say so.
"""
from __future__ import annotations

import os
import random
import shutil
import subprocess
from pathlib import Path

import pytest

from fddproto.paths import REPO

pytestmark = pytest.mark.sim

TOOL = REPO / ".pio/native-tests/dos_session"


@pytest.fixture(scope="module", autouse=True)
def tools():
    missing = [t for t in ("mformat", "mcopy", "mmd", "mdel", "fsck.fat") if not shutil.which(t)]
    if not TOOL.exists():
        missing.append("dos_session (run test/run_native_tests.sh)")
    if missing:
        message = f"needs {', '.join(missing)} (brew install mtools dosfstools)"
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)


def mtools(*args: str):
    subprocess.run(list(args), check=True, capture_output=True, env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))


def make_disk(tmp: Path, kb: int) -> tuple[Path, dict[str, bytes]]:
    """A DOS disk with a volume label, a few files (one deliberately
    fragmented), and a subdirectory with a file in it."""
    rng = random.Random(kb)
    img = tmp / "disk.img"
    mtools("mformat", "-i", str(img), "-C", "-f", str(kb), "-v", "FDDSYNTH", "::")
    files = {
        "AUTOEXEC.BAT": b"@ECHO OFF\r\nPROMPT $P$G\r\nPATH A:\\\r\n",
        "README.TXT": b"FDD Synth Pro test disk.\r\n" * 40,
        "A.BIN": rng.randbytes(30000),
        "GAME.EXE": rng.randbytes(20000),
    }
    for name, data in files.items():
        (tmp / name).write_bytes(data)
        mtools("mcopy", "-i", str(img), str(tmp / name), "::")
    # Delete A.BIN, then copy a big file: it fills A's gap and carries on past
    # GAME.EXE, so its clusters aren't in one run.
    mtools("mdel", "-i", str(img), "::A.BIN")
    del files["A.BIN"]
    files["FRAG.DAT"] = rng.randbytes(60000)
    (tmp / "FRAG.DAT").write_bytes(files["FRAG.DAT"])
    mtools("mcopy", "-i", str(img), str(tmp / "FRAG.DAT"), "::")
    mtools("mmd", "-i", str(img), "::GAMES")
    (tmp / "INSIDE.TXT").write_bytes(b"In a subdirectory.\r\n")
    mtools("mcopy", "-i", str(img), str(tmp / "INSIDE.TXT"), "::GAMES/INSIDE.TXT")
    return img, files


def session(tmp: Path, img: Path, new_name: str, new_data: bytes):
    out = tmp / "after.img"
    extract = tmp / "extracted"
    extract.mkdir()
    (tmp / "new.bin").write_bytes(new_data)
    r = subprocess.run([str(TOOL), str(img), str(out), str(extract), new_name, str(tmp / "new.bin")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return out, extract, r.stdout


def check(tmp: Path, img: Path, files: dict[str, bytes], new_name: str, new_data: bytes):
    out, extract, report = session(tmp, img, new_name, new_data)
    # Every file the PC read through the drive is byte for byte the original.
    for name, data in files.items():
        assert (extract / name).read_bytes() == data, name
    assert "retries=0" in report, report
    # fsck.fat finds the disk consistent: the FATs, directory and chains.
    r = subprocess.run(["fsck.fat", "-n", str(out)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    # mtools reads the new file, the old ones and the subdirectory's.
    got = tmp / "got"
    mtools("mcopy", "-n", "-i", str(out), f"::{new_name}", str(got))
    assert got.read_bytes() == new_data
    for name, data in files.items():
        mtools("mcopy", "-n", "-i", str(out), f"::{name}", str(got))
        assert got.read_bytes() == data, name
    mtools("mcopy", "-n", "-i", str(out), "::GAMES/INSIDE.TXT", str(got))
    assert got.read_bytes() == b"In a subdirectory.\r\n"


def test_dos_session_1440(tmp_path):
    img, files = make_disk(tmp_path, 1440)
    check(tmp_path, img, files, "NEW.TXT", b"Written by the FDD Synth Pro virtual PC.\r\n")


def test_dos_session_720(tmp_path):
    img, files = make_disk(tmp_path, 720)
    check(tmp_path, img, files, "NEW.TXT", b"A 720 KB disk, at 250 kbit/s.\r\n")


def test_big_file_across_tracks_and_heads(tmp_path):
    """100 KB: over 200 sectors, across several tracks and both heads."""
    img, files = make_disk(tmp_path, 1440)
    check(tmp_path, img, files, "BIG.DAT", random.Random(3).randbytes(100_000))


def test_copy_from_a_to_b(tmp_path):
    """The drive as A (1.44 MB) and B (a blank 720 KB disk) on one cable: DOS
    copies every file from A to B, changing drives for each. B must hold them
    all, A must be untouched, and both must pass fsck."""
    a_img, files = make_disk(tmp_path, 1440)
    b_img = tmp_path / "b.img"
    mtools("mformat", "-i", str(b_img), "-C", "-f", "720", "-v", "DRIVEB", "::")
    a_out, b_out = tmp_path / "a_after.img", tmp_path / "b_after.img"
    r = subprocess.run([str(TOOL), "--copy", str(a_img), str(b_img), str(a_out), str(b_out)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "retries=0" in r.stdout and "a_writes=0" in r.stdout, r.stdout
    assert a_out.read_bytes() == a_img.read_bytes()
    for img in (a_out, b_out):
        fsck = subprocess.run(["fsck.fat", "-n", str(img)], capture_output=True, text=True)
        assert fsck.returncode == 0, fsck.stdout + fsck.stderr
    got = tmp_path / "got"
    for name, data in files.items():
        mtools("mcopy", "-n", "-i", str(b_out), f"::{name}", str(got))
        assert got.read_bytes() == data, name


@pytest.mark.parametrize("kb", [1200, 360])
def test_dos_session_five_and_a_quarter(tmp_path, kb):
    """5.25-inch disks, by their size: 1.2 MB at 360 RPM, 360 KB on 40 cylinders."""
    img, files = make_disk(tmp_path, kb)
    check(tmp_path, img, files, "NEW.TXT", f"A {kb} KB disk.\r\n".encode())
