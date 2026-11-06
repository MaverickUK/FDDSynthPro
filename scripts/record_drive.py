#!/usr/bin/env python3
"""Records a real drive for a sample pack: drives it through every sound
with gw (the Feather or a Greaseweazle), records the Mac's microphone with
ffmpeg, and logs when each action ran (host/fddproto/drive_session.py).
Then scripts/make_drive_pack.py cuts the pack.

    scripts/record_drive.py --list-mics
    scripts/record_drive.py                      # MacBook Air Microphone, into host/runs/
    scripts/record_drive.py --mic 0 --writes        # a mic by its number in --list-mics
    scripts/record_drive.py --no-record          # record it yourself; the cutter finds the offset
    scripts/record_drive.py --dry-run            # no gw, no recording: the plan and its timing
    scripts/record_drive.py --mic 0 --check      # 15 s of the motor: does the mic keep its hum?

Before: the real drive on the Feather's cable (the BB48R unplugged from it),
powered from its own supply, a disk in it. --writes overwrites cylinders
38-42 of that disk. The mic close to the drive, the room quiet. macOS asks
for microphone access the first time.

The iPhone's microphone (Continuity) can take 30 s or more to start, so the
session waits until audio is arriving, and saves how much was recorded
before it began (recording.json), which the cutter uses as the offset.

macOS's voice processing removes a steady hum within a second or two, so
the motor vanishes from a spin. Set Mic Mode to Wide Spectrum (Control
Centre, while the mic is recording) and run --check: it says whether the
hum held.
"""
from __future__ import annotations

import argparse
import array
import json
import math
import signal
import wave
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "host"))

from fddproto import drive_session  # noqa: E402
from fddproto.drive_session import Action, Logged  # noqa: E402

DEFAULT_MIC = "MacBook Air Microphone"


def list_mics() -> str:
    p = subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
                       capture_output=True, text=True)
    lines = p.stderr.splitlines()
    out, audio = [], False
    for line in lines:
        if "audio devices" in line:
            audio = True
        elif audio and "] [" in line:
            out.append(line.split("] ", 1)[1])
    return "\n".join(out)


class Recorder:
    """ffmpeg recording the mic to a WAV until stopped."""

    def __init__(self, mic: str, path: Path):
        self.path = path
        self.p = subprocess.Popen(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "avfoundation", "-i", f":{mic}",
             "-ac", "1", "-ar", "48000", str(path)], stdin=subprocess.PIPE)

    def audio_seconds(self) -> float:
        """How much audio is in the file so far (48 kHz mono 16-bit)."""
        try:
            return max(0, self.path.stat().st_size - 44) / 96000
        except FileNotFoundError:
            return 0.0

    def wait_for_audio(self, timeout: float = 120) -> float:
        """Until audio is arriving; returns the seconds of it already in the file."""
        start = time.monotonic()
        print("waiting for the microphone's audio to start...", flush=True)
        while self.audio_seconds() < 2.0:
            if self.p.poll() is not None:
                raise SystemExit("ffmpeg stopped: try --list-mics")
            if time.monotonic() - start > timeout:
                raise SystemExit(f"no audio from the microphone after {timeout:.0f} s")
            time.sleep(0.1)
        lead = self.audio_seconds()
        print(f"audio is arriving (after {time.monotonic() - start:.1f} s)")
        return lead

    def stop(self):
        try:
            self.p.communicate(b"q", timeout=10)  # ffmpeg's own quit: the WAV's header is finished
        except subprocess.TimeoutExpired:
            self.p.send_signal(signal.SIGINT)
            self.p.wait(10)


def run_action(gw, x: Action, scratch: Path, dry: bool) -> tuple[bool, str]:
    if x.kind == "pause":
        time.sleep(x.args["s"])
        return True, ""
    if x.kind == "prompt":
        input(f"\n>>> {x.args['say']} now, then press Enter once it's done... ")
        return True, ""
    if dry:
        time.sleep(drive_session.duration_s([x]))
        return True, "(dry run)"
    if x.kind == "seek":
        args = [str(x.args["cyl"])] + (["--force"] if x.args.get("force") else [])
        r = gw.run("seek", *args, timeout=30)
    elif x.kind == "spin":
        # A flux read of one track: the motor on for its revolutions.
        r = gw.run("read", "--revs", str(x.args["revs"]), "--tracks", f"c={x.args['cyl']}:h=0",
                   str(scratch / "spin.scp"), timeout=60)
    elif x.kind == "read":
        r = gw.read(scratch / "read.img", "ibm.1440", x.args["tracks"])
    elif x.kind == "write":
        r = gw.read(scratch / "keep.img", "ibm.1440", x.args["tracks"])  # what's there, written back
        if r.ok:
            r = gw.write(scratch / "keep.img", "ibm.1440", x.args["tracks"])
    else:
        raise ValueError(x.kind)
    return r.ok, r.output


def levels(path: Path, step_s: float = 0.5) -> list[float]:
    """RMS in dBFS for each step_s of a mono 16-bit WAV."""
    with wave.open(str(path)) as w:
        x = array.array("h", w.readframes(w.getnframes()))
        n = int(w.getframerate() * step_s)
    out = []
    for i in range(0, len(x) - n, n):
        seg = x[i:i + n]
        out.append(20 * math.log10(max(1.0, math.sqrt(sum(v * v for v in seg) / n)) / 32768))
    return out


def check(mic: str):
    """Records 4 s of the room, then 15 s of the motor turning: says whether
    the hum held or the mic's voice processing took it out."""
    from fddproto.greaseweazle import Greaseweazle
    gw = Greaseweazle()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "check.wav"
        rec = Recorder(mic, path)
        rec.wait_for_audio()
        time.sleep(4)
        t_motor = rec.audio_seconds()
        r = gw.run("read", "--revs", "75", "--tracks", "c=40:h=0", str(Path(tmp) / "spin.scp"), timeout=60)
        time.sleep(1)
        rec.stop()
        if not r.ok:
            raise SystemExit(f"gw read failed:\n{r.output}")
        db = levels(path)
    room = sorted(db[2:int(t_motor * 2) - 1])[len(db[2:int(t_motor * 2) - 1]) // 2]
    # From 3 s after the motor starts (past the spin-up) to the end of the read
    spin = db[int(t_motor * 2) + 6:-3]
    low = min(spin)
    print(f"room {room:.1f} dB; motor {sorted(spin)[len(spin) // 2]:.1f} dB typical, {low:.1f} dB lowest")
    print("half-seconds: " + " ".join(f"{v:.0f}" for v in db[int(t_motor * 2):]))
    if low - room < 6:
        print("FAIL: the hum fell to the room's level: the mic's voice processing is removing it. "
              "Set Mic Mode to Wide Spectrum (Control Centre, while this runs) and try again.")
    else:
        print(f"OK: the hum held, {low - room:.0f} dB or more over the room")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--mic", default=DEFAULT_MIC, help=f"the microphone (default: {DEFAULT_MIC})")
    p.add_argument("--list-mics", action="store_true", help="list the microphones and stop")
    p.add_argument("--no-record", action="store_true", help="don't record (you record it some other way)")
    p.add_argument("--writes", action="store_true", help="also write cylinders 38-42 (their own data, back)")
    p.add_argument("--repeats", type=int, default=2, help="how many of each step and seek (default 2)")
    p.add_argument("--dry-run", action="store_true", help="no gw and no recording: try the plan")
    p.add_argument("--out", type=Path, help="the session folder (default host/runs/<time>-drive)")
    p.add_argument("--check", action="store_true", help="15 s of the motor: does the mic keep the hum?")
    args = p.parse_args()
    if args.list_mics:
        print(list_mics())
        return
    if args.check:
        check(args.mic)
        return

    actions = drive_session.plan(writes=args.writes, repeats=args.repeats)
    out = args.out or REPO / "host/runs" / time.strftime("%Y%m%d-%H%M%S-drive")
    out.mkdir(parents=True, exist_ok=True)
    print(f"{len(actions)} actions, about {drive_session.duration_s(actions) / 60:.0f} minutes; into {out}")

    gw = None
    if not args.dry_run:
        from fddproto.greaseweazle import Greaseweazle
        gw = Greaseweazle()
        info = gw.info()
        if not info.ok:
            raise SystemExit(f"gw info failed:\n{info.output}")

    recorder = None
    if not args.dry_run and not args.no_record:
        recorder = Recorder(args.mic, out / "recording.wav")
        time.sleep(0.5)
        if recorder.p.poll() is not None:
            raise SystemExit(f"ffmpeg couldn't record from {args.mic!r}: try --list-mics")
        lead = recorder.wait_for_audio()
    start = time.monotonic()
    if recorder:
        # Recording time minus log time, for the cutter
        (out / "recording.json").write_text(json.dumps({"offset_s": round(lead, 3)}))
    log: list[Logged] = []
    try:
        with tempfile.TemporaryDirectory() as tmp:
            for i, x in enumerate(actions):
                t0 = time.monotonic() - start
                ok, output = run_action(gw, x, Path(tmp), args.dry_run)
                t1 = time.monotonic() - start
                log.append(Logged(x.kind, x.label, x.args, round(t0, 4), round(t1, 4), ok, output.strip()[-400:]))
                drive_session.save(out / "session.jsonl", log)  # as it goes: a stopped session still cuts
                if x.kind != "pause":
                    print(f"{t0:7.2f} s  {i + 1:3}/{len(actions)}  {x.label:<12} {x.args}  {'' if ok else 'FAILED'}")
    finally:
        if recorder:
            recorder.stop()
        if gw:
            (out / "gw.txt").write_text(gw.transcript())
    failed = [e for e in log if not e.ok]
    print(f"done: {len(log)} actions, {len(failed)} failed" + (f" ({sorted({e.label for e in failed})})" if failed else ""))
    print(f"next: scripts/make_drive_pack.py {out}")


if __name__ == "__main__":
    main()
