# Pico Floppy Drive Emulator: Prototype Plan

22 Oct 2026, Peter Bridger.

## Goal and scope

The prototype proves one thing: an RP2350B can act as a 3.5-inch PC floppy drive, GoTek style, that a real floppy controller reads reliably from a disk image. Because it *is* the drive, it knows exactly when the motor runs, where the head is and how it moves, which is what the FDD Synth needs to play convincing drive sounds. Writing, the sounds themselves and the final board come later, in thin layers on top of a working base.

**In scope**
- One drive on a 34-pin PC (Shugart-style) interface, answering one drive select.
- 3.5-inch MFM formats: 1.44 MB (HD, 500 kbit/s) first, then 720 KB (DD, 250 kbit/s).
- Read first: the host can seek, read every sector and see a steady 300 RPM index.
- Raw sector images (IMG/IMA) on an SD card, chosen by file size.
- Activity events for the sounds: motor, steps, seeks, the head hitting the track 0 stop.

**Out of scope for now**
- The audio itself (samples, I2S, mixing): HDD Synth code merges in later.
- 5.25-inch drives (360 RPM for HD, 40-track double stepping), 8-inch, FM.
- Flux images (HFE, SCP), copy-protected disks, non-IBM formats (Amiga ADF, Atari ST, Mac GCR).
- Image selection UI (buttons, OLED), bus buffers on a PCB, the final board.

**Done means:** a Greaseweazle reads a 1.44 MB image from the drive byte for byte, ten times in a row, and writes and reads back 100 images with zero mismatches; then a real PC boots DOS from it and copies files both ways.

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

- **A whole raw track fits in RAM easily:** 12.5 KB per side, 25 KB per cylinder. The 1.44 MB image itself does not (the BB48 has 520 KB of SRAM and no PSRAM), so the drive loads a cylinder from the SD card on each step.
- **Steps leave time for that.** Hosts wait for the head to settle (about 15 ms) after the last step before reading. A cylinder is 18 KB of sector data, a few milliseconds from SD even over SPI. If a read starts before the data is ready, the host simply retries on the next revolution.
- **The track is a loop.** The bit stream starts at the index pulse and wraps every 200 ms. The host doesn't care where in the loop it starts reading, only that the sector headers and CRCs are right and the timing is steady.

## Test rig

The Mac that builds the firmware also flashes it, drives a Greaseweazle as the floppy controller, and reads the drive's own log over USB serial.

| Part | Role |
|---|---|
| Olimex RP2350-PICO2-BB48 on a Perma-Proto board | The emulated drive |
| 34-pin IDC breakout + floppy ribbons (one straight, one PC cable with the twist) | Host connection, wired per [wiring.md](wiring.md) |
| SN7407N (DIP, hex open-collector buffer, 40 mA sink) | The drive's five outputs |
| 1 kΩ resistors (or a 10-pin SIP network) to 3.3 V | Termination pull-ups on the drive's ten inputs |
| Greaseweazle V4.1 | The host under test, driven by `gw` from the Mac |
| A real 3.5-inch PC floppy drive + 5 V supply | The known-good reference: proves the Greaseweazle setup, and gives real timings and flux to compare with |
| Adafruit SDIO microSD breakout + five 10 kΩ pull-ups | Image storage from slice 3 (as on HDDSynthPro) |
| Spare Pico 2 as a logic analyser (sigrok) | Traces when a test fails; floppy signals are slow enough for it |
| Later: a retro PC with a 1.44 MB controller | The real-host test (slice 6) |

**Pin map:**

| Lines | GPIO |
|---|---|
| Inputs: DENSEL, MOTEA, DRVSB, DRVSA, MOTEB, DIR, STEP, WDATA, WGATE, SIDE1 | 4–13 |
| Outputs (through the SN7407): INDEX, TRK00, WPT, RDATA, DSKCHG | 14–18 |
| SD card (slice 3) | 32–37, as on HDDSynthPro |

GPIO 0–3 and 25 are avoided for the same reasons as on HDDSynthPro (the BB48's I2C pull-ups and LED). The inputs are on consecutive pins so one read captures them all, and everything is inside one PIO block's GPIO 0–31 window.

**Drive select:** the firmware watches all four select and motor lines (floppy pins 10, 12, 14, 16). A real PC drive answers on pins 12 (select) and 16 (motor); the cable's twist routes the host's drive A lines there. Slice 0 records which pins `gw --drive A` and `--drive B` use, and slice 1 answers on the ones in `cfg`.

**Power-order rule:** the BB48 is powered before the Greaseweazle's USB is plugged in, and the Greaseweazle is unplugged before the BB48. There are no buffers on the inputs, and the RP2350's pins are only 5 V tolerant while it is powered.

## Automation loop

`scripts/rig.sh -m sliceN` runs the host unit tests, then builds, flashes and runs the rig tests. Every code change gets the full suite with no hands on the rig: the Greaseweazle needs no replugging, and `gw reset` puts it back in a known state between tests.

- **Flash:** `pio run -t upload` resets the board into the bootloader over serial and loads it with picotool. The runner then waits for the `READY` line.
- **Host actions:** `host/fddproto/greaseweazle.py` runs `gw` (`seek`, `rpm`, `read`, later `write`) and keeps a transcript.
- **Tests:** pytest, one marker per slice, so earlier slices re-run as regression tests. The `sim` marker covers the tests that need no hardware.

**Two views of every test.**
- **Host view:** what the Greaseweazle reports: the sectors it found, the image it read, the RPM it measured.
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
| `mfm`, `ibm_track` | MFM encoding with sync marks (A1/C2 with missing clocks); the IBM track layout (gaps, IAM, IDAM, DAM, CRC-16-CCITT) | Yes, and against `gw convert` | 2 |
| `gen_disk` | A disk whose sector content is computed from its position, as on HDDSynthPro | Yes, with a Python twin | 2 |
| `rdata` (PIO + C++ wrapper) | Streams the current track's flux pulses on RDATA, aligned to index | In a simulator: `pio_sim.py`, brought over from HDDSynthPro | 2 |
| `image`, `track_cache` | Geometry from file size; loading a cylinder from SD on each step | Yes, with a fake SD | 3 |
| `activity` | Motor, steps, seeks, recalibrate bumps, reads, for the sounds | Yes, and on recorded traces | 4 |
| `wdata` (PIO), `mfm_decode` | Capturing WDATA transitions and decoding them back to sectors | Yes, and against `gw convert` | 5 |
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
- **Slice 2: read a generated disk.** *Track encoder done and matching Greaseweazle cell for cell; the PIO streamer is next.*
  - Build each track's MFM stream from a generated 1.44 MB disk and stream it with PIO on RDATA, aligned to the index pulse. Rebuild on a step or side change.
  - *Before hardware:* the track encoder checked byte for byte against `gw convert` output and decoded by `gw convert`; the PIO program in `pio_sim.py`, decoded by a Python model of a floppy controller's data separator.
  - **Exit:** `gw read --format ibm.1440` gives the generated image byte for byte, ten times; random single-track reads too.
- **Slice 3: images on SD.**
  - IMG/IMA files, geometry from size (720 KB and 1.44 MB to start), one cylinder cached, loaded on each step within the settle time. 720 KB brings the 250 kbit/s rate.
  - *Before hardware:* geometry and cache tests with a fake SD, including how long a load may take.
  - **Exit:** real images (a DOS boot disk, a full data disk) read byte for byte; seek-heavy reads at the Greaseweazle's fastest step rate.
- **Slice 4: activity events for the sounds.**
  - Motor spin-up and spin-down, single steps, seeks (with distance), recalibration and the track 0 stop, reading versus idle, disk insert and eject. Printed as `ACT` lines, like HDDSynthPro.
  - *Before hardware:* the classifier, on recorded slice 0–3 traces.
  - **Exit:** scripted Greaseweazle sessions produce the expected event sequence every time.
- **Slice 5: write.**
  - WGATE and WDATA captured by PIO, decoded from MFM, written back to the image on SD when the head moves or the motor stops. WPT follows a per-image setting.
  - *Before hardware:* the decoder against flux from `gw convert`; the capture program in `pio_sim.py`.
  - **Exit:** `gw write` then `gw read`, 100 images, zero mismatches; the image survives a power cycle.
- **Slice 6: a real PC, disk swaps and soak.**
  - BIOS boot from the drive, DOS `DIR`, `COPY` and `FORMAT`. A button to change image, with DSKCHG set as a real disk change does.
  - **Exit:** an overnight soak of copies and compares with zero mismatches and zero hangs.

## Later layers

1. HDD Synth integration: I2S audio, samples driven by the activity events.
2. Image selection: buttons or an encoder and an OLED, GoTek style.
3. 5.25-inch drives: 360 KB and 1.2 MB, 360 RPM for HD, double stepping.
4. Other formats: Atari ST, Amiga ADF (a different sector layout and drive-ready behaviour).
5. Flux images (HFE) for copy-protected disks.
6. A PCB with proper input buffering (Schmitt triggers) and a 74LVC07A-class output buffer.

## Risks and open questions

| Risk | Mitigation |
|---|---|
| The Greaseweazle's outputs drive 5 V push-pull, not open collector | Measure them before connecting (bring-up step 3). If they do, add 1 kΩ series resistors on the inputs, or move the pull-ups to 5 V and keep the power-order rule |
| Noise on STEP gives false steps | The slice 0 trace shows it: steps with no seek around them. A short deglitch on STEP (a few hundred ns) if needed |
| Wrong power order | The rule above. The SN7407 runs from the BB48's VBUS, so an unpowered BB48 never sinks the outputs |
| Erratum E9 on A2-stepping RP2350s | Never use internal pull-downs |
| RDATA pulse width and jitter the controller won't accept | Match the reference drive (measured with the logic analyser), and check `gw read` margins with its flux histogram |
| SD reads slower than the settle time | Measure in slice 3; the host retries on the next revolution, and a second cylinder buffer can prefetch neighbours |

**Answered from the Greaseweazle firmware source (`floppy.c`, 23 Oct 2026):**
- On the IBM PC bus, `--drive A` uses pins 14 (select) and 10 (motor); `--drive B` uses 12 and 16. Slice 0 confirms it on the cable.
- Steps: DIR is set 10 µs before, STEP is asserted for 15 µs, then the step delay (10 ms by default). The first seek checks TRK00 before each step out, up to 256 steps, then waits 15 ms to settle. It never steps out past a TRK00 it can see.
- It waits 750 ms after turning a motor on, and deselects everything after 10 s of no commands.
- `gw pin get` reads pins 8, 26 and 28 on every model, and other pins (such as 34) where the board supports them.

**Open questions:**
- How long is a real drive's index pulse, and how soon after a step does it update TRK00? The reference drive and the logic analyser answer these before slice 1.
- Does the Greaseweazle look at DSKCHG (pin 34) at all? If not, DSKCHG waits for the real-PC tests in slice 6.
- Does the BB48 in the rig have the A2 or A4 stepping?
