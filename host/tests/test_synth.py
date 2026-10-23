"""The firmware's mixer (src/synth.*) against the player (fddproto.player),
which is its spec: the same events and pack must give the same samples,
exactly. No hardware needed. test/tools/render_events runs the mixer.
Every test runs with both packs: the stand-in, and the one recorded from a
real drive, which has no insert or eject.
"""
from __future__ import annotations

import array
import os
import random
import subprocess

import pytest

from fddproto import player, trace
from fddproto.paths import REPO

pytestmark = pytest.mark.sim

TOOL = REPO / ".pio/native-tests/render_events"
SESSION = REPO / ".pio/native-tests/pc_session_act.txt"


PACKS = [player.PACK, REPO / "samples" / "Recorded"]


@pytest.fixture(scope="module", params=PACKS, ids=lambda p: p.name)
def pack(request):
    if not TOOL.exists():
        message = "render_events not built: run test/run_native_tests.sh"
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)
    loaded = player.load_pack(request.param)
    loaded.folder = request.param
    return loaded


def mixer(lines: list[str], folder) -> list[int]:
    r = subprocess.run([str(TOOL), str(folder)], input="\n".join(lines).encode(),
                       capture_output=True, check=True)
    return list(array.array("h", r.stdout))


def compare(lines: list[str], pack):
    want = player.render(trace.parse(lines).activity, pack)
    got = mixer(lines, pack.folder)
    assert len(got) == len(want)
    if got != want:
        i = next(k for k in range(len(got)) if got[k] != want[k])
        pytest.fail(f"first difference at sample {i} ({i / pack.rate:.4f} s): mixer {got[i]}, player {want[i]}")


def test_the_virtual_pcs_session(pack):
    if not SESSION.exists():
        pytest.skip("run test/run_native_tests.sh first")
    compare(SESSION.read_text().splitlines(), pack)


def test_motor_off_during_spin_up(pack):
    compare(["ACT 0 MOTOR_ON", "ACT 200000 MOTOR_OFF", "ACT 300000 MOTOR_ON", "ACT 2000000 MOTOR_OFF"], pack)


def test_overlapping_seeks_and_clicks(pack):
    # A seek's tail still sounding when the next seek starts, single steps
    # between, a bump, and the motor running throughout.
    compare([
        "ACT 0 MOTOR_ON",
        "ACT 600000 STEP IN 1", "ACT 603000 SEEK_START IN 2", "ACT 700000 SEEK_END 34 34",
        "ACT 703000 STEP OUT 33", "ACT 706000 SEEK_START OUT 32", "ACT 800000 SEEK_END 0 34",
        "ACT 803000 BUMP OUTER", "ACT 806000 BUMP OUTER",
        "ACT 900000 EJECT", "ACT 1400000 INSERT",
        "ACT 2000000 MOTOR_OFF",
    ], pack)


def test_random_sessions(pack):
    """Random event sequences, as a tracker could produce them."""
    rng = random.Random(9)
    for _ in range(5):
        t, lines, motor, seeking = 0, [], False, False
        for _ in range(60):
            t += rng.choice((3000, 6000, 30000, 200000))
            choice = rng.random()
            if choice < 0.15:
                lines.append(f"ACT {t} {'MOTOR_OFF' if motor else 'MOTOR_ON'}")
                motor = not motor
            elif choice < 0.35 and not seeking:
                lines.append(f"ACT {t} SEEK_START IN 5")
                seeking = True
            elif choice < 0.55 and seeking:
                lines.append(f"ACT {t} SEEK_END 40 20")
                seeking = False
            elif choice < 0.9:
                lines.append(f"ACT {t} STEP OUT 7")
            else:
                lines.append(f"ACT {t} BUMP OUTER")
        compare(lines, pack)
