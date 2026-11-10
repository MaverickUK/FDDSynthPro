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

- **Turn the motor off:** after a command it can keep the motor running, so the session's spin-ups aren't from rest and its spin-downs are doubtful. `samples/RealDrive` (6 November) has real steps, spin, inserts and ejects, but its spin-up and spin-down want recording again: with a real Greaseweazle, or with the drive out of the case on a PC's cable (inside it, the PC's noise hides the motor: below).

- **Fast seeks:** it steps every 26 ms, so its seeks sound like separate clicks, which is what the drive itself does at that rate. A PC steps every 3–6 ms. Its seeks need a real Greaseweazle (`gw delays`), or a PC.
- **Bumps:** it stops stepping out at TRK00, and `gw seek` refuses a negative cylinder ("must be 0 or greater"). The cutter says if it heard no bump; `--borrow` takes another pack's.

**A PC does what it can't, but not everything:** its BIOS stops the motor 2 s after each access, and it seeks at 3-6 ms a step. A Voice Memo of a real drive in a PC running FDDBench gave the seek; the rest stayed out of reach (the Evesham, 6 November):

- **The seek: yes.** FDDBench's Full stroke (ten seeks of 79 steps) is a loud, clean buzz, 30 dB over the room, at the Evesham's 5.05 ms a step. A seek's peak is 6.5 dB over a single step's: overlapping steps ring together.
- **Spin-up, spin-down and the hum: no.** Inside the PC the motor doesn't show over the PC's own noise at all, even with the phone at the drive's front.
- **A bump: no.** The BIOS's recalibrate stops at TRK00 and never knocks.

## Cutting a seek from a PC recording

```
scripts/cut_seek.py memo.m4a --seeks 145.5:153.5 --steps 126.5:134.5 --step-ms 5.052 --pack samples/RealDrive --audition /tmp/seek.wav
```

It loops 40 whole steps from the middle of the median seek in `--seeks`, starting on a step, and writes the pack's `seek.wav` at the level that keeps it against the pack's `step.wav` as the recording had it against the single steps in `--steps`. If the pack no longer fits the sample pool, it shortens the spin-down, as the cutter does.

- **Finding the windows:** FDDBench's `FDDBENCH.LOG` timestamps each scenario. Line it up with the recording by a distinctive one: a sequential read clicks every 0.6 s, three revolutions a cylinder, and Full stroke is ten buzzes 0.8 s apart.
- **The step period:** from FDD Synth Duo's log of the same PC, a 79-step seek's `STEP` span over 78 (394,079 µs on the Evesham); or from the recording, where the buzz's peaks repeat.
- **The drive's speed** comes free: a sequential read's clicks are three revolutions apart by the phone's clock, which is independent of the PC (the Evesham's drive: 0.6016 s, 299.2 RPM).

`host/tests/test_cut_seek.py` runs it on a simulated recording.

`samples/RealDrive`'s seek comes from the Evesham's memo (`--seeks 145.5:153.5 --steps 126.5:134.5 --step-ms 5.052`); its steps, spin, inserts and ejects from the Feather session; its bump from Recorded. Its spin-up and spin-down are still the Feather's.
