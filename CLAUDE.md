# CLAUDE.md: context for working on FDDSynthPro

FDDSynthPro will become an enhanced FDD Synth that emulates the floppy drive itself, GoTek style, instead of listening to one. It starts as a proof of concept: an Olimex RP2350-PICO2-BB48 (RP2350B) acting as a 3.5-inch PC floppy drive. The host is a Feather RP2040 + Adafruit Floppy FeatherWing running Adafruit's Greaseweazle-compatible firmware (a real Greaseweazle also works). The `gw` tools drive either. The plan, in thin slices, is in [docs/plan.md](docs/plan.md); the wiring is in [docs/wiring.md](docs/wiring.md).

It is the sister project of HDDSynthPro (`../HDDSynthPro`, the same idea for IDE) and follows its structure: Arduino-Pico under PlatformIO, core 0 for USB/SD, core 1 for the real-time work. Reuse its patterns (and `host/hddproto/pio_sim.py` when the first PIO program arrives) rather than inventing new ones. FDD Synth sound code merges in later; keep the audio out of this repo for now.

The owner is an experienced software architect (C#/.NET, TypeScript, Python) who is new to embedded C++. Explain embedded-specific decisions briefly, and favour clear, idiomatic code over clever code.

## Rules that keep the interface safe

- **Pins:** the pin map in `src/config.h`, `docs/wiring.md` and the `FDD`/`OTHER` tables in `docs/wiring.html` must agree, and `short_test.cpp`'s names with them. `test_no_pin_used_twice` checks every pin in `config.h`, planned ones included.
  - GPIO 0–3 stay off the interface: the BB48 pulls GPIO 2/3 up to 3.3 V (R17/R18).
  - GPIO 25 is the LED.
  - The ten inputs stay on consecutive pins (one read captures them).
- **Every floppy signal is active low.** In the code, "asserted" means low; the trace prints it as `ON`.
- **Never use internal pull-downs** (RP2350 erratum E9). Internal pull-ups are fine.
- **The drive terminates its inputs:** a socketed pull-up network to 3.3 V on the board, unlike IDE, where the host provides the pulls. Use 10 kΩ with the FeatherWing, whose outputs are level-shifted to 5 V, and 1 kΩ for a real PC.
- **Outputs only through the SN7407**, which is open collector: GPIO low asserts the line, GPIO high releases it. Never drive a floppy line directly from a GPIO.
- **Slice 0 never drives a pin:** every pin stays an SIO input, the outputs on their pull-ups, so the SN7407 stays released.
- **Core split:** core 1 runs the drive and never touches USB, SD or anything slow. Core 0 does the slow work (SD, USB serial) and must never make core 1 wait.

## Toolchain

- **Build:** `pio run` (PlatformIO lives in `~/.platformio-venv/bin/pio` on the dev Mac). The board is `olimex_pico2bb48`. There are two builds, selected by `FIRMWARE_SLICE`:
  - `bb48` (0): listen only;
  - `bb48_drive` (5): the drive, slices 1-5.

  `main.cpp` holds both. Rig tests skip unless the board runs the build they need, and `scripts/rig.sh` takes `PIO_ENV`.
  - The variant defines `PIN_LED` as a macro, so don't reuse Arduino pin-macro names for constants.
- **Host unit tests:** `test/run_native_tests.sh` (plain `c++` with ASan/UBSan). CI builds them with GCC on Linux, the Mac with Clang: anything the two may treat differently (undefined behaviour, ODR) shows up there first.
  - Each test file's own types and tests live in an anonymous namespace; only its `run...Tests()` is outside. Two test files once defined different `MemorySource` classes: an ODR violation that Clang's link happened to survive and GCC's didn't.
  - To run them with GCC locally: `docker run --rm -v "$PWD":/src:ro gcc:13 bash -c 'cp -r /src /tmp/w && cd /tmp/w && rm -rf .pio && test/run_native_tests.sh'` (ThreadSanitizer doesn't run in Docker). Keep `fdd_signals`, `pin_watch`, `trace_format`, `spsc_ring`, `short_test`, `head`, `rotation`, `drive`, `crc16`, `mfm`, `ibm_track`, `rdata_stream`, `gen_disk`, `track_source`, `cylinder_loader`, `activity`, `mfm_decode`, `track_write`, `write_burst`, `synth`, `wav`, `ui`, `screens`, `canvas`, `quadrature` and `button` free of Arduino and SDK headers so they stay testable here; new logic (image, activity, MFM decoding) follows the same rule. When touching the ring or anything shared between cores, also run `SANITIZE=thread test/run_native_tests.sh`.
- **CI:** `.github/workflows/build.yml` builds the firmware and runs the C++ tests and `pytest -m sim` on every push. A `vX.Y.Z` tag that matches `FIRMWARE_VERSION` publishes a release with the UF2.
- **Versioning:** SemVer in `src/version.h`. Bump it in the same commit as any firmware change, and add a `CHANGELOG.md` entry.
- **Python tests:** in `host/` (pytest). One marker per slice; `sim` needs no hardware.
  - Plain `pytest` runs only the `sim` tests.
  - `host/fddproto/greaseweazle.py` wraps the `gw` tool; every call goes into a transcript that failing tests save.
  - The Feather host offers only `--drive A` (pins 12/16), has no `gw pin get 34`, and must never get `gw update`. Rig tests skip what a host can't do rather than fail.
  - Greaseweazle's codec is the reference for MFM. `tests/test_track_encoder.py` uses it as a library (`greaseweazle.codec.codec.get_diskdef`), at the pinned version:
    - every track from `test/tools/build_track` must match its encoder cell for cell;
    - its decoder must read every track back to the original data.

    `ibm::HD_1440` and `ibm::DD_720` copy its `ibm.1440` and `ibm.720` definitions (gap3 108 and 84).
  - `run_native_tests.sh` also builds the tools in `test/tools/` that the Python tests run.
  - `host/fddproto/pio_sim.py` simulates the PIO instructions the programs use and runs the real `pioasm` output. Extend it when a program uses a new instruction, rather than letting it guess. Tests read a program's `.define public` values from `program.defines`.
  - The Greaseweazle tools aren't on PyPI; `requirements.txt` pins a tag from GitHub.
- **On the rig:** `scripts/rig.sh -m sliceN` runs the unit tests, builds, flashes and runs the rig tests.
  - Evidence goes to `host/runs/` (git-ignored); reference traces live in `host/reference/`.
  - `host/rig.env` (git-ignored, from `rig.env.example`) sets the host's drive letter (A for the Feather) and its port.
- **Host OS:** the dev machine is macOS.

## Drive model

Core 1 owns a `drive::Drive` (`src/drive.h`) and calls it on every pass: `onSample()` with the pins, then `outputs()` onto the SN7407. It never waits.
- Outputs are released while the drive is deselected; steps count only while selected.
- The head knocks against its outer stop on a step out at cylinder 0 (`head::Step::AtOuterStop`): that's a sound event, not an error.
- Time is `time_us_32()`, which wraps every 71 minutes. Take differences in unsigned arithmetic; `rotation::Spindle` keeps its anchor close to now.
- `test/native/test_drive.cpp` has virtual hosts that follow their firmware's source. Keep them faithful when adding host behaviour.
  - `VirtualGreaseweazle` (`floppy.c`): a 15 µs STEP pulse, a 10 ms step delay, TRK00 checked before each step out (up to 256), and a 15 ms settle.
  - `VirtualAdafruitFloppy` (Adafruit_Floppy, the Feather): 13 ms STEP pulses, a search for track 0 on every seek to 0 (100 out, then 20 in), seeks capped at 79, and a 1 s motor delay.

## Tracks

A track is an array of 16-bit cell words, one per encoded byte, first cell in the top bit: the order RDATA sends them. HD is 12,500 words (200,000 cells of 1 µs), DD 6,250 (cells of 2 µs). `ibm::buildTrack` fills one from the track's sector data in IMG order; `gen_disk::track` supplies that data for the generated disk.

## RDATA

`src/fdd_rdata.pio` streams the current cylinder: 16 PIO cycles per cell slot, a 6-cycle low pulse for a 1 cell. Each slot carries both heads' cells, and the program picks one by reading SIDE1 (the jmp pin). `rdata_pio` (SDK side) loops two DMA channels over the buffer. It restarts the stream at each index pulse, and holds RDATA released with a GPIO output override when the drive shouldn't be sending.
- Keep `rdata::CELL_CYCLES` and the program's `CELL_CYCLES` equal; a `static_assert` checks.
- `gpio_set_function` (inside `pio_gpio_init`) clears a pin's overrides, so set overrides after it.
- Core 0 rebuilds the buffer in place when the head moves, within the host's settle time.

## Loading cylinders

At power-on core 0 mounts the SD card and picks the disk (`sd_image::open()`, else `GeneratedSource`). Only then does core 1 start the streamer, since the data rate depends on the format. From then on core 0 calls `CylinderLoader::service()` on every pass: one bounded step (read a track, or encode one into the live buffer).
- `ready()` is atomic: -1 before the first write into the buffer, the cylinder after the last. Core 1 only lets RDATA out when it equals the head's cylinder.
- A head move mid-load restarts the load. Without that, a seek finishes one load later and misses the 15 ms settle time (`test_ready_within_settle_time` proves both).
- **PIO blocks:** SdFat's SDIO driver takes PIO 0 and sets its window to GPIO 16–47; `rdata_pio` claims PIO 2 or 1 with the GPIO 0–31 window. Keep new programs off PIO 0.

## Writes

The capture streams through a 16 KB ring (`cfg::CAPTURE_RING_BYTES`). An endless DMA fills it, and `fdd_wdata` runs only while WGATE is asserted and the drive is selected, has a disk and isn't write-protected.
- **Core 1** marks each burst's start (with its cylinder, head and track position, `start_byte` from the spindle's phase) and end on `g_capture_marks`. Every pass it publishes `wdata_pio::produced()`, the DMA's free-running position.
- **Core 0's `CaptureStream`** feeds each burst to a `BurstWriter` in chunks (`WRITE_CHUNK_BYTES`) as it arrives, so nothing waits for a whole burst and audio gets a turn between chunks.
- **If core 0 falls a whole ring behind,** that burst is abandoned and counted (`capture_overruns`), never decoded from overwritten bytes. A burst with no room for its marks is dropped (`bursts_dropped`).

`BurstWriter` (`begin`/`feed`/`finish`, or `apply` for a whole burst) runs the intervals through `mfm::DataSeparator`, `mfm::FieldScanner` and `track_write::interpret`. It then patches each sector into the loader's track (`CylinderLoader::writeSector` re-encodes that head in place; unchanged cells are rewritten with the same values, so RDATA stays valid) and writes it through to the `TrackSource`.
- `fdd_wdata.pio` doesn't count `EDGE_TICKS` (2) at each edge; the reader adds them back. Its 8-bit counts wrap after about 16 cells, which only the first interval can reach.
- The data separator uses Greaseweazle's gains (60% phase, 5% period). Lower gains take more jitter but follow speed errors worse; `test/native/test_write_decode.cpp` measures both.
- **In flux generators, round each edge's absolute time to a tick, not each interval:** per-interval rounding adds up to a random walk that looks like a decoder fault.
- **Card first, then the live track:** `BurstWriter::finish` writes each sector to the `TrackSource` and patches the loader only if that worked, so a read-back after a card failure shows the old data.
- **Lost writes** (`lostWrite`: a sector the card refused, or an overrun) set `g_write_fault`. Core 1 then asserts write-protect until the next insert (console `d`, or a swap), so the host's next write fails visibly. A floppy can't report a failed write any other way.
- `WDATA` and `RDATA` programs both live in PIO 2 or 1 (`pio_get_gpio_base == 0`), never PIO 0.

## Swapping images

The loader and `BurstWriter` work through a `DiskSlot`, never an image directly. A swap is a four-state handshake (`g_swap` in `main.cpp`):
1. Core 0 asks for an eject.
2. Core 1 ejects, once no capture is running.
3. After `SWAP_EJECTED_MS`, with no burst pending, core 0 opens the image, puts it in the slot and resets and reloads the loader.
4. Core 1 retunes `rdata_pio`/`wdata_pio` if the format changed, and inserts the disk.

## The virtual PC

`test/native/virtual_pc.h` drives a `Machine` (`test/native/machine.h`): the drive as both cores run it, in simulated time.
- Core 1's per-sample work (sound events, `WriteGate`) runs on every pin change and on every `tick`.
- Core 0's loader steps take `Core0Costs` (3 ms a read, 2.5 ms an encode until the board measures them).
- The PC reads by watching `Machine::rdataCells` (nothing while `Drive::streams` is false), and writes by feeding `captureInterval`.

`test/native/dos.h` adds just enough DOS (FAT12) on top, and `test/tools/dos_session` runs a session on a real image. `host/tests/test_dos_disk.py` builds that image with `mtools` and checks the result with `mtools` and `fsck.fat` (`brew install mtools dosfstools`; CI installs them). Those tools are the independent judge: don't relax a check to make it pass.

Keep its BIOS and controller timings faithful. It's the stand-in for slice 6's real PC, so new drive behaviour should get a session test here.

## Sound events

`src/activity.*` (rationale: `docs/activity.md`). Core 1's `DriveObserver` diffs the drive model after each sample (motor, disk, and the `StepEvent`) into `DriveEvent`s on `g_drive_events`. Core 0's `Tracker` turns them into `ACT` lines.
- A step against a stop is a `BUMP`, never a click, and ends a seek.
- A seek is two or more same-direction steps within `seek_gap_us` (20 ms). Its first step has already clicked, and `SEEK_END`'s count includes it.
- `host/fddproto/player.py` turns `ACT` lines into a WAV with a sample pack. It's the spec for the firmware's mixer (`src/synth.*`): `host/tests/test_synth.py` requires the two to match sample for sample (integer mix, saturating). Change both together, and `docs/activity.md` with them.
- **Audio runs on core 0** (`audio.h`): I2S on GPIO 26–28 (below 32, so its PIO shares the GPIO 0–31 window with RDATA/WDATA), begun on core 0 so its DMA interrupt never touches core 1. `audio::service()` runs between core 0's slower steps; the queue (`AUDIO_BUFFERS` x `AUDIO_BLOCK_FRAMES`) must outlast the longest one.
- Tests drive it through the virtual hosts (`test/native/virtual_hosts.h`); drain the tracker as core 0 does, or its 32-event queue drops the oldest.

## Front panel

`src/ui.*` is the model: inputs (`ui::Input`) in, the screen and requests (`takeSwap`, `takeEjectToggle`) out. `src/screens.*` draws each `ui::Screen` from a `ui::Status` snapshot onto a `Canvas`. `canvas`, `font5x7`, `quadrature` and `button` are copied unchanged from HDDSynth_Native; keep them in step with it rather than editing them here.
- **The font has no descenders:** avoid lower-case g, j, p, q, y in headings and key words.
- **Previews:** `FDDSYNTH_PREVIEW_DIR=docs/oled test/run_native_tests.sh && python3 test/pbm_to_png.py docs/oled` regenerates the reference PNGs. Add `FDDSYNTH_PREVIEW_ALL=1` for the extra frames, or run `scripts/build_panel_page.py out.html` for the interactive page. Look at the PNGs after changing a screen.
- **Not wired yet:** the SSD1306 driver, the encoder and button pins, and the volume in the mixer. HDDSynth_Native's I2C frame takes about 25 ms of core 0, so pace frames or use DMA.

## Trace format

Core 1 records events into an SPSC ring, and core 0 prints them (`src/trace_format.h`).
- `SIG <t_us> NAME=ON|OFF ...`: input lines that changed. WDATA is never traced (it toggles every few µs during a write).
- `STEP <t_us> IN|OUT x<count> <span_us>`: consecutive steps in one direction merge into one line unless 50 ms apart. IN is towards higher cylinders.
- `ACT <t_us> <SOUND> ...`: sound events (drive build), from `activity::format`.
- `DROP <n>` when the ring overflowed.

The Python parser is `host/fddproto/trace.py`; change both together.

Console:
- `h`: help, and the READY line with version and build;
- `p`: input pin levels (wiring check);
- `s`: stats; on the drive build also `cyl`, `side`, `selected`, `disk` and the knock counts;
- `d`: eject or insert the disk (drive build);
- `i`: the image being served (drive build);
- `[` / `]`: swap to the previous or next image on the card (drive build);
- `m`: print a `MARK` timestamp;
- `x` then `Y`: the solder-bridge test, floppy cable unplugged (`short_test.*`; the board reboots afterwards);
- `b`: reboot to the bootloader.
