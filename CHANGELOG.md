# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [SemVer](https://semver.org/).

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
