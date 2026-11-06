# Recording a real drive for a sample pack

Two scripts turn a real 3.5-inch drive into a sample pack. `scripts/record_drive.py` drives the drive with `gw` while the Mac records it and logs when each action ran. `scripts/make_drive_pack.py` then cuts the pack from the recording and the log.

## Setup

- **The drive on the Feather's cable**, in place of the BB48R: unplug the carrier from the Feather. The FeatherWing doesn't power a drive, so give the drive its own 5 V (and 12 V, if it's an old one).
- **A disk in it.** `--writes` overwrites cylinders 38–42 with their own data, so it's safe on any disk, but use a spare one anyway.
- **The microphone** close to the drive (10–20 cm, beside it rather than on it), in a quiet room. The MacBook Air's own mic is too far away unless the Mac is right next to the drive.
- **Record on the phone with Voice Memos, not the Mac.** The iPhone as the Mac's microphone (Continuity) drops about 10 ms of audio every 85 ms (12% of it, measured with a tone: the pitch is right, the sound is chopped), and takes up to 40 s to start. Voice Memos: Settings → Apps → Voice Memos → Audio Quality → Lossless. Turn Continuity Camera off on the phone for the session (Settings → General → AirPlay & Continuity), and set the Mac's input to its own mic: otherwise any app that opens the microphone takes the phone over and stops the memo.
- **This drive's motor is quiet once it's turning** (13 dB over the room at 15 cm) and loud while it spins up. `--check` runs the motor for 15 s and says whether the mic hears it.

## Recording

```
scripts/record_drive.py --no-record --writes      # with a Voice Memo running
scripts/record_drive.py --mic 1 --writes          # or the Mac's own mic (it waits for audio, and saves the offset)
```

It takes about 6 minutes, plus the prompts. It records, in order:
1. 4 s of the room;
2. single steps in and out at cylinders 10, 30, 50 and 70;
3. seeks of 2, 3, 5, 10, 20, 40 and 78 steps, each way;
4. steps past the outer stop, to get a bump, if the Feather's firmware will do it;
5. two long reads of one track, each followed by 4 s of quiet: spin-up, the disk turning, spin-down;
6. a read of five tracks (and with `--writes`, a write of them);
7. five ejects and five inserts.

At each `EJECT` or `INSERT` prompt, do it, wait for the drive to go quiet, then press Enter. Everything goes into `host/runs/<time>-drive/`: `recording.wav`, `session.jsonl` (when each action ran) and `gw.txt` (what `gw` said).

`--dry-run` tries the plan with no `gw` and no recording. `--no-record` leaves the recording to you (a phone, say): pass that file to the cutter with `--recording`.

## Cutting the pack

```
scripts/make_drive_pack.py host/runs/<time>-drive --recording memo.m4a --borrow Recorded --audition /tmp/drive.wav
```

`--recording` is the phone's file (AirDrop it over); the cutter finds the offset from the clicks. `--borrow Recorded` fills what the Feather can't record (the seek and the bump) from another pack, at that pack's levels. Inserts and ejects, done with a hand by the mic, are capped under the peak and don't set the pack's gain.

It writes `samples/Drive/` (`--name` for another name): step, seek, bump, spinup, spin, spindown, insert and eject. It also writes every seek whole into the run's `extras/` folder, with the measured step rate in `pack_notes.json`, for seek sounds that follow the rate (not in the firmware yet). It prints what it heard: steps found, the offset between the recording and the log, the step rate and the disk's revolution. Listen to the audition file before using the pack. Copy the folder to the card's `/samples/`, then pick it on the Sounds screen.

If it heard too few steps, the recording is too quiet or the offset is wrong: `--offset` sets it by hand (recording time minus log time). `host/tests/test_drive_pack.py` runs the cutter on a simulated session.

## What the Feather can't do

- **Turn the motor off:** after a command it can keep the motor running, so the session's spin-ups aren't from rest and its spin-downs are doubtful. `samples/RealDrive` (6 November) has real steps, spin, inserts and ejects, but its spin-up and spin-down want recording again, best from a PC (below).

- **Fast seeks:** it steps every 26 ms, so its seeks sound like separate clicks, which is what the drive itself does at that rate. A PC steps every 3–6 ms. Its seeks need a real Greaseweazle (`gw delays`), or a PC.
- **Bumps:** it stops stepping out at TRK00, and `gw seek` refuses a negative cylinder ("must be 0 or greater"). The cutter says if it heard no bump; `--borrow` takes another pack's.

**A PC does all three:** its BIOS stops the motor 2 s after each access, it seeks at 3-6 ms a step, and a recalibrate from far out knocks the stop. A Voice Memo of a real drive in a PC running FDDBench (spin-up and idle, recalibrate, full stroke, butterfly) is the source for the spin-up, spin-down, seek and bump.
