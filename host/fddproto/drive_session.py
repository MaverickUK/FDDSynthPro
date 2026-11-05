"""A recording session with a real drive: what the host does, and the log
of when it did it, for cutting a sample pack from the recording.

The Feather (or a Greaseweazle) drives a real 3.5-inch drive through every
sound a pack needs, with pauses between, while a microphone records it.
Each action is logged with the times it started and ended, in seconds from
the start of the session, so scripts/make_drive_pack.py can find each sound
in the recording. Inserts and ejects are done by hand when prompted.

The Feather's firmware steps every 26 ms (a 13 ms STEP pulse, then 13 ms),
looks for track 0 on every seek to 0 (stepping out until TRK00), and caps
seeks at 79 (test/native/virtual_hosts.h, VirtualAdafruitFloppy). So:
seeks here never end at 0 unless they mean to, and a bump (a step against
the outer stop) needs `gw seek --force` to a negative cylinder, which a
firmware may refuse; the cutter reports it if no bump was heard.
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

# Lengths of the seeks recorded, in steps, each way.
SEEK_LENGTHS = (2, 3, 5, 10, 20, 40, 78)
# Cylinders where single steps are recorded (the click changes across the disk).
STEP_CYLINDERS = (10, 30, 50, 70)
INSERTS = 5  # inserts and ejects, each


@dataclass
class Action:
    """One thing the host does. `kind` is what (seek, spin, read, write,
    prompt, pause); `label` is the sound it's for, which the cutter looks
    up (position: only getting somewhere, not a sound)."""
    kind: str
    label: str
    args: dict = field(default_factory=dict)


@dataclass
class Logged:
    """An action as it happened: its times in seconds from the session's
    start, and what gw said."""
    kind: str
    label: str
    args: dict
    t0: float
    t1: float
    ok: bool = True
    output: str = ""


def plan(writes: bool = False, repeats: int = 2) -> list[Action]:
    """The session, in order: about 6 minutes plus the prompts. Pauses keep the sounds
    apart, so each can be found and cut on its own."""
    a: list[Action] = [Action("pause", "silence", {"s": 4.0})]  # the room's noise, to measure clicks against
    a.append(Action("seek", "position", {"cyl": 40}))  # wherever the head was, it's at 40 now
    a.append(Action("pause", "gap", {"s": 2.0}))
    for _ in range(repeats):
        for c in STEP_CYLINDERS:
            a += [Action("seek", "position", {"cyl": c}), Action("pause", "gap", {"s": 1.5}),
                  Action("seek", "step_in", {"cyl": c + 1}), Action("pause", "gap", {"s": 1.5}),
                  Action("seek", "step_out", {"cyl": c}), Action("pause", "gap", {"s": 1.5})]
    for n in SEEK_LENGTHS:
        for _ in range(repeats):
            a += [Action("seek", "position", {"cyl": 1}), Action("pause", "gap", {"s": 1.5}),
                  Action("seek", f"seek_in_{n}", {"cyl": 1 + n}), Action("pause", "gap", {"s": 1.5}),
                  Action("seek", f"seek_out_{n}", {"cyl": 1}), Action("pause", "gap", {"s": 1.5})]
    # The outer stop: to 0 (a search for TRK00), then past it, if the
    # firmware will.
    a += [Action("seek", "position", {"cyl": 0}), Action("pause", "gap", {"s": 1.5})]
    for _ in range(3):
        a += [Action("seek", "bump", {"cyl": -3, "force": True}), Action("pause", "gap", {"s": 1.5}),
              Action("seek", "position", {"cyl": 0}), Action("pause", "gap", {"s": 1.5})]
    # The motor: a long read of one track (spin-up, the disk turning), then
    # quiet while it spins down.
    a += [Action("seek", "position", {"cyl": 40}), Action("pause", "gap", {"s": 1.5})]
    for _ in range(repeats):
        a += [Action("spin", "spin", {"cyl": 40, "revs": 25}), Action("pause", "spindown", {"s": 4.0})]
    # Real work, as a PC would do it: reading (and writing) a few tracks.
    a += [Action("read", "read", {"tracks": "c=38-42"}), Action("pause", "spindown", {"s": 4.0})]
    if writes:
        a += [Action("write", "write", {"tracks": "c=38-42"}), Action("pause", "spindown", {"s": 4.0})]
    for _ in range(INSERTS):
        a += [Action("prompt", "eject", {"say": "EJECT the disk"}), Action("pause", "gap", {"s": 1.5}),
              Action("prompt", "insert", {"say": "INSERT the disk"}), Action("pause", "gap", {"s": 1.5})]
    a.append(Action("pause", "silence", {"s": 3.0}))
    return a


def save(path: Path, entries: list[Logged]):
    path.write_text("".join(json.dumps(asdict(e)) + "\n" for e in entries))


def load(path: Path) -> list[Logged]:
    return [Logged(**json.loads(line)) for line in path.read_text().splitlines() if line.strip()]


def duration_s(actions: list[Action]) -> float:
    """Roughly how long the session takes: pauses, about 1 s a gw command
    on top of its steps, 6 s a spin, 10 s a read or write, 4 s a prompt."""
    t = 0.0
    for x in actions:
        t += {"pause": x.args.get("s", 0), "seek": 1.0 + 0.026 * 40, "spin": 6.0, "read": 10.0,
              "write": 15.0, "prompt": 4.0}[x.kind]
    return t
