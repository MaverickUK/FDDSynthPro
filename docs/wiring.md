# Wiring: floppy header to Olimex RP2350-PICO2-BB48

The pin map lives in [src/config.h](../src/config.h); this table must match it.

## Why these pins

- **GPIO 0–3 are not used.** The BB48 pulls GPIO 2 and 3 up to 3.3 V through 2.2 kΩ (R17/R18, for the UEXT and Qwiic I2C connectors). GPIO 0/1 stay free for a debug UART.
- **GPIO 25 is not used.** It drives the board's LED.
- **The ten inputs are on consecutive pins (GPIO 4–13)**, so one read captures them all. The five outputs follow on GPIO 14–18.
- **Everything is within GPIO 0–31**, the range one PIO block can reach.
- **These parts of the variant file can be ignored.** The core's variant file names GPIO 8 as PSRAM chip-select and GPIO 9–11 as SD lines. Those parts are only fitted on the BB48**R**; on the plain BB48 the pins are free.

BB48 header pins: GPIO 0–23 are on EXT1 pins 4–27. EXT1 pin 1 is +3.3 V and pin 2 is GND. On EXT2, pin 1 is VBUS and pin 3 is GND.

## Signal conventions

- **Every floppy signal is active low.** "Asserted" or "ON" means the line is pulled low.
- **Inputs (host → drive)** come from the host's drivers. The drive end of the cable terminates them, so each one has a pull-up to **+3.3 V** on our side, from a **10-pin SIP resistor network in a socket** (common pin to +3.3 V). Pulling up to 3.3 V, not 5 V, keeps 5 V off the RP2350's pins from our side.
  - **10 kΩ with the Feather + FeatherWing.** Its outputs are level-shifted to 5 V and may be push-pull. Against 1 kΩ each line would feed about 1.7 mA into the BB48's 3.3 V rail; 10 kΩ cuts that to 0.17 mA.
  - **1 kΩ for a real PC**, whose open-collector outputs rely on the drive's termination. Swap the network when you move to a PC.
- **Outputs (drive → host)** go through an **SN7407** open-collector buffer. The host pulls them up. The firmware drives the buffer's input low to assert a line and high to release it. The SN7407 can sink 40 mA, enough for an old host's 150 Ω termination, which a GPIO can't.

## Connections

Odd floppy pins 1–33 are all ground.

| Floppy pin | Signal | Direction (drive's view) | GPIO | BB48 pin | Via |
|---|---|---|---|---|---|
| 2 | DENSEL | in | 4 | EXT1-8 | pull-up |
| 8 | INDEX | out | 14 | EXT1-18 | SN7407 1A (1) → 1Y (2) |
| 10 | MOTEA (motor enable A) | in | 5 | EXT1-9 | pull-up |
| 12 | DRVSB (drive select B) | in | 6 | EXT1-10 | pull-up |
| 14 | DRVSA (drive select A) | in | 7 | EXT1-11 | pull-up |
| 16 | MOTEB (motor enable B) | in | 8 | EXT1-12 | pull-up |
| 18 | DIR | in | 9 | EXT1-13 | pull-up |
| 20 | STEP | in | 10 | EXT1-14 | pull-up |
| 22 | WDATA | in | 11 | EXT1-15 | pull-up |
| 24 | WGATE | in | 12 | EXT1-16 | pull-up |
| 26 | TRK00 | out | 15 | EXT1-19 | SN7407 2A (3) → 2Y (4) |
| 28 | WPT (write protect) | out | 16 | EXT1-20 | SN7407 3A (5) → 3Y (6) |
| 30 | RDATA | out | 17 | EXT1-21 | SN7407 4A (9) → 4Y (8) |
| 32 | SIDE1 | in | 13 | EXT1-17 | pull-up |
| 34 | DSKCHG | out | 18 | EXT1-22 | SN7407 5A (11) → 5Y (10) |
| 1, 3, … 33 | GND | | | EXT1-2, EXT2-3 | |

**Not connected:** 4 and 6 (unused on 3.5-inch PC drives).

### The SN7407

| SN7407 pin | Connection |
|---|---|
| 14 (VCC) | BB48 VBUS (EXT2-1), with 100 nF to GND close to the chip |
| 7 (GND) | GND |
| 1, 3, 5, 9, 11 (1A–5A) | GPIO 14–18, as in the table |
| 2, 4, 6, 8, 10 (1Y–5Y) | Floppy pins 8, 26, 28, 30, 34 |
| 13 (6A) | GND (unused gate) |
| 12 (6Y) | Not connected |

Its TTL inputs read the RP2350's 3.3 V as high. Running it from VBUS means it is only powered when the BB48 is.

### The SD card (slice 3)

An Adafruit SDIO microSD breakout, as on HDDSynthPro, on 4-bit SDIO. DAT0–3 must be on consecutive pins.

| Breakout pin | Signal | GPIO | BB48 pin |
|---|---|---|---|
| CLK | SDIO clock | 32 | EXT2-12 |
| CMD | SDIO command | 33 | EXT2-13 |
| DAT0 | data 0 | 34 | EXT2-14 |
| DAT1 | data 1 | 35 | EXT2-15 |
| DAT2 | data 2 | 36 | EXT2-16 |
| DAT3 | data 3 | 37 | EXT2-17 |
| 3V | +3.3 V | | EXT1-1 |
| GND | GND | | EXT2-3 |

Fit 10 kΩ pull-ups to +3.3 V on CMD and DAT0–3 (five resistors), unless the breakout already has them. Keep the six signal wires short and of similar length.

## Notes

- **Grounds:** wire at least five of the odd floppy pins, spread along the connector, to the BB48's ground. All seventeen is better.
- **Never connect a floppy power connector to the BB48.** The BB48 runs from USB. Leave the FeatherWing's 5 V screw terminal unconnected too: it only powers a real drive.
- **Pull-ups only:** the firmware never enables internal pull-downs (RP2350 erratum E9). In slice 0 it enables the internal pull-ups on GPIO 14–18, which keeps the SN7407's outputs released.
- **Power order:** power the BB48 (USB-C) before plugging in the host's USB (the Feather), and unplug the host first. The RP2350's pins are only 5 V tolerant while it is powered.
- **Keep the wires short** (under 15 cm if possible), especially STEP and WDATA.

## Wiring check

The full order for hardware day, including the solder-bridge test (console `x`), is in [bringup.md](bringup.md).

With the host connected and reset (`gw reset --device <port>`), the console's `p` command should show every input `OFF`:

```
PINS DENSEL=OFF MOTEA=OFF DRVSB=OFF DRVSA=OFF MOTEB=OFF DIR=OFF STEP=OFF WDATA=OFF WGATE=OFF SIDE1=OFF
```

DENSEL is the exception: it's a level, not a request, so a host may hold it either way. Any other input reading `ON` while idle is usually a wire on the wrong pin, or a missing pull-up.
