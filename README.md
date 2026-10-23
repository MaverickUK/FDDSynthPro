# FDD Synth Pro

<img src="media/FDDSynthPro.png" alt="FDD Synth Pro logo" width="300" align="left">

An RP2350 that pretends to be a 3.5-inch floppy drive, GoTek style, as the base for a future FDD Synth that *is* the drive rather than one that listens to it. Being the drive, it knows exactly when the motor spins and how the head moves, so the sounds can follow every step.

At this stage it is a proof of concept on an Olimex RP2350-PICO2-BB48, tested from the same Mac that builds it. The host is a Feather RP2040 with an Adafruit Floppy FeatherWing, running Adafruit's Greaseweazle-compatible firmware, so the Greaseweazle tools (`gw`) drive it. See [docs/plan.md](docs/plan.md) for the slices and [docs/wiring.md](docs/wiring.md) for the connections. [docs/bringup.md](docs/bringup.md) is the checklist for hardware day.

It follows the approach of [HDDSynthPro](https://github.com/MaverickUK/HDDSynthPro), which does the same for IDE hard disks.

## Status

Slice 0 (listen only): the firmware records every drive select, motor, side, density and write-gate change and every head step it sees, without driving any pin, and streams the trace over USB serial.

The drive build (`pio run -e bb48_drive`, slices 1-3) is ready for when the hardware arrives:
- it tracks the head, and drives TRK00, a 300 RPM index, write protect and disk change;
- it streams a 720 KB or 1.44 MB image from the SD card on RDATA, or a generated disk without one;
- it takes writes back into the image, from single sectors to whole tracks;
- it swaps between the images on the card, GoTek style, including between 720 KB and 1.44 MB;
- it reports what it does as sound events (motor, clicks, seeks, bumps against the end stops, disk changes), ready for the FDD Synth. See [docs/activity.md](docs/activity.md), and `samples/Placeholder/` for a synthesised stand-in sample pack.

Its logic is tested on the Mac against virtual hosts (the Feather's firmware and a Greaseweazle's), and its PIO program in a simulator whose output Greaseweazle's own decoder reads back. It has not run on hardware yet.

## Quick start

```sh
test/run_native_tests.sh                 # C++ unit tests (no hardware)
cd host && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pytest                         # simulator and parser tests (no hardware)

pio run -t upload                        # build and flash the BB48
pio device monitor                       # h help, p pin levels, s stats

scripts/rig.sh -m slice0                 # everything, against the real host
```

Read the power-order rule in [docs/wiring.md](docs/wiring.md) before connecting the host.

## Licence

To be decided (HDDSynth_Native is GPL-3.0).
