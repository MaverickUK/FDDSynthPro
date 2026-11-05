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
  SEEK_START  seek.wav, one step of a seek, clicking at the seek's own rate
              (SEEK_START's interval), cut short when the steps come faster
              than it lasts; until the last step's click, from SEEK_END's
              step count
  BUMP        bump.wav
  INSERT      insert.wav;  EJECT  eject.wav

The mix is the plain sum of everything playing, saturated to 16 bits, in
integers: exactly what the firmware's mixer (src/synth.*) does, so the two
can be compared sample for sample (host/tests/test_synth.py).
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
    sounds: dict[str, list[int]]  # name -> 16-bit samples


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
            sounds[name] = list(array.array("h", w.readframes(w.getnframes())))
    for required in ("spinup", "spin", "spindown", "step", "seek", "bump"):
        if required not in sounds:
            raise ValueError(f"{folder}: no {required}.wav")
    return Pack(rate, sounds)


def cdiv(a: int, b: int) -> int:
    """Integer division towards zero, as C++'s."""
    q = abs(a) // b
    return q if a >= 0 else -q


@dataclass
class Seek:
    """A seek's clicks: seek.wav's first `length` samples every `period`
    samples from `start`, the last `fade` of them fading out."""
    start: int
    period: int
    length: int
    fade: int


def seek_clicks(seek: list[int], start: int, interval_us: int, rate: int) -> Seek:
    """How a seek clicks: at its rate, or with no rate given (an old trace)
    at seek.wav's own length. A click longer than the step is cut short,
    fading over its last quarter, at most 1 ms."""
    period = max(1, interval_us * rate // 1_000_000) if interval_us else len(seek)
    length = min(period, len(seek))
    fade = min(length // 4, rate // 1000) if period < len(seek) else 0
    return Seek(start, period, length, fade)


def render(events: list[trace.Act], pack: Pack) -> list[int]:
    """Mixes the pack's sounds as the events call for them. Returns 16-bit samples."""
    if not events:
        return []
    t0 = events[0].t_us - int(LEAD_IN_S * 1e6)
    end_us = events[-1].t_us + int(TAIL_S * 1e6)
    rate = pack.rate
    out = [0] * ((end_us - t0) * rate // 1_000_000 + 1)

    def at(t_us: int) -> int:
        return (t_us - t0) * rate // 1_000_000

    def place(name: str, start: int):
        s = pack.sounds.get(name)
        if not s:
            return
        for i, v in enumerate(s, start):
            if i >= len(out):
                break
            out[i] += v

    def loop(name: str, start: int, stop: int):
        s = pack.sounds[name]
        for i in range(max(0, start), min(stop, len(out))):
            out[i] += s[(i - start) % len(s)]

    def clicks(k: Seek, stop: int):
        s = pack.sounds["seek"]
        for i in range(max(0, k.start), min(stop, len(out))):
            phase = (i - k.start) % k.period
            if phase < k.length:
                v = s[phase]
                if k.fade and phase >= k.length - k.fade:
                    v = cdiv(v * (k.length - phase), k.fade)
                out[i] += v

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
            interval = int(e.args[2]) if len(e.args) > 2 else 0
            seek_on = seek_clicks(pack.sounds["seek"], n, interval, rate)
        elif e.sound == "SEEK_END" and seek_on is not None:
            # The clicks run until this event (it comes once the gap shows
            # the seek is over, 20 ms after its last step: the firmware's
            # mixer can't know sooner), then the one sounding finishes,
            # unless the step count says the last step's click is over.
            # The seek clicked from its second step (the first was a STEP).
            steps = int(e.args[1]) if len(e.args) > 1 else 0
            stop = n + seek_on.length
            if steps >= 2:
                stop = max(n, min(stop, seek_on.start + (steps - 2) * seek_on.period + seek_on.length))
            clicks(seek_on, stop)
            seek_on = None
        elif e.sound == "BUMP":
            place("bump", n)
        elif e.sound in ("INSERT", "EJECT"):
            place(e.sound.lower(), n)
    if motor_on is not None:  # still spinning at the end of the trace
        loop("spin", motor_on, len(out))
    if seek_on is not None:
        clicks(seek_on, len(out))

    return [32767 if v > 32767 else -32768 if v < -32768 else v for v in out]


def write_wav(path: Path, samples: list[int], rate: int):
    data = array.array("h", samples)
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
