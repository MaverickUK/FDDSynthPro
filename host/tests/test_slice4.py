"""Slice 4: sound events, with the host driving the drive.

Exit criterion: scripted `gw` sessions produce the expected sound events
every time, and every head move the cable carried is accounted for.
"""
from __future__ import annotations

import pytest

from conftest import require_build
from fddproto import trace

pytestmark = pytest.mark.slice4

QUIET_S = 0.5
TIMEOUT_S = 30


@pytest.fixture(autouse=True)
def drive_build(firmware_slice):
    require_build(firmware_slice, drive_build=True)


@pytest.fixture
def capture(drive, run_dir):
    """Runs an action and returns the trace of everything that followed."""
    def run(name: str, action) -> trace.Trace:
        mark = drive.mark()
        action()
        drive.wait_quiet(QUIET_S, timeout=TIMEOUT_S)
        lines = drive.lines_since(mark)
        (run_dir / f"{name}.txt").write_text("\n".join(lines) + "\n")
        return trace.parse(lines)
    return run


def test_motor_on_then_off(capture, gw):
    t = capture("rpm", gw.rpm)
    motor = [s for s in t.sounds() if s.startswith("MOTOR")]
    assert motor == ["MOTOR_ON", "MOTOR_OFF"], t.sounds()


def test_every_step_is_heard(capture, gw):
    """A full seek and back: the sounds account for every step on the cable,
    as clicks (slow hosts) or seeks (fast ones), and the host never knocks
    the head against a stop."""
    assert gw.seek(0).ok
    for name, cyl in (("in", 79), ("out", 0)):
        t = capture(f"seek_{name}", lambda: gw.seek(cyl))
        assert t.act_moving_steps() == t.step_count == 79, (t.step_count, t.sounds())
        assert "BUMP" not in t.sounds()


def test_single_steps_click(capture, gw):
    assert gw.seek(10).ok
    t = capture("one_step", lambda: gw.seek(11))
    assert t.sounds() == ["STEP"], t.sounds()
    assert t.activity[0].args == ["IN", "11"]


def test_eject_and_insert(drive, capture):
    def toggle_twice():
        drive.command("d", "# disk")
        drive.command("d", "# disk")
    t = capture("disk", toggle_twice)
    assert t.sounds() == ["EJECT", "INSERT"], t.sounds()
