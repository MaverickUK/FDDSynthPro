"""Shared fixtures for the rig tests.

  drive    the drive's serial log (opened once per session, waits for READY)
  gw       the host side: a Greaseweazle, driven through `gw`
  assemble assembles a src/*.pio program for the PIO simulator
  run_dir  a timestamped folder under host/runs/ for this test's evidence;
           on failure the drive log and the Greaseweazle transcript land there too
"""
from __future__ import annotations

import sys
import os
from datetime import datetime

import pytest

from fddproto import pio_sim
from fddproto.drive_log import DriveLog
from fddproto.greaseweazle import Greaseweazle
from fddproto.paths import REPO, RUNS


def pytest_addoption(parser):
    parser.addoption("--save-reference", action="store_true",
                     help="save this run's traces as the new references in host/reference/")


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    report = yield
    setattr(item, f"report_{report.when}", report)
    return report


@pytest.fixture(scope="session")
def assemble():
    """Assembles src/<name>.pio with the real pioasm. Skips without pioasm,
    except in CI, where a missing pioasm must not hide these tests."""
    pioasm = pio_sim.find_pioasm()
    if not pioasm:
        message = "pioasm not found (build once with `pio run`, or set PIOASM to its path)"
        if os.environ.get("CI"):
            pytest.fail(message)
        pytest.skip(message)
    return lambda name: pio_sim.assemble(REPO / "src" / f"{name}.pio", pioasm)


@pytest.fixture(scope="session")
def session_stamp():
    return datetime.now().strftime("%Y%m%d-%H%M%S")


@pytest.fixture(scope="session")
def drive(session_stamp):
    RUNS.mkdir(parents=True, exist_ok=True)
    log = DriveLog(raw_log_path=RUNS / f"{session_stamp}-serial.log")
    log.open()
    log.ready = log.command("h", "READY", timeout=5)  # "READY FDDSynthPro <version> slice<N> [direct]"
    yield log
    log.close()


@pytest.fixture
def firmware_slice(drive) -> int:
    """Which build the board runs: 0 = listen only, 5 = the drive (slices 1-5)."""
    return int(drive.ready.split()[3].removeprefix("slice"))


def require_build(firmware_slice: int, drive_build: bool):
    """Skips unless the board runs the build the test needs."""
    if drive_build and firmware_slice == 0:
        pytest.skip("needs the drive build: PIO_ENV=bb48_drive scripts/rig.sh ...")
    if not drive_build and firmware_slice != 0:
        pytest.skip("needs the listen-only build: scripts/rig.sh ... (PIO_ENV=bb48)")


def rpm_tolerance(gw) -> float:
    """How far from 300 the host's `gw rpm` may read. The Feather's clock runs
    0.4-0.6% fast against the 24 MHz its firmware reports (measured
    2026-11-04: its flux intervals are long by the same ratio, and the BB48
    keeps within 30 ppm of the Mac), so it reads 298.3-298.7: it gets a
    looser bound than a Greaseweazle."""
    return 3.0 if "Adafruit" in gw.info().output else 0.1


@pytest.fixture
def gw():
    """A Greaseweazle, reset to its power-on state before and after the test."""
    g = Greaseweazle()
    g.reset()
    yield g
    g.reset()


@pytest.fixture
def ask(request):
    """Shows a message to the person at the rig, even while pytest captures output."""
    capman = request.config.pluginmanager.getplugin("capturemanager")

    def show(message: str):
        with capman.global_and_fixture_disabled():
            print(f"\n>>> {message}", file=sys.stderr, flush=True)

    return show


@pytest.fixture
def run_dir(request, session_stamp):
    path = RUNS / f"{session_stamp}-{request.node.name}"
    path.mkdir(parents=True, exist_ok=True)
    drive_mark = None
    if "drive" in request.fixturenames:
        drive_mark = request.getfixturevalue("drive").mark()
    # Fetched now, so it's set up before this fixture and torn down after it:
    # a test that names `gw` after a fixture using run_dir would otherwise
    # lose it before its transcript is saved.
    gw = request.getfixturevalue("gw") if "gw" in request.fixturenames else None
    yield path
    report = getattr(request.node, "report_call", None)
    if report is not None and report.failed:
        if drive_mark is not None:
            lines = request.getfixturevalue("drive").lines_since(drive_mark)
            (path / "drive.log").write_text("\n".join(lines) + "\n")
        if gw is not None:
            (path / "gw.log").write_text(gw.transcript())
        (path / "failure.txt").write_text(report.longreprtext)
