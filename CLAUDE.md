# CLAUDE.md: context for working on FDDSynthPro

FDDSynthPro will become an enhanced FDD Synth that emulates the floppy drive itself, GoTek style, instead of listening to one. It starts as a proof of concept: an Olimex RP2350-PICO2-BB48 (RP2350B) acting as a 3.5-inch PC floppy drive, with a Greaseweazle V4.1 as the host. The plan, in thin slices, is in [docs/plan.md](docs/plan.md); the wiring is in [docs/wiring.md](docs/wiring.md).

It is the sister project of HDDSynthPro (`../HDDSynthPro`, the same idea for IDE) and follows its structure: Arduino-Pico under PlatformIO, core 0 for USB/SD, core 1 for the real-time work. Reuse its patterns (and `host/hddproto/pio_sim.py` when the first PIO program arrives) rather than inventing new ones. FDD Synth sound code merges in later; keep the audio out of this repo for now.

The owner is an experienced software architect (C#/.NET, TypeScript, Python) who is new to embedded C++. Explain embedded-specific decisions briefly, and favour clear, idiomatic code over clever code.

## Rules that keep the interface safe

- **Pins:** the pin map in `src/config.h` and `docs/wiring.md` must agree, and `short_test.cpp`'s names with both.
  - GPIO 0–3 stay off the interface: the BB48 pulls GPIO 2/3 up to 3.3 V (R17/R18).
  - GPIO 25 is the LED.
  - The ten inputs stay on consecutive pins (one read captures them).
- **Every floppy signal is active low.** In the code, "asserted" means low; the trace prints it as `ON`.
- **Never use internal pull-downs** (RP2350 erratum E9). Internal pull-ups are fine.
- **The drive terminates its inputs:** 1 kΩ pull-ups to 3.3 V on the board, unlike IDE, where the host provides the pulls.
- **Outputs only through the SN7407**, which is open collector: GPIO low asserts the line, GPIO high releases it. Never drive a floppy line directly from a GPIO.
- **Slice 0 never drives a pin:** every pin stays an SIO input, the outputs on their pull-ups, so the SN7407 stays released.
- **Core split:** core 1 runs the drive and never touches USB, SD or anything slow. Core 0 does the slow work (SD, USB serial) and must never make core 1 wait.

## Toolchain

- **Build:** `pio run` (PlatformIO lives in `~/.platformio-venv/bin/pio` on the dev Mac). The board is `olimex_pico2bb48`. There are two builds, selected by `FIRMWARE_SLICE`:
  - `bb48` (0): listen only;
  - `bb48_drive` (1): the drive.

  `main.cpp` holds both. Rig tests skip unless the board runs the build they need, and `scripts/rig.sh` takes `PIO_ENV`.
  - The variant defines `PIN_LED` as a macro, so don't reuse Arduino pin-macro names for constants.
- **Host unit tests:** `test/run_native_tests.sh` (plain `c++` with ASan/UBSan). Keep `fdd_signals`, `pin_watch`, `trace_format`, `spsc_ring`, `short_test`, `head`, `rotation`, `drive`, `crc16`, `mfm`, `ibm_track` and `gen_disk` free of Arduino and SDK headers so they stay testable here; new logic (image, activity, MFM decoding) follows the same rule. When touching the ring or anything shared between cores, also run `SANITIZE=thread test/run_native_tests.sh`.
- **CI:** `.github/workflows/build.yml` builds the firmware and runs the C++ tests and `pytest -m sim` on every push. A `vX.Y.Z` tag that matches `FIRMWARE_VERSION` publishes a release with the UF2.
- **Versioning:** SemVer in `src/version.h`. Bump it in the same commit as any firmware change, and add a `CHANGELOG.md` entry.
- **Python tests:** in `host/` (pytest). One marker per slice; `sim` needs no hardware.
  - Plain `pytest` runs only the `sim` tests.
  - `host/fddproto/greaseweazle.py` wraps the `gw` tool; every call goes into a transcript that failing tests save.
  - Greaseweazle's codec is the reference for MFM. `tests/test_track_encoder.py` uses it as a library (`greaseweazle.codec.codec.get_diskdef`), at the pinned version:
    - every track from `test/tools/build_track` must match its encoder cell for cell;
    - its decoder must read every track back to the original data.

    `ibm::HD_1440` and `ibm::DD_720` copy its `ibm.1440` and `ibm.720` definitions (gap3 108 and 84).
  - `run_native_tests.sh` also builds the tools in `test/tools/` that the Python tests run.
  - The Greaseweazle tools aren't on PyPI; `requirements.txt` pins a tag from GitHub.
- **On the rig:** `scripts/rig.sh -m sliceN` runs the unit tests, builds, flashes and runs the rig tests.
  - Evidence goes to `host/runs/` (git-ignored); reference traces live in `host/reference/`.
  - `host/rig.env` (git-ignored, from `rig.env.example`) sets the Greaseweazle's drive letter and ports.
- **Host OS:** the dev machine is macOS.

## Drive model

Core 1 owns a `drive::Drive` (`src/drive.h`) and calls it on every pass: `onSample()` with the pins, then `outputs()` onto the SN7407. It never waits.
- Outputs are released while the drive is deselected; steps count only while selected.
- The head knocks against its outer stop on a step out at cylinder 0 (`head::Step::AtOuterStop`): that's a sound event, not an error.
- Time is `time_us_32()`, which wraps every 71 minutes. Take differences in unsigned arithmetic; `rotation::Spindle` keeps its anchor close to now.
- `test/native/test_drive.cpp` has a virtual Greaseweazle that follows its firmware's `floppy.c`: a 15 µs STEP pulse, a 10 ms step delay, TRK00 checked before each step out (up to 256), and a 15 ms settle. Keep it faithful when adding host behaviour.

## Tracks

A track is an array of 16-bit cell words, one per encoded byte, first cell in the top bit: the order RDATA sends them. HD is 12,500 words (200,000 cells of 1 µs), DD 6,250 (cells of 2 µs). `ibm::buildTrack` fills one from the track's sector data in IMG order; `gen_disk::track` supplies that data for the generated disk.

## Trace format

Core 1 records events into an SPSC ring, and core 0 prints them (`src/trace_format.h`).
- `SIG <t_us> NAME=ON|OFF ...`: input lines that changed. WDATA is never traced (it toggles every few µs during a write).
- `STEP <t_us> IN|OUT x<count> <span_us>`: consecutive steps in one direction merge into one line unless 50 ms apart. IN is towards higher cylinders.
- `DROP <n>` when the ring overflowed.

The Python parser is `host/fddproto/trace.py`; change both together.

Console:
- `h`: help, and the READY line with version and build;
- `p`: input pin levels (wiring check);
- `s`: stats; on the drive build also `cyl`, `side`, `selected`, `disk` and the knock counts;
- `d`: eject or insert the disk (drive build);
- `m`: print a `MARK` timestamp;
- `x` then `Y`: the solder-bridge test, floppy cable unplugged (`short_test.*`; the board reboots afterwards);
- `b`: reboot to the bootloader.
