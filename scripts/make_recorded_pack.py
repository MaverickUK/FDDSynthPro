#!/usr/bin/env python3
"""Cuts a sample pack from a recording of a real 3.5-inch drive:
media/FDDSynth_ExampleBehaviours.m4a, the drive driven through a series of
operations by the FDD Synth's own software.

The recording has no insert or eject (both optional in a pack). Where each
sound comes from, found by measuring the recording (its level envelope,
click onsets and their spacing, and the spectra):

  spinup    2.13-2.95 s  the motor starting: two soft clicks, then the disk
                         turning. The knock at 2.363 s (the bump below) is
                         cut out and the two sides joined.
  spin      44.0 s on    4 revolutions of the idle disk (one soft thump every
                         199.8 ms: 300 RPM), joined end to start.
  spindown  47.85-49.4 s the motor off: the thumps slow and fade.
  step      28.401 s     one step of a slow sweep across the disk (79 steps,
                         172 ms apart): the most typical of the 79.
  seek      5.55 s on    a seek: steps 10 ms apart, 20 of them, looped.
  bump      2.363 s      the loudest click in the recording, matching the
                         knocks of the recalibrate at 4.8-5.5 s (a seek about
                         12 dB louder than the others): a step against the
                         end stop.

The whole pack gets one gain, so the sounds keep the drive's own balance:
the clicks loud over a quiet motor. The format is the firmware's: mono,
16-bit, 22.05 kHz, within the 160 KB sample pool (cfg::SAMPLE_POOL_MAX_BYTES).

    scripts/make_recorded_pack.py                       # writes samples/Recorded/
    scripts/make_recorded_pack.py --audition out.wav    # ...and each sound in turn, to listen to

Needs ffmpeg (to decode the AAC).
"""
from __future__ import annotations

import argparse
import array
import math
import subprocess
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RECORDING = REPO / "media/FDDSynth_ExampleBehaviours.m4a"
PACK = REPO / "samples/Recorded"
RATE = 22050
ROTATION_S = 0.1998  # measured over the idle stretches
SEEK_STEP_S = 0.010  # measured in the seek at 5.5 s
PEAK = 10 ** (-1 / 20)  # the pack's loudest sample at -1 dBFS
POOL_BYTES = 160 * 1024


def decode(path: Path) -> list[float]:
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
                         check=True, capture_output=True).stdout
    return [s / 32768 for s in array.array("h", raw)]


class Recording:
    def __init__(self, samples: list[float]):
        self.x = samples

    def at(self, t: float) -> int:
        return int(round(t * RATE))

    def cut(self, t0: float, t1: float) -> list[float]:
        return self.x[self.at(t0):self.at(t1)]

    def onset(self, near: float) -> float:
        """The first sample of a click near `near`: where the high-passed
        signal first jumps well above the 20 ms before it."""
        x = self.x
        hp = lambda i: x[i] - 2 * x[i - 1] + x[i - 2]
        i0 = self.at(near - 0.03)
        base = math.sqrt(sum(hp(i) ** 2 for i in range(i0, i0 + self.at(0.02))) / self.at(0.02))
        for i in range(i0 + self.at(0.02), i0 + self.at(0.08)):
            if abs(hp(i)) > 6 * base:
                return i / RATE
        raise ValueError(f"no click near {near} s")

    def sharpest(self, t0: float, t1: float) -> float:
        """The sharpest edge between t0 and t1: a click in a run of them,
        where onset() has no quiet stretch to compare with."""
        x = self.x
        i = max(range(self.at(t0), self.at(t1)), key=lambda i: abs(x[i] - 2 * x[i - 1] + x[i - 2]))
        return i / RATE

    def quietest(self, t0: float, t1: float, window: float = 0.005) -> float:
        """The quietest moment between t0 and t1: a place to start a loop."""
        n = self.at(window)
        best, where = None, t0
        for i in range(self.at(t0), self.at(t1) - n, n // 2):
            e = sum(v * v for v in self.x[i:i + n])
            if best is None or e < best:
                best, where = e, i / RATE
        return where

    def loop(self, start: float, length: int, crossfade: int) -> list[float]:
        """`length` samples from `start` that repeat seamlessly: the samples
        just after the end fade into the start."""
        s = self.at(start)
        body = self.x[s:s + length]
        tail = self.x[s + length:s + length + crossfade]
        for i in range(crossfade):
            w = i / crossfade
            body[i] = body[i] * w + tail[i] * (1 - w)
        return body


def fade(x: list[float], fade_in: float = 0.0, fade_out: float = 0.0) -> list[float]:
    x = list(x)
    n_in, n_out = int(fade_in * RATE), int(fade_out * RATE)
    for i in range(min(n_in, len(x))):
        x[i] *= i / n_in
    for i in range(min(n_out, len(x))):
        x[len(x) - 1 - i] *= i / n_out
    return x


def join(a: list[float], b: list[float], crossfade: float) -> list[float]:
    n = int(crossfade * RATE)
    mid = [a[len(a) - n + i] * (1 - i / n) + b[i] * (i / n) for i in range(n)]
    return a[:len(a) - n] + mid + b[n:]


def make_sounds(rec: Recording) -> dict[str, list[float]]:
    bump_at = rec.onset(2.36)
    step_at = rec.onset(28.401)
    seek_at = rec.sharpest(5.55, 5.55 + SEEK_STEP_S)
    spin_at = rec.quietest(44.0, 44.0 + ROTATION_S)
    sounds = {
        "spinup": fade(join(rec.cut(2.13, bump_at - 0.004), rec.cut(bump_at + 0.105, 2.95), 0.015), fade_in=0.01),
        "spin": rec.loop(spin_at, int(round(4 * ROTATION_S * RATE)), int(0.02 * RATE)),
        "spindown": fade(rec.cut(47.85, 49.40), fade_out=0.4),
        "step": fade(rec.cut(step_at - 0.003, step_at + 0.080), fade_in=0.002, fade_out=0.025),
        "seek": rec.loop(seek_at - 0.002, int(round(20 * SEEK_STEP_S * RATE)), int(0.003 * RATE)),
        "bump": fade(rec.cut(bump_at - 0.003, bump_at + 0.100), fade_in=0.002, fade_out=0.03),
    }
    gain = PEAK / max(abs(v) for s in sounds.values() for v in s)
    return {name: [v * gain for v in s] for name, s in sounds.items()}


def write_wav(path: Path, x: list[float]):
    pcm = array.array("h", (max(-32768, min(32767, int(round(v * 32767)))) for v in x))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def make_pack(out: Path = PACK, recording: Path = RECORDING) -> dict[str, list[float]]:
    sounds = make_sounds(Recording(decode(recording)))
    total = sum(len(s) for s in sounds.values()) * 2
    if total > POOL_BYTES:
        raise SystemExit(f"the pack is {total} bytes, over the {POOL_BYTES}-byte sample pool")
    out.mkdir(parents=True, exist_ok=True)
    for name, x in sounds.items():
        write_wav(out / f"{name}.wav", x)
    return sounds


def audition(sounds: dict[str, list[float]]) -> list[float]:
    """Each sound in turn, a gap between: the loops played several times
    over, so a bad join would be heard."""
    gap = [0.0] * int(0.6 * RATE)
    out: list[float] = []
    for name, times in (("spinup", 1), ("spin", 4), ("step", 1), ("step", 1), ("step", 1), ("seek", 5),
                        ("bump", 1), ("spindown", 1)):
        out += sounds[name] * times + gap
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--audition", type=Path, help="also write each sound in turn to this WAV")
    args = p.parse_args()
    sounds = make_pack()
    for name, x in sounds.items():
        print(f"{name:<9} {len(x) / RATE:5.2f} s  peak {20 * math.log10(max(abs(v) for v in x)):6.1f} dBFS")
    print(f"total {sum(len(x) for x in sounds.values()) * 2 // 1024} KB of {POOL_BYTES // 1024} KB, in {PACK}")
    if args.audition:
        write_wav(args.audition, audition(sounds))
        print(f"wrote {args.audition}")


if __name__ == "__main__":
    main()
