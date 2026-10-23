# Parts list

Everything for the prototype, grouped by what it's for. Prices and stock were checked on 23 Oct 2026 and change often: check each link before ordering. The pin-by-pin wiring is in [wiring.md](wiring.md) and the [interactive diagram](wiring.html).

Status: **buy**, **have** (you already own it), or **optional**.

## The drive

| Part | Qty | Status | Why | Where |
|---|---|---|---|---|
| Olimex RP2350-PICO2-BB48 (the plain BB48, **not** the BB48R) | 1 | buy | The emulated drive. The BB48R's PSRAM and SD slot use pins this design needs | [The Pi Hut](https://thepihut.com/products/rp2350-pico2-bb48) (about £11; low stock) |
| Adafruit Perma-Proto full-sized breadboard PCB | 1 | buy | Carries everything, laid out as a breadboard | [The Pi Hut](https://thepihut.com/products/adafruit-perma-proto-full-sized-breadboard-pcb-single) (about £11; low stock) |
| Female header strip, 2.54 mm, 1 × 40 | 2 | buy | Cut to two 1 × 27 sockets for the BB48, so it plugs in and can be swapped | Any supplier |
| 34-way IDC box header, 2 × 17, 2.54 mm, straight | 1 | buy | Where the floppy cable plugs in | [The Pi Hut](https://thepihut.com/products/2x17-34-pin-idc-box-header-0-1-2-54mm-pitch), [Rapid](https://rapidonline.com/truconnect-34-way-idc-straight-boxed-header-2-54mm-pitch-19-0038) (19-0038) |
| SN7407N, hex open-collector buffer, DIP-14 | 1 (+1 spare) | buy | The drive's five outputs: sinks 40 mA, enough for any host's pull-ups | [RS](https://uk.rs-online.com/web/p/line-interface-ics/0306336) (306-336) |
| DIP-14 IC socket | 1 | buy | So the SN7407 can be swapped | Any supplier |
| Bourns 4611X-101-103LF, 10 kΩ bussed SIP network, **11 pins** (10 resistors) | 1 | buy | Pull-ups on the ten floppy inputs, with the FeatherWing host (its outputs are 5 V) | Bourns 4600X series ([the 10-pin version at RS](https://ie.rs-online.com/web/p/resistor-arrays/5224807) shows the family; order the 11-pin) |
| Bourns 4611X-101-102LF, 1 kΩ bussed SIP network, 11 pins | 1 | buy | The same, for a real PC (it needs the stronger termination) | Same series |
| Female header, 1 × 11, 2.54 mm | 1 | buy | A socket for the SIP network, so 10 kΩ and 1 kΩ swap | Any supplier |
| 100 nF ceramic capacitor | 4 | buy | Decoupling: the SN7407, the SD breakout, the amp | Any supplier |

## Storage and sound

| Part | Qty | Status | Why | Where |
|---|---|---|---|---|
| Adafruit Micro SD SPI or SDIO card breakout, 3 V only (4682) | 1 | buy | Images and the sample pack, over 4-bit SDIO | [The Pi Hut](https://thepihut.com/products/adafruit-micro-sd-spi-or-sdio-card-breakout-board-3v-only) (about £3.40) |
| 10 kΩ resistor | 5 | buy | Pull-ups on SD CMD and DAT0–3, unless the breakout has them | Any supplier |
| microSD card, A1 or A2 rated, 8–32 GB | 1 | buy | FAT32, for the images and the pack | Any supplier |
| Adafruit MAX98357A I2S 3 W amp breakout (3006) | 1 | buy | The FDD Synth's sound, as on HDDSynth_Native | [The Pi Hut](https://thepihut.com/products/adafruit-i2s-3w-class-d-amplifier-breakout-max98357a) (about £5.70) |
| Speaker, 4 Ω, 3 W | 1 | buy | The Pi Hut's enclosed 3 W 4 Ω speaker suits the amp | The Pi Hut |

## The front panel (planned; the firmware comes later)

| Part | Qty | Status | Why | Where |
|---|---|---|---|---|
| SSD1306 OLED, 128 × 64, I2C, with a Qwiic/STEMMA QT socket | 1 | have | The screens in [docs/oled](oled). Plugs into the BB48's own Qwiic socket | As on the HDD Synth |
| Rotary encoder with push switch | 1 | have | Volume, and choosing disks | As on the HDD Synth |
| Push button | 1 | have | The action button: next screen, hold to eject | As on the HDD Synth |
| Qwiic/STEMMA QT cable | 1 | have or buy | OLED to the BB48 | Any supplier |

## The host and the test rig

| Part | Qty | Status | Why | Where |
|---|---|---|---|---|
| Adafruit Feather RP2040 | 1 | have | The host, running Adafruit's Greaseweazle-compatible firmware | — |
| Adafruit Floppy FeatherWing, 34-pin IDC | 1 | have | The host's floppy interface | — |
| 34-pin floppy ribbon cable, straight | 1 | buy | Host to drive. The Feather's drive A is pins 12/16, where this drive answers | Retro-computing or electronics suppliers |
| 34-pin floppy ribbon cable with the twist | 1 | buy | For a real PC (slice 6): the drive after the twist is A | Retro-computing suppliers |
| Spare Pico 2 | 1 | optional | A logic analyser with sigrok, for bring-up measurements | — |
| A real 3.5-inch PC floppy drive and a 5 V 2 A supply | 1 | optional | The known-good reference, and recordings for a real sample pack | Second hand |
| Greaseweazle V4.1 | 1 | optional | A second host with the real Greaseweazle firmware, both drive selects and full pin access | — |

## Consumables

Hook-up wire in several colours (the wiring diagram suggests one per signal group), solder, and a multimeter with a continuity beeper.

## Before ordering

- **The BB48 must be the plain one.** On the BB48R, GPIO 8 is the PSRAM chip-select and GPIO 9–11 go to its SD slot; here they carry MOTEB, DIR, STEP and WDATA.
- **The SN7407 runs at 5 V** (from the BB48's VBUS), the only 5 V part on the drive side. Its TTL inputs read the RP2350's 3.3 V as high.
- **The SIP networks are bussed, 11 pins**: pin 1 (marked with a dot) is the common end, to +3.3 V; pins 2–11 are the ten resistors, one per input. The 10-pin version has only nine.
