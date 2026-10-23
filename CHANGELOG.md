# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

## [Unreleased]

## [0.7.0] - 2026-10-23

### Added
- **The front panel works (drive build)**: an SSD1306 OLED on the BB48's Qwiic socket (I2C1, GPIO 2/3), a rotary encoder (GPIO 20-22) and an action button (GPIO 23), as on the HDD Synth (`src/panel.*`).
  - Turn for the volume. Press for the disk list, turn to choose, press to insert. The action button steps through home, head trace, sounds and stats; holding it for a second ejects the disk or inserts it again.
  - A frame goes to the I2C FIFO a few bytes per pass of core 0's loop, so it never holds up the loader or the audio.
  - Console `s` adds `oled`, `frames`, `draw_us_max`, `oled_aborts` and `volume`.
  - Host-tested: the controls as inputs (`controls.h`), the bus bytes (`ssd1306.h`), and the mixer's volume, which glides over 12 ms so it never clicks.
- **A virtual PC driving the whole drive** (`test/native/virtual_pc.h`, `test_virtual_pc.cpp`). It models a BIOS and an 82077-style controller:
  - 3 ms steps, a 15 ms settle and a 500 ms motor start;
  - recalibrating by checking TRK00 before each step;
  - finding sectors in the drive's own RDATA cells, and giving up after two index pulses;
  - sector writes with the controller's gap 2 timing;
  - three tries with recalibration, and the disk-change check.

  It runs against a `Machine` (`test/native/machine.h`): the drive model, loader, write path and sound events, with core 0 loading in the background at SDIO speed. A DOS-like session passes first time throughout, with no retries:
  - boot from cylinder 30;
  - read the boot sector, FAT and root directory;
  - read a file across the disk;
  - write a track and the FAT, and read them back from the card;
  - a disk swap.

  Further tests cover a head switch with no settle, a write-protected disk, a 720 KB disk, and a card too slow to ever answer, where the read fails rather than returning stale data. The session's sound events are saved to `.pio/native-tests/pc_session_act.txt`.

- **Swapping images, GoTek style (drive build).** The drive lists every 720 KB and 1.44 MB image on the card in name order. Console `]` and `[` swap to the next or previous image:
  1. the disk is ejected, setting DSKCHG;
  2. half a second later the new image is opened and the cylinder under the head loaded;
  3. both PIO programs are retuned if the data rate changed;
  4. the disk is inserted.

  `i` shows `index=2/3`. A button can drive the same swap later.
- `DiskSlot` (the disk in the drive, which the loader and write path go through), `ImageList`, `CylinderLoader::reset`, `rdata_pio::setFormat` and `wdata_pio::setRate`.
- **The virtual PC finds a disk's data rate** after a change, trying 500 and then 250 kbit/s as a BIOS does. Tests cover a swap from 1.44 MB to 720 KB and back, writes going to the disk that's in, and the image list's ordering and filtering. A rig test (`test_swaps.py`, `-m slice6`) swaps through every image on the card.

- **The FDD Synth plays (drive build).**
  - **The mixer** (`synth.h`, pure C++): sound events in, samples out, by the player's rules. It has 32 one-shot voices (the oldest is stolen past that), the spin loop and up to four seek loops, and saturates at 16 bits.
  - **The pack:** at power-on, core 0 loads the first folder under `/samples` on the card into a 160 KB pool in RAM.
  - **Output:** core 0 mixes into a MAX98357A over I2S (GPIO 26–28), topping up a 46 ms queue between its slower steps.
  - **Events:** reach the mixer as soon as core 0 takes them; the `ACT` lines follow when the serial link can take them.
  - **Console:** `s` adds `pack`, `audio_underruns` and `voices_stolen`.
  - **Golden test:** the mixer matches the Python player sample for sample, on the virtual PC's session, motor and seek edge cases, and random event sequences (`test/tools/render_events`, `host/tests/test_synth.py`).
  - `wav.h` is copied from HDDSynth_Native, so packs follow the same rules.
- **The sample player** (`python -m fddproto.player <trace> <out.wav>`) mixes a trace's sound events with a sample pack, by the rules the synth's firmware will follow. It works on the virtual PC's session or a real drive's log.

- **Streaming write capture.** WDATA goes into a 16 KB ring by endless DMA (RP2350), in place of an 80 KB buffer for a whole burst.
  - Core 1 marks each burst's start and end; core 0's `CaptureStream` decodes behind it in 2 KB chunks, with audio served between them.
  - Bursts queue rather than wait for a single buffer.
  - If core 0 ever falls a whole ring behind, that burst is dropped and counted (`capture_overruns`), never decoded from overwritten data.
  - `BurstWriter` takes a burst in pieces (`begin`, `feed`, `finish`).
  - Tests: a sector write across the ring's wrap, back-to-back bursts, a whole track through 16 KB, and an overrun that writes nothing. The virtual PC's sessions run through the streaming path.
- **The sample pool is one allocation sized to the pack** (up to 160 KB), loaded in two passes, instead of a fixed 160 KB array.
- Static RAM in the drive build falls from 85% to 41%.

- **The front panel, ready for an OLED, an encoder and an action button (screens and controls only; the drivers come with the hardware).**
  - `ui.h`: the controls, as on the HDD Synth. Turn for volume, or to choose on the disk list; press for the disk list, or to insert the chosen disk; action for the next screen (home, head trace, sounds, stats); hold action to eject or insert.
  - `screens.h`: the 128 × 64 screens. Home shows the image, a big track number, the head, the format, write-protect, the activity, a disk that spins with the motor, and a head marker across 80 cylinders. The others are the disk list, the disk change, volume, the head trace (the head's cylinder over the last few seconds, so seeks draw themselves), sounds and stats.
  - The canvas, font, encoder decoder and button handling are copied from HDDSynth_Native, so the two synths share a look and a feel.
  - Host tests cover the controls and render every screen. `docs/oled/` holds the PNGs. `scripts/build_panel_page.py` builds an interactive panel page from the firmware's own frames.
  - The mixer gains no volume yet: the UI model holds it for when the panel is wired.

- **A hardware pack.**
  - `docs/parts.md` lists every part, with links to UK suppliers where they stock it, marking which you have and which are optional.
  - `docs/wiring.html` is an interactive pin-to-pin diagram with a soldering checklist covering the floppy header, the SN7407, the pull-ups, the SD card, the amp and the planned front panel, plus a zone plan for the Perma-Proto board.
  - The front panel's pins are planned and reserved in `config.h`: the OLED on the BB48's own Qwiic socket (GPIO 2/3), the encoder on GPIO 20–22 and the action button on 23. A test checks no GPIO is used twice.
  - The input pull-ups are an 11-pin bussed SIP network: ten inputs need ten resistors, which the 10-pin network doesn't have.

- **Lost writes are no longer silent.** Each sector goes to the card first, and into the live track only if the card took it, so a host that verifies sees the failure. After any lost write (a card error, the card pulled out, or a capture overrun), the drive reports write-protect until the disk is inserted again (`d`, or a swap), so the host's next write fails visibly. The console prints `# WRITE FAULT`; `s` adds `write_faults`.
- **Tests:**
  - random intervals and random fields behind real sync marks, which never write a sector;
  - a property test that damaging any interval of a real write stores exactly the right sector or nothing (1,500 cases, never wrong data);
  - a virtual PC writing to a failing card, which gets its old data back, then write-protect, then works again after a re-insert.

- **A real DOS disk through the whole drive.**
  - `mtools` builds a genuine FAT12 disk with a volume label, a fragmented file and a subdirectory.
  - The virtual PC mounts it through the drive with a minimal DOS (`test/native/dos.h`): it reads the BIOS parameter block, FAT and root directory, reads every file by its cluster chain, and writes a new one (data, both FATs, the directory entry).
  - `fsck.fat` then finds the disk consistent, and `mtools` reads every file, old and new.
  - Covered: 1.44 MB, 720 KB (found by rate detection), and a 100 KB file across tracks and both heads (`host/tests/test_dos_disk.py`, `test/tools/dos_session`).
  - CI installs `mtools` and `dosfstools`.

### Fixed
- The test suite failed on CI (GCC): two test files defined different `MemorySource` classes, an ODR violation. One is gone, and each test file's helpers now live in an anonymous namespace.

### Changed
- The player mixes in integers and saturates, as the firmware does, instead of normalising the whole render.
- Core 1's RDATA gate and write-capture control moved out of `main.cpp` into pure code (`drive::Drive::streams`, `WriteGate` in `write_burst.h`), so the firmware and the virtual PC share them.
- Faster CI and local test runs:
  - the C++ tests build through `test/native.mk`, in parallel and only what changed;
  - the firmware build and the tests run as separate jobs side by side, and the tests job fetches only `pioasm` rather than all of PlatformIO;
  - the Greaseweazle comparison checks a spread of tracks by default, and all 160 on tags, on a manual run, or with `FDDPRO_FULL_TRACKS=1`.

## [0.6.0] - 2026-10-23

### Added
- **Slice 5, writing (`bb48_drive` build, now `FIRMWARE_SLICE` 5).** The drive takes writes into an image on the SD card.
  - **Capture:** `fdd_wdata.pio` times WDATA's flux transitions while WGATE is asserted: 16 ticks a cell, one byte an interval, DMA into an 80 KB buffer. Core 1 notes where on the track the write began.
  - **Decoding:** core 0 decodes each burst. `mfm_decode` is a data separator (a PLL with Greaseweazle's gains) and a field scanner. `track_write` places each data field: by position for a single-sector write, as a PC's controller makes it, and by its ID field for a whole-track write such as FORMAT or `gw write`.
  - **Storing:** written sectors go into the live track at once, so a verify read sees them, and through to the image on the card, synced.
  - **Write protection:** an image is writable unless the file is marked read-only; the generated disk stays write-protected.
  - **Console:** `s` adds `writable`, `written`, `write_bad_crc`, `write_unplaced`, `bursts_dropped` and `write_us_max`; `i` shows `writable`.
- **Host tests, all without hardware:**
  - The data separator tolerates about 0.2 cell of random jitter on every edge and a host clock about 10% off. Both were measured, not assumed.
  - Every track Greaseweazle's encoder makes decodes exactly (`test/tools/decode_write`).
  - The WDATA program, in the simulator, measures intervals to within a tick with no bias, and a sector write it captures decodes into the right sector.
  - The whole write path, end to end.
- The PIO simulator gains `IN` and autopush. Rig tests for slice 5 (`-m slice5`) write random tracks with `gw write`, read them back, and restore the originals.
- A synthesised stand-in sample pack in `samples/Placeholder/`, until a real drive is recorded. It has `spinup`, `spin` (loop), `spindown`, `step`, `seek` (loop), `bump`, `insert` and `eject`, as mono 16-bit 22.05 kHz WAVs.
- `scripts/make_placeholder_samples.py` regenerates the pack the same way every time; `--demo` writes a short session to listen to.

### Changed
- `rdata::clockDivider` takes the program's cycles per cell.

## [0.5.0] - 2026-10-23

### Added
- **Slice 4, sound events (`bb48_drive` build, now `FIRMWARE_SLICE` 4).**
  - **Core 1:** watches the drive model for motor, step (with its outcome and cylinder) and disk changes, and passes them to core 0 on a ring.
  - **Core 0:** turns them into `ACT` lines: `MOTOR_ON`, `MOTOR_OFF`, `STEP`, `SEEK_START`, `SEEK_END`, `BUMP`, `INSERT` and `EJECT`. Steps closer than 20 ms make a seek, and further apart they're separate clicks. A step against an end stop is a `BUMP`, not a click.
  - `s` adds `clicks`, `seeks`, `bumps` and `act_dropped`. See `docs/activity.md`, which also proposes an FDD sample pack.
- **Host tests:** the tracker and observer on their own, then fed through the drive model by the virtual hosts. A Greaseweazle's seek is one seek, the Feather's 26 ms steps are clicks, and a blind recalibrate grinds the stop.
- The trace parser reads `ACT` lines; rig tests for slice 4 (`-m slice4`) check that the sounds account for every step on the cable.

### Changed
- The virtual hosts moved to `test/native/virtual_hosts.h`, with an optional per-sample hook.

## [0.4.0] - 2026-10-23

### Added
- **Slice 3, images on the SD card (`bb48_drive` build, now `FIRMWARE_SLICE` 3).**
  - **The image:** at power-on the drive mounts the card over 4-bit SDIO (GPIO 32–37) and serves the first `.img`/`.ima` in the root, in name order, that is 720 KB or 1.44 MB. The format and data rate follow the file size. Without a card or an image, it serves the generated disk as before.
  - **Loading:** `cylinder_loader` reads both tracks of the cylinder under the head and encodes them into the RDATA buffer in four steps, one per pass of core 0's loop. If the head moves mid-load, it starts again for the new cylinder.
  - **RDATA:** stays released until the buffer holds the cylinder under the head. Stale sector IDs would look like a seek error to the host.
  - **Cylinders 80–83:** the image has no data there, so those cylinders get a blank track.
- Console `i` names the image; `s` adds `ready`, `loads`, `restarts`, `read_us_max` and `encode_us_max`.
- Host tests for image geometry and the loader. A timing simulation checks that a cylinder is ready within the host's 15 ms settle time after any seek at PC, Greaseweazle and Feather step rates, with SDIO-speed reads. It also shows that SPI-speed reads miss it.
- `python -m fddproto.make_images <folder>` writes test images for the card. Rig tests for slice 3 (`-m slice3`) compare the host's reads with them.

### Changed
- The rig's host is now a Feather RP2040 + Adafruit Floppy FeatherWing running Adafruit's Greaseweazle-compatible firmware. A Greaseweazle is optional.
  - `gw` defaults to drive A, the Feather's only select, which uses pins 12/16.
  - The slice 0 drive-letter test accepts a host that refuses one letter.
  - The input pull-ups become a socketed SIP network: 10 kΩ with the FeatherWing (its outputs are 5 V), 1 kΩ for a real PC.

### Added
- `VirtualAdafruitFloppy` in the C++ tests, following Adafruit_Floppy's seek, step and motor timing. The drive meets it as it meets the virtual Greaseweazle.

- The RDATA streamer claims PIO 2 or 1, never 0, which SdFat's SDIO driver takes for the GPIO 16–47 window.
- Slice 2's rig tests skip when an image is on the card.

## [0.3.0] - 2026-10-23

### Added
- **Slice 2, RDATA (`bb48_drive` build, now `FIRMWARE_SLICE` 2).** The drive streams the generated 1.44 MB disk:
  - `fdd_rdata.pio` sends one flux pulse per 1 cell: 375 ns low, in a 1 µs slot (HD).
  - Both heads of the cylinder are interleaved in one stream. The program reads SIDE1 on every cell, so a head switch takes effect on the next cell, in step with the rotation.
  - Two DMA channels loop over the 50 KB cylinder buffer. The stream restarts from the first cell at each index pulse.
  - RDATA is held released unless the drive is selected, has a disk and is at speed.
  - Core 0 rebuilds the buffer in place when the head moves; `s` shows the cylinder built and how long the build took.
- The track encoder, all pure C++:
  - `crc16`: CRC-16-CCITT as IBM formats use it;
  - `mfm`: MFM encoding, with the A1 and C2 sync words;
  - `ibm_track`: one track of a 1.44 MB or 720 KB disk, in Greaseweazle's `ibm.1440` and `ibm.720` layouts;
  - `rdata_stream`: the interleaved cylinder, and the PIO clock divider for a data rate;
  - `gen_disk`: the generated test disk, with the same pattern and known answers as HDDSynthPro's, and a Python twin.
- `test/tools/build_track` writes every track of a disk (generated or from an IMG file), or one cylinder as the streamer is fed it.
- Tests with no hardware (`-m sim`):
  - `test_track_encoder.py` checks every track of four disks (generated and random, both formats) against Greaseweazle: its encoder produces the same cells, and its decoder reads ours back to the original data.
  - `test_rdata_sim.py` runs the real PIO program in the simulator. It checks that every pulse lands on the right cell for either head, and that a head switch keeps the phase. It also checks that Greaseweazle decodes sectors from the pulse timings alone, and that a starved FIFO leaves RDATA released.
  - `pio_sim.py`, copied from HDDSynthPro, gains left shifts, autopull and the programs' `.define public` values.
- Rig tests for slice 2 (`-m slice2`): ten full reads, random single tracks, the cylinder build time against the settle time, and RPM.

## [0.2.0] - 2026-10-23

### Added
- **Slice 1, the drive (`bb48_drive` build).** Answers on drive select B / motor B (floppy pins 12/16, a PC drive's own), with a read-only disk in from power-on:
  - the head follows STEP and DIR while selected, between cylinder 0 and an inner stop at 83. A step outwards at cylinder 0 is a knock against the outer stop, counted for the sounds;
  - TRK00 on cylinder 0;
  - INDEX: a 2 ms pulse every 200 ms (300 RPM) while the motor is on and a disk is in;
  - WPT for a read-only disk or no disk;
  - DSKCHG from power-on and after an eject, cleared by a step with a disk in;
  - every output released while the drive is deselected.
- Console `d` ejects or inserts the disk; `s` adds the cylinder, side, selection, disk and knock counts.
- `head`, `rotation` and `drive`: the drive's logic as pure C++, with host tests. These include a virtual Greaseweazle that seeks and measures RPM the way its firmware does.
- Rig tests for slice 1 (`-m slice1`): seeking to every cylinder and back, full sweeps, the TRK00, WPT and DSKCHG lines, and `gw rpm`.

## [0.1.0] - 2026-10-22

### Added
- **Slice 0, listen only (`bb48` build).** Core 1 polls the ten floppy inputs and records:
  - every change of drive select, motor enable, side, density and write gate, as `SIG` lines;
  - every step, merged into one `STEP` line per run of steps in the same direction.
  - Every pin stays an input; the SN7407's outputs stay released.
- Console: `h` help, `p` input levels, `s` stats, `m` mark, `x`/`Y` the solder-bridge test, `b` bootloader.
- Host-side C++ tests for the pin layout, the pin watcher, the trace format and coalescer, the SPSC ring and the solder-bridge analysis.
- Host tools: the trace parser, a Greaseweazle wrapper, and rig tests for slice 0 (`-m slice0`).
- A `gw convert` round-trip test, the base for checking the MFM encoder in slice 2.
- Docs: the plan, the wiring and the bring-up checklist.
