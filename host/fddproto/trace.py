"""Parses the drive's trace lines (src/trace_format.h) and summarises them.

    SIG <t_us> <NAME>=ON|OFF ...
    STEP <t_us> IN|OUT x<count> <span_us>
    DROP <n>
    MARK <t_us>

ON means asserted (the line is low). IN is towards higher cylinders.
Other lines (STATS, PINS, # comments) are kept in `other`.
"""
from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class StepRun:
    t_us: int
    direction: str  # "IN" or "OUT"
    count: int
    span_us: int

    @property
    def delta(self) -> int:
        """Cylinders moved: positive inwards."""
        return self.count if self.direction == "IN" else -self.count

    @property
    def interval_us(self) -> float | None:
        """Average time between steps, or None for a single step."""
        return self.span_us / (self.count - 1) if self.count > 1 else None


@dataclass
class Change:
    t_us: int
    signals: dict[str, bool]  # name -> asserted


@dataclass
class Trace:
    steps: list[StepRun] = field(default_factory=list)
    changes: list[Change] = field(default_factory=list)
    dropped: int = 0
    marks: list[int] = field(default_factory=list)
    other: list[str] = field(default_factory=list)

    @property
    def empty(self) -> bool:
        return not self.steps and not self.changes

    @property
    def net_steps(self) -> int:
        return sum(s.delta for s in self.steps)

    @property
    def step_count(self) -> int:
        return sum(s.count for s in self.steps)

    def asserted_signals(self) -> set[str]:
        """Every signal that was asserted at some point."""
        return {name for c in self.changes for name, on in c.signals.items() if on}

    def final_state(self) -> dict[str, bool]:
        """The last reported state of each signal that changed."""
        state: dict[str, bool] = {}
        for c in self.changes:
            state.update(c.signals)
        return state


def parse_line(line: str, t: Trace):
    parts = line.split()
    if not parts:
        return
    kind = parts[0]
    if kind == "SIG" and len(parts) >= 3:
        signals = {}
        for item in parts[2:]:
            name, _, value = item.partition("=")
            signals[name] = value == "ON"
        t.changes.append(Change(int(parts[1]), signals))
    elif kind == "STEP" and len(parts) == 5:
        t.steps.append(StepRun(int(parts[1]), parts[2], int(parts[3].removeprefix("x")), int(parts[4])))
    elif kind == "DROP" and len(parts) == 2:
        t.dropped += int(parts[1])
    elif kind == "MARK" and len(parts) == 2:
        t.marks.append(int(parts[1]))
    else:
        t.other.append(line)


def parse(lines: list[str]) -> Trace:
    t = Trace()
    for line in lines:
        parse_line(line.strip(), t)
    return t


def summary(t: Trace) -> str:
    """A readable account of a trace, for run folders and references."""
    out = [f"{len(t.changes)} signal changes, {t.step_count} steps in {len(t.steps)} runs "
           f"(net {t.net_steps:+d}), {t.dropped} dropped"]
    start = min([c.t_us for c in t.changes] + [s.t_us for s in t.steps], default=0)
    events = [(c.t_us, "SIG", c) for c in t.changes] + [(s.t_us, "STEP", s) for s in t.steps]
    for when, kind, e in sorted(events, key=lambda x: x[0]):
        ms = (when - start) / 1000
        if kind == "SIG":
            text = " ".join(f"{n}={'ON' if on else 'OFF'}" for n, on in e.signals.items())
        else:
            interval = f", every {e.interval_us / 1000:.1f} ms" if e.interval_us else ""
            text = f"{e.direction} x{e.count}{interval}"
        out.append(f"  {ms:10.1f} ms  {kind:4}  {text}")
    return "\n".join(out)
