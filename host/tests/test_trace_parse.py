"""The trace parser, on lines in the firmware's format (the same strings as
test/native/test_main.cpp). No hardware needed."""
from __future__ import annotations

import pytest

from fddproto import trace

pytestmark = pytest.mark.sim

SEEK = [
    "READY FDDSynthPro 0.1.0 slice0",
    "SIG 1000 DRVSB=ON MOTEB=ON SIDE1=OFF",
    "STEP 1500 OUT x80 237000",
    "SIG 240000 DIR=ON",
    "STEP 241000 IN x2 3000",
    "MARK 250000",
    "SIG 300000 DRVSB=OFF MOTEB=OFF",
    "DROP 3",
]


def test_parse_lines():
    t = trace.parse(SEEK)
    assert len(t.changes) == 3
    assert t.changes[0].signals == {"DRVSB": True, "MOTEB": True, "SIDE1": False}
    assert t.steps[0] == trace.StepRun(1500, "OUT", 80, 237000)
    assert t.steps[1].delta == 2
    assert t.dropped == 3
    assert t.marks == [250000]
    assert t.other == ["READY FDDSynthPro 0.1.0 slice0"]


def test_derived_values():
    t = trace.parse(SEEK)
    assert t.step_count == 82
    assert t.net_steps == -78
    assert t.steps[0].interval_us == 3000
    assert trace.StepRun(0, "IN", 1, 0).interval_us is None
    assert t.asserted_signals() == {"DRVSB", "MOTEB", "DIR"}
    assert t.final_state() == {"DRVSB": False, "MOTEB": False, "SIDE1": False, "DIR": True}


def test_activity_lines():
    t = trace.parse([
        "ACT 10 MOTOR_ON",
        "ACT 100 STEP IN 1",
        "ACT 110 SEEK_START IN 2",
        "ACT 880 SEEK_END 79 79",
        "ACT 900 BUMP OUTER",
        "ACT 950 STEP OUT 78",
        "ACT 990 MOTOR_OFF",
    ])
    assert t.sounds() == ["MOTOR_ON", "STEP", "SEEK_START", "SEEK_END", "BUMP", "STEP", "MOTOR_OFF"]
    assert t.activity[3].args == ["79", "79"]
    assert t.act_moving_steps() == 80  # 79 in the seek (its first is the STEP before it), plus one
    assert t.empty  # sound events alone aren't cable activity


def test_empty_and_noise():
    t = trace.parse(["", "# h help", "STATS events=0 dropped=0 steps=0 ring=0", "STEP garbage"])
    assert t.empty
    assert len(t.other) == 3


def test_summary_is_in_time_order():
    text = trace.summary(trace.parse(SEEK))
    lines = text.splitlines()
    assert lines[0].startswith("3 signal changes, 82 steps in 2 runs (net -78), 3 dropped")
    assert "OUT x80, every 3.0 ms" in lines[2]
    assert lines[1].split()[0] == "0.0"
    assert "DRVSB=OFF" in lines[-1]
