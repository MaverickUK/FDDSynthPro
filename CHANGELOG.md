# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- **Slice 2's track encoder, tested on the Mac** (not yet in the firmware):
  - `crc16`: CRC-16-CCITT as IBM formats use it;
  - `mfm`: MFM encoding, with the A1 and C2 sync words;
  - `ibm_track`: one track of a 1.44 MB or 720 KB disk, in Greaseweazle's `ibm.1440` and `ibm.720` layouts;
  - `gen_disk`: the generated test disk, with the same pattern and known answers as HDDSynthPro's, and a Python twin.
- `test/tools/build_track` writes every track of a disk, generated or from an IMG file.
- `host/tests/test_track_encoder.py` (`-m sim`) checks every track of four disks (generated and random, both formats) against Greaseweazle:
  - its encoder produces the same cells;
  - its decoder, the one `gw read` uses, reads ours back to the original data.

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
