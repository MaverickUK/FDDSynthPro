# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

## [Unreleased]

## [0.15.0] - 2026-10-29

### Added
- **The welcome disk,** as HDDSynthPro has: with no SD card, or no image on it, the drive serves a 1.44 MB DOS floppy (volume FDDSYNTHPRO) holding `README.TXT`, which explains how to set the drive up. Its text is [welcome/README.TXT](welcome/README.TXT) in the repo, embedded at build time by `scripts/embed_welcome.py`. Every sector is computed, so it costs no RAM; writes stay in PSRAM until a restart. A PC that boots it is told it isn't a system disk. Checked by mtools and fsck.fat, and read and written by the virtual PC.
- Console `g` swaps the generated test disk in, and back. Slice 2's rig test uses it, so it no longer needs the card taken out.

### Changed
- Without a card, the drive serves the welcome disk instead of the generated one. `i` prints `IMAGE welcome 1440` (or `IMAGE generated 1440`).

## [0.14.0] - 2026-10-29

### Added
- **The Amiga:** `interface=amiga` with `drive=df0` (pin 10), `df1` (pin 12) or both. The motor line is latched as the drive is selected, and pin 34 is /RDY, giving a standard drive's ID while the motor is off.
- **ADF images** (`.adf`, 880 KB): AmigaDOS tracks (`src/amiga_track.*`), matched cell for cell against Greaseweazle's `amiga.amigados`. The Amiga's whole-track writes, from anywhere round the track, are decoded (`amiga::SectorScanner`) and written back to the image. A virtual Amiga reads and writes tracks through the whole drive in the tests.
- **Pin 2 as an output** (`pin02=chg`, GPIO 23): DISK CHANGE for Shugart and Amiga hosts, through the SN7407's spare gate after a rework of the rev B carrier ([docs/wiring.md](docs/wiring.md)). Never driven with `interface=ibmpc`. The solder-bridge test now drives GPIO 23 too.

## [0.13.0] - 2026-10-29

### Added
- **Disk formats beyond 720 KB and 1.44 MB.** `ibm::Format` now describes any IBM-style MFM disk: cylinders (up to 255), one or two heads, double stepping, the image's file layout, 300 or 360 RPM, the data rate, and per-track layouts (sector count and size, first sector ID, head ID, interleave, skew, gaps, index mark). Gaps and rates left on auto are chosen by Greaseweazle's rules.
- **PC images by size:** 160, 180, 320 and 360 KB (40 cylinders), 720 and 800 KB, 1.2 MB (360 RPM), 1.44 MB and 1.68 MB (DMF), each matched cell for cell against Greaseweazle's `ibm.*` definitions. 2.88 MB isn't: its 1 Mbit/s track is twice what the streamer holds.
- **GoTek's IMG.CFG** (`src/img_cfg.*`): FlashFloppy's community geometry file, read from `/IMG.CFG` or `/FF/IMG.CFG`, matched by tag and size as FlashFloppy does. Images it describes (`.img`, `.ima`, `.dsk`) are listed and served. Tested on sections from FlashFloppy's own examples (Roland, Kaypro, GRiD, Bung, Ensoniq), whose tracks match Greaseweazle's cell for cell. A section that needs FM recording, mixed sector sizes on a track, `img_bps` or per-track rates is reported on the console, and the image goes by its size instead.
- The virtual PC runs DOS on 1.2 MB and 360 KB disks.

### Changed
- The image cache holds up to 2 MB a drive (Ensoniq's 1.6 MB, Bung's 1.76 MB); a bigger image's tracks are read from the card.
- Core 1 no longer reads core 0's format objects: core 0 publishes each disk's rate, track length, revolution and last cylinder, and core 1 sets the drive's speed and inner stop from them at each insert.
- The panel names any size: 360K, 1.2M, 1.44M, 1.6M.

## [0.12.0] - 2026-10-29

### Added
- **Two drives:** `drive=a,b` (or two Shugart units) in `/FDDSYNTH.CFG` makes the emulator A: and B: on one cable, each with its own disk (`image`, `image2`), head, motor, image cache in PSRAM and cylinder buffer. Core 1 runs a drive model for each and puts their outputs on the cable together; the RDATA streamer follows the selected drive, starting at the word under its head (`rdata_pio::switchTo`), and the capture runs at its rate. Write bursts carry their drive, and each drive's writer takes its own (`CaptureStream::setWriter`).
- The panel has a home screen per drive (`A: GAMES.IMG`); the disk list and the long press act on the drive last shown. The console's `1` and `2` choose which drive `i`, `[`, `]` and `d` act on; `s` adds the second drive's fields with a `2` suffix.
- The virtual PC changes drives as the BIOS does (`useDrive`), and `dos_session --copy` copies every file from A: to B:, checked by mtools and fsck.fat (`test_copy_from_a_to_b`).

### Changed
- An image the other drive has open opens read-only.
- The sound events treat two drives as one mechanism: the motor runs while either's does.

## [0.11.0] - 2026-10-29

### Added
- **Drive select, as a real drive's jumpers:** `interface`, `cable` and `drive` in `/FDDSYNTH.CFG` (`src/bus.*`, [docs/config.md](docs/config.md)). An IBM PC drive is A or B, past a twist or on a straight cable, and answers on 12/16 or 14/10 to match. A Shugart drive answers on DS0, DS1 or DS2 (pins 10, 12, 14) with MOTOR ON on pin 16, and puts READY on pin 34 instead of DISK CHANGE. The default is unchanged: a PC drive past the twist, on 12/16. `s` reports the setup in use.

## [0.10.0] - 2026-10-26

### Added
- **The disk is served from PSRAM.** Each image is copied into the BB48R's 8 MB PSRAM before it goes in (`ImageCache`, `src/image_cache.*`): at power-on, about 0.6 s, and during a swap's ejected pause, a track per pass so the panel and sound carry on. Track reads become memory copies, so a cylinder is ready about 7 ms after the head stops (it was 11), and a panel frame can take 8 ms before the settle time is at risk (it was 4). Writes go to the card first, then the cache, so a refused write never shows as written. Without PSRAM it reads from the card as before. The cache is read through PSRAM's uncached alias, so the copies don't evict core 1's code from the XIP cache it shares.
- **Builds for wiring the outputs straight to the header** while the SN7407 is on order: `bb48_direct` and `bb48_drive_direct`. The five outputs work as open drain (the host's pull-ups take them to 5 V); RDATA uses the same PIO program with its side-set driving the pin's direction. FeatherWing host only, never a real PC. The READY line ends in `direct`, and `docs/wiring.html` has a **Without the SN7407** switch.
- `s` reports `cached`, `card_read_us_max` and `fill_ms`.

### Changed
- **The SD card runs at 25 MHz**, the spec's default speed, instead of 37.5 MHz: the cache takes the card off the seek path.

### Fixed
- Restarting RDATA at each index pulse could send one stray flux pulse: the jump that restarts the program carried side-set 0 (asserted). It now carries side-set 1.

## [0.9.0] - 2026-10-26

### Changed
- **The pin map is now for the BB48R**, the BB48 with PSRAM and a microSD slot. The old map put MOTEB, DIR, STEP and WDATA on GPIO 8–11, which the R uses for its PSRAM select and SD slot. **Rewire before flashing:** inputs DENSEL–SIDE1 move to GPIO 12–21 (same order), the outputs to GPIO 4–7 (INDEX, TRK00, WPT, RDATA) and 22 (DSKCHG), and the encoder and action button to GPIO 32–35. The I2S amp and the OLED stay where they were. `docs/wiring.html` draws the new wiring.
- **The SD card is read through the BB48R's own slot**, in SPI mode on SPI1 (GPIO 9, 10, 11, 24), instead of a 4-bit SDIO breakout on GPIO 32–37. It runs at 37.5 MHz: about 2 ms a track, inside the 3 ms the settle-time tests allow. 25 MHz would take 3.5 ms, and a panel frame starting as the head steps could then make a cylinder late.
- The solder-bridge test drives only the fifteen floppy pins, never the board's PSRAM and SD pins, and watches GPIO 0–3 and 23 as neighbours.

## [0.8.1] - 2026-10-23

### Changed
- **The panel never starts a frame while a cylinder is loading** (`CylinderLoader::loading()`), so drawing can't eat into the host's settle time mid-load.

### Added
- **The panel's cost on core 0 is now tested, not argued.** The virtual PC's machine draws a frame every 100 ms (2 ms each, pessimistic; drifting against the disk's rotation, as on the board) between loader steps. Through a session of random seeks and single steps every cylinder is ready within 14 ms of the last step (settle: 15 ms), the frame rate holds, and core 0's longest step is a fraction of the audio queue. The worst case, a frame starting just as the head steps, is built on purpose: frames up to 4 ms still make every settle time, so the board's `draw_us_max` must stay under that.

## [0.8.0] - 2026-10-23

### Added
- **The drive remembers the disk, the sample pack and the volume across power-off**, GoTek style, in `/FDDSYNTH.CFG` on the card (`src/settings.*`): plain text, by name, so adding files doesn't change which disk comes back. A change is written 3 s after the last one, and only while the drive is idle. A missing or damaged file falls back to the defaults.
- **Choosing the sample pack on the panel:** on the Sounds screen, press the encoder for the card's packs (every folder under `/samples`), turn and press to load one (console `k`: the next). It loads when the drive is idle (`LOADING WHEN IDLE` until then); the old pack's memory is freed first, and a running motor carries on in the new pack's sound.
- **A sample pack from a real drive** (`samples/Recorded`, `scripts/make_recorded_pack.py`): spin-up, spin, spin-down, step, seek and bump, cut from a recording of a 3.5-inch drive driven by the FDD Synth's software. Each cut was found by measuring the recording (click onsets and spacing, the 199.8 ms revolution, spectra); the loops are whole revolutions and whole steps, joined smoothly. No insert or eject: the recording has none. 147 KB, inside the 160 KB pool. The showreel now uses it (`make_showreel.py --pack` picks another), and calls the project FDD Synth Pro.
- **A showreel** (`scripts/make_showreel.py`, `test/tools/showreel.cpp`): a 36-second video of a virtual PC using a real FAT12 disk while the front panel is worked. A swap from the panel, boot and `DIR`, reads with the volume turned down, the head trace across a long seek, the sounds and stats screens, a save with the volume back up, motor off and an eject. The OLED frames and the audio come from the firmware's own screens and mixer, in simulated real time.

### Fixed
- The swap screen named the new disk while the old one was coming out ("EJECTING GAMES.IMG" when DOS622 was ejecting). It names the disk going out, then the one going in.
- **A write could be decoded from bytes the DMA was overwriting.** The capture stream checked for an overrun once, then read; if core 0 was nearly a whole ring behind, the DMA could lap the bytes as they were read. It now gives up a quarter ring early, before reading any of it.
- **A swap to an image that won't open left a closed file in the drive.** Opening the new image closes the old one, so a later insert served stale or blank tracks. The old image is now reopened and put back; if that fails too, the drive stays empty and refuses an insert until a swap works.
- An eject or insert (console `d`, or holding the action button) during a swap could put the old disk back mid-swap, and a write then landed in the new image. It's now refused while a swap runs.
- A disk chosen on the panel during a console swap was dropped. It now waits and swaps when the first one finishes.
- Looking for the OLED could block core 0 for up to 50 ms after the drive had started; it now happens first.
- The solder-bridge test names the encoder, button and amp pins.

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
