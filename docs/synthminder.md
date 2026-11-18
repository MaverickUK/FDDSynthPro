# SynthMinder and the drive: the mailbox at cylinder 82

**Status: built in 0.25.0-dev.1, host-tested (`test/native/test_mailbox.cpp`); not yet tried on a PC.**

[SynthMinder](https://github.com/MaverickUK/SynthMinder) is a DOS program that shows and changes how the IDE Synth Bay and the HDD Synth are set up. This page is the reference for how it talks to the FDD Synth Duo.

## Why a mailbox

An IDE drive takes commands, so SynthMinder talks to the Bay with one of its own (ATA command 8Dh). The floppy cable carries none: the PC's controller holds the commands, and the cable carries only pulses and levels. What does reach the drive is **sector data**. The drive already decodes the PC's writes to update its images, and builds the read data itself, so the two exchange sectors.

## Where: cylinder 82

The mailbox is **cylinder 82, head 0**: past the last cylinder of an 80-track disk (79), inside the head's travel (83), and on whichever drive the Duo serves. With two drives (`drive=a,b`) each has one. `MailboxSource` (`src/mailbox.*`) sits between each drive's `DiskSlot` and its loader and writer: it serves cylinder 82 itself and never passes it to the image, so the user's disk is never touched.

- The track is laid out as the disk's own tracks, at the disk's data rate: the rate the BIOS has set the controller to for the disk in.
- With the head at cylinder 82, the drive never reports write-protect, so SynthMinder can write whatever disk is in. Everywhere else, a protected disk stays protected.
- Cylinders between the disk's end and 82 behave as before: blank, and writes there dropped.
- **Which disks have one:** IBM MFM, 80 tracks or fewer, single-stepped, at least 9 sectors of 512 bytes numbered from 1. That covers 720 KB, 1.2 MB, 1.44 MB and 1.68 MB DMF disks and the welcome disk. Flux images (HFE), Amiga disks, FM and 40-track disks have none, and nor does an empty drive.

## Finding the drive

Before its first request SynthMinder only **reads**: sectors 5-9 of cylinder 82 hold `$FDK,1,HELLO,OK,` with sequence 0 from power-on. A real drive's disk has no track 82, so the read fails (sector not found), and SynthMinder never writes to a drive that isn't a Duo.

## An exchange

1. SynthMinder writes the request to **sector 1** (INT 13h, AH=03h; CH=82, CL=1, DH=0).
2. The drive decodes the write as it decodes any other. It answers at once (`src/duo_commands.*`), and the reply goes into **sectors 5-9** of the same track, with the request's sequence number. It's all done in the write's finish, which re-encodes the head.
3. SynthMinder reads **sectors 5-9** (AH=02h; CL=5, AL=5). If they still carry the previous sequence number, the drive hadn't answered yet: it reads them again on the next revolution.

Sector 5 is four sector slots after sector 1: 44 ms at 500 kbit/s, 89 at 250. The write's decode and the head's re-encode take about 4 and 3 ms on the board, so the reply is there on the first revolution (`test_mailbox_reply_on_the_first_revolution`).

- **The time an exchange takes:** the seek out to 82 (about 0.25 s from cylinder 0, at 3 ms a step), the write (up to a revolution, 200 ms at 300 rpm, waiting for sector 1), and the read (about 100 ms). Add 0.5 s if the motor was off. So SynthMinder asks when it opens its Floppy tab and after each change, never on a timer.
- **The same request written twice** (a retry after an error) is answered once: its reply stays, and it isn't run again.
- **A stray write to cylinder 82** (a disk tool writing every cylinder) is ignored: anything that isn't an SMND request in sector 1 is counted (`s`: the third number of `mailbox0=`) and dropped.

## The sector

Both ways, 512 bytes: the same as SynthMinder's IDE command (its `docs/protocol.md`).

| Offset | Size | |
|---|---|---|
| 0 | 4 | `SMND` |
| 4 | 1 | protocol version: `1` |
| 5 | 1 | flags: bit 0 *more* (the reply continues in the next sector) |
| 6 | 2 | sequence number, little-endian: SynthMinder counts its requests; the reply carries its request's |
| 8 | 2 | text length in this sector, little-endian, up to 496 |
| 10 | 6 | zero |
| 16 | 496 | text: sentences, each ending in a line feed. A sentence is never split between sectors |

A request holds one sentence. A reply has up to five sectors (2,480 bytes of text). Every sector of the reply carries the sequence number, and all but the last used have *more* set.

## The sentences

Each is a sentence in the HDD Synth's format: `$BODY*CS`, with CS the XOR of BODY's characters as two hex digits (`src/sentence.h`). `$FDQ` asks and `$FDC` changes. The letters A and B are the PC's names for the drives, as `drive` in FDDSYNTH.CFG sets them.

| Request | Reply |
|---|---|
| `FDQ,1,STATUS` | one `$FDV,1,<key>,<value>` per value (below) |
| `FDQ,1,FILES[,<from>]` | `$FDF,1,<index>,<count>,<name>,<KB>,<kind>` for each image on the card, from `<from>` (0 if left out), as many as fit; SynthMinder asks again from the next. Kind: `I` a sector image, `H` a flux image (HFE), `A` an Amiga disk. From past the end: `$FDF,1,<count>,<count>,-,0,-` |
| `FDQ,1,PACKS` | `$FDP,1,<index>,<count>,<name>` for each sample pack |
| `FDQ,1,SETTINGS` | `$FDS,1,<key>,<next>,<now>` for each setting SET changes: `drive`, `device0`, `device1` (not `interface` or `cable`: set wrong from the PC, they would lose it the drive). `<next>` is FDDSYNTH.CFG's (from the next power-on), `<now>` what the drive runs with; `-` for none |
| `FDC,1,INSERT,<A\|B>,<name>` | `$FDK,1,INSERT,OK,`: the disk goes into that drive, as from the front panel (any case) |
| `FDC,1,EJECT,<A\|B>` | `$FDK,1,EJECT,OK,` |
| `FDC,1,PACK,<index>` | `$FDK,1,PACK,OK,`: loaded once the drive is idle |
| `FDC,1,VOL,<0-100>` | `$FDK,1,VOL,OK,` |
| `FDC,1,SET,<key>,<value>` | `$FDK,1,SET,OK,`: into FDDSYNTH.CFG once the drive is idle, from the next power-on. The value may hold commas (`drive`'s `a,b`): the fields after the key are joined again. `-` clears `device0` or `device1` (the drive then starts with the last disk in) |

STATUS's keys: `firmware`, `drives` (`A`, `AB`), `mailbox` (the drive the request came through), `volume`, `pack` and `pack_index`, then for each drive by its letter (`a_`, `b_`): `disk` (the image, `welcome`, `generated`, or `-` for none), and with a disk in, `kb`, `format` (e.g. `1440 KB MFM 300 rpm`), `cylinder`, `motor` and `protect` (`on`/`off`).

**Errors:** a change that wasn't done answers `$FDK,1,<what>,ERR,<reason>`:
- `notfound`: no image of that name;
- `nodrive`: no drive with that letter;
- `busy`: a swap is under way;
- `empty`: nothing to eject;
- `range`: the pack or volume is out of range;
- `key`: not a setting SET changes;
- `value`: a value the setting doesn't take;
- `card`: no card to save it to.

A request that isn't a sentence, or not version 1: `$FDK,1,REQUEST,ERR,invalid`. One the drive doesn't know: `$FDK,1,<its first word>,ERR,unknown`. Values never contain `,`, `*` or `$`: they become `_`.

**Swapping the drive the request came through:** the reply must still be read from it, so the swap waits until the head leaves cylinder 82 (SynthMinder seeks back to cylinder 0 after a change), or 1.5 s. Then the drive ejects and inserts as usual, and DOS sees the disk change.

## To settle on a PC

- **BIOSes that check the cylinder.** Most pass a cylinder past the disk's end straight to the controller; some may check it against their drive table and refuse. SynthMinder would then need to program the controller itself (ports 3F0h-3F7h, DMA channel 2).
- **An empty drive.** With no disk there's no index pulse, so the controller times out before it reads. The drive could keep the mailbox working by spinning a mailbox-only disk while empty, without telling DOS a disk has arrived.
- **The seek's sound.** The synth plays the head's trip out to 82 and back as a seek. That's authentic, but it happens on every exchange.
