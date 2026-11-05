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

Before: the real drive on the Feather's cable (the BB48R unplugged from it),
powered from its own supply, a disk in it. --writes overwrites cylinders
38-42 of that disk. The mic close to the drive, the room quiet. macOS asks
for microphone access the first time.
"""
from __future__ import annotations

import argparse
import signal
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


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--mic", default=DEFAULT_MIC, help=f"the microphone (default: {DEFAULT_MIC})")
    p.add_argument("--list-mics", action="store_true", help="list the microphones and stop")
    p.add_argument("--no-record", action="store_true", help="don't record (you record it some other way)")
    p.add_argument("--writes", action="store_true", help="also write cylinders 38-42 (their own data, back)")
    p.add_argument("--repeats", type=int, default=2, help="how many of each step and seek (default 2)")
    p.add_argument("--dry-run", action="store_true", help="no gw and no recording: try the plan")
    p.add_argument("--out", type=Path, help="the session folder (default host/runs/<time>-drive)")
    args = p.parse_args()
    if args.list_mics:
        print(list_mics())
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
    start = time.monotonic()
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
