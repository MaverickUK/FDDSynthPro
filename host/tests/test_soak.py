"""A long soak on the rig: random reads, writes, swaps and restarts, for
FDDPRO_SOAK_HOURS (default 1), stopping at the first mismatch or fault.

It uses the PC images that have copies in FDDPRO_IMAGES (1.44 MB and 720
KB), switching between them with `]` and with `e device0=...` and `!` (a
restart fills PSRAM afresh, where the cache-line bug of 0.22 showed). Every
write is undone and checked undone, so the images keep matching their
copies. It puts device0 back as it found it. Progress goes to
host/runs/<stamp>-soak.log as it runs.

    PIO_ENV=bb48_drive FDDPRO_SOAK_HOURS=8 scripts/rig.sh -m soak
"""
from __future__ import annotations

import os
import random
import time
from pathlib import Path

import pytest

from conftest import require_build
from fddproto.paths import RUNS

pytestmark = pytest.mark.soak

FORMATS = {1474560: ("ibm.1440", 18), 737280: ("ibm.720", 9)}
HOURS = float(os.environ.get("FDDPRO_SOAK_HOURS", "1"))
FAULTS = ("write_faults", "capture_overruns", "write_bad_crc", "bursts_dropped")


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


def stats(drive) -> dict[str, str]:
    return dict(i.split("=", 1) for i in drive.command("s", "STATS").split()[1:] if "=" in i)


def test_soak(drive, gw, tmp_path, session_stamp):
    folder = Path(os.environ.get("FDDPRO_IMAGES", "")).expanduser()
    if not folder.is_dir():
        pytest.skip("set FDDPRO_IMAGES")
    images = sorted(p.name for p in folder.glob("*.IMG") if p.stat().st_size in FORMATS)
    if len(images) < 2:
        pytest.skip("needs two or more 1.44 MB or 720 KB images with copies")
    log_path = RUNS / f"{session_stamp}-soak.log"
    log = log_path.open("a")

    def note(text: str):
        line = f"{time.strftime('%H:%M:%S')} {text}"
        log.write(line + "\n")
        log.flush()

    s = stats(drive)
    before_device0 = drive.set_setting(f"volume={s['volume']}").get("device0", "")
    base = {k: int(s[k]) for k in FAULTS}
    rng = random.Random(int(time.time()))
    counts = dict.fromkeys(("reads", "writes", "full", "swaps", "restarts"), 0)
    deadline = time.monotonic() + HOURS * 3600

    def serve(name: str, restart: bool):
        if restart:
            drive.set_setting(f"device0={name}")
            drive.restart()
            counts["restarts"] += 1
        else:
            for _ in range(32):  # `]` round the card's list to it
                if drive.command("i", "IMAGE").split()[1] == name:
                    break
                mark = drive.mark()
                drive.send("]")
                drive.wait_for(lambda l: " INSERT" in l, mark, 15)
            counts["swaps"] += 1
        served = drive.command("i", "IMAGE").split()[1]
        assert served == name, (served, name)
        assert gw.seek(0).ok

    def read_tracks(spec: str, data: bytes, fmt: str, per: int, tracks: list[tuple[int, int]]):
        out = tmp_path / "r.img"
        r = gw.read(out, fmt=fmt, tracks=spec)
        assert r.ok, r.output
        got = out.read_bytes()
        for c, h in tracks:
            n = (c * 2 + h) * per
            assert got[n:n + per] == data[n:n + per], f"track {c}.{h} differs"

    try:
        name = rng.choice(images)
        serve(name, restart=True)
        step = 0
        while time.monotonic() < deadline:
            step += 1
            data = (folder / name).read_bytes()
            fmt, secs = FORMATS[len(data)]
            per = secs * 512
            roll = rng.random()
            if roll < 0.08:
                name = rng.choice(images)
                serve(name, restart=rng.random() < 0.5)
                note(f"step {step}: serving {name}")
                continue
            if roll < 0.13:
                out = tmp_path / "full.img"
                r = gw.read(out, fmt=fmt)
                assert r.ok, r.output
                assert out.read_bytes() == data, f"full read of {name} differs"
                counts["full"] += 1
            elif roll < 0.55:
                tracks = [(rng.randrange(80), rng.randrange(2)) for _ in range(3)]
                spec = ":".join([f"c={','.join(str(c) for c, _ in tracks)}", "h=0-1"])
                read_tracks(spec, data, fmt, per, [(c, h) for c, _ in tracks for h in (0, 1)])
                counts["reads"] += 1
            else:
                c, h = rng.randrange(80), rng.randrange(2)
                n = (c * 2 + h) * per
                img = tmp_path / "w.img"
                for payload in (rng.randbytes(per), data[n:n + per]):  # the new data, then the original back
                    img.write_bytes(data[:n] + payload + data[n + per:])
                    r = gw.write(img, fmt=fmt, tracks=f"c={c}:h={h}")
                    assert r.ok, r.output
                    out = tmp_path / "r.img"
                    r = gw.read(out, fmt=fmt, tracks=f"c={c}:h={h}")
                    assert r.ok, r.output
                    assert out.read_bytes()[n:n + per] == payload, f"{name} track {c}.{h} after a write"
                counts["writes"] += 1
            now = stats(drive)
            for k in FAULTS:
                assert int(now[k]) == base[k], f"{k} went from {base[k]} to {now[k]} at step {step}"
            if step % 20 == 0:
                note(f"step {step}: {counts}, decode_kbps={now.get('decode_kbps')} finish_us_max={now.get('finish_us_max')}"
                     f" audio_underruns={now.get('audio_underruns')} audio_gap_us_max={now.get('audio_gap_us_max')}")
        note(f"done: {counts}")
        print(f"soak {HOURS} h: {counts}")
    except Exception as e:
        note(f"FAILED at {counts}: {e}")
        raise
    finally:
        log.close()
        drive.set_setting(f"device0={before_device0}")
        drive.restart()
