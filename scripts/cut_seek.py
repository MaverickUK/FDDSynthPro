#!/usr/bin/env python3
"""Cuts a pack's seek.wav from a recording of a real drive in a PC: its
fast seeks (a PC steps every 3-6 ms, the Feather every 26), looped whole
steps at the PC's own step rate.

    scripts/cut_seek.py memo.m4a --seeks 145.5:153.5 --steps 126.5:134.5 \\
        --step-ms 5.052 --pack samples/RealDrive --audition /tmp/seek.wav

--seeks is the stretch of the recording holding the long seeks (FDDBench's
Full stroke: ten of 79 steps), --steps one holding single steps (its Single
steps), both in seconds into the recording. --step-ms is the host's step
period: from the drive's own log of the same PC (a STEP line's span over
its steps less one), or from the recording.

How it cuts:
  - The seeks are the runs of loud blocks in --seeks lasting 150 ms or more.
  - From the middle of each, 40 whole steps, starting on a step (the
    sharpest edge within one step period); the median-loudness one is kept,
    looped with a 3 ms crossfade, as make_drive_pack.py's loops.
  - Its level: the same gain that puts the recording's median single step at
    the pack's step.wav peak, so the seek sits against the pack's step as it
    did against the drive's in the room; no more than the pack's peak.
  - The pack must still fit the firmware's sample pool: as make_drive_pack.py,
    the spin-down is shortened if it doesn't.
"""
from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

from make_drive_pack import BLOCK_S, Analysis, fit, loop  # noqa: E402
from make_recorded_pack import PEAK, RATE, decode, fade, write_wav  # noqa: E402

LOOP_STEPS = 40
MIN_SEEK_S = 0.15
CROSSFADE_S = 0.003


def peak(x: list[float]) -> float:
    return max((abs(v) for v in x), default=0.0)


def rms(x: list[float]) -> float:
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def seek_runs(a: Analysis, t0: float, t1: float) -> list[tuple[float, float]]:
    """The runs of blocks over 6x the room's loudness, MIN_SEEK_S or longer."""
    threshold = 6 * a.rms_floor
    runs, on = [], None
    for b in range(int(t0 / BLOCK_S), min(len(a.rms), int(t1 / BLOCK_S))):
        t = b * BLOCK_S
        if a.rms[b] > threshold and on is None:
            on = t
        elif a.rms[b] <= threshold and on is not None:
            if t - on >= MIN_SEEK_S:
                runs.append((on, t))
            on = None
    return runs


@dataclass
class Seek:
    sound: list[float]
    source: tuple[float, float]  # the seek it came from, in the recording
    start: int  # the loop's first sample in the recording
    seek_over_step_db: float  # the loop's peak against a single step's, in the recording
    single_steps: int
    seeks: int
    gain: float  # applied to the recording


def cut(x: list[float], seeks: tuple[float, float], steps: tuple[float, float], step_s: float,
        step_peak: float) -> Seek:
    a = Analysis(x)
    singles = a.clicks_in(*steps)
    if not singles:
        raise SystemExit(f"no single steps heard in {steps[0]}-{steps[1]} s")
    peaks = sorted(peak(a.cut(c.t - 0.003, c.t + 0.080)) for c in singles)
    single = peaks[len(peaks) // 2]

    runs = seek_runs(a, *seeks)
    n = int(round(LOOP_STEPS * step_s * RATE))
    period = int(step_s * RATE)
    candidates = []
    for s, e in runs:
        if e - s < n / RATE:
            continue
        i0 = int(((s + e) / 2 - n / RATE / 2) * RATE)
        edge = max(range(i0, i0 + period), key=lambda i: abs(x[i] - 2 * x[i - 1] + x[i - 2]))
        start = edge - int(0.0005 * RATE)  # just before the step's edge
        candidates.append((rms(x[start:start + n]), start, (s, e)))
    if not candidates:
        raise SystemExit(f"no seek of {LOOP_STEPS} steps found in {seeks[0]}-{seeks[1]} s")
    candidates.sort()
    _, start, source = candidates[len(candidates) // 2]
    body = loop(x, start, n, int(CROSSFADE_S * RATE))

    ratio = peak(body) / single
    gain = min(step_peak / single, PEAK / peak(body))
    return Seek([v * gain for v in body], source, start, 20 * math.log10(ratio), len(singles), len(runs), gain)


def audition(step: list[float], seek: list[float], whole: list[float], step_s: float) -> list[float]:
    """Three steps, a 79-step seek as the firmware plays it (the step, then the
    loop), and the recorded seek."""
    gap = [0.0] * int(0.3 * RATE)
    out: list[float] = []
    for _ in range(3):
        out += step + gap
    played = step + [0.0] * max(0, int(78 * step_s * RATE) - len(step))
    for i in range(int(78 * step_s * RATE)):
        played[i] += seek[i % len(seek)]
    out += gap + fade(played, fade_out=0.005) + gap + whole + gap
    return [max(-1.0, min(1.0, v)) for v in out]


def span(text: str) -> tuple[float, float]:
    t0, t1 = text.split(":")
    return float(t0), float(t1)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("recording", type=Path)
    p.add_argument("--seeks", type=span, required=True, help="START:END seconds holding the long seeks")
    p.add_argument("--steps", type=span, required=True, help="START:END seconds holding single steps")
    p.add_argument("--step-ms", type=float, required=True, help="the host's step period")
    p.add_argument("--pack", type=Path, required=True, help="the pack: its step.wav sets the level")
    p.add_argument("--audition", type=Path, help="also write a WAV to listen to")
    args = p.parse_args()

    step = decode(args.pack / "step.wav")
    x = decode(args.recording)
    step_s = args.step_ms / 1000
    s = cut(x, args.seeks, args.steps, step_s, peak(step))
    write_wav(args.pack / "seek.wav", s.sound)
    spindown = args.pack / "spindown.wav"
    if spindown.exists():
        sounds = {f.stem: decode(f) for f in sorted(args.pack.glob("*.wav"))}
        before = len(sounds["spindown"])
        sounds = fit(sounds)
        if len(sounds["spindown"]) != before:
            write_wav(spindown, sounds["spindown"])
            print(f"spindown.wav shortened by {before - len(sounds['spindown'])} samples to fit the sample pool")
    print(f"{s.single_steps} single steps and {s.seeks} seeks heard; looped {LOOP_STEPS} steps of "
          f"{args.step_ms} ms ({len(s.sound)} samples) from the seek at {s.source[0]:.3f}-{s.source[1]:.3f} s")
    print(f"a seek's peak is {s.seek_over_step_db:+.1f} dB against a single step's; "
          f"seek.wav peaks at {20 * math.log10(peak(s.sound)):.1f} dBFS")
    if args.audition:
        whole = fade([v * s.gain for v in x[int((s.source[0] - 0.01) * RATE):int((s.source[1] + 0.1) * RATE)]],
                     fade_in=0.002, fade_out=0.03)
        write_wav(args.audition, audition(step, s.sound, whole, step_s))
        print(f"audition: {args.audition}")


if __name__ == "__main__":
    main()
