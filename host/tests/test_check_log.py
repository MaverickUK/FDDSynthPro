"""scripts/check_log.py: what it makes of console logs from a session on a
real computer, clean and with each kind of trouble."""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import check_log  # noqa: E402

pytestmark = pytest.mark.sim

STATS = ("STATS events=10 dropped=0 steps=8 ring=0 core1=5 interface=ibmpc cable=twisted drive=a written=36 "
         "write_bad_crc=0 write_unplaced=0 bursts_dropped=0 capture_overruns={overruns} write_faults=0 "
         "decode_kbps=1650 finish_us_max=4100 audio_underruns=1 audio_gap_us_max=27000 act_dropped=0 oled_aborts=0")

CLEAN = [
    "READY FDDSynthDuo 0.23.6 slice5",
    "SIG 1000000 DRVSB=ON MOTEB=ON",
    "ACT 1000100 MOTOR_ON",
    "SIG 1200000 DIR=ON",
    "ACT 1210000 SEEK_START IN 1",
    "ACT 1240000 SEEK_END 10 10",
    "STEP 1210000 IN x10 30000",
    "SIG 1300000 WGATE=ON",
    "SIG 1310000 WGATE=OFF",
    "# saved: ! restarts the drive with it",
    "# restarting",
    "READY FDDSynthDuo 0.23.6 slice5",
    "SIG 50000 DRVSB=OFF MOTEB=OFF",
    STATS.format(overruns=0),
]


def test_clean_session():
    r = check_log.check(CLEAN)
    assert r.problems == []
    summary = "\n".join(r.summary)
    assert "0.23.6" in summary
    assert "step rate in seeks: fastest 3.3 ms" in summary
    assert "cylinders reached: 1-10" in summary
    assert "WGATE asserted 1 times" in summary
    assert "restarts: 1, each asked for" in summary


def test_restart_on_its_own():
    log = CLEAN[:2] + ["SIG 1500000 DIR=OFF", "SIG 2000 DIR=ON"] + CLEAN[2:]
    r = check_log.check(log)
    assert any("without being asked" in p and "line(s) 4" in p for p in r.problems)


def test_clock_wrap_is_not_a_restart():
    log = ["SIG 4294000000 DIR=ON", "SIG 100000 DIR=OFF"]
    assert check_log.check(log).problems == []


def test_lost_writes_and_knocks():
    log = CLEAN[:-1] + [
        "# WRITE FAULT on A: a sector the card refused; write-protected until the disk is inserted again",
        "ACT 1500000 BUMP INNER",
        STATS.format(overruns=2),
    ]
    r = check_log.check(log)
    text = "\n".join(r.problems)
    assert "WRITE FAULT" in text
    assert "past the last cylinder" in text
    assert "capture_overruns=2" in text


def test_no_stats_line():
    r = check_log.check(CLEAN[:-1])
    assert any("no `s` line" in n for n in r.notes)
