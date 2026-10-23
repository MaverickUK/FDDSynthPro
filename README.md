# FDD Synth Pro

An RP2350 that pretends to be a 3.5-inch floppy drive, GoTek style, as the base for a future FDD Synth that *is* the drive rather than one that listens to it. Being the drive, it knows exactly when the motor spins and how the head moves, so the sounds can follow every step.

At this stage it is a proof of concept on an Olimex RP2350-PICO2-BB48, tested with a Greaseweazle from the same Mac that builds it. See [docs/plan.md](docs/plan.md) for the slices and [docs/wiring.md](docs/wiring.md) for the connections. [docs/bringup.md](docs/bringup.md) is the checklist for hardware day.

It follows the approach of [HDDSynthPro](https://github.com/MaverickUK/HDDSynthPro), which does the same for IDE hard disks.

## Status

Slice 0 (listen only): the firmware records every drive select, motor, side, density and write-gate change and every head step it sees, without driving any pin, and streams the trace over USB serial.

The drive build (`pio run -e bb48_drive`, slices 1-2) is ready for when the hardware arrives:
- it tracks the head, and drives TRK00, a 300 RPM index, write protect and disk change;
- it streams a generated 1.44 MB disk on RDATA.

Its logic is tested on the Mac against a virtual Greaseweazle, and its PIO program in a simulator whose output Greaseweazle's own decoder reads back. It has not run on hardware yet.

## Quick start

```sh
test/run_native_tests.sh                 # C++ unit tests (no hardware)
cd host && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pytest                         # simulator and parser tests (no hardware)

pio run -t upload                        # build and flash the BB48
pio device monitor                       # h help, p pin levels, s stats

scripts/rig.sh -m slice0                 # everything, against the Greaseweazle
```

Read the power-order rule in [docs/wiring.md](docs/wiring.md) before connecting the Greaseweazle.

## Licence

To be decided (HDDSynth_Native is GPL-3.0).
