# The settings file

`/FDDSYNTH.CFG` on the SD card holds what the drive remembers across power-off, and how it sits on the cable. It's plain text, one `key=value` a line. The drive writes it a few seconds after you change the disk, the pack or the volume, so those three look after themselves. Edit the file on a computer to set the rest.

A damaged or missing file never stops the drive starting: an unknown line is ignored, and a bad value keeps the default.

```
image=GAMES.IMG
pack=Recorded
volume=75
interface=ibmpc
cable=twisted
drive=a
```

| Key | Values | Default | |
|---|---|---|---|
| `image` | a file name on the card | the first | The disk that was in |
| `image2` | a file name on the card | the second | The second drive's, with two drives |
| `pack` | a folder under `/samples` | the first | The sample pack |
| `volume` | 0–100 | 75 | |
| `interface` | `ibmpc`, `shugart` | `ibmpc` | What the host expects on the cable |
| `cable` | `twisted`, `straight` | `twisted` | `ibmpc` only: is there a twist between the host and the drive? |
| `drive` | `a`, `b` or `a,b` (`ibmpc`); one or two of `ds0`, `ds1`, `ds2` (`shugart`) | `a` | Which drive this is: a real drive's select jumper. Two make it two drives |

The drive reads `interface`, `cable` and `drive` at power-on. The console's `s` shows the ones in use.

## Two drives

`drive=a,b` (or two Shugart units, `drive=ds0,ds1`) makes the emulator both drives on the cable, each with its own disk, head and motor. With a PC that's A: and B:, and `COPY A:*.* B:` works as it would with two real drives. The first drive's disk is `image`, the second's `image2`; with neither set, they start with the card's first two images.

- **The front panel** has a home screen for each, titled with its letter (`A: GAMES.IMG`). The action button goes from A's to B's, then on to the head trace. Choosing a disk, or a long press to eject, acts on the drive whose home screen was last shown.
- **The console's** `i`, `[`, `]` and `d` act on one drive: `1` or `2` chooses it. `s` reports the second drive's state with a `2` on each name (`cyl2`, `disk2`, `ready2`...).
- **The same image in both** is read-only in the second: two drives writing one file would each overwrite the other's changes.
- **The sound** is one mechanism: the motor runs while either drive's does, and you hear both drives' steps.
- Each drive keeps its own disk in PSRAM (1.44 MB each, of 8 MB) and its own cylinder buffer in RAM.

## Choosing the drive select

These do what the DS jumpers do on a real drive, and what the jumpers and FlashFloppy's `interface` setting do on a GoTek.

**An IBM PC** drives each drive on a pair of lines: select with motor. Drive A is on pins 14 and 10, drive B on 12 and 16. On a PC's cable the drive on the end connector, past the twist, sees A's lines on 12 and 16, so a PC drive is always jumpered for 12/16 and the cable decides whether it's A or B.

| Where the drive is | `cable` | `drive=a` answers on | `drive=b` answers on |
|---|---|---|---|
| Past the twist (the end of a PC cable) | `twisted` | 12 / 16 | 14 / 10 |
| No twist (a straight cable, or a PC cable's middle connector) | `straight` | 14 / 10 | 12 / 16 |

The rig's Feather host drives its drive A on 12 and 16, over a straight ribbon: the default (`twisted`, `a`) answers it. A Greaseweazle on a straight cable drives A on 14/10 and B on 12/16.

Pin 34 carries DISK CHANGE, and pin 2 is the host's density select, which the drive reads.

**A Shugart host** (the Atari ST, Amstrad CPC and PCW, many synths and samplers, most non-PC machines) has one select line per drive, DS0 to DS3, and one MOTOR ON line for all of them.

| `drive` | Select | Motor |
|---|---|---|
| `ds0` | pin 10 | pin 16 |
| `ds1` | pin 12 | pin 16 |
| `ds2` | pin 14 | pin 16 |

Pin 34 carries READY: asserted while the drive is selected, with a disk in and the motor at speed. DS3 (pin 6) isn't wired on the carrier.

A Shugart host also expects DISK CHANGE on pin 2. The carrier can only read pin 2, so the drive can't send it; hosts that need it see the disk change through READY instead. The carrier's sixth SN7407 gate is free for it, with a small rework (U2 pin 13 lifted from ground to a spare GPIO, and pin 12 to floppy pin 2).
