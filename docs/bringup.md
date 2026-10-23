# Bring-up checklist

Hardware day, in order. Each step either passes or tells you what to fix before the next one. The wiring itself is in [wiring.md](wiring.md).

You'll need: a multimeter with a continuity beeper, the Mac, the Feather RP2040 with its Floppy FeatherWing, a plain **BB48** (not the BB48R), and ideally the reference floppy drive with a 5 V 2 A supply.

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

## If something fails

- **Evidence.** Each failing test leaves a folder in `host/runs/` with the drive's trace and the `gw` transcript.
- **No activity at all.**
  - Check `p` while running `gw seek 0`; a select line should go `ON`.
  - Check the drive letter: try the other one.
  - Run the solder-bridge test again with the cable unplugged.
- **`gw` can't find the host.** Set `FDDPRO_GW_DEVICE` in `host/rig.env` to its port (`ls /dev/cu.usbmodem*`).
- **RDATA pulses look rounded** on the logic analyser: the FeatherWing's pull-up is too weak for 375 ns pulses. See the plan's risks.
