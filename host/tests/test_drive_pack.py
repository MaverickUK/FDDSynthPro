"""The recording session's cutter (scripts/make_drive_pack.py), on a
session played out in simulation: the plan from fddproto.drive_session,
timed as the Feather would run it, and a recording made from the
Placeholder pack's sounds where each action makes them, with room noise and
the mic starting late. The cutter must find the offset and every sound."""
from __future__ import annotations

import math
import random
import sys
import wave
import array
from pathlib import Path

import pytest

from fddproto import drive_session
from fddproto.drive_session import Logged

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import make_drive_pack as cutter  # noqa: E402

RATE = cutter.RATE
STEP_S = 0.026  # the Feather's step rate
OFFSET_S = 0.83  # the mic started this long before the log's zero
GW_START_S = 0.35  # a gw command's start-up before it steps
ROTATION_S = 0.2

pytestmark = pytest.mark.sim


def load(name: str) -> list[float]:
    with wave.open(str(REPO / "samples/Placeholder" / f"{name}.wav")) as w:
        assert w.getframerate() == RATE
        return [v / 32768 for v in array.array("h", w.readframes(w.getnframes()))]


class Session:
    """The plan played out: the log, and when each sound happened (log time)."""

    def __init__(self, actions: list[drive_session.Action]):
        self.log: list[Logged] = []
        self.events: list[tuple[float, str]] = []  # (log time, sound)
        self.spins: list[tuple[float, float]] = []  # motor on, motor off
        cyl, t = 40, 0.0
        for x in actions:
            t0 = t
            if x.kind == "pause":
                t += x.args["s"]
            elif x.kind == "prompt":
                self.events.append((t + 1.2, x.label))  # a person, after the prompt
                t += 3.0
            elif x.kind == "seek":
                t += GW_START_S
                target = x.args["cyl"]
                if target < 0:  # past the stop: each step knocks
                    for i in range(-target):
                        self.events.append((t, "bump"))
                        t += STEP_S
                    target = 0
                else:
                    for i in range(abs(target - cyl)):
                        self.events.append((t, "step"))
                        t += STEP_S
                cyl = target
                t += 0.05
            elif x.kind in ("spin", "read"):
                t += GW_START_S
                on = t
                t += 1.0 + (x.args.get("revs", 15)) * ROTATION_S
                self.spins.append((on, t))
                t += 0.05
            self.log.append(Logged(x.kind, x.label, x.args, round(t0, 4), round(t, 4)))

    def recording(self) -> list[float]:
        rnd = random.Random(1)
        n = int((self.log[-1].t1 + OFFSET_S + 3) * RATE)
        x = [rnd.gauss(0, 0.0015) for _ in range(n)]

        def place(t: float, s: list[float], gain: float = 1.0):
            i = int((t + OFFSET_S) * RATE)
            for k, v in enumerate(s[: max(0, n - i)]):
                x[i + k] += v * gain

        sounds = {k: load(k) for k in ("step", "bump", "insert", "eject", "spinup", "spin", "spindown")}
        for t, name in self.events:
            place(t, sounds[name], 0.5)
        # A real drive's disk thumps softly once a revolution (the recording
        # behind the Recorded pack: every 199.8 ms); the Placeholder spin is
        # mostly hiss, so add one.
        thump = [0.02 * math.exp(-i / (0.008 * RATE)) * math.sin(2 * math.pi * 80 * i / RATE)
                 for i in range(int(0.03 * RATE))]
        for on, off in self.spins:
            place(on, sounds["spinup"], 0.3)
            at = on + len(sounds["spinup"]) / RATE
            while at < off:
                place(at, sounds["spin"], 0.3)
                at += len(sounds["spin"]) / RATE
            at = on + 0.3
            while at < off:
                place(at, thump)
                at += ROTATION_S
            place(off, sounds["spindown"], 0.3)
        return x


@pytest.fixture(scope="module")
def cut():
    session = Session(drive_session.plan(repeats=1))
    a = cutter.Analysis(session.recording())
    offset = cutter.find_offset(a, session.log)
    return session, a, offset, cutter.make_sounds(a, session.log, offset)


def test_plan_covers_every_sound():
    labels = {x.label for x in drive_session.plan(writes=True)}
    for want in ("step_in", "step_out", "bump", "spin", "read", "write", "insert", "eject"):
        assert want in labels
    for n in drive_session.SEEK_LENGTHS:
        assert f"seek_in_{n}" in labels and f"seek_out_{n}" in labels
    assert drive_session.duration_s(drive_session.plan()) < 25 * 60


def test_log_round_trip(tmp_path):
    entries = [Logged("seek", "step_in", {"cyl": 11}, 1.0, 1.4, True, "ok")]
    drive_session.save(tmp_path / "s.jsonl", entries)
    assert drive_session.load(tmp_path / "s.jsonl") == entries


def test_finds_the_offset(cut):
    # Anywhere from the true offset to it plus a gw command's start-up (not
    # logged) puts every click in its action; the cutter takes the middle.
    _, _, offset, _ = cut
    assert OFFSET_S - 0.05 < offset < OFFSET_S + GW_START_S


def test_hears_every_step(cut):
    session, _, _, c = cut
    heard, of = c.notes["steps_heard"].split(" of ")
    assert heard == of


def test_measures_the_step_rate(cut):
    _, _, _, c = cut
    assert abs(c.notes["seek_step_ms"] - STEP_S * 1000) < 1.5


def test_cuts_every_sound(cut):
    _, _, _, c = cut
    assert set(c.sounds) == {"step", "seek", "bump", "spinup", "spin", "spindown", "insert", "eject"}
    assert c.notes["bumps_heard"] == 3
    assert c.notes["inserts_heard"] == drive_session.INSERTS
    assert c.notes["ejects_heard"] == drive_session.INSERTS
    # Every seek saved whole, for seek sounds that follow the rate.
    assert len(c.extras) == 2 * len(drive_session.SEEK_LENGTHS)


def test_step_is_a_step(cut):
    # The cut step starts at the click: its loudest moment within 10 ms.
    _, _, _, c = cut
    s = c.sounds["step"]
    loudest = max(range(len(s)), key=lambda i: abs(s[i]))
    assert loudest < 0.012 * RATE


def test_spin_loop_is_whole_revolutions(cut):
    _, _, _, c = cut
    assert abs(c.notes["rotation_ms"] - ROTATION_S * 1000) < 6
    assert abs(len(c.sounds["spin"]) / RATE - cutter.LENGTHS["spin_revs"] * c.notes["rotation_ms"] / 1000) < 0.002


def test_pack_fits_the_pool(cut):
    _, _, _, c = cut
    sounds = cutter.fit(cutter.balance(c.sounds))
    assert sum(len(s) for s in sounds.values()) * 2 <= cutter.POOL_BYTES
    peak = {k: max(abs(v) for v in s) for k, s in sounds.items()}
    assert abs(peak["bump"] - cutter.PEAK) < 1e-6
    assert all(p <= cutter.PEAK + 1e-6 for p in peak.values())
