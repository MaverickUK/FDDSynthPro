"""Slice 0: listen only. Drive the Greaseweazle and check what the drive saw.

Nothing answers yet: there is no TRK00 and no index, so the Greaseweazle's
seeks and RPM checks fail. The point is the trace of what it tried.

Exit criterion: clean, repeatable traces of a seek and an RPM check, saved
in host/reference/ (run with --save-reference).
"""
from __future__ import annotations

import shutil

import pytest

from conftest import require_build
from fddproto import trace
from fddproto.paths import REFERENCE

pytestmark = pytest.mark.slice0

QUIET_S = 0.5   # the action is over once the trace is quiet this long
TRACE_TIMEOUT_S = 30


@pytest.fixture(autouse=True)
def listen_build(firmware_slice):
    require_build(firmware_slice, drive_build=False)


@pytest.fixture
def capture(drive, gw, run_dir, request):
    """Runs one Greaseweazle action and returns (its result, the drive's trace)."""

    def run(name: str, action):
        mark = drive.mark()
        result = action()
        drive.wait_quiet(QUIET_S, timeout=TRACE_TIMEOUT_S)
        lines = drive.lines_since(mark)
        stats = drive.command("s", "STATS")

        t = trace.parse(lines)
        (run_dir / f"{name}.txt").write_text("\n".join(lines) + "\n")
        report = (f"$ {' '.join(result.command)}\n(exit {result.returncode})\n{result.output}\n"
                  f"{trace.summary(t)}\n{stats}\n")
        (run_dir / f"{name}_summary.txt").write_text(report)
        print(report)
        if request.config.getoption("--save-reference"):
            REFERENCE.mkdir(exist_ok=True)
            shutil.copy(run_dir / f"{name}.txt", REFERENCE / f"slice0_{name}.txt")
            shutil.copy(run_dir / f"{name}_summary.txt", REFERENCE / f"slice0_{name}_summary.txt")
        return result, t

    return run


def test_idle_lines(drive, gw):
    """With the Greaseweazle reset, every input line is released (OFF).
    DENSEL is a level, not a request, so a host may hold it either way."""
    pins = drive.command("p", "PINS")
    asserted = [item for item in pins.split()[1:] if item.endswith("=ON") and not item.startswith("DENSEL")]
    assert not asserted, f"lines asserted while idle (wiring?): {asserted} in {pins}"


def test_seek_trace(capture, gw):
    """A seek: the Greaseweazle selects the drive and steps out, looking for
    track 0. Without TRK00 it gives up after its own step limit."""
    _, t = capture("seek", lambda: gw.seek(2))
    assert not t.empty, "no activity on the cable during gw seek"
    assert not t.dropped, "the trace lost events"
    assert {"DRVSA", "DRVSB"} & t.asserted_signals(), "no drive select during the seek"
    assert any(s.direction == "OUT" for s in t.steps), "no steps outwards (towards track 0)"
    state = t.final_state()
    assert not state.get("DRVSA") and not state.get("DRVSB"), "a drive is still selected afterwards"


def test_rpm_trace(capture, gw):
    """An RPM check turns the motor on and waits for index pulses, which
    don't come yet."""
    _, t = capture("rpm", gw.rpm)
    assert not t.dropped, "the trace lost events"
    assert {"MOTEA", "MOTEB"} & t.asserted_signals(), "no motor enable during gw rpm"
    state = t.final_state()
    assert not state.get("MOTEA") and not state.get("MOTEB"), "a motor is still on afterwards"


def test_drive_letters(capture, gw):
    """Which select and motor pins `--drive A` and `--drive B` use, so the
    firmware answers on the right ones."""
    seen = {}
    for letter in "AB":
        _, t = capture(f"drive_{letter}", lambda: gw.seek(0, motor_on=True, drive=letter))
        seen[letter] = t.asserted_signals() & {"DRVSA", "DRVSB", "MOTEA", "MOTEB"}
    print(f"drive letters: {seen}")
    assert seen["A"] and seen["B"], f"a drive letter asserted nothing: {seen}"
    assert seen["A"] != seen["B"], f"A and B used the same pins: {seen}"
