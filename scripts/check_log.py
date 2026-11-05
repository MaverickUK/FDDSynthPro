#!/usr/bin/env python3
"""Reads a console log from a session on a real computer (docs/hardware-tests.md:
`pio device monitor ... | tee host/runs/realhw-....log`) and says what in it
needs a look: the board restarting, lost writes, the head knocking against
its stops, trace lines lost, and the counters in the last `s` line. It also
summarises what the computer did: which lines it selected with, its step
rate, its writes, its disk changes.

    scripts/check_log.py host/runs/realhw-20261106-1000.log
    scripts/check_log.py LOG --drive a        # the bus setup it ran with (default: from the log's `s` lines)

Exits 1 when it found a problem, so it can end a script.
"""
from __future__ import annotations

import argparse
import re
import statistics
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "host"))

from fddproto import trace as tr  # noqa: E402

# Counters in `s` that should stay 0, and what each means.
FAULTS = {
    "write_faults": "sectors the card refused (each set write-protect)",
    "capture_overruns": "writes lost: core 0 fell a whole capture ring behind",
    "bursts_dropped": "writes lost: no room for their marks",
    "write_bad_crc": "writes the decoder couldn't read (CRC errors)",
    "write_unplaced": "sectors written where the drive didn't expect them",
    "dropped": "trace events lost (the ring overflowed)",
    "act_dropped": "sound events lost",
    "audio_underruns": "times the audio ran dry (1 at power-on is normal)",
    "oled_aborts": "panel frames abandoned",
}
SELECT_LINES = ("DRVSA", "DRVSB", "MOTEA", "MOTEB")


@dataclass
class Report:
    problems: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    summary: list[str] = field(default_factory=list)


def stats_lines(lines: list[str]) -> list[dict[str, str]]:
    out = []
    for line in lines:
        if line.startswith("STATS "):
            out.append(dict(item.split("=", 1) for item in line.split()[1:] if "=" in item))
    return out


def check(lines: list[str]) -> Report:
    r = Report()
    lines = [line.rstrip("\r\n") for line in lines]
    t = tr.parse(lines)

    # The board restarting: its clock (the trace's times) going back to
    # near zero, other than the wrap every 71 minutes. Its boot messages
    # are often lost (the USB port reconnects after them), and `h` prints a
    # READY line too, so READY lines can't tell.
    readies = [i for i, line in enumerate(lines) if line.startswith("READY ")]
    if readies:
        r.summary.append(f"firmware: {lines[readies[0]].split(' ', 1)[1]}")
    resets, prev = [], None
    for i, line in enumerate(lines):
        parts = line.split()
        # SIG lines only: they come out in order (a STEP line carries the
        # time its run began, and ACT lines can lag).
        if len(parts) < 2 or parts[0] != "SIG" or not parts[1].isdigit():
            continue
        now = int(parts[1])
        wrapped = prev is not None and prev > 2**32 - 600_000_000 and now < 600_000_000
        if prev is not None and now + 1_000_000 < prev and not wrapped:
            resets.append(i)
        prev = now
    # Asked for: `!` or `b` just before (its own line can be lost in the
    # reconnect, so `e`'s "saved: ! restarts" counts too).
    asked_marks = ("# restarting", "# rebooting", "# saved: ! restarts")
    unasked = [i + 1 for i in resets if not any(l.startswith(asked_marks) for l in lines[max(0, i - 40):i])]
    if unasked:
        r.problems.append(f"the board restarted {len(unasked)} time(s) without being asked (a crash, or its "
                          f"power): its clock went back at line(s) {', '.join(map(str, unasked))}")
    if resets:
        r.summary.append(f"restarts: {len(resets)}" + (f", {len(resets) - len(unasked)} asked for" if unasked else
                                                       ", each asked for (`!` or `b`)"))

    for i, line in enumerate(lines):
        if line.startswith("# WRITE FAULT"):
            r.problems.append(f"line {i + 1}: {line[2:]}")
        elif line.startswith(("# can't open", "# pack ", "# couldn't save", "# no PSRAM", "# no RAM")):
            if not line.startswith("# pack ") or "incomplete" in line:
                r.notes.append(f"line {i + 1}: {line[2:]}")
    if t.dropped:
        r.notes.append(f"{t.dropped} trace events lost (DROP lines): the trace has gaps")

    # The head: knocks against the stops, and the step rate.
    bumps = Counter(a.args[0] if a.args else "?" for a in t.activity if a.sound == "BUMP")
    if bumps.get("INNER"):
        r.problems.append(f"{bumps['INNER']} step(s) past the last cylinder: the computer thinks the disk has "
                          "more tracks (BIOS drive type, or a 40-track disk stepped as 80)")
    if bumps.get("OUTER"):
        r.notes.append(f"{bumps['OUTER']} step(s) out past cylinder 0: a recalibrate that didn't stop at TRK00 "
                       "(normal for an Amiga's, not for a PC's)")
    intervals = [s.interval_us for s in t.steps if s.interval_us]
    steps = t.step_count
    if steps:
        line = f"steps: {steps} in {len(t.steps)} runs"
        if intervals:
            line += (f"; step rate in seeks: fastest {min(intervals) / 1000:.1f} ms, "
                     f"typical {statistics.median(intervals) / 1000:.1f} ms")
        r.summary.append(line)
    cyls = [int(a.args[1]) for a in t.activity if a.sound in ("STEP", "SEEK_START") and len(a.args) > 1]
    ends = [int(a.args[0]) for a in t.activity if a.sound == "SEEK_END" and a.args]
    if cyls or ends:
        r.summary.append(f"cylinders reached: {min(cyls + ends)}-{max(cyls + ends)}")

    # Which lines the computer used.
    held = t.held_signals()
    used = [name for name in SELECT_LINES if name in held]
    r.summary.append(f"select and motor lines held: {', '.join(used) or 'none'}")
    if "DRVSA" in held and "DRVSB" in held:
        r.notes.append("both drive selects were used: the computer has two drives set up, or the cable is "
                       "the other way round")
    gates = sum(1 for c in t.changes if c.signals.get("WGATE") is True)
    if gates:
        r.summary.append(f"writes: WGATE asserted {gates} times")
    densel = [c.signals["DENSEL"] for c in t.changes if "DENSEL" in c.signals]
    if densel:
        r.summary.append(f"DENSEL (pin 2) last {'ON' if densel[-1] else 'OFF'}, changed {len(densel)} times")
    sounds = Counter(a.sound for a in t.activity)
    if sounds.get("EJECT") or sounds.get("INSERT"):
        r.summary.append(f"disk changes: {sounds.get('EJECT', 0)} ejects, {sounds.get('INSERT', 0)} inserts")
    if sounds.get("MOTOR_ON"):
        r.summary.append(f"motor: on {sounds['MOTOR_ON']} times")

    # The counters, from the last `s` (and how far they moved since the first).
    st = stats_lines(lines)
    if not st:
        r.notes.append("no `s` line in the log: type s at the end of a session to get the counters")
    else:
        first, last = st[0], st[-1]
        for key, what in FAULTS.items():
            if key not in last:
                continue
            value = int(last[key])
            limit = 1 if key == "audio_underruns" else 0
            if value > limit:
                moved = f" ({value - int(first.get(key, 0)):+d} since the first s)" if len(st) > 1 else ""
                (r.problems if key in ("write_faults", "capture_overruns", "bursts_dropped", "write_bad_crc")
                 else r.notes).append(f"{key}={value}{moved}: {what}")
        for key in ("written", "decode_kbps", "finish_us_max", "audio_gap_us_max", "draw_us_max"):
            if key in last:
                r.summary.append(f"{key}={last[key]}")
        if "decode_kbps" in last and 0 < int(last["decode_kbps"]) < 600:
            r.problems.append(f"decode_kbps={last['decode_kbps']}: too close to an HD write's 370 KB/s")
        bus = " ".join(f"{k}={last[k]}" for k in ("interface", "cable", "drive") if k in last)
        if bus:
            r.summary.append(f"bus: {bus}")
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("log", type=Path)
    args = p.parse_args()
    text = args.log.read_text(errors="replace")
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)  # the monitor's colours, if any
    r = check(text.splitlines())
    for title, items in (("PROBLEMS", r.problems), ("WORTH A LOOK", r.notes), ("SUMMARY", r.summary)):
        print(f"{title}:" + ("" if items else " none"))
        for item in items:
            print(f"  - {item}")
    sys.exit(1 if r.problems else 0)


if __name__ == "__main__":
    main()
