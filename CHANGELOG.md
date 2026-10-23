# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

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
