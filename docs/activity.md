# Sound events

The original FDD Synth listens to the floppy cable and works out what the drive is doing from the select, motor, step and direction lines. FDDSynthPro *is* the drive, so it knows rather than guesses:
- when the motor starts and stops;
- every step, and the cylinder it took the head to;
- when the head knocks against an end stop instead of moving;
- when a disk goes in or comes out.

This note covers the events, how steps become clicks or seeks, and a proposed sample pack. The code is `src/activity.*`, tested in `test/native/test_activity.cpp`.

## The events

The drive build prints one `ACT` line per event:

| Line | When | Suggested sound |
|---|---|---|
| `ACT <t> MOTOR_ON` | The motor line has been asserted for 1 ms | `spinup.wav`, then `spin.wav` looping |
| `ACT <t> MOTOR_OFF` | The motor line has been released for 1 ms | `spindown.wav` |
| `ACT <t> STEP IN\|OUT <cyl>` | One step, on its own | `step.wav`, a single click |
| `ACT <t> SEEK_START IN\|OUT <cyl> <interval>` | A second step close behind the first: a seek begins. `<interval>` is the µs between the two: the seek's rate | `seek.wav` clicking at that rate: the buzz of rapid steps |
| `ACT <t> SEEK_END <cyl> <steps>` | No step for 20 ms: the seek is over | Stop the clicks. `<steps>` gives the distance |
| `ACT <t> BUMP OUTER\|INNER` | A step out at cylinder 0, or in at the last cylinder: the head knocks the stop and stays put | `bump.wav`; a run of them is the recalibrate "grind" |
| `ACT <t> INSERT` / `EJECT` | A disk goes in or out (console `d` now; a button later) | `insert.wav`, `eject.wav` |

The motor waits 1 ms (`DriveObserver::MOTOR_HOLD_US`) before it's heard either way: the Feather host pulses the motor line for up to 34 µs at the start of every command, as it sets its pins up, and a real motor doesn't notice that. 1 ms is nothing beside a spin-up.

`<t>` is the drive's microsecond clock. A seek's first step has already played as a `STEP`. `SEEK_END` is timed at the seek's last step, but printed 20 ms later, once the gap shows that the seek is over.

## Clicks or a seek

A real drive clicks once per step. When steps come every few milliseconds, the clicks run together into a buzz. The tracker splits on the gap between steps (`activity::Config::seek_gap_us`, 20 ms):

| Host | Step interval | Sounds like |
|---|---|---|
| PC BIOS / DOS | 3–6 ms | A seek: one buzz |
| Greaseweazle | 10 ms | A seek |
| Feather + FeatherWing | 26 ms | A run of separate clicks |

The first step of a run always clicks at once: the second step tells the tracker it's a seek. A change of direction or a bump ends a seek.

### A seek at its own rate

A 3 ms seek and a 10 ms seek buzz at different pitches, so one fixed seek loop can't sound right for both. `SEEK_START` carries the time between the seek's first two steps, and the synth clicks at that rate:
- `seek.wav` is **one step of a seek**: a click as it sounds among others, cut from a real seek;
- the synth plays it once a step, from the seek's second step (its first has played as a `STEP`);
- when the steps come faster than the click lasts, each is cut short, fading over its last quarter (at most 1 ms), so a PC's 3 ms seek is a buzz of click fronts;
- the clicks stop when `SEEK_END` arrives, 20 ms after the last step: nothing tells the synth sooner. The step count then trims the click still sounding, if the last step's should already be over.

With no interval (a trace from before 0.24), the click repeats at its own length. A pack from before 0.24 whose `seek.wav` holds many steps plays its first `<interval>` of it each step: fine at a PC's rate, but a slower seek would play more than one click per step.

## Why the bump matters

A host looking for track 0 normally checks TRK00 before each step, so the head never hits the stop: the Greaseweazle, the Feather and a PC's controller all do. Some software doesn't. It steps out a fixed number of times and lets the head hammer the outer stop: that's the grinding sound of an old PC finding track 0, and of some copy-protection checks. A listen-only synth can't tell a bump from a step. This drive can, because the head model knows it's already at cylinder 0 (`head::Step::AtOuterStop`).

## A sample pack

Following the HDD Synth packs (`spinup`, `idle`, `access`, `spindown`), an FDD pack could be:

```
samples/<pack>/
├── spinup.wav     motor on
├── spin.wav       loops while the motor runs
├── spindown.wav   motor off
├── step.wav       one click
├── seek.wav       one step of a seek, repeated at the seek's rate
├── bump.wav       the head hitting a stop
├── insert.wav     optional
└── eject.wav      optional
```

Until a real drive is recorded, `samples/Placeholder/` holds a synthesised stand-in pack (mono, 16-bit, 22.05 kHz), made by `scripts/make_placeholder_samples.py`. It's a rough model of each mechanism, not a recording:
- `spin` is a motor hum with a once-per-revolution flutter;
- `step` is a damped tick over a small thump;
- `seek` is one 6 ms tick;
- `bump` is a heavier, lower tick.

`--demo demo.wav` writes a short session to listen to (`afplay demo.wav` on a Mac). `spin` and `seek` loop seamlessly: whole revolutions and whole steps, with their filters run circularly.

`samples/Recorded/` is cut from a recording of a real 3.5-inch drive (`media/FDDSynth_ExampleBehaviours.m4a`, the drive driven by the FDD Synth's software), by `scripts/make_recorded_pack.py`. The script's notes say where in the recording each sound comes from:
- `spinup` is the motor starting (a knock in the middle is cut out: it's the `bump`);
- `spin` is four revolutions of the idle disk (a soft thump every 199.8 ms), joined end to start;
- `step` is the most typical of a slow 79-step sweep across the disk;
- `seek` is one step of a seek whose steps came 10 ms apart, made to loop;
- `bump` is the loudest click in the recording, a step against the end stop.

The recording has no insert or eject, so the pack has neither. It keeps the drive's own balance, with one gain for the whole pack: loud clicks over a quiet motor. `--audition out.wav` writes each sound in turn to listen to.

Put any number of packs on the card, one folder each under `/samples`. On the panel's Sounds screen, press the encoder for the list: turn to one and press to load it (console `k` loads the next). It loads once the drive is idle, and the drive remembers it across power-off (`/FDDSYNTH.CFG`). The first time, it plays the first folder in name order.

## Hearing it

`host/fddproto/player.py` plays any trace's `ACT` lines with a pack, into a WAV, by the synth's rules:
- spin-up, then the spin loop until `MOTOR_OFF`, then spin-down;
- a click per `STEP`;
- the seek's clicks at its rate, from `SEEK_START` to `SEEK_END`;
- a bump, insert or eject sample where those happen.

```
cd host
.venv/bin/python -m fddproto.player ../.pio/native-tests/pc_session_act.txt session.wav
afplay session.wav
```

`pc_session_act.txt` is the virtual PC's DOS-like session (`test/run_native_tests.sh` writes it): boot, a file read across the disk, a file written, a disk swap. A drive's serial log works the same way, with `--pack` choosing another pack.

Recording these from a real drive is easiest with a Greaseweazle or the Feather host driving it: `gw seek` to make single steps and seeks on demand, and a short sector loop to keep the motor running.

## Limits and open questions

- **Reads and writes sound the same as idle** on a 3.5-inch drive: the heads are always loaded. 5.25-inch drives with a head-load solenoid would add a `HEAD_LOAD` event.
- **Seek speed** is followed from 0.24 (above), from the seek's first two steps. A seek that changes speed partway keeps its first rate, and its clicks run on for the 20 ms it takes to know it's over. An event per step would fix both, at the cost of up to 330 events a second.
- **Tuning.** The 20 ms gap is a starting value. The Feather's 26 ms steps fall outside it and play as separate full clicks; recording a real drive at that rate (docs/recording.md) will say whether they should be a seek too. Listening against real hosts will refine it, and the synth may want two thresholds (a "fast click" region).
