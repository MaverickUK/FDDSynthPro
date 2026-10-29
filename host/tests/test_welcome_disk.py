"""The welcome disk, the drive's disk with no card or image
(src/welcome_disk.*), judged by tools independent of our code: fsck.fat finds
it a sound FAT12 volume, and mtools reads its label and README.TXT, which
must be welcome/README.TXT with DOS line endings and the version filled in.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess

import pytest

from fddproto.paths import REPO

pytestmark = pytest.mark.sim

TOOL = REPO / ".pio/native-tests/welcome_image"


@pytest.fixture(scope="module")
def image(tmp_path_factory):
    missing = [t for t in ("mdir", "mcopy", "fsck.fat") if not shutil.which(t)]
    if not TOOL.exists():
        missing.append("welcome_image (run test/run_native_tests.sh)")
    if missing:
        message = f"needs {', '.join(missing)} (brew install mtools dosfstools)"
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)
    path = tmp_path_factory.mktemp("welcome") / "welcome.img"
    subprocess.run([str(TOOL), str(path)], check=True)
    return path


def mtools(*args: str) -> str:
    return subprocess.run(list(args), check=True, capture_output=True, text=True,
                          env=dict(os.environ, MTOOLS_SKIP_CHECK="1")).stdout


def test_a_sound_fat12_floppy(image):
    assert image.stat().st_size == 1474560
    r = subprocess.run(["fsck.fat", "-n", str(image)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def test_label_and_files(image):
    listing = mtools("mdir", "-i", str(image), "::")
    assert "FDDSYNTHPRO" in listing
    assert re.search(r"README\s+TXT", listing), listing


def test_readme_is_the_repos(image, tmp_path):
    out = tmp_path / "README.TXT"
    mtools("mcopy", "-n", "-i", str(image), "::README.TXT", str(out))
    version = re.search(r'#define FIRMWARE_VERSION "([^"]+)"', (REPO / "src/version.h").read_text()).group(1)
    want = (REPO / "welcome/README.TXT").read_text().replace("{VERSION}", version)
    assert out.read_bytes() == want.replace("\n", "\r\n").encode("ascii")
