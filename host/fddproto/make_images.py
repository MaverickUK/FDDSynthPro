"""Writes test images for the SD card. Copy one to the card's root; the drive
serves the first .img in name order. Keep the folder: the slice 3 tests
compare what the host reads with these files (FDDPRO_IMAGES).

    .venv/bin/python -m fddproto.make_images ~/fdd-images
"""
from __future__ import annotations

import random
import sys
from pathlib import Path

from fddproto import gen_disk

IMAGES = {
    # name: (sectors per track, contents)
    "GEN1440.IMG": (18, "generated"),
    "RAND1440.IMG": (18, "random"),
    "RAND720.IMG": (9, "random"),
}


def contents(name: str) -> bytes:
    sectors, kind = IMAGES[name]
    if kind == "generated":
        return gen_disk.image(sectors)
    return random.Random(name).randbytes(80 * 2 * sectors * 512)


def write_all(folder: Path) -> list[Path]:
    folder.mkdir(parents=True, exist_ok=True)
    paths = []
    for name in IMAGES:
        path = folder / name
        path.write_bytes(contents(name))
        paths.append(path)
    return paths


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    for p in write_all(Path(sys.argv[1]).expanduser()):
        print(p)
