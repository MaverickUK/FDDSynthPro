"""The sample player (fddproto.player): sound events to a WAV. No hardware needed."""
from __future__ import annotations

import pytest

from fddproto import player, trace
from fddproto.paths import REPO

pytestmark = pytest.mark.sim

SESSION = REPO / ".pio/native-tests/pc_session_act.txt"


@pytest.fixture(scope="module")
def pack():
    return player.load_pack(player.PACK)


def loudness(samples, rate, start_s, end_s) -> float:
    """Mean level over a window, as a fraction of full scale."""
    a, b = int(start_s * rate), int(end_s * rate)
    window = samples[a:b]
    return sum(abs(v) for v in window) / max(1, len(window)) / 32768


def events(lines):
    return trace.parse(lines).activity


def test_motor_spins_between_on_and_off(pack):
    out = player.render(events(["ACT 1000000 MOTOR_ON", "ACT 4000000 MOTOR_OFF"]), pack)
    r = pack.rate
    lead = player.LEAD_IN_S
    assert loudness(out, r, 0, lead * 0.9) == 0               # silence before
    assert loudness(out, r, lead + 1.5, lead + 2.5) > 0.01    # the spin loop, well after spin-up
    assert loudness(out, r, lead + 3.0 + 0.6, len(out) / r) == 0  # after spin-down: silent


def test_steps_and_seeks(pack):
    out = player.render(events([
        "ACT 1000000 STEP IN 1",
        "ACT 2000000 STEP IN 2",
        "ACT 2003000 SEEK_START IN 3",
        "ACT 2300000 SEEK_END 80 80",
        "ACT 3000000 BUMP OUTER",
    ]), pack)
    r, lead = pack.rate, player.LEAD_IN_S
    step_len = len(pack.sounds["step"]) / r
    assert loudness(out, r, lead, lead + step_len) > 0.01           # the first click
    assert loudness(out, r, lead + step_len + 0.01, lead + 0.9) == 0  # then quiet
    assert loudness(out, r, lead + 1.05, lead + 1.25) > 0.01        # the seek's buzz
    assert loudness(out, r, lead + 1.4, lead + 1.9) == 0            # over
    assert loudness(out, r, lead + 2.0, lead + 2.1) > 0.01          # the bump


def test_unfinished_loops_run_to_the_end(pack):
    out = player.render(events(["ACT 0 MOTOR_ON"]), pack)
    assert loudness(out, pack.rate, len(out) / pack.rate - 0.1, len(out) / pack.rate) > 0.01


def test_saturates_rather_than_wrapping(pack):
    """Fifty clicks on top of each other overflow 16 bits: the mix holds at
    full scale, as the firmware does, rather than wrapping round."""
    lines = ["ACT 0 MOTOR_ON"] + [f"ACT {1000000 + i * 1000} BUMP OUTER" for i in range(50)]
    out = player.render(events(lines), pack)
    assert max(out) == 32767 or min(out) == -32768
    assert all(-32768 <= v <= 32767 for v in out)


def test_the_virtual_pcs_session(pack, tmp_path):
    """The session test/native/test_virtual_pc.cpp saves: renders, starts
    with the motor and ends spun down."""
    if not SESSION.exists():
        pytest.skip("run test/run_native_tests.sh first")
    out_path = tmp_path / "session.wav"
    assert player.main([str(SESSION), str(out_path)]) == 0
    assert out_path.stat().st_size > 100_000


def test_a_seek_clicks_at_its_rate(pack):
    # A seek alone (its first step's click left out), 3 ms a step: seek.wav's
    # start every 3 ms (cut short, faded), the same each time, for steps 2
    # to 20, then nothing.
    period = 3000 * pack.rate // 1_000_000
    out = player.render(events(["ACT 1000000 SEEK_START IN 2 3000", "ACT 1054000 SEEK_END 20 20"]), pack)
    start = int(player.LEAD_IN_S * pack.rate)
    seek = pack.sounds["seek"]
    assert out[start] == seek[0]
    assert all(out[i] == out[i + period] for i in range(start, start + 18 * period))
    assert any(out[start:start + period])
    length = min(period, len(seek))
    assert all(v == 0 for v in out[start + 18 * period + length:])


def test_seek_click_longer_than_the_step_fades(pack):
    seek = pack.sounds["seek"]
    k = player.seek_clicks(seek, 0, 3000, pack.rate)
    assert k.period == 3000 * pack.rate // 1_000_000
    if k.period < len(seek):
        assert k.length == k.period and 0 < k.fade <= pack.rate // 1000
    slow = player.seek_clicks(seek, 0, 100000, pack.rate)  # slower than the click lasts
    assert slow.length == len(seek) and slow.fade == 0
