# Bring-up checklist

Hardware day, in order. Each step either passes or tells you what to fix before the next one. The wiring itself is in [wiring.md](wiring.md).

You'll need: a multimeter with a continuity beeper, the Mac, the Greaseweazle V4.1, a plain **BB48** (not the BB48R), and ideally the reference floppy drive with its 5 V supply.

## 1. Before any power

- [ ] **Header orientation.** Check that the BB48's silkscreen matches the wiring table: USB-C at the top, GPIO 0–23 on the EXT1 row.
- [ ] **IDC helper strips.** Check with the meter that floppy pins 1 and 2 are **not** connected to each other; nor are any other neighbouring pins. Each header pin needs its own strip.
- [ ] **Continuity, end to end.** For each row of the wiring table, beep from the floppy header pin to its GPIO pin (inputs) or to the SN7407 output pin (outputs). Tick it off as you go.
- [ ] **Pull-ups.** Each input pin reads about 1 kΩ to +3.3 V (EXT1-1).
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

## 3. Check the Greaseweazle on its own

- [ ] Install its tools: `cd host && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`. Then `.venv/bin/gw info` should find it and show its firmware version. Update with `gw update` if it's old.
- [ ] **With the reference drive (recommended).** Connect the real drive and its power, insert a 1.44 MB disk, and run `gw rpm` and `gw read --format ibm.1440 ref.img`. If this fails, the problem is the Greaseweazle setup or the cable, not the emulator. Note which drive letter (`--drive A`/`B`) the real drive needed with your cable.
- [ ] **Measure its idle output levels** (open question in the plan). With nothing on the cable, measure floppy pins 10–24 (even) and 32 to GND. Then check:
  - About 5 V or 3.3 V with a weak feel: likely pull-ups or push-pull outputs. Note which.
  - **5 V push-pull** means current would flow into our 3.3 V pull-ups. Stop and add 1 kΩ series resistors on the inputs first (see the plan's risks).
- [ ] Measure pins 8, 26, 28, 30 and 34 too: these are its inputs and should sit high on its own pull-ups.

## 4. Connect the Greaseweazle (power order)

- [ ] The BB48 is powered **first**: it stays on USB from now on.
- [ ] Plug the floppy cable into both ends, then plug the Greaseweazle's USB into the Mac.
- [ ] Run `gw reset`, then type `p` on the drive's console. Every input should be `OFF` (DENSEL may be either).
- [ ] From now on: **unplug the Greaseweazle before the BB48**.

## 5. Slice 0: listen to the Greaseweazle

- [ ] Copy `host/rig.env.example` to `host/rig.env` and set `FDDPRO_GW_DRIVE` to the letter that matches your cable (B for a straight cable).
- [ ] Run `scripts/rig.sh -m slice0 --save-reference`. It runs `gw seek`, `gw rpm` and both drive letters, and saves each trace and its summary to `host/reference/`.
- [ ] Read the summaries. Things to look for:
  - **Which select and motor pins does each drive letter use?** That decides the pins slice 1 answers on.
  - **How does the Greaseweazle look for track 0?** How many steps out, how far apart, before it gives up. Turn that into a virtual-host test for the `head` module.
  - **The step interval**, compared with `gw delays` (its Step Delay setting).
  - **No `DROP` lines**, and no lone steps outside a seek (noise on STEP).
- [ ] Commit the reference traces.

## 6. Reference measurements for slice 1 (logic analyser)

With the reference drive on the Greaseweazle and the logic analyser on the cable:
- [ ] The index pulse: its width, and its period at 300 RPM.
- [ ] TRK00: how soon after a step to cylinder 0 it asserts.
- [ ] RDATA: the pulse width of one flux transition.
- [ ] DSKCHG: its level after power-on, and what clears it.

Note the numbers in `docs/plan.md`, under the open questions.

## If something fails

- **Evidence.** Each failing test leaves a folder in `host/runs/` with the drive's trace and the Greaseweazle transcript.
- **No activity at all.**
  - Check `p` while running `gw seek 0`; a select line should go `ON`.
  - Check the drive letter: try the other one.
  - Run the solder-bridge test again with the cable unplugged.
- **`gw` can't find the Greaseweazle.** Set `FDDPRO_GW_DEVICE` in `host/rig.env` to its port (`ls /dev/cu.usbmodem*`).
