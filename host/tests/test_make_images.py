"""The SD card test images. No hardware needed."""
from __future__ import annotations

import pytest

from fddproto import gen_disk, make_images
FORMAT_SIZES = {1474560, 737280}  # src/track_source.h: formatForImageSize

pytestmark = pytest.mark.sim


def test_images(tmp_path):
    paths = {p.name: p for p in make_images.write_all(tmp_path)}
    assert gen_disk.first_mismatch(paths["GEN1440.IMG"].read_bytes()) is None
    for name, path in paths.items():
        assert path.stat().st_size in FORMAT_SIZES, name
    # Random images are the same every time, so a copy on the card stays valid.
    assert make_images.contents("RAND720.IMG") == paths["RAND720.IMG"].read_bytes()
