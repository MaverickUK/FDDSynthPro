# Pico Floppy Drive Emulator: Prototype Plan

22 Oct 2026, Peter Bridger. Status updated 29 Oct 2026 (firmware 0.17.0).

## Where it stands

Every slice below is built and passes its tests on the Mac, against virtual hosts (a Greaseweazle, the Feather, a PC's BIOS and controller, an Amiga) and Greaseweazle's own encoder and decoder. **None of it has run on hardware yet:** each slice's exit test waits for the rev B carrier board ([hardware/README.md](../hardware/README.md)), ordered from JLCPCB, and slice 6 for a real PC. The bring-up steps are in [bringup.md](bringup.md).

The prototype has also gone well past its first scope:

| Added | Version |
|---|---|
| The FDD Synth's sound (I2S amp, sample packs), and the front panel (OLED, encoder, disk and pack lists) | 0.7, 0.8 |
| The BB48R pin map, disk images in PSRAM, builds without the SN7407 | 0.9, 0.10 |
| The carrier board (rev B), for JLCPCB assembly | |
| Drive select as a real drive's jumpers: PC A or B on either cable, Shugart DS0–DS2 | 0.11 |
| Two drives on one cable, each with its own disk | 0.12 |
| Formats: PC 160 KB to 1.68 MB (5.25- and 3.5-inch, 360 RPM), and GoTek's `IMG.CFG` for other machines | 0.13 |
| The Amiga: ADF images, DF0/DF1, pin 2 (/CHNG) after a board rework | 0.14 |
| The welcome disk (with no card), a fixed startup image | 0.15, 0.16 |
| FM (single density), tracks at half a disk's rate | 0.17 |

The settings, formats and interfaces are described in [config.md](config.md).

## Goal and scope

The prototype proves one thing: an RP2350B can act as a 3.5-inch PC floppy drive, GoTek style, that a real floppy controller reads reliably from a disk image. Because it *is* the drive, it knows exactly when the motor runs, where the head is and how it moves, which is what the FDD Synth needs to play convincing drive sounds. Writing, the sounds themselves and the final board come later, in thin layers on top of a working base.

**In scope** (as first planned; see above for what's been added since)
- One drive on a 34-pin PC (Shugart-style) interface, answering one drive select.
- 3.5-inch MFM formats: 1.44 MB (HD, 500 kbit/s) first, then 720 KB (DD, 250 kbit/s).
- Read first: the host can seek, read every sector and see a steady 300 RPM index.
- Raw sector images (IMG/IMA) on an SD card, chosen by file size.
- Activity events for the sounds: motor, steps, seeks, the head hitting the track 0 stop.

**Still out of scope**
- Flux images (HFE, SCP) and copy-protected disks; Mac GCR, Apple II and C64 disks.
- ED disks (2.88 MB, the Sinclair QL's 3.2 MB): a 1 Mbit/s track is twice what the buffers hold.
- FlashFloppy's `FF.CFG` and its `host=` geometry tables (Akai, Ensoniq, Roland and the like by size alone): `IMG.CFG` covers them.

**Done means:** the host reads a 1.44 MB image from the drive byte for byte, ten times in a row, and writes and reads back 100 images with zero mismatches; then a real PC boots DOS from it and copies files both ways.

## How a floppy interface differs from IDE

| | HDDSynthPro (IDE) | FDDSynthPro (floppy) |
|---|---|---|
| Signals | 16 data lines, register file, strobes | 10 single-bit inputs, 5 single-bit outputs |
| Timing | Tens of nanoseconds (IORDY within 35 ns) | Microseconds: flux transitions every 2, 3 or 4 µs (HD); steps at least ~3 ms apart |
| Who asks | The host addresses registers; the drive answers | The host only steps and selects. The drive streams the track under the head continuously, whether anyone listens or not |
| Pull-ups | On the host | **On the drive** for its inputs (cable termination); on the host for the drive's outputs |
| Outputs | Driven (DD), open drain (IORDY) | All open collector, able to sink a real termination |
| Hardest part | Bus timing | The MFM bit stream, the IBM track layout and CRCs, and a rotation model the host can lock to |

**Numbers that shape the design (3.5-inch, 300 RPM):**

| | DD (720 KB) | HD (1.44 MB) |
|---|---|---|
| Data rate | 250 kbit/s | 500 kbit/s |
| Flux transition spacing | 4, 6 or 8 µs | 2, 3 or 4 µs |
| Raw bytes per track (one 200 ms revolution) | 6,250 | 12,500 |
| Geometry | 80 cylinders × 2 heads × 9 sectors × 512 bytes | 80 × 2 × 18 × 512 |

- **A whole raw track fits in RAM easily:** 12.5 KB per side, 25 KB per cylinder. The 1.44 MB image itself does not fit the RP2350's 520 KB of SRAM, so the drive loads a cylinder on each step: from the BB48R's 8 MB PSRAM, where the whole image is copied before the disk goes in (0.10), since the SD card over SPI is too slow to keep up with seeks.
- **Steps leave time for that.** Hosts wait for the head to settle (about 15 ms) after the last step before reading. A cylinder is 18 KB of sector data, a few milliseconds from SD even over SPI. If a read starts before the data is ready, the host simply retries on the next revolution.
- **The track is a loop.** The bit stream starts at the index pulse and wraps every 200 ms. The host doesn't care where in the loop it starts reading, only that the sector headers and CRCs are right and the timing is steady.

## Test rig

The Mac that builds the firmware also flashes it, drives the host (the floppy controller) through Greaseweazle's `gw` tools, and reads the drive's own log over USB serial.

| Part | Role |
|---|---|
| Olimex RP2350-PICO2-BB48R on the rev B carrier board ([hardware/](../hardware/README.md)) | The emulated drive, with its own microSD slot. The carrier holds the floppy header, the SN7407 and the pull-ups, wired per [wiring.md](wiring.md); a hand-wired Perma-Proto came first |
| Floppy ribbons (one straight, one PC cable with the twist) | Host connection |
| SN7407 (SOIC on the carrier; hex open-collector buffer, 40 mA sink) | The drive's outputs |
| 0402 pull-ups on the carrier, 10 kΩ, plus 1 kΩ switched in by a jumper (J7) | Termination on the drive's ten inputs: 10 kΩ with the FeatherWing, 1 kΩ added for a real PC |
| Feather RP2040 + Adafruit Floppy FeatherWing | The host under test: Adafruit's Greaseweazle-compatible firmware, driven by `gw` from the Mac |
| Optional: Greaseweazle V4.1 | A second host, running the real Greaseweazle firmware, with both drive selects and full pin access |
| A real 3.5-inch PC floppy drive + 5 V 2 A supply | The known-good reference: proves the host setup, and gives real timings and flux to compare with |
| Spare Pico 2 as a logic analyser (sigrok) | Traces when a test fails; floppy signals are slow enough for it |
| Later: a retro PC with a 1.44 MB controller | The real-host test (slice 6) |

**Pin map:**

| Lines | GPIO |
|---|---|
| Inputs: DENSEL, MOTEA, DRVSB, DRVSA, MOTEB, DIR, STEP, WDATA, WGATE, SIDE1 | 12–21 |
| Outputs (through the SN7407): INDEX, TRK00, WPT, RDATA | 4–7 |
| Output (through the SN7407): DSKCHG (READY for Shugart and Amiga) | 22 |
| Output, after a rework: pin 2's DISK CHANGE for Shugart and Amiga | 23 |
| The BB48R's own: PSRAM select; SD card (SPI1: CS, SCK, MOSI, MISO) | 8; 9, 10, 11, 24 |

GPIO 0–3 and 25 are avoided for the same reasons as on HDDSynthPro (the BB48R's I2C pull-ups and LED), and GPIO 8–11 and 24 are the board's. The inputs are on consecutive pins so one read captures them all, and everything is inside one PIO block's GPIO 0–31 window.

**Drive select:** the firmware watches all four select and motor lines (floppy pins 10, 12, 14, 16). A real PC drive answers on pins 12 (select) and 16 (motor); the cable's twist routes the host's drive A lines there. The Feather host has one select, `gw --drive A`, on pins 12/16, so a straight cable reaches our drive directly. A Greaseweazle on a straight cable needs `--drive B` for the same pins. Slice 0 records which pins each letter uses. Since 0.11 the lines are set in `/FDDSYNTH.CFG` (`interface`, `cable`, `drive`: [config.md](config.md)); the default is the PC's 12/16.

**Power-order rule:** the BB48 is powered before the host's USB is plugged in, and the host is unplugged before the BB48. There are no buffers on the inputs, and the RP2350's pins are only 5 V tolerant while it is powered.

## Automation loop

`scripts/rig.sh -m sliceN` runs the host unit tests, then builds, flashes and runs the rig tests. Every code change gets the full suite with no hands on the rig: the host needs no replugging, and `gw reset` puts it back in a known state between tests.

- **Flash:** `pio run -t upload` resets the board into the bootloader over serial and loads it with picotool. The runner then waits for the `READY` line.
- **Host actions:** `host/fddproto/greaseweazle.py` runs `gw` (`seek`, `rpm`, `read`, later `write`) and keeps a transcript.
- **Tests:** pytest, one marker per slice, so earlier slices re-run as regression tests. The `sim` marker covers the tests that need no hardware.

**Two views of every test.**
- **Host view:** what `gw` reports: the sectors it found, the image it read, the RPM it measured.
- **Drive view:** the trace streamed over USB serial. A test passes only when both views agree.

**A third-party decoder, without hardware.** `gw convert` turns an IMG into flux (`.scp`) and back. So the drive's own track encoder can be checked against Greaseweazle's decoder on the Mac, long before the first plug-in (`host/tests/test_gw_tools.py` proves the pipeline already).

## Firmware shape

Arduino-Pico (earlephilhower core, on the Pico SDK) under PlatformIO, as in HDDSynth_Native and HDDSynthPro.

| Module | Job | Testable on the Mac? | Slice |
|---|---|---|---|
| `fdd_signals`, `pin_watch` | Pin sample decoding; turning samples into trace events | Yes | 0 |
| `trace_format`, `spsc_ring` | Trace events, their text form, the core 1 → core 0 ring | Yes | 0 |
| `short_test` | Solder-bridge test analysis | Yes | 0 |
| `head` | Cylinder position, step limits, TRK00, the track 0 stop, side, DSKCHG | Yes | 1 |
| `rotation` | Index timing at 300 RPM while selected and spinning; position within the revolution | Yes | 1 |
| `mfm`, `ibm_track` | MFM encoding with sync marks (A1/C2 with missing clocks); the IBM track layout (gaps, IAM, IDAM, DAM, CRC-16-CCITT); since 0.13 any IBM-style geometry, and FM | Yes, and against Greaseweazle's encoder | 2 |
| `gen_disk` | A disk whose sector content is computed from its position, as on HDDSynthPro | Yes, with a Python twin | 2 |
| `rdata` (PIO + C++ wrapper) | Streams the current track's flux pulses on RDATA, aligned to index | In a simulator: `pio_sim.py`, brought over from HDDSynthPro | 2 |
| `track_source`, `image_cache`, `cylinder_loader` | Images by size or `IMG.CFG` (`img_cfg`); the image in PSRAM; loading a cylinder on each step | Yes, with a fake SD | 3 |
| `activity` | Motor, steps, seeks, recalibrate bumps, reads, for the sounds | Yes, and on recorded traces | 4 |
| `wdata` (PIO), `mfm_decode` | Capturing WDATA transitions and decoding them back to sectors | Yes, and against `gw convert` | 5 |
| `synth`, `wav`, `audio` | The FDD Synth's mixer and the I2S amp | Yes (`audio` no) | 4 |
| `ui`, `screens`, `panel`, `settings` | The front panel and `/FDDSYNTH.CFG` | Yes (`panel` no) | later |
| `bus`, `amiga_track`, `welcome_disk` | Drive select and interfaces; AmigaDOS tracks; the welcome disk | Yes | later |
| `main` | Console and core wiring | No | all |

**Core split:**
- Core 1 runs the drive: it watches the inputs, moves the head, keeps the rotation going and feeds the RDATA state machine. It never touches the SD card or USB.
- Core 0 owns USB serial, the SD card and the trace output, and prepares the next track when the head moves.

## Thin slices

Each slice ends in a passing automated test before the next starts. Each one also lists what can be built and tested on the Mac **before** the hardware is wired, which is most of the work.

- **Slice 0: listen only.**
  - Every pin is an input; the SN7407's outputs stay released. Core 1 polls the inputs and records select, motor, side, density and write gate changes, and every step.
  - *Before hardware:* the pin watcher, trace format and coalescer (done, in `test/native`); the trace parser (done, `-m sim`).
  - **Exit:** clean, repeatable traces of `gw seek` and `gw rpm`, saved with `--save-reference`, and the pins each drive letter uses.
- **Slice 1: head, index and status lines.** *Logic and firmware done (0.2.0), tested against a virtual Greaseweazle; waiting for hardware.*
  - Answer on one select/motor pair. Track the cylinder (0–83), assert TRK00 on cylinder 0, ignore steps outwards past 0 (but record them: that's the head hitting its stop). Index pulses every 200 ms while selected with the motor on. WPT asserted (read-only for now). DSKCHG as a PC expects: asserted at power-on, cleared by a step with a disk present.
  - *Before hardware:* `head` and `rotation` host tests, including a virtual host that recalibrates the way the slice 0 trace shows.
  - **Exit:** `gw seek` to every cylinder and back, 20 times, with the trace agreeing on the position; `gw rpm` reports 300 RPM.
- **Slice 2: read a generated disk.** *Done in firmware (0.3.0) and in the simulator; waiting for hardware.*
  - Build each track's MFM stream from a generated 1.44 MB disk and stream it with PIO on RDATA, aligned to the index pulse.
  - Both heads are interleaved in one stream and chosen per cell by the PIO program, so a head switch needs nothing from the CPU. Rebuild the cylinder on a step.
  - *Before hardware:* the track encoder checked byte for byte against `gw convert` output and decoded by `gw convert`; the PIO program in `pio_sim.py`, decoded by a Python model of a floppy controller's data separator.
  - **Exit:** `gw read --format ibm.1440` gives the generated image byte for byte, ten times; random single-track reads too.
- **Slice 3: images on SD.** *Done in firmware (0.4.0) and in host tests; waiting for hardware.*
  - IMG/IMA files, geometry from size (720 KB and 1.44 MB to start), one cylinder cached, loaded on each step within the settle time. 720 KB brings the 250 kbit/s rate.
  - *Before hardware:* geometry and cache tests with a fake SD, including how long a load may take.
  - **Exit:** real images (a DOS boot disk, a full data disk) read byte for byte; seek-heavy reads at the host's fastest step rate.
- **Slice 4: activity events for the sounds.** *Done in firmware (0.5.0) and host tests; waiting for hardware. See [activity.md](activity.md).*
  - Motor spin-up and spin-down, single steps, seeks (with distance), recalibration and the track 0 stop, reading versus idle, disk insert and eject. Printed as `ACT` lines, like HDDSynthPro.
  - *Before hardware:* the classifier, on recorded slice 0–3 traces.
  - **Exit:** scripted `gw` sessions produce the expected event sequence every time.
- **Slice 5: write.** *Done in firmware (0.6.0) and host tests; waiting for hardware.*
  - WGATE and WDATA captured by PIO, decoded from MFM, written back to the image on SD when the head moves or the motor stops. WPT follows a per-image setting.
  - *Before hardware:* the decoder against flux from `gw convert`; the capture program in `pio_sim.py`.
  - **Exit:** `gw write` then `gw read`, 100 images, zero mismatches; the image survives a power cycle.
- **Slice 6: a real PC, disk swaps and soak.** *A virtual PC already reads and writes a real FAT12 disk end to end on the Mac, checked by mtools and fsck.fat (`host/tests/test_dos_disk.py`); the real PC waits for hardware.*
  - BIOS boot from the drive, DOS `DIR`, `COPY` and `FORMAT`. A button to change image, with DSKCHG set as a real disk change does.
  - **Exit:** an overnight soak of copies and compares with zero mismatches and zero hangs.

## Later layers

1. HDD Synth integration: I2S audio, samples driven by the activity events. *Done (0.7).*
2. Image selection: buttons or an encoder and an OLED, GoTek style. *Done (0.8): `src/panel.*` and the screens in `docs/oled`.*
3. 5.25-inch drives: 360 KB and 1.2 MB, 360 RPM for HD, double stepping. *Done (0.13).*
4. Other formats: Atari ST, Amiga ADF (a different sector layout and drive-ready behaviour). *Done: ST disks are PC-style MFM, served by size or `IMG.CFG` (0.13); the Amiga in 0.14.*
5. Flux images (HFE) for copy-protected disks. *Not started.*
6. A PCB. *Rev B done: the carrier board, with the SN7407 and termination. A rev C would route the SN7407's spare gate to floppy pin 2 (the Amiga's /CHNG), and could add Schmitt-trigger input buffering.*

## Risks and open questions

| Risk | Mitigation |
|---|---|
| The FeatherWing's outputs are level-shifted to 5 V, possibly push-pull: against 1 kΩ pull-ups to 3.3 V, each line feeds about 1.7 mA into the BB48's 3.3 V rail | 10 kΩ pull-ups with this host (0.17 mA a line), 1 kΩ only for a real PC. Measure the levels before connecting (bring-up step 3), and keep the power-order rule |
| The FeatherWing's pull-ups on our outputs are too weak for RDATA's 375 ns pulses to rise cleanly | Look at RDATA with the logic analyser in bring-up. If its edges are slow, add a 1 kΩ pull-up to 5 V at the FeatherWing end, or widen the pulse (`PULSE_CYCLES`) |
| Noise on STEP gives false steps | The slice 0 trace shows it: steps with no seek around them. A short deglitch on STEP (a few hundred ns) if needed |
| Wrong power order | The rule above. The SN7407 runs from the BB48's VBUS, so an unpowered BB48 never sinks the outputs |
| Erratum E9 on A2-stepping RP2350s | Never use internal pull-downs |
| RDATA pulse width and jitter the controller won't accept | Match the reference drive (measured with the logic analyser), and check `gw read` margins with its flux histogram |
| Core 0 falls behind a long write | Writes stream through a 16 KB ring (44 ms at 1.44 MB) and decode in 2 KB chunks. An overrun drops that burst rather than writing junk; watch `capture_overruns` |
| RAM: the drive build uses 58% statically (buffers for two drives and the largest track), plus the sample pack at run time (152 KB for the stand-in pack, up to 160 KB) | Watch for heap trouble at bring-up. The second drive's buffers could be allocated only when it's set up; packs with longer samples would stream from SD, as HDDSynth_Native does |
| Core 0 stalls longer than the 46 ms audio queue | Writes decode in chunks with audio between them; the longest remaining step is an SD read or a track encode (a few ms). `audio_underruns` in `s` |
| The card fails or is pulled out while writing | Sectors reach the live track only once the card has them, so a read-back shows the old data. After any lost write, the drive reports write-protect until the disk is inserted again, so the host's next write fails visibly (`write_faults` in `s`). A failed read gives a blank track: the host sees missing sectors |
| SD reads slower than the settle time | They are (3.5 ms a track at 25 MHz): since 0.10 each image is copied into PSRAM before it goes in, and tracks load from there |
| Switching RDATA between two drives mid-revolution (`rdata_pio::switchTo`) misbehaves | It restarts the DMA part-way into a buffer and relies on the channel reloading its full count afterwards; check with the logic analyser at bring-up |

**Answered from the Greaseweazle firmware source (`floppy.c`, 23 Oct 2026):**
- On the IBM PC bus, `--drive A` uses pins 14 (select) and 10 (motor); `--drive B` uses 12 and 16. Slice 0 confirms it on the cable.
- Steps: DIR is set 10 µs before, STEP is asserted for 15 µs, then the step delay (10 ms by default). The first seek checks TRK00 before each step out, up to 256 steps, then waits 15 ms to settle. It never steps out past a TRK00 it can see.
- It waits 750 ms after turning a motor on, and deselects everything after 10 s of no commands.
- `gw pin get` reads pins 8, 26 and 28 on every model, and other pins (such as 34) where the board supports them.

**Answered from the Adafruit_Floppy source (0.6.x, 23 Oct 2026), for the Feather host:**
- One select and one motor line: pins 12 and 16, which `gw` calls drive A. Drive B is refused.
- A step holds STEP asserted for 13 ms, then waits 13 ms; a seek of 80 cylinders takes about 2 s.
- Every seek to cylinder 0, and the first seek of all, looks for track 0: up to 100 steps out, checking TRK00 first, then up to 20 in. Seeks stop at 79.
- Motor on waits 1 s, then up to 1 s for an index pulse.
- `gw pin get` reads pins 26 and 28, not 34. Flux reads and writes are both supported on the Feather RP2040.

**Open questions:**
- How long is a real drive's index pulse, and how soon after a step does it update TRK00? The reference drive and the logic analyser answer these before slice 1.
- Does any of our hosts look at DSKCHG (pin 34)? The Feather can't read it, so DSKCHG waits for a Greaseweazle or the real-PC tests in slice 6.
- Does `gw rpm` work with the Feather's firmware?
- Does the BB48 in the rig have the A2 or A4 stepping?
