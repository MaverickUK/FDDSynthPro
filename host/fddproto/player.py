"""Plays a drive's sound events (ACT lines) with a sample pack, into a WAV:
what the FDD Synth will sound like, before it exists.

    .venv/bin/python -m fddproto.player <trace> <out.wav> [--pack <folder>]

<trace> is anything holding ACT lines: a drive's serial log, a rig run's
trace, or the virtual PC's session (.pio/native-tests/pc_session_act.txt,
written by test/run_native_tests.sh). The pack defaults to
samples/Placeholder.

The rules are the ones the synth's firmware will follow (docs/activity.md):
  MOTOR_ON    spinup.wav, then spin.wav looping until MOTOR_OFF
  MOTOR_OFF   the spin loop stops; spindown.wav
  STEP        step.wav
  SEEK_START  seek.wav looping until SEEK_END (at its last step)
  BUMP        bump.wav
  INSERT      insert.wav;  EJECT  eject.wav
"""
from __future__ import annotations

import argparse
import array
import sys
import wave
from dataclasses import dataclass
from pathlib import Path

from fddproto import trace
from fddproto.paths import REPO

PACK = REPO / "samples" / "Placeholder"
LEAD_IN_S = 0.2   # silence before the first event
TAIL_S = 0.5      # after the last
NAMES = ("spinup", "spin", "spindown", "step", "seek", "bump", "insert", "eject")


@dataclass
class Pack:
    rate: int
    sounds: dict[str, list[float]]  # name -> samples in [-1, 1]


def load_pack(folder: Path) -> Pack:
    sounds, rate = {}, None
    for name in NAMES:
        path = folder / f"{name}.wav"
        if not path.exists():
            continue  # insert/eject are optional
        with wave.open(str(path)) as w:
            if w.getnchannels() != 1 or w.getsampwidth() != 2:
                raise ValueError(f"{path}: must be mono 16-bit")
            if rate is None:
                rate = w.getframerate()
            elif w.getframerate() != rate:
                raise ValueError(f"{path}: every sample in a pack must have the same rate")
            data = array.array("h", w.readframes(w.getnframes()))
            sounds[name] = [v / 32768 for v in data]
    for required in ("spinup", "spin", "spindown", "step", "seek", "bump"):
        if required not in sounds:
            raise ValueError(f"{folder}: no {required}.wav")
    return Pack(rate, sounds)


def render(events: list[trace.Act], pack: Pack) -> list[float]:
    """Mixes the pack's sounds as the events call for them. Returns samples."""
    if not events:
        return []
    t0 = events[0].t_us - int(LEAD_IN_S * 1e6)
    end_us = events[-1].t_us + int(TAIL_S * 1e6)
    rate = pack.rate
    out = [0.0] * ((end_us - t0) * rate // 1_000_000 + 1)

    def at(t_us: int) -> int:
        return (t_us - t0) * rate // 1_000_000

    def place(name: str, start: int, gain: float = 1.0):
        s = pack.sounds.get(name)
        if not s:
            return
        for i, v in enumerate(s, start):
            if i >= len(out):
                break
            out[i] += gain * v

    def loop(name: str, start: int, stop: int, gain: float = 1.0):
        s = pack.sounds[name]
        for i in range(max(0, start), min(stop, len(out))):
            out[i] += gain * s[(i - start) % len(s)]

    motor_on = seek_on = None
    for e in events:
        n = at(e.t_us)
        if e.sound == "MOTOR_ON" and motor_on is None:
            place("spinup", n)
            motor_on = n + len(pack.sounds["spinup"])  # the loop takes over where spin-up ends
        elif e.sound == "MOTOR_OFF" and motor_on is not None:
            loop("spin", motor_on, n)
            place("spindown", n)
            motor_on = None
        elif e.sound == "STEP":
            place("step", n)
        elif e.sound == "SEEK_START" and seek_on is None:
            seek_on = n
        elif e.sound == "SEEK_END" and seek_on is not None:
            loop("seek", seek_on, n + len(pack.sounds["step"]))  # until the last step's click dies away
            seek_on = None
        elif e.sound == "BUMP":
            place("bump", n)
        elif e.sound in ("INSERT", "EJECT"):
            place(e.sound.lower(), n)
    if motor_on is not None:  # still spinning at the end of the trace
        loop("spin", motor_on, len(out))
    if seek_on is not None:
        loop("seek", seek_on, len(out))

    peak = max((abs(v) for v in out), default=0.0)
    if peak > 0.95:
        out = [v * 0.95 / peak for v in out]
    return out


def write_wav(path: Path, samples: list[float], rate: int):
    data = array.array("h", (int(max(-1.0, min(1.0, v)) * 32767) for v in samples))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data.tobytes())


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--pack", type=Path, default=PACK)
    args = parser.parse_args(argv)
    events = trace.parse(args.trace.read_text().splitlines()).activity
    if not events:
        print(f"{args.trace}: no ACT lines", file=sys.stderr)
        return 1
    pack = load_pack(args.pack)
    samples = render(events, pack)
    write_wav(args.out, samples, pack.rate)
    print(f"{args.out}: {len(events)} events, {len(samples) / pack.rate:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
