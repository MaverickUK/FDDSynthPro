#!/usr/bin/env python3
"""Synthesises a stand-in FDD sample pack, until a real drive is recorded.

Each sound is a rough model of the mechanism, not a recording, so it won't
sound like any particular drive. The pack follows docs/activity.md and the
HDD Synth packs' format (mono, 16-bit), at 22.05 kHz. A fixed seed makes
every run produce the same files.

    scripts/make_placeholder_samples.py                    # writes samples/Placeholder/
    scripts/make_placeholder_samples.py --demo demo.wav    # ...and a demo sequence
    afplay demo.wav                                        # listen (macOS)
"""
from __future__ import annotations

import argparse
import math
import random
import struct
import wave
from pathlib import Path

RATE = 22050
ROTATION_HZ = 5.0  # 300 RPM
PACK = Path(__file__).resolve().parent.parent / "samples" / "Placeholder"

rng = random.Random(1987)


# ---- Building blocks ---------------------------------------------------------------

def silence(seconds: float) -> list[float]:
    return [0.0] * int(seconds * RATE)


def noise(n: int) -> list[float]:
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def lowpass(x: list[float], cutoff_hz: float, circular: bool = False) -> list[float]:
    """One-pole low-pass. Circular runs it round the buffer twice, so the
    result loops without a seam."""
    a = 1.0 - math.exp(-2 * math.pi * cutoff_hz / RATE)
    y, out = 0.0, [0.0] * len(x)
    for _ in range(2 if circular else 1):
        for i, v in enumerate(x):
            y += a * (v - y)
            out[i] = y
    return out


def highpass(x: list[float], cutoff_hz: float, circular: bool = False) -> list[float]:
    low = lowpass(x, cutoff_hz, circular)
    return [v - l for v, l in zip(x, low)]


def ring(freq_hz: float, decay_s: float, seconds: float, phase: float = 0.0) -> list[float]:
    """A damped sine: something metal struck once."""
    n = int(seconds * RATE)
    return [math.sin(2 * math.pi * freq_hz * i / RATE + phase) * math.exp(-i / (decay_s * RATE)) for i in range(n)]


def mix(*parts: tuple[float, list[float]]) -> list[float]:
    n = max(len(p) for _, p in parts)
    out = [0.0] * n
    for gain, p in parts:
        for i, v in enumerate(p):
            out[i] += gain * v
    return out


def envelope(x: list[float], attack_s: float, decay_s: float) -> list[float]:
    a, n = max(1, int(attack_s * RATE)), len(x)
    return [v * min(1.0, i / a) * math.exp(-i / (decay_s * RATE)) for i, v in enumerate(x)]


def place(dest: list[float], src: list[float], at: int, gain: float = 1.0):
    for i, v in enumerate(src):
        if 0 <= at + i < len(dest):
            dest[at + i] += gain * v


def normalise(x: list[float], peak: float) -> list[float]:
    m = max(abs(v) for v in x) or 1.0
    return [v * peak / m for v in x]


# ---- The sounds ------------------------------------------------------------------------

def motor(seconds: float, speed: list[float] | None = None) -> list[float]:
    """The spinning disk: a hum whose pitch follows the motor speed, and a
    rubbing hiss that rises and falls once per revolution. `speed` (0..1 per
    sample) is for spin-up and spin-down; None is full speed."""
    n = int(seconds * RATE)
    speed = speed or [1.0] * n
    hiss = highpass(lowpass(noise(n), 2500, circular=True), 300, circular=True)
    out, phase_hum, phase_rot = [0.0] * n, 0.0, 0.0
    for i in range(n):
        s = speed[i]
        phase_hum += 2 * math.pi * 120.0 * s / RATE      # motor hum, rising with speed
        phase_rot += 2 * math.pi * ROTATION_HZ * s / RATE
        flutter = 0.6 + 0.4 * (0.5 + 0.5 * math.sin(phase_rot))  # once per revolution
        hum = math.sin(phase_hum) + 0.35 * math.sin(2 * phase_hum) + 0.15 * math.sin(3 * phase_hum)
        out[i] = s * (0.35 * hum + 0.9 * flutter * hiss[i])
    return out


def spin() -> list[float]:
    # 1 s: a whole number of revolutions (5) and hum cycles (120), so it loops.
    return motor(1.0)


def spinup() -> list[float]:
    seconds = 0.6
    n = int(seconds * RATE)
    speed = [min(1.0, (i / n) ** 0.6) for i in range(n)]
    clamp = mix((1.0, ring(90, 0.03, 0.12)), (0.5, envelope(lowpass(noise(int(0.05 * RATE)), 900), 0.001, 0.01)))
    out = motor(seconds, speed)
    place(out, clamp, 0, 1.6)  # the hub clamping the disk
    return out


def spindown() -> list[float]:
    seconds = 0.5
    n = int(seconds * RATE)
    speed = [max(0.0, 1.0 - i / n) ** 1.5 for i in range(n)]
    return motor(seconds, speed)


def tick(weight: float = 1.0, seconds: float = 0.04) -> list[float]:
    """A head step: a short metallic tick over a small thump. Heavier means
    lower and longer, for the stop."""
    click = envelope(highpass(noise(int(0.004 * RATE)), 2000), 0.0002, 0.0015)
    metal = ring(1700 / weight, 0.006 * weight, seconds, rng.uniform(0, 6.28))
    thump = ring(160 / weight, 0.012 * weight, seconds)
    return mix((0.8, click), (0.6, metal), (0.7 * weight, thump))


def step() -> list[float]:
    return tick(1.0, 0.05)


def seek() -> list[float]:
    # Steps every 6 ms, a whole number of them, so it loops: 20 x 6 ms.
    interval = int(0.006 * RATE)
    out = [0.0] * (interval * 20)
    for k in range(20):
        t = tick(0.9, 0.006)
        # Wrap the tail of each tick round, so the loop has no seam.
        for i, v in enumerate(t):
            out[(k * interval + i) % len(out)] += v * rng.uniform(0.8, 1.0)
    return out


def bump() -> list[float]:
    return tick(2.2, 0.12)


def insert() -> list[float]:
    slide = envelope(lowpass(highpass(noise(int(0.25 * RATE)), 400), 3000), 0.08, 0.09)
    out = slide + silence(0.15)
    place(out, tick(1.6, 0.12), int(0.22 * RATE), 1.2)   # the shutter
    place(out, ring(220, 0.04, 0.12), int(0.27 * RATE), 0.9)  # the disk seating
    return out


def eject() -> list[float]:
    out = silence(0.35)
    place(out, tick(1.8, 0.1), 0, 1.2)                     # the button releasing the latch
    spring = envelope(lowpass(highpass(noise(int(0.2 * RATE)), 600), 3500), 0.005, 0.05)
    place(out, spring, int(0.02 * RATE), 0.9)              # the disk sliding out
    place(out, ring(300, 0.02, 0.08), int(0.12 * RATE), 0.6)
    return out


SOUNDS = {
    "spinup.wav": (spinup, 0.5),
    "spin.wav": (spin, 0.35),
    "spindown.wav": (spindown, 0.35),
    "step.wav": (step, 0.8),
    "seek.wav": (seek, 0.7),
    "bump.wav": (bump, 0.9),
    "insert.wav": (insert, 0.7),
    "eject.wav": (eject, 0.7),
}
LOOPS = ("spin.wav", "seek.wav")


# ---- Files ---------------------------------------------------------------------------

def write_wav(path: Path, x: list[float]):
    frames = b"".join(struct.pack("<h", max(-32767, min(32767, int(v * 32767)))) for v in x)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)


def make_pack(folder: Path = PACK) -> dict[str, list[float]]:
    folder.mkdir(parents=True, exist_ok=True)
    sounds = {}
    for name, (make, peak) in SOUNDS.items():
        sounds[name] = normalise(make(), peak)
        write_wav(folder / name, sounds[name])
    return sounds


def demo(sounds: dict[str, list[float]]) -> list[float]:
    """What a short session might sound like: insert, spin up, a few steps, a
    seek, a grind at track 0, spin down, eject."""
    spin_loop = sounds["spin.wav"]
    out = sounds["insert.wav"] + silence(0.4) + sounds["spinup.wav"] + spin_loop * 4 + sounds["spindown.wav"]
    out += silence(0.3) + sounds["eject.wav"]
    t = len(sounds["insert.wav"]) + int(0.4 * RATE) + len(sounds["spinup.wav"])
    for k in range(3):  # single steps
        place(out, sounds["step.wav"], t + int((0.3 + 0.25 * k) * RATE))
    seek_at = t + int(1.3 * RATE)
    for k in range(6):  # a seek: the loop played six times
        place(out, sounds["seek.wav"], seek_at + k * len(sounds["seek.wav"]))
    grind_at = t + int(2.6 * RATE)
    for k in range(6):  # bumps 6 ms apart: the recalibrate grind
        place(out, sounds["bump.wav"], grind_at + k * int(0.006 * RATE), 0.6)
    return normalise(out, 0.9)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=PACK, help="pack folder (default: samples/Placeholder)")
    parser.add_argument("--demo", type=Path, help="also write a demo sequence to this WAV")
    args = parser.parse_args()
    pack = make_pack(args.out)
    for name, x in pack.items():
        print(f"{args.out / name}  {len(x) / RATE:.3f} s{'  (loop)' if name in LOOPS else ''}")
    if args.demo:
        write_wav(args.demo, demo(pack))
        print(args.demo)
