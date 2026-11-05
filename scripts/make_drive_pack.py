#!/usr/bin/env python3
"""Cuts a sample pack from a session recorded by scripts/record_drive.py:
its recording.wav and session.jsonl (when each action ran).

    scripts/make_drive_pack.py host/runs/<time>-drive                 # writes samples/Drive/
    scripts/make_drive_pack.py RUN --name Mitsumi --audition out.wav
    scripts/make_drive_pack.py RUN --recording phone.m4a              # recorded some other way

How it finds each sound:
  - Clicks are found in the recording's high frequencies (a second
    difference), in 1 ms blocks, against the room's noise: an onset is a
    block over the threshold after 10 ms under it.
  - The recording and the log start at different moments (the mic opens
    late, or was started by hand). The offset is the one that puts the most
    clicks inside the logged actions and the fewest inside the pauses.
  - step: the median-loudness single step (step_in and step_out, at several
    cylinders). seek: a loop of steps from the longest seek, at the host's
    measured step rate. bump: a step past the outer stop, if the firmware
    did one. spinup, spin (whole revolutions, looped), spindown: from the
    long reads. insert, eject: the median-loudness one of each prompt.
  - Every seek is also saved whole under the run's extras/ folder, with the
    measured step rate, for seek sounds that follow the rate (not in the
    pack yet).

One gain for the pack, as make_recorded_pack.py: a step 6 dB under full
scale and nothing but the bump over the peak; the bump alone to the peak.
The pack must fit the firmware's 160 KB sample pool: the spin-down is
shortened first if it doesn't. Needs ffmpeg (to decode the recording).
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from bisect import bisect_left
from operator import mul, sub
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "host"))
sys.path.insert(0, str(REPO / "scripts"))

from fddproto import drive_session  # noqa: E402
from fddproto.drive_session import Logged  # noqa: E402
from make_recorded_pack import PEAK, POOL_BYTES, RATE, decode, fade, write_wav  # noqa: E402

BLOCK = RATE // 1000  # samples: about 1 ms
BLOCK_S = BLOCK / RATE  # exactly (0.998 ms: over a session, the difference adds up to seconds)
QUIET_BLOCKS = 10  # an onset follows this long under the threshold
DEFAULT_STEP_S = 0.026  # the Feather's: 13 ms STEP pulses, 13 ms apart
LENGTHS = {"spinup": 0.75, "spin_revs": 3, "spindown": 1.4, "insert": 0.45, "eject": 0.45}


@dataclass
class Click:
    t: float  # seconds into the recording
    level: float  # the loudest block in its first 20 ms


class Analysis:
    """The recording's clicks and loudness, block by block."""

    def __init__(self, x: list[float]):
        self.x = x
        n = len(x) // BLOCK
        self.hp: list[float] = []  # the loudest second difference in each block: clicks
        self.rms: list[float] = []  # each block's loudness: the motor
        # A thousand blocks at a time, with map() doing the arithmetic: a
        # 15-minute recording is 20 million samples.
        chunk = 1000 * BLOCK
        for c0 in range(0, n * BLOCK, chunk):
            c1 = min(c0 + chunk, n * BLOCK)
            seg = ([0.0, 0.0] + x[0:c1]) if c0 == 0 else x[c0 - 2:c1]  # two before, for the difference
            d1 = list(map(sub, seg[1:], seg[:-1]))
            d2 = list(map(abs, map(sub, d1[1:], d1[:-1])))  # d2[i] is at sample c0 + i
            sq = list(map(mul, seg[2:], seg[2:]))
            for i in range(0, c1 - c0, BLOCK):
                self.hp.append(max(d2[i:i + BLOCK]))
                self.rms.append(math.sqrt(sum(sq[i:i + BLOCK]) / BLOCK))
        # The room: most of a session is pauses, so the quiet fifth of it.
        self.floor = sorted(self.hp)[n // 5] or 1e-6
        self.rms_floor = sorted(self.rms)[n // 5] or 1e-6
        self.threshold = 8 * self.floor
        self.clicks = self._clicks()
        self.times = [c.t for c in self.clicks]

    def _clicks(self) -> list[Click]:
        out, quiet = [], QUIET_BLOCKS
        for b, v in enumerate(self.hp):
            if v > self.threshold:
                if quiet >= QUIET_BLOCKS:
                    out.append(Click(b * BLOCK_S, max(self.hp[b:b + 20])))
                quiet = 0
            else:
                quiet += 1
        return out

    def clicks_in(self, t0: float, t1: float) -> list[Click]:
        return self.clicks[bisect_left(self.times, t0):bisect_left(self.times, t1)]

    def cut(self, t0: float, t1: float) -> list[float]:
        return self.x[max(0, int(t0 * RATE)):max(0, int(t1 * RATE))]


def sounding(e: Logged) -> bool:
    return e.kind in ("seek", "spin", "read", "write") and e.label != "position"


def find_offset(a: Analysis, log: list[Logged]) -> float:
    """Recording time minus log time: the offset that puts the most clicks
    in the actions that make them, and the fewest in the gaps."""
    acts = [e for e in log if sounding(e) or e.label == "position"]
    gaps = [e for e in log if e.kind == "pause" and e.label in ("gap", "silence")]
    candidates = {round(c.t - e.t0, 3) for e in acts[:8] for c in a.clicks[:80] if 0 < c.t - e.t0 + 5 < 600}
    candidates |= {round(c.t - e.t0 - d, 3) for e in acts[:8] for c in a.clicks[:80] for d in (0.1, 0.2, 0.3)}

    def score(off: float) -> int:
        hit = sum(1 for e in acts if a.clicks_in(e.t0 + off, e.t1 + off + 0.15))
        noise = sum(len(a.clicks_in(e.t0 + off + 0.3, e.t1 + off - 0.1)) for e in gaps if e.t1 - e.t0 > 0.5)
        return 2 * hit - noise

    if not candidates:
        return 0.0
    # Offsets within a few tens of ms of each other score the same (the
    # windows have slack): the middle of the best ones.
    scored = {off: score(off) for off in candidates}
    best = max(scored.values())
    top = sorted(off for off, s in scored.items() if s == best)
    return top[len(top) // 2]


def median_by_level(clicks: list[Click]) -> Click:
    ranked = sorted(clicks, key=lambda c: c.level)
    return ranked[len(ranked) // 2]


def loop(x: list[float], start: int, length: int, crossfade: int) -> list[float]:
    body = x[start:start + length]
    tail = x[start + length:start + length + crossfade]
    for i in range(min(crossfade, len(tail))):
        w = i / crossfade
        body[i] = body[i] * w + tail[i] * (1 - w)
    return body


def rotation_s(a: Analysis, t0: float, t1: float) -> float:
    """The disk's revolution, from the loudness's repeat (190-215 ms at 300 RPM)."""
    b0, b1 = int(t0 / BLOCK_S), int(t1 / BLOCK_S)
    raw = a.rms[b0:b1]
    if len(raw) < 600:
        return 0.2
    env = [sum(raw[i:i + 10]) / 10 for i in range(len(raw) - 10)]  # 10 ms smoothing: the hiss is noisy
    mean = sum(env) / len(env)
    env = [v - mean for v in env]

    def corr(lag: int) -> float:  # per term: a longer lag has fewer
        return sum(env[i] * env[i + lag] for i in range(len(env) - lag)) / (len(env) - lag)

    return max(range(int(0.190 / BLOCK_S), int(0.216 / BLOCK_S)), key=corr) * BLOCK_S


@dataclass
class Cut:
    sounds: dict[str, list[float]]
    extras: dict[str, list[float]]
    notes: dict


def make_sounds(a: Analysis, log: list[Logged], offset: float) -> Cut:
    notes: dict = {"offset_s": offset, "threshold": a.threshold, "clicks": len(a.clicks)}
    sounds: dict[str, list[float]] = {}
    extras: dict[str, list[float]] = {}

    def window(e: Logged, pad: float = 0.15) -> list[Click]:
        return a.clicks_in(e.t0 + offset, e.t1 + offset + pad)

    # A single step: the first click of each step action.
    steps = [w[0] for e in log if e.label in ("step_in", "step_out") for w in [window(e)] if w]
    notes["steps_heard"] = f"{len(steps)} of {sum(1 for e in log if e.label in ('step_in', 'step_out'))}"
    if not steps:
        raise SystemExit("no single steps found: check the recording's level, or the offset")
    step = median_by_level(steps)
    sounds["step"] = fade(a.cut(step.t - 0.003, step.t + 0.080), fade_in=0.002, fade_out=0.025)

    # Seeks: each whole, and the host's step rate from their clicks.
    periods = []
    for e in log:
        if not e.label.startswith("seek_"):
            continue
        w = window(e)
        if not w:
            continue
        periods += [b.t - c.t for c, b in zip(w, w[1:]) if 0.008 < b.t - c.t < 0.06]
        name = f"{e.label}_{sum(1 for k in extras if k.startswith(e.label))}"
        extras[name] = fade(a.cut(w[0].t - 0.003, w[-1].t + 0.08), fade_in=0.002, fade_out=0.03)
    step_s = statistics.median(periods) if periods else DEFAULT_STEP_S
    notes["seek_step_ms"] = round(step_s * 1000, 2)
    longest = max((e for e in log if e.label.startswith("seek_in_")), key=lambda e: int(e.label.rsplit("_", 1)[1]),
                  default=None)
    w = window(longest) if longest else []
    if len(w) >= 12:
        mid = w[len(w) // 2]
        n = int(round(10 * step_s * RATE))
        sounds["seek"] = loop(a.x, int((mid.t - 0.002) * RATE), n, int(0.003 * RATE))
    else:
        notes["seek"] = "not enough clicks in the longest seek to loop"

    bumps = [w[0] for e in log if e.label == "bump" for w in [window(e)] if w]
    notes["bumps_heard"] = len(bumps)
    if bumps:
        b = median_by_level(bumps)
        sounds["bump"] = fade(a.cut(b.t - 0.003, b.t + 0.100), fade_in=0.002, fade_out=0.03)

    # The motor: the first long read.
    spin = next((e for e in log if e.label == "spin" and e.ok), None)
    if spin:
        t0, t1 = spin.t0 + offset, spin.t1 + offset
        b0 = int(t0 / BLOCK_S)
        loud = 3 * a.rms_floor
        start = next((b for b in range(b0, int(t1 / BLOCK_S)) if a.rms[b] > loud), b0) * BLOCK_S
        sounds["spinup"] = fade(a.cut(start - 0.02, start + LENGTHS["spinup"]), fade_in=0.01)
        rev = rotation_s(a, start + 1.5, t1 - 0.3)
        notes["rotation_ms"] = round(rev * 1000, 1)
        mid = start + 1.5 + rev
        quiet = min(range(int(mid / BLOCK_S), int((mid + rev) / BLOCK_S)), key=lambda b: a.rms[b]) * BLOCK_S
        sounds["spin"] = loop(a.x, int(quiet * RATE), int(round(LENGTHS["spin_revs"] * rev * RATE)), int(0.02 * RATE))
        sounds["spindown"] = fade(a.cut(t1 - 0.05, t1 - 0.05 + LENGTHS["spindown"]), fade_out=0.4)
    else:
        notes["spin"] = "no spin recorded"

    for kind in ("insert", "eject"):
        found = []
        for e in log:
            if e.label != kind:
                continue
            w = a.clicks_in(e.t0 + offset, e.t1 + offset)
            if w:
                found.append(max(w, key=lambda c: c.level))
        notes[f"{kind}s_heard"] = len(found)
        if found:
            c = median_by_level(found)
            sounds[kind] = fade(a.cut(c.t - 0.05, c.t + LENGTHS[kind]), fade_in=0.01, fade_out=0.1)
    return Cut(sounds, extras, notes)


def balance(sounds: dict[str, list[float]]) -> dict[str, list[float]]:
    """make_recorded_pack.py's rule: one gain putting a step 6 dB under the
    peak and nothing but the bump over it; the bump alone to the peak."""
    peak = {k: max(abs(v) for v in s) or 1e-9 for k, s in sounds.items()}
    others = [p for k, p in peak.items() if k != "bump"]
    gain = min(PEAK / 2 / peak["step"], PEAK / max(others))
    return {k: [v * (PEAK / peak[k] if k == "bump" else gain) for v in s] for k, s in sounds.items()}


def fit(sounds: dict[str, list[float]]) -> dict[str, list[float]]:
    """Into the sample pool: the spin-down shortened (down to 0.5 s) if not."""
    total = sum(len(s) for s in sounds.values()) * 2
    if total > POOL_BYTES and "spindown" in sounds:
        over = (total - POOL_BYTES) // 2 + 1
        keep = max(len(sounds["spindown"]) - over, int(0.5 * RATE))
        sounds["spindown"] = fade(sounds["spindown"][:keep], fade_out=0.3)
        total = sum(len(s) for s in sounds.values()) * 2
    if total > POOL_BYTES:
        raise SystemExit(f"the pack is {total} bytes, over the {POOL_BYTES}-byte sample pool")
    return sounds


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("run", type=Path, help="the session folder from record_drive.py")
    p.add_argument("--recording", type=Path, help="the recording, if not the run's recording.wav")
    p.add_argument("--name", default="Drive", help="the pack's name: samples/<name>/ (default Drive)")
    p.add_argument("--offset", type=float, help="recording time minus log time, if the search gets it wrong")
    p.add_argument("--audition", type=Path, help="also write each sound in turn to this WAV")
    args = p.parse_args()

    log = drive_session.load(args.run / "session.jsonl")
    print("decoding and analysing...")
    a = Analysis(decode(args.recording or args.run / "recording.wav"))
    offset = args.offset if args.offset is not None else find_offset(a, log)
    cut = make_sounds(a, log, offset)
    sounds = fit(balance(cut.sounds))

    out = REPO / "samples" / args.name
    out.mkdir(parents=True, exist_ok=True)
    for name, x in sounds.items():
        write_wav(out / f"{name}.wav", x)
    extras = args.run / "extras"
    extras.mkdir(exist_ok=True)
    for name, x in cut.extras.items():
        write_wav(extras / f"{name}.wav", x)
    (args.run / "pack_notes.json").write_text(json.dumps(cut.notes, indent=2) + "\n")

    for k, v in cut.notes.items():
        print(f"  {k}: {v}")
    for name, x in sounds.items():
        print(f"{name:<9} {len(x) / RATE:5.2f} s  peak {20 * math.log10(max(abs(v) for v in x) or 1e-9):6.1f} dBFS")
    missing = sorted({"step", "seek", "bump", "spinup", "spin", "spindown", "insert", "eject"} - sounds.keys())
    if missing:
        print(f"missing: {', '.join(missing)} (copy them from another pack, or record again)")
    print(f"total {sum(len(x) for x in sounds.values()) * 2 // 1024} KB of {POOL_BYTES // 1024} KB, in {out}; "
          f"{len(cut.extras)} whole seeks in {extras}")
    if args.audition:
        gap = [0.0] * int(0.6 * RATE)
        order = [("spinup", 1), ("spin", 4), ("step", 3), ("seek", 5), ("bump", 1), ("spindown", 1), ("eject", 1),
                 ("insert", 1)]
        audition: list[float] = []
        for name, times in order:
            if name in sounds:
                audition += (sounds[name] + ([] if name in ("spin", "seek") else gap)) * times + gap
        write_wav(args.audition, audition)
        print(f"wrote {args.audition}")


if __name__ == "__main__":
    main()
