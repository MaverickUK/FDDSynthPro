# Testing on real computers

The rig tests ([bringup.md](bringup.md)) prove the drive against the Feather host. These steps take it to the machines it's for: a PC first, then an Amiga. Each step says what should happen, so a failure is easy to spot. Keep the console log (step 2 below): it records every select, step and write the computer made.

The test card ([make_images](../host/fddproto/make_images.py), plus the ADFs) holds:

| Image | What it is |
|---|---|
| `DOS1440.IMG` | 1.44 MB FAT12: `README.TXT`, `CHANGES.TXT`, `RANDOM.BIN` (300 KB), folder `GAMES` |
| `DOS720.IMG` | The same at 720 KB (no `RANDOM.BIN`) |
| `GEN1440.IMG`, `RAND1440.IMG`, `RAND720.IMG` | Test patterns, no file system: safe to format |
| `GEN1440.HFE` | The generated disk as a flux image |
| `AMIGADOS.ADF` | AmigaDOS (FFS) disk `FDDSynthPro`: `README`, `random.bin` (200 KB), folder `Work` |
| `RAND880.ADF` | 880 KB of random data: safe to format |

Copies of all of them are in `~/fdd-images`, to check against afterwards and to put back.

## Before either computer

1. **Change settings over USB:** in the console, type `e`, the setting, and Enter (`e` `interface=ibmpc` Enter), then `!` to restart with it. `s` shows what's in use. No need to take the card out.
2. **Log the console** for the whole session:
   ```
   ~/.platformio-venv/bin/pio device monitor -p /dev/cu.usbmodem11101 | tee host/runs/realhw-$(date +%Y%m%d-%H%M).log
   ```
3. **Power order:** the BB48R on USB first, then the computer. Off: the computer first, then unplug the BB48R. Its inputs are only 5 V tolerant while it's powered.
4. **At the end of each session**, type `s` and keep the line. `write_faults`, `capture_overruns`, `write_bad_crc` and `audio_underruns` should all be 0; `decode_kbps` well above 370.
5. **Then check the log:** `scripts/check_log.py host/runs/realhw-....log` lists anything that needs a look (the board restarting on its own, lost writes, steps past either end of the disk, non-zero fault counters) and summarises the session: the lines the computer selected with, its step rate, its writes and disk changes.

## A PC

### Setup
- **J7 closed** (adds the 1 kΩ pull-ups a PC's controller needs). Open it again before going back to the Feather.
- **On the cable's end connector**, past the twist: the drive is then A:. On the middle connector it's B:, with `cable=straight`.
- **Settings:** `interface=ibmpc`, `cable=twisted`, `drive=a` (the defaults), and `device0=DOS1440.IMG` to start with that disk every time. `s` should show the first three, `i` the image.
- **BIOS:** drive A: set to 1.44 MB 3.5".

### Steps

| # | Do | Expect |
|---|---|---|
| 1 | Power on, BIOS POST | The drive is found with no error; the console shows the BIOS selecting it, stepping to 0 and back (`STEP`, `ACT STEP`/`BUMP`) |
| 2 | Boot with the disk in | `DOS1440.IMG` isn't bootable (mtools made it): a message that it isn't a system disk, or the BIOS going on to the hard disk. Or eject it first with `d` |
| 3 | `DIR A:` | `README.TXT`, `CHANGES.TXT`, `RANDOM.BIN`, `GAMES` |
| 4 | `TYPE A:README.TXT` | The project's README text |
| 5 | `COPY A:RANDOM.BIN C:\` then `FC /B A:RANDOM.BIN C:\RANDOM.BIN` | "no differences encountered" |
| 6 | `COPY C:\AUTOEXEC.BAT A:\GAMES` (any small file), `DIR A:\GAMES`, `TYPE A:\GAMES\AUTOEXEC.BAT` | The file is there and reads back |
| 7 | `COPY C:\RANDOM.BIN A:\COPY.BIN`, then `FC /B C:\RANDOM.BIN A:\COPY.BIN` | No differences: a 300 KB write across many tracks |
| 8 | **Disk change:** console `]` (next image: `DOS720.IMG`), then `DIR A:` | The 720 KB disk's listing, not the old one. If DOS shows the old listing, DSKCHG (pin 34) isn't reaching the PC |
| 9 | `CHKDSK A:` on the 720 KB disk | No errors; 730,112 bytes total |
| 10 | Console `d` (eject), `DIR A:` | "Not ready reading drive A" (Abort). `d` again puts it back |
| 11 | Swap to `RAND1440.IMG` (`]`/`[`, `i` to check), `FORMAT A:` | Formats all 80 cylinders; a track write each, so watch `s` afterwards for write faults |
| 12 | Copy files onto the new disk, `CHKDSK A:` | No errors |
| 13 | Soak: a batch file that copies and compares in a loop for an hour or overnight | No errors, no hangs |

### Afterwards
- Card into the Mac: `fsck.fat -n "/Volumes/NO NAME/DOS1440.IMG"` and `mdir -i "/Volumes/NO NAME/DOS1440.IMG" ::GAMES` should agree with what the PC did.
- Put the test images back from `~/fdd-images` (step 11 formatted `RAND1440.IMG`; the rig tests compare against the copies).
- Clear the fixed disk with `e` `device0=` if you want the drive to remember the last disk again.

### If something fails
- **"General failure" or "Not ready" on every access:** wrong drive select. Check `p` while the PC accesses A: (`DRVSB` and `MOTEB` should go `ON`); try `cable=straight` or `drive=b`; check J7.
- **Read errors (CRC, sector not found):** the PC's controller doesn't like RDATA. Note which tracks; try a 720 KB disk; a logic analyser on pin 30 against a real drive's tells more.
- **The old directory after a swap (step 8):** DSKCHG. The Feather never could read pin 34, so this is its first real test.
- **Write errors:** keep the `s` line: `write_bad_crc` (the decoder couldn't read the PC's writes), `write_unplaced` (sectors where the drive didn't expect them), `capture_overruns` (core 0 fell behind).

## An Amiga

### First: the pin 2 rework
Without it the Amiga reads the disk that was in at boot but never sees a swap. On the rev B carrier ([wiring.md](wiring.md#pin-2-for-shugart-and-amiga-hosts-a-rework)):
1. Lift U2 pin 13 (6A) off its ground pad, or cut its track, and wire it to GPIO 23 (BB48R EXT1-27).
2. Wire U2 pin 12 (6Y) to floppy pin 2 (J1-2).
3. Run the solder-bridge test again (`x` then `Y`, floppy cable unplugged).

### Setup
- **J7 open** (10 kΩ) to start with: weaker pull-ups suit any driver. Close it only if the console shows steps or selects the Amiga didn't make.
- **Where it goes:**
  - **As DF0**, on the internal 34-pin cable (A500, A600, A1200) in place of the internal drive. The Amiga then boots from it, so put a bootable ADF of your own (Workbench) on the card. `AMIGADOS.ADF` isn't bootable: Kickstart shows its insert-disk screen.
  - **As DF1**, on an external-port adapter, booting from the real DF0 or a hard disk. Easier for a first test: Workbench is up before the drive is touched.
- **Settings:** `interface=amiga`, `drive=df0` (or `df1`), `pin02=chg`, `device0=AMIGADOS.ADF` (or your Workbench ADF for DF0). `!` to restart, `s` to check.
- HD ADFs (1.76 MB) aren't served: they need a 150 RPM drive.

### Steps

| # | Do | Expect |
|---|---|---|
| 1 | Power on | The console shows the Amiga reading the drive ID (`/RDY` with the motor off) and the motor latched at each select (`ACT MOTOR_ON`) |
| 2 | (DF1) Workbench up | A `FDDSynthPro` disk icon; (DF0) the Workbench ADF boots |
| 3 | Open the disk; read `README` (MultiView or `type DF1:README` in a Shell) | "FDD Synth Pro test disk..." |
| 4 | Shell: `copy DF1:random.bin RAM:`, then `copy RAM:random.bin DF1:again.bin`; on the Mac afterwards, `xdftool` can pull both out to compare | Both copies complete with no requester |
| 5 | Copy a file from RAM: or the hard disk to the floppy | Written; reads back. Watch `s` |
| 6 | **Disk change:** console `]` / `[` to another ADF | The icon goes and the new disk's appears (needs the rework and `pin02=chg`) |
| 7 | Swap to `RAND880.ADF`, Format (Quick off) from Workbench | Formats all 80 cylinders, whole-track writes |
| 8 | DiskCopy between DF0 and DF1 (with a real drive as the other) | Copies both ways |

### Afterwards
- Card into the Mac: `host/.venv/bin/xdftool "/Volumes/NO NAME/AMIGADOS.ADF" list` shows what the Amiga left, and `xdftool ... validate` checks the file system.
- Put the ADFs back from `~/fdd-images`.

### If something fails
- **No disk icon at all:** the select or motor line. `p` while the Amiga accesses the drive; the right `DF0`/`DF1` setting; the motor must be on as the drive is selected (the latch).
- **"Not a DOS disk" or read errors:** note the tracks; the rig test (`-m amiga`) proves the same tracks with the Feather.
- **No icon change on a swap:** pin 2: the rework, `pin02=chg`, and `interface=amiga` (it's never driven with `ibmpc`).
