# Rev C: the integrated board

Rev B is a carrier for the Olimex BB48R, with the amp, OLED and encoder on flying leads. It works in a real PC, and the amp and OLED work. The Amiga doesn't notice disk changes, because rev B leaves floppy pin 2 (/CHNG) unconnected unless you do the rework.

Rev C puts everything on one board, as HDDSynthPro does: the RP2350B and its support parts, PSRAM, the microSD socket, the MAX98357A amp, power from the floppy power connector or USB-C, and pin 2 wired. It keeps rev B's outline, and a 3D-printed sled fits it in a 3.5-inch drive bay.

Status (8 Nov 2026): **the schematic is done (0 ERC violations, and `host/tests/test_revc_schematic.py` checks it against `config.h`); the board is placed and autorouted: DRC finds 0 unconnected items and 0 differences from the schematic. It needs a review by eye before ordering.** The files are in [hardware/revc/](../hardware/revc/README.md), the sled in [hardware/mount/](../hardware/mount/).

## What was decided

| Question | Decision | Why |
|---|---|---|
| Outline | Unchanged: the HDD Synth MK5's 100.33 × 69.91 mm, its four M3 holes, speaker holes and grille | It still fits the MK5's case, and the speaker can still screw onto the board |
| Placement in the bay | Lengthwise: the board runs front to back. The MK5's speaker end is the front, and J1's edge is the rear | The bay is 101.6 mm wide and at most 26.1 mm tall (SFF-8301). Lengthwise, the speaker can sit on top of the board (3 + 1.6 + 20 = 24.6 mm), and the 16 mm on each side takes the sled's rails and side screws. J1 stays on the edge it's on now |
| J1 and the power connector | J1 right-angle, on the rear edge between the rear M3 holes. The power header straight (TE 171825-4), in the corner beyond the lower hole, its plug from above | As on a real drive, J1's cable leaves straight backwards. J1's courtyard (51.9 mm) only just fits between the rear holes, so a right-angle power header has no room on that edge. The straight one fits past the hole, JLCPCB can fit it, and its cable still leaves backwards |
| Speaker connector | JST-PH 2-pin, fitted; the MK5's 3.5 mm screw terminal footprint beside it, not fitted | Pimoroni's mini speaker (4 Ω, 3 W, 40 × 40 × 20 mm, M2.5 holes 32.5 mm apart) has an 80 mm lead with a JST-PH plug. The terminal keeps the HDD Synth's style possible |
| microSD and USB-C | On the board's long edges, either side of the speaker: the card goes in from one side, USB-C from the other. The board sits 15 mm behind the bezel; the activity LED reaches the bezel through a light pipe | The front edge is all speaker and the MK5's front M3 holes, with no room for either socket. On the long edges they face the bay's side walls (16 mm away), so in the bay the card is reached through a microSD extender (its card end in a bezel slot), and USB-C with the sled out or a right-angle cable. Setting the board back keeps both openings clear of the bay's front side screws (28.5 mm from the front) |
| Front-panel display | Decided later. Rev C keeps the OLED, encoder and button headers; the first sled has no display openings | A 0.96-inch 128 × 64 module's PCB is 27.3 mm tall, too tall for a 25.4 mm bezel. A 0.91-inch 128 × 32 module would fit (with screens redrawn for 32 rows), and so would a 5.25-inch bezel |
| Amiga /CHNG | Copper, not a rework: GPIO 23 → SN7407 gate 6 → J1 pin 2, with 10 kΩ from GPIO 23 to 3.3 V | The board tested in the Amiga wasn't reworked. This is the cause documented in [wiring.md](wiring.md#pin-2-for-shugart-and-amiga-hosts-a-rework) |

## The circuit

### Same pins as the BB48R

The RP2350B gets the BB48R's pins, flash and PSRAM, so the `bb48_drive` firmware runs on rev C unchanged. A new PlatformIO environment only names the board. Every GPIO in [src/config.h](../src/config.h) keeps its job:

- GPIO 8: the PSRAM's chip select (XIP CS1), as on the BB48R. The PSRAM is an APS6404L (8 MB, SOP-8) on the QSPI bus beside the flash.
- GPIO 9, 10, 11, 24: the microSD socket in SPI mode (CS, SCK, MOSI, MISO), with the card's pull-ups. The firmware runs the card at 25 MHz in SPI mode and serves every disk from PSRAM, so 4-bit SDIO would gain nothing.
- GPIO 2, 3: I2C1 for the OLED, pulled up with 2.2 kΩ as on the BB48R (R17/R18).
- GPIO 25: the activity LED (D2), at the front edge for a light pipe in the bezel. J13, beside it, takes an external LED through its own 330 Ω (R46): a case's 2-pin HDD LED lead fits, pin 1 (+) the anode. The pin then lights two LEDs, about 7 mA; the RP2350's default 4 mA drive strength still lights both, a little dimmer, and the firmware can raise it to 8 mA.
- GPIO 0, 1: not connected. Rev B's debug UART header (J6) is left off: the firmware doesn't use it, and SWD covers debugging.
- GPIO 32–35: the encoder and button headers, as rev B's J3 and J4. These need the RP2350B (QFN-80); the MK5's RP2350A (QFN-60) doesn't have them.

**Checked:** the BB48R has 16 MB of flash and 8 MB of QSPI PSRAM (Olimex's specifications). Rev C has a W25Q128JV (16 MB) and an APS6404L (8 MB), so the board definition and the PSRAM set-up stay the same. Olimex's schematic names their exact parts; any 8 MB QSPI PSRAM works with the core's driver.

**New pins, planned:**
- GPIO 29: the amp's SD_MODE, with a 10 kΩ pull-up so the amp plays with the firmware unchanged. Later firmware can mute it when the drive is silent, to stop the hiss.
- GPIO 30: the microSD socket's card detect, pulled up.
- GPIO 36 and 42: floppy pins 4 and 6, read only. Pin 6 is the fourth drive select (DS3) on Shugart hosts; pin 4 means different things to different machines. Each has a 10 kΩ pull-up and a 1 kΩ series resistor, and no 1 kΩ termination, so a host that uses them some other way barely notices. The firmware ignores them until a host needs them.
- GPIO 37: the board ID, tied to GND through 4.7 kΩ (R62). The BB48R leaves GPIO 37 open, so with the internal pull-up on, it reads high on the BB48R and low on rev C: one firmware build can find out which board it's on, and only use GPIO 29 and 30 on rev C. 4.7 kΩ is under the 8.2 kΩ that erratum E9 asks for, so it reads low even without the pull-up.
- GPIO 38–41: J14, a 2 × 3 header (not fitted): 1 = 3V3, 2 = GND, 3–6 = GPIO 38, 39, 40 and 41. GPIO 40 and 41 are also ADC inputs. For whatever comes later, such as a display or buttons on the bezel; solder in a header or wires.

All of them go into `config.h` as planned pins in the firmware step, so `test_no_pin_used_twice` covers them.

### The microcontroller

HDDSynthPro's `mcu.kicad_sch` is a checked RP2350B design (Raspberry Pi's minimal design: the core regulator and its layout, a 12 MHz crystal, the W25Q flash, USB). Rev C reuses it and adds the PSRAM. HDDSynthPro's `check_netlist.py` checks the support circuit against Raspberry Pi's design; rev C's netlist test reuses those checks.

- **L1 (3.3 µH, C42411119) was out of stock** at JLCPCB on 27 Oct 2026. Raspberry Pi require this part. Check stock before ordering, and pre-order it or send your own if needed.

### Power

- **Inputs:** the floppy power connector (the 4-pin 2.5 mm "mini" Berg, as on a 3.5-inch drive: 1 = +5 V, 2 and 3 = GND, 4 = +12 V, which isn't used) and USB-C (5.1 kΩ CC resistors, USBLC6 ESD protection).
- **Combining them:** TI's LM66200, a dual ideal diode (about 40 mΩ; the higher input wins), not Schottky diodes. The SN7407 is TTL and needs 4.75 V, and a Schottky diode drops 0.4–0.5 V at the amp's current peaks, which would leave about 4.5 V. It also stops USB from feeding the PC's 5 V rail while the PC is off.
- **Protection** on the floppy 5 V input: a polyfuse, an SMAJ5.0A TVS (as on the MK5) and bulk capacitance.
- **3.3 V:** an LDO from the combined 5 V, as HDDSynthPro does.
- **The SN7407** runs from the combined 5 V.
- **The amp** runs from the combined 5 V. At 3 W into 4 Ω its peaks are close to 1 A, which the floppy supply covers easily.

This changes one rule from rev B: **floppy power now powers the board.** Rev B's warning "never connect a floppy power connector to the BB48R" applies to rev B only. Running from the host's own supply is better: the RP2350's pins are 5 V tolerant only while it's powered, and with floppy power it powers up with the host.

### The floppy interface

As rev B, with gate 6 wired:

- **Ten inputs**, each with a 10 kΩ pull-up to 3.3 V and a 1 kΩ to `PC_TERM`. J7's jumper joins `PC_TERM` to 3.3 V for a real PC.
  - Each then reaches its GPIO through a 1 kΩ series resistor (R47–R56). The RP2350's pins tolerate 5 V only while it's powered, so a host driving a line high while the FDD Synth is off would push current straight into the chip. The resistor holds that under 5 mA. The pull-ups stay on the cable side, so a host driving high can still feed a little current into the unpowered 3.3 V supply: about 0.2 mA a line with J7 off, 2 mA with it on. A PC's controller never drives its lines high, so this matters for push-pull hosts such as the FeatherWing.
  - Floppy pins 4 and 6 are read on GPIO 36 and 31 the same way (R57–R60), with only the 10 kΩ pull-up.
- **Six outputs** through the SN7407: GPIO 4, 5, 6, 7, 22 and 23 to floppy pins 8, 26, 28, 30, 34 and 2. Each GPIO has a 10 kΩ pull-up, so the lines stay released from power-on until the firmware drives them: the RP2350's pins start as inputs with a weak pull-down, which would assert them.
- **Pin 2** goes to GPIO 12 (DENSEL in) and to gate 6's output. The firmware only drives it with `pin02=chg` and never with `interface=ibmpc`.

### The amp

This is the MK5's amplifier sheet (`HDDSynth/2_prototype_mk5/hardware/amplifier.kicad_sch`):

- a MAX98357A (TQFN-16) on GPIO 26, 27 and 28;
- 100 nF and 10 µF on its supply;
- a ferrite bead and 1 nF on each output;
- the outputs going to the JST-PH and the unfitted KF350 terminal.

The outputs are bridge-tied, so neither speaker wire is ground.

**Gain:** GAIN_SLOT is left open, for 9 dB, as on the MK5. R61 (0402, not fitted, under R40) takes it to GND: a solder bridge across its pads gives 12 dB, a 100 kΩ gives 15 dB. Quieter needs no part: that's the firmware's volume.

## In the bay

All heights are from the bottom of the bay.

| Item | Height |
|---|---|
| The board's underside | 3.0 mm (clears the through-hole pins) |
| The board's top | 4.6 mm |
| The speaker's top, on the MK5 holes | 24.6 mm, against 25.4 nominal and 26.1 maximum |
| J1's right-angle header | 13.7 mm |

**The board's layout:**
- **The front edge** (x = 0) is the MK5's speaker end. The speaker's 40 mm square (x 0–41, y 20.3–60) has no parts and no copper on any layer, as on rev B.
- **Above the speaker:**
  - the microSD socket, its card in from the top edge;
  - the SWD header;
  - the activity LED at the front edge, and J13 for an external LED beside it.
- **Below the speaker:**
  - USB-C, its plug in from the bottom edge;
  - the amp and the speaker connectors, by the speaker's corner (J5 1.8 mm clear of its square, so the plug goes in beside the fitted speaker).
- **The rest:**
  - the RP2350B, turned so its floppy pins face J1;
  - the flash and PSRAM;
  - the SN7407;
  - the pull-ups in two columns beside J1;
  - J14, the spare GPIO header (not fitted), between the PSRAM and J7;
  - the power parts by the power header;
  - the panel headers and the buttons along the top edge;
  - the logo in silkscreen, between the speaker and the flash (`tools/make_logo.py` makes it from `media/FDDSynthDuo.png`, simplified to print: 11.8 × 8.7 mm, 1.5 mm clear of the speaker's square).
  - the header labels in the HDD Synth MK5's style: bold, knocked out of a filled box, in plain names ("Rotary encoder", "Boot sel", "Floppy power"). "+5V" stays by J8's pin 1, to show which way round the plug goes.
- **On the back:**
  - the MK5's credit, "Nostalgia by Peter Bridger", beside the speaker grille, where the MK5 has it on the same outline;
  - an easter egg: the "Don't copy that floppy" sign (`media/dont-copy-that-floppy.png`, 26 × 12.8 mm, also by `make_logo.py`), under the PSRAM and the power parts, the one patch there clear of through-hole pads.
  - a second: DOS's "Abort, Retry, Fail?", vertical, beside the floppy header's pins.

**The sled** ([hardware/mount/fddsynthpro_bay.scad](../hardware/mount/fddsynthpro_bay.scad)) is parametric OpenSCAD, printed in PETG:

- **The board** stands on four 3 mm bosses at its M3 holes, 15 mm behind the bezel. Nothing else touches its underside, so the through-hole pins stay clear.
- **Each side** has a low rail and a wall block around each of the bay's side holes (M3 heat-set inserts). The middle of each side stays open for the microSD card and the USB-C plug.
- **The bottom holes** sit in low blocks, under the card's path.
- **The bezel** (101.6 × 25.4 mm) has:
  - grille slots in front of the speaker;
  - a hole for the LED's light pipe;
  - a slot for a microSD extender's card end.

  The encoder (EC11, 7 mm bush) and a 12 mm button are options, off by default. EC11 encoders fit the bezel's height.
- **A low bar behind the board** joins the rails under the cables.
- **The hole positions** follow SFF-8301, and are all parameters:
  - side holes 6.35 mm up and 101.6 mm apart, the first 28.5 mm from the front;
  - bottom holes 95.25 mm apart across the drive, and 44.45 mm apart along it;
  - height 26.1 mm maximum, depth 147 mm maximum.

  Floppy drives don't always follow SFF-8301, so measure an old floppy drive or the case before printing.

It renders as one closed solid (OpenSCAD 2021.01), exported for the slicer as [fddsynthpro_bay.3mf](../hardware/mount/fddsynthpro_bay.3mf): print it as it loads, floor on the bed, no supports. It hasn't been printed yet; check it against a real bay, and the board with `show_board = true`, first.

## Parts (LCSC, checked 8 Nov 2026)

| Part | LCSC | Stock then | Note |
|---|---|---|---|
| RP2350B | C42415655 | 2,157 | |
| W25Q128JVSIQ, 16 MB flash | C97521 | **20** | Enough for a few boards; GigaDevice's GD25Q128E is the fallback |
| APS6404L-3SQR-SN, 8 MB PSRAM | C5333729 | 2,882 | |
| AOTA-B201610S3R3, 3.3 µH (Raspberry Pi's required inductor) | C42411119 | **0** | Pre-order, or send your own |
| SN7407DR | C40481 | 4,375 | |
| MAX98357AETE+T | C910544 | 31,208 | |
| LM66200DRLR | C3235556 | 21,535 | |
| XFCN BH254R-34P, 2 × 17 right-angle box header | C48603650 | 438 | |
| TE 171825-4, floppy power header | C210162 | 5,310 | |
| JST B2B-PH-K-S, speaker | C131337 | 60,580 | |
| KF350-3.5-2P, speaker terminal (not fitted) | C474892 | 12,225 | |
| 1 × 2 header, external LED (J13) | C124375 | | As J4 and J7 |
| 330 Ω 0402, external LED (R46) | C25104 | | Basic part |
| 1 kΩ 0402, series on the floppy inputs (R47–R56, R59, R60) | C11702 | | Basic part, as the termination |
| 10 kΩ 0402, pull-ups on pins 4 and 6 (R57, R58) | C25744 | | Basic part |
| 4.7 kΩ 0402, board ID (R62) | C25900 | | Basic part |
| Gain (R61), spare GPIO header (J14) | | | Not fitted: no part |
| SMAJ5.0A | C113952 | 448,980 | |
| 1.5 A polyfuse, 1206 (JK-nSMD150/12V) | C2830250 | 15,687 | Basic. The same part as the 6 V JK-nSMD150/6V (C2830249, extended), rated 12 V |

The passives are LCSC basic parts. Two of HDDSynthPro's were out of stock: its 100 nF 0603 (C14663) and 4.7 µF 0402 (C23733). Rev C uses the 0402 100 nF (C1525) throughout, and the basic 4.7 µF 0402 (C23733) again: check its stock before ordering, and if it's out, Samsung's CL05A475K (C368809, extended, $3.07 more) is the same capacitor at ±10%.

## Order of work

1. ~~**Check the parts.**~~ Done (above).
2. ~~**The schematic**~~, generated as HDDSynthPro's is (`hardware/revc/tools/gen_schematic.py`). 0 ERC violations; `host/tests/test_revc_schematic.py` checks its netlist against `config.h`.
3. ~~**Placement**~~ (`hardware/revc/tools/layout_pcb.py`). Four layers (signal, GND, 3.3 V, signal), as HDDSynthPro.
4. ~~**Routing**~~ with HDDSynthPro's Freerouting flow (`hardware/revc/tools/autoroute.py`; `hardware/revc/build.sh` runs it all).
   - **Result:** 0 unconnected items and 0 parity issues. DRC's 7 remaining warnings are J1's, J8's and J9's outlines overhanging the edge, as meant.
   - **Track widths:** power tracks are 0.4 mm, so they can enter the LM66200's and the amp's 0.5 mm-pitch pads; the speaker lines are 0.3 mm.
   - **Under the speaker:** copper is kept off the grille and the speaker's screws only, not the whole square, so USB and the SD lines can pass under it.
   - **USB:** D+ and D- leave the 27R resistors by fixed vias, through Raspberry Pi's QSPI fan-out.
   - **Rerouted in full (8 Nov 2026)** after the external LED header (J13, R46), the logo, J5's move clear of the speaker, and the series resistors and pins 4 and 6 (R47-R60), which crowded the floppy inputs too much to add around locked routing. Then again after the board ID strap (R62), the gain pad (R61) and the spare GPIO header (J14), whose tracks cross the board from the RP2350B's GPIO 37–41 side.
     - That route couldn't reach GPIO 31 in U1's crowded top corner, even with its neighbours taken up again, so floppy pin 6 moved to GPIO 42, beside pin 4's GPIO 36 (`add_parts.py` routed it alone).
     - Two hand edits after it: the long VBUS track along the bottom edge lifted 0.03 mm (Freerouting left it 0.01 mm inside the 0.3 mm edge clearance), and a dead branch of SWCLK off J11 removed.
     - A rerun of `build.sh` routes afresh, so look for both again after one.

   **Still to do: a review by eye** (`hardware/revc/revc-top.png`, `revc-bottom.png`), above all:
   - the RP2350B's regulator copper (turned 180° with the chip);
   - the USB pair's route (about 50 mm, on the bottom layer);
   - the 5 V paths' widths;
   - the outer ground pour, which the floppy signals cut into strips (the inner GND plane is whole).
5. **The sled:** ~~render it~~ (done, `fddsynthpro_bay.3mf`); check it against the board's STEP export, and print a test.
6. **The firmware:**
   - the planned GPIO 29, 30, 36, 37, 38–41 and 42 in `config.h`;
   - read GPIO 37 at power-on (pull-up on: low is rev C), and use the amp's mute and the card detect only on rev C;
   - a `revc` PlatformIO environment (or `bb48_drive` as it is: the pins, flash and PSRAM are the BB48R's);
   - these documents.

   [wiring.md](wiring.md), [hardware/README.md](../hardware/README.md) and the `FDD`/`OTHER` tables in [wiring.html](wiring.html) must agree with `config.h`.
