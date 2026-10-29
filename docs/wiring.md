# Wiring: floppy header to Olimex RP2350-PICO2-BB48R

The pin map lives in [src/config.h](../src/config.h); this table must match it, and so must the `FDD` and `OTHER` tables in [wiring.html](wiring.html).

[wiring.html](wiring.html) shows the same connections as an interactive diagram, with a soldering checklist and a plan for the Perma-Proto board. Open it in a browser: GitHub shows HTML files as source. Its ticks are saved in that browser only. The parts are in [parts.md](parts.md).

The board is the **BB48R**: the BB48 with 8 MB of PSRAM and a microSD slot, so the SD card needs no wiring. Firmware before 0.9.0 used a map for the plain BB48 that clashes with the R's PSRAM and SD pins.

## Why these pins

- **GPIO 0–3 are not used.** The BB48R pulls GPIO 2 and 3 up to 3.3 V through 2.2 kΩ (R17/R18, for the UEXT and Qwiic I2C connectors). GPIO 0/1 stay free for a debug UART.
- **GPIO 8–11 and 24 belong to the board.** GPIO 8 is the PSRAM's chip select (the core sets it up at start-up), and GPIO 9, 10, 11 and 24 are the microSD slot's CS (DAT3), CLK, CMD and DAT0 (Olimex's schematic). They still come out on the headers: leave EXT1-12 to 15 and EXT2-4 unwired.
- **GPIO 25 is not used.** It drives the board's LED.
- **The ten inputs are on consecutive pins (GPIO 12–21)**, so one read captures them all. The outputs go around the board's pins: GPIO 4–7 below them, GPIO 22 (and 23, pin 2 after a rework) after the inputs.
- **Everything is within GPIO 0–31**, the range one PIO block can reach.

BB48R header pins: GPIO 0–23 are on EXT1 pins 4–27. EXT1 pin 1 is +3.3 V and pin 2 is GND. On EXT2, pin 1 is VBUS and pin 3 is GND.

## Signal conventions

- **Every floppy signal is active low.** "Asserted" or "ON" means the line is pulled low.
- **Inputs (host → drive)** come from the host's drivers. The drive end of the cable terminates them, so each one has a pull-up to **+3.3 V** on our side, from an **11-pin bussed SIP resistor network in a socket** (pin 1, the common, to +3.3 V; one resistor per input). Pulling up to 3.3 V, not 5 V, keeps 5 V off the RP2350's pins from our side.
  - **10 kΩ with the Feather + FeatherWing.** Its outputs are level-shifted to 5 V and may be push-pull. Against 1 kΩ each line would feed about 1.7 mA into the BB48R's 3.3 V rail; 10 kΩ cuts that to 0.17 mA.
  - **1 kΩ for a real PC**, whose open-collector outputs rely on the drive's termination. Swap the network when you move to a PC.
- **Outputs (drive → host)** go through an **SN7407** open-collector buffer. The host pulls them up. The firmware drives the buffer's input low to assert a line and high to release it. The SN7407 can sink 40 mA, enough for an old host's 150 Ω termination, which a GPIO can't.

## Connections

Odd floppy pins 1–33 are all ground.

| Floppy pin | Signal | Direction (drive's view) | GPIO | BB48R pin | Via |
|---|---|---|---|---|---|
| 2 | DENSEL | in | 12 | EXT1-16 | pull-up |
| 2 | CHNG (Shugart, Amiga: disk change) | out | 23 | EXT1-27 | SN7407 6A (13) → 6Y (12): **only after the rework below** |
| 8 | INDEX | out | 4 | EXT1-8 | SN7407 1A (1) → 1Y (2) |
| 10 | MOTEA (motor enable A) | in | 13 | EXT1-17 | pull-up |
| 12 | DRVSB (drive select B) | in | 14 | EXT1-18 | pull-up |
| 14 | DRVSA (drive select A) | in | 15 | EXT1-19 | pull-up |
| 16 | MOTEB (motor enable B) | in | 16 | EXT1-20 | pull-up |
| 18 | DIR | in | 17 | EXT1-21 | pull-up |
| 20 | STEP | in | 18 | EXT1-22 | pull-up |
| 22 | WDATA | in | 19 | EXT1-23 | pull-up |
| 24 | WGATE | in | 20 | EXT1-24 | pull-up |
| 26 | TRK00 | out | 5 | EXT1-9 | SN7407 2A (3) → 2Y (4) |
| 28 | WPT (write protect) | out | 6 | EXT1-10 | SN7407 3A (5) → 3Y (6) |
| 30 | RDATA | out | 7 | EXT1-11 | SN7407 4A (9) → 4Y (8) |
| 32 | SIDE1 | in | 21 | EXT1-25 | pull-up |
| 34 | DSKCHG (PC), READY (Shugart, Amiga) | out | 22 | EXT1-26 | SN7407 5A (11) → 5Y (10) |
| 1, 3, … 33 | GND | | | EXT1-2, EXT2-3 | |

**Not connected:** 4 and 6 (unused on 3.5-inch PC drives).

### The SN7407

| SN7407 pin | Connection |
|---|---|
| 14 (VCC) | BB48R VBUS (EXT2-1), with 100 nF to GND close to the chip |
| 7 (GND) | GND |
| 1, 3, 5, 9, 11 (1A–5A) | GPIO 4, 5, 6, 7 and 22, as in the table |
| 2, 4, 6, 8, 10 (1Y–5Y) | Floppy pins 8, 26, 28, 30, 34 |
| 13 (6A) | GND (unused gate); GPIO 23 after the rework |
| 12 (6Y) | not connected; floppy pin 2 after the rework |

### Pin 2 for Shugart and Amiga hosts (a rework)

A PC drives floppy pin 2 (density select), so the carrier only reads it. Shugart and Amiga hosts expect the drive to drive it, with DISK CHANGE (/CHNG): without it, an Amiga never notices a disk swap. The SN7407's sixth gate is spare for it. On the rev B carrier:

1. Lift U2 pin 13 (6A) from its ground pad, or cut its track, and wire it to GPIO 23 (BB48R EXT1-27).
2. Wire U2 pin 12 (6Y) to floppy pin 2 (J1-2).
3. Set `pin02=chg` in `/FDDSYNTH.CFG` ([config.md](config.md)), with `interface=shugart` or `interface=amiga`.

Pin 2 then works both ways: the SN7407 releases it unless the disk has changed, and the drive still reads it on GPIO 12. With `interface=ibmpc` the firmware never drives it, whatever `pin02` says. Without the rework, GPIO 23 goes nowhere, so the setting does nothing.
| 12 (6Y) | Not connected |

Its TTL inputs read the RP2350's 3.3 V as high. Running it from VBUS means it is only powered when the BB48R is.

### The SD card

The card goes in the BB48R's own microSD slot: nothing to wire. Only DAT0 and DAT3 of the slot are connected, so the firmware runs it in SPI mode on SPI1, at 37.5 MHz (`cfg::SD_SPI_MHZ`). The board has the card's pull-ups.

| Slot signal | SPI use | GPIO | BB48R pin |
|---|---|---|---|
| DAT3 | CS | 9 | EXT1-13 (leave unwired) |
| CLK | SCK | 10 | EXT1-14 (leave unwired) |
| CMD | MOSI | 11 | EXT1-15 (leave unwired) |
| DAT0 | MISO | 24 | EXT2-4 (leave unwired) |

GPIO 8 (EXT1-12), the PSRAM's chip select, is also the board's own.

### The I2S amp (the FDD Synth)

A MAX98357A breakout, as on HDDSynth_Native, with a small speaker. BCLK and LRCLK must be on consecutive pins.

| Amp pin | Signal | GPIO | BB48R pin |
|---|---|---|---|
| DIN | I2S data | 26 | EXT2-6 |
| BCLK | bit clock | 27 | EXT2-7 |
| LRC | word clock | 28 | EXT2-8 |
| Vin | +5 V | | VBUS (EXT2-1) |
| GND | GND | | EXT2-3 |

Leave SD_MODE and GAIN unconnected (the firmware sends the same sample on both channels).

### The front panel

The drive build drives these ([src/panel.h](../src/panel.h); [docs/oled](oled) has the screens). Without an OLED it logs `# oled: no display answering` and the controls still work.

| Part | Signal | GPIO | BB48R pin |
|---|---|---|---|
| SSD1306 OLED, 128 × 64 | SDA, SCL (I2C1) | 2, 3 | The BB48R's Qwiic/UEXT socket, by cable. The board already pulls them up |
| Rotary encoder | A | 32 | EXT2-12 |
| Rotary encoder | B | 33 | EXT2-13 |
| Rotary encoder | switch | 34 | EXT2-14 |
| Action button | | 35 | EXT2-15 |

The encoder's common pin and the other side of each switch go to GND; the firmware uses the RP2350's internal pull-ups, as HDDSynth_Native does. If clockwise turns the volume down, swap A and B, or set `ENCODER_REVERSED` in `src/config.h`.

## Without the SN7407: FeatherWing bring-up

Until the SN7407 arrives, the five outputs can go straight from the header to their GPIOs (floppy 8 to EXT1-8, 26 to EXT1-9, 28 to EXT1-10, 30 to EXT1-11, 34 to EXT1-26), with no pull-ups of ours. [wiring.html](wiring.html) draws this with **Without the SN7407** ticked.

- **Flash a direct build:** `bb48_direct` (listen) or `bb48_drive_direct` (the drive). Their outputs are open drain: a pin pulls its line low to assert it and lets go to release it, and the host's pull-up takes the line to 5 V, which the RP2350 tolerates while it's powered. The ordinary builds drive the pins high and would fight those pull-ups. The READY line ends in `direct`.
- **Measure the host's pull-ups first** (bring-up step 3): each output line to 5 V through 1 kΩ or more. That's 5 mA or less for the pin to sink; the direct builds set the pins to 12 mA drive.
- **The FeatherWing only.** Fit the SN7407 before a real PC: an old controller's 150 Ω termination needs 33 mA, and the SN7407 also stands between the RP2350 and the cable.
- **Power order matters even more:** with no SN7407, the host's 5 V reaches the RP2350's pins directly, and they're only tolerant while it is powered.

When the SN7407 goes in, the header-to-chip and chip-to-GPIO wires replace the direct ones, and the ordinary builds go back on.

## Notes

- **Grounds:** wire at least five of the odd floppy pins, spread along the connector, to the BB48R's ground. All seventeen is better.
- **Never connect a floppy power connector to the BB48R.** The BB48R runs from USB. Leave the FeatherWing's 5 V screw terminal unconnected too: it only powers a real drive.
- **Pull-ups only:** the firmware never enables internal pull-downs (RP2350 erratum E9). In slice 0 it enables the internal pull-ups on the five output pins (GPIO 4–7 and 22), which keeps the SN7407's outputs released.
- **Power order:** power the BB48R (USB-C) before plugging in the host's USB (the Feather), and unplug the host first. The RP2350's pins are only 5 V tolerant while it is powered.
- **Keep the wires short** (under 15 cm if possible), especially STEP and WDATA.

## Wiring check

The full order for hardware day, including the solder-bridge test (console `x`), is in [bringup.md](bringup.md).

With the host connected and reset (`python -m fddproto.gw_reset --device <port>` in `host/`; `gw reset` hangs on the Feather), the console's `p` command should show every input `OFF`:

```
PINS DENSEL=OFF MOTEA=OFF DRVSB=OFF DRVSA=OFF MOTEB=OFF DIR=OFF STEP=OFF WDATA=OFF WGATE=OFF SIDE1=OFF
```

DENSEL is the exception: it's a level, not a request, so a host may hold it either way. Any other input reading `ON` while idle is usually a wire on the wrong pin, or a missing pull-up.
