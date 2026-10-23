# Bring-up checklist

Hardware day, in order. Each step either passes or tells you what to fix before the next one. The wiring itself is in [wiring.md](wiring.md).

The parts are in [parts.md](parts.md). You'll need: a multimeter with a continuity beeper, the Mac, the Feather RP2040 with its Floppy FeatherWing, a plain **BB48** (not the BB48R), and ideally the reference floppy drive with a 5 V 2 A supply.

## 1. Before any power

- [ ] **Header orientation.** Check that the BB48's silkscreen matches the wiring table: USB-C at the top, GPIO 0–23 on the EXT1 row.
- [ ] **IDC helper strips.** Check with the meter that floppy pins 1 and 2 are **not** connected to each other; nor are any other neighbouring pins. Each header pin needs its own strip.
- [ ] **Continuity, end to end.** For each row of the wiring table, beep from the floppy header pin to its GPIO pin (inputs) or to the SN7407 output pin (outputs). Tick it off as you go.
- [ ] **Pull-ups.** The 10 kΩ network is in its socket, and each input pin reads about 10 kΩ to +3.3 V (EXT1-1).
- [ ] **Grounds.** Check that every odd floppy pin you wired beeps to BB48 GND, and that SN7407 pin 7 does too.
- [ ] **SN7407 power.** Pin 14 beeps to VBUS (EXT2-1), not to +3.3 V.
- [ ] **No power across.** Check that BB48 +3.3 V, VBUS and VSYS do **not** beep to any floppy pin.

## 2. First power: BB48 only, floppy cable unplugged

- [ ] Plug the BB48 into the Mac and flash the listen build: `pio run -t upload`.
- [ ] Open the console: `pio device monitor`. Type `h`; it should answer `READY FDDSynthPro 0.1.0 slice0`.
- [ ] **Solder-bridge test.** Type `x`, then `Y`. With the cable unplugged, every pin should read high on its pull-up, and pulling each one low should move no other pin.
  - `SHORTTEST OK 15 pins`: no solder bridges.
  - `SHORT GPIO9 (DIR) GPIO10 (STEP)`: those two are bridged. Fix it, then run the test again.
  - `STUCK GPIO15 (TRK00) low`: that pin is shorted to ground.
  - The board reboots when the test finishes.
- [ ] **SN7407 outputs released.** With the meter, the SN7407 outputs (pins 2, 4, 6, 8, 10) should read open: no continuity to GND while the board is running.

## 3. Set up the host on its own

- [ ] Install the tools: `cd host && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`.
- [ ] Flash the Feather RP2040 with Adafruit's Greaseweazle-compatible UF2 (from the FeatherWing guide, or built from Adafruit_Floppy's `examples/greaseweazle`): hold BOOT, click RESET, and copy the UF2 to the RPI-RP2 drive.
- [ ] Set the FeatherWing's write switch to **NoWr** until slice 5.
- [ ] Find its port (`ls /dev/cu.usbmodem*`), put it in `host/rig.env` as `FDDPRO_GW_DEVICE`, and check `.venv/bin/gw info --device <port>` answers. **Never run `gw update` on it**: that's for real Greaseweazles.
- [ ] **With the reference drive (recommended).** Connect the real drive and its own 5 V supply (not the FeatherWing's screw terminal). Insert a 1.44 MB disk and run `gw read --device <port> --drive A --format ibm.1440 ref.img`.
  - If this fails, the problem is the host setup or the cable, not the emulator.
  - Try `gw rpm` too: whether it works with this firmware is an open question.
- [ ] **Measure its idle output levels.** With nothing else on the cable, measure floppy pins 10–24 (even) and 32 to GND. Expect 5 V (the wing's level shifters), and note whether a line still reads 5 V through a 10 kΩ resistor to GND, which would mean push-pull. Either is fine with the 10 kΩ network; 1 kΩ only goes in for a real PC.
- [ ] Measure pins 8, 26, 28, 30 and 34 too: these are its inputs and should sit high on its own pull-ups.

## 4. Connect the host (power order)

- [ ] The BB48 is powered **first**: it stays on USB from now on.
- [ ] Plug the floppy cable into both ends (straight cable), then plug the Feather's USB into the Mac.
- [ ] Run `gw reset --device <port>`, then type `p` on the drive's console. Every input should be `OFF` (DENSEL may be either).
- [ ] From now on: **unplug the Feather before the BB48**.

## 5. Slice 0: listen to the host

- [ ] Check `host/rig.env` (from `host/rig.env.example`): `FDDPRO_GW_DEVICE` set, `FDDPRO_GW_DRIVE=A`.
- [ ] Run `scripts/rig.sh -m slice0 --save-reference`. It runs `gw seek`, `gw rpm` and both drive letters, and saves each trace and its summary to `host/reference/`. The Feather refuses drive B; the test notes that and carries on.
- [ ] Read the summaries. Things to look for:
  - **Drive A asserts pins 12 and 16** (DRVSB and MOTEB in the trace), as the source says.
  - **The search for track 0:** up to 100 steps out, then 20 in. Compare with `VirtualAdafruitFloppy` in `test/native/test_drive.cpp`, and fix the model if they differ.
  - **The step interval**, about 26 ms.
  - **No `DROP` lines**, and no lone steps outside a seek (noise on STEP).
- [ ] Commit the reference traces.

## 6. Reference measurements for slice 1 (logic analyser)

With the reference drive on the host and the logic analyser on the cable:
- [ ] The index pulse: its width, and its period at 300 RPM.
- [ ] TRK00: how soon after a step to cylinder 0 it asserts.
- [ ] RDATA: the pulse width of one flux transition.
- [ ] DSKCHG: its level after power-on, and what clears it.

Note the numbers in `docs/plan.md`, under the open questions.

## 7. Slice 1: the drive build

- [ ] Set `index_pulse_us` in `rotation::Config` to the width measured in step 6, if it differs from 2 ms.
- [ ] Run `PIO_ENV=bb48_drive scripts/rig.sh -m slice1`. The checks:
  - seeks to every cylinder and back, each agreeing with the drive's `cyl`;
  - 20 full sweeps;
  - TRK00 and WPT through `gw pin get`;
  - `gw rpm` within 0.1 of 300;
  - no index without a disk;
  - DSKCHG, if the host can read pin 34 (the Feather can't; the test skips).
- [ ] If your host reaches the drive on other pins, change `select_pin` and `motor_pin` in `drive::Config` instead.

## 8. Slice 2: reading

- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice2`. The checks:
  - ten full `gw read`s of the generated disk, byte for byte;
  - 20 random single tracks;
  - every cylinder build within the 15 ms settle time;
  - 300 RPM still.
- [ ] Measure on the board, now that it exists: the `build_us` in `s` after a seek, and the RDATA pulse width with the logic analyser (375 ns expected at HD).
- [ ] If reads fail but slice 1 passed, capture flux with `gw read --raw flux.scp` and look at its histogram (`gw convert` or HxC). Peaks should be clean at 2, 3 and 4 µs.

## 9. Slice 3: images on the SD card

- [ ] Wire the SD breakout ([wiring.md](wiring.md)), with the BB48 unplugged.
- [ ] Make the test images: `cd host && .venv/bin/python -m fddproto.make_images ~/fdd-images`. Set `FDDPRO_IMAGES=~/fdd-images` in `host/rig.env`.
- [ ] Format a card FAT32 and copy **one** image to its root (the drive serves the first `.img` in name order).
  - Copy it onto a freshly formatted card, so its clusters are contiguous.
  - Try `GEN1440.IMG` first, then `RAND1440.IMG`, then `RAND720.IMG` (720 KB, the 250 kbit/s rate).
- [ ] Power the BB48 and type `i`: `IMAGE GEN1440.IMG 1440 contiguous=1`.
- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice3`. The checks:
  - three full reads byte for byte;
  - 20 random tracks;
  - the slowest read and encode, which must fit the settle time.

  The test prints the measured times. Compare them with the assumptions in `test/native/test_loader.cpp` (3 ms a read, 2.5 ms an encode), and update the assumptions if they're off.
- [ ] Repeat with each image, power-cycling between them.

## 10. Slice 4: sound events

- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice4`. The checks:
  - `gw rpm` gives `MOTOR_ON` then `MOTOR_OFF`;
  - a full seek's sounds account for all 79 steps, with no bumps;
  - one step is one `STEP`;
  - `d` twice gives `EJECT` then `INSERT`.
- [ ] Watch the `ACT` lines in `pio device monitor` while running `gw read`. With the Feather host, expect separate `STEP`s, since it steps every 26 ms.

## 11. Slice 5: writing

- [ ] Set the FeatherWing's write switch to **WR**.
- [ ] Copy a fresh test image to the card (not marked read-only). Type `i`: `writable=1`.
- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice5`. It writes random tracks with `gw write`, reads each back, checks one survives a seek away and back, and puts the originals back. `FDDPRO_WRITE_CYCLES=100` gives the plan's 100 cycles.
- [ ] Check `s`: `write_bad_crc=0`, `bursts_dropped=0`, `capture_overruns=0`, and `write_us_max` (the longest chunk of decoding, which should stay well under the 46 ms audio queue).
- [ ] **A card fault:** with a test image in, start a long `gw write`, and pull the card part way. Expect `# WRITE FAULT` on the console, `write_faults` counting, and the next `gw write` failing with write-protect. Put the card back, type `d` twice (eject, insert), and writes work again.
- [ ] **Power cycle:** write a track, unplug the BB48 (Feather first), plug it back and read the track: the card kept it.
- [ ] Afterwards, copy the image back to the Mac and compare it with the local copy: identical, since the tests restored every track.

## 12. Swapping images

- [ ] Copy all three test images to the card (`GEN1440.IMG`, `RAND1440.IMG`, `RAND720.IMG`).
- [ ] Type `]` and `i` a few times: the index moves round the list and back, and each swap prints `EJECT` and then `INSERT`.
- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice6 -k swap`. Each image reads as its local copy, including the 720 KB one at 250 kbit/s.
- [ ] **No eject mid-swap:** type `]` and at once `d` (or hold the action button, once the panel is wired). The console prints `# a swap is under way`, and the swap finishes with the new disk in: one `EJECT`, one `INSERT`.
- [ ] **A swap that can't open its image:** with the drive idle, pull the SD card out, then type `]`. The console prints `# can't open ...; putting ... back`, then `# can't open that either; the drive stays empty`. `s` shows `disk=0`, `gw read` fails cleanly (no data, no hang), and `d` prints `# no disk to insert`. Put the card back and reboot (the card is only read at power-on).
- [ ] If a swap ever prints only `# can't open X; putting Y back` (a damaged file, say), check that `i` names Y, `gw read` matches Y's local copy, and `gw write` works.

## 13. The FDD Synth: sound

- [ ] Wire the MAX98357A and a speaker ([wiring.md](wiring.md)).
- [ ] Copy `samples/Placeholder/` and `samples/Recorded/` from the repo to `/samples/` on the card. The first time, the drive plays the first in name order.
- [ ] Power up and type `s`: `pack=Placeholder`.
- [ ] Type `k`: `# pack Recorded`, and `s` agrees. Its sounds are cut from a real drive; it has no insert or eject.
- [ ] Run `gw seek` and `gw read`: spin-up, the spin loop, clicks or seeks, spin-down. `[` and `]` add eject and insert.
- [ ] After a long `gw read` and a `gw write`, `s` should show `audio_underruns=0`. If not, core 0 stalled longer than the 46 ms queue: note what it was doing (`write_us_max` is the likely one).

## 14. The front panel

- [ ] Plug the OLED into the BB48's Qwiic socket, and wire the encoder and the action button ([wiring.md](wiring.md)).
- [ ] Power up: the console shows `# oled: found at 0x3c` and the home screen appears. `s` shows `oled=1` and `frames` counting up by about 10 a second.
- [ ] Turn the encoder clockwise: the volume screen, rising, and the sound gets louder. If it falls instead, set `ENCODER_REVERSED` in `src/config.h`.
- [ ] Press the encoder: the disk list. Turn to another disk and press: the Swap screen, the eject and insert sounds, then home with the new disk (`i` agrees).
- [ ] Press the action button: head trace, sounds, stats, home. Hold it for a second: the disk ejects (`DSKCHG`); hold again to insert.
- [ ] On the Sounds screen, press the encoder: the pack list, `>` on the one loaded. Choose the other and press: it loads (with the host using the drive, the screen says `LOADING WHEN IDLE` until it stops). A running motor carries on in the new pack's sound.
- [ ] **Remembered:** choose a disk, a pack and a volume, wait 5 seconds with the drive idle, and power off. Power on: the same three come back. `/FDDSYNTH.CFG` on the card holds them, and can be edited by hand.
- [ ] Delete `/FDDSYNTH.CFG`, or put nonsense in it: the drive still starts, on the first disk and pack at 75%.
- [ ] During a long `gw read` with the panel on, `audio_underruns` stays 0, `draw_us_max` is well under 4000 (the most `test_pc_panel_margin` allows: above that, a frame just as the head steps could make a cylinder late for a PC's 15 ms settle time), and `oled_aborts=0`. Also note `read_us_max` and `encode_us_max`: the margin assumes 3 ms and 2.5 ms; if they're higher, rerun the test with them. Then repeat slice 2's seek test: the panel must not make a read miss its settle time.

## If something fails

- **Evidence.** Each failing test leaves a folder in `host/runs/` with the drive's trace and the `gw` transcript.
- **No activity at all.**
  - Check `p` while running `gw seek 0`; a select line should go `ON`.
  - Check the drive letter: try the other one.
  - Run the solder-bridge test again with the cable unplugged.
- **`gw` can't find the host.** Set `FDDPRO_GW_DEVICE` in `host/rig.env` to its port (`ls /dev/cu.usbmodem*`).
- **RDATA pulses look rounded** on the logic analyser: the FeatherWing's pull-up is too weak for 375 ns pulses. See the plan's risks.
