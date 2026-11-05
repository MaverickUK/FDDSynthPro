# Recording a real drive for a sample pack

Two scripts turn a real 3.5-inch drive into a sample pack. `scripts/record_drive.py` drives the drive with `gw` while the Mac records it and logs when each action ran. `scripts/make_drive_pack.py` then cuts the pack from the recording and the log.

## Setup

- **The drive on the Feather's cable**, in place of the BB48R: unplug the carrier from the Feather. The FeatherWing doesn't power a drive, so give the drive its own 5 V (and 12 V, if it's an old one).
- **A disk in it.** `--writes` overwrites cylinders 38–42 with their own data, so it's safe on any disk, but use a spare one anyway.
- **The microphone** close to the drive (10–20 cm), in a quiet room. `scripts/record_drive.py --list-mics` lists them: the MacBook Air's is the default; `--mic 0` (its number in the list) uses the phone's. macOS asks for microphone access the first time.

## Recording

```
scripts/record_drive.py --writes
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
scripts/make_drive_pack.py host/runs/<time>-drive --audition /tmp/drive.wav
```

It writes `samples/Drive/` (`--name` for another name): step, seek, bump, spinup, spin, spindown, insert and eject. It also writes every seek whole into the run's `extras/` folder, with the measured step rate in `pack_notes.json`, for seek sounds that follow the rate (not in the firmware yet). It prints what it heard: steps found, the offset between the recording and the log, the step rate and the disk's revolution. Listen to the audition file before using the pack. Copy the folder to the card's `/samples/`, then pick it on the Sounds screen.

If it heard too few steps, the recording is too quiet or the offset is wrong: `--offset` sets it by hand (recording time minus log time). `host/tests/test_drive_pack.py` runs the cutter on a simulated session.

## What the Feather can't do

- **Fast seeks:** it steps every 26 ms, so its seeks sound like separate clicks, which is what the drive itself does at that rate. A PC steps every 3–6 ms. Its seeks need a real Greaseweazle (`gw delays`), or a PC.
- **Bumps:** it stops stepping out at TRK00, and stepping past the stop needs `gw seek --force` to a negative cylinder, which its firmware may refuse. The cutter says if it heard no bump; keep the old pack's then.
