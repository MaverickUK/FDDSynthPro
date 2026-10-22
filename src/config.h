// Pin map and sizes. Pure constants, so host tests can include it.
// docs/wiring.md maps each pin to the floppy header.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cfg {

// The floppy interface uses GPIO 4-18, all inside one PIO block's GPIO 0-31
// window. Avoided on purpose:
//   GPIO 0-3  the BB48 pulls GPIO 2/3 up to 3.3 V through 2.2k (R17/R18,
//             UEXT/Qwiic I2C); GPIO 0/1 stay free for a UART if needed.
//   GPIO 25   the on-board LED (2.2k + LED to GND).
//
// Every floppy signal is active low. Inputs come from the host's open-collector
// drivers and are pulled up to 3.3 V by 1k resistors on our side (the drive
// end terminates the cable). Outputs go through an SN7407 open-collector
// buffer: GPIO low asserts the line, GPIO high releases it.

// Inputs (host -> drive), on consecutive pins so one read captures them all.
constexpr unsigned PIN_DENSEL = 4;   // floppy pin 2: density select
constexpr unsigned PIN_MOTEA = 5;    // floppy pin 10: motor enable A
constexpr unsigned PIN_DRVSB = 6;    // floppy pin 12: drive select B (a PC drive's own select)
constexpr unsigned PIN_DRVSA = 7;    // floppy pin 14: drive select A
constexpr unsigned PIN_MOTEB = 8;    // floppy pin 16: motor enable B (a PC drive's own motor)
constexpr unsigned PIN_DIR = 9;      // floppy pin 18: asserted = step in (towards higher cylinders)
constexpr unsigned PIN_STEP = 10;    // floppy pin 20: one step per assertion
constexpr unsigned PIN_WDATA = 11;   // floppy pin 22: write data (slice 5)
constexpr unsigned PIN_WGATE = 12;   // floppy pin 24: write gate
constexpr unsigned PIN_SIDE1 = 13;   // floppy pin 32: asserted = head 1

// Outputs (drive -> host), through the SN7407.
constexpr unsigned PIN_INDEX = 14;   // floppy pin 8: once per revolution
constexpr unsigned PIN_TRK00 = 15;   // floppy pin 26: head on cylinder 0
constexpr unsigned PIN_WPT = 16;     // floppy pin 28: write protected
constexpr unsigned PIN_RDATA = 17;   // floppy pin 30: read data (flux pulses)
constexpr unsigned PIN_DSKCHG = 18;  // floppy pin 34: disk changed

constexpr unsigned PIN_BOARD_LED = 25;  // on-board LED, not a floppy line

constexpr unsigned FIRST_INPUT_PIN = PIN_DENSEL;
constexpr unsigned LAST_INPUT_PIN = PIN_SIDE1;
constexpr unsigned FIRST_OUTPUT_PIN = PIN_INDEX;
constexpr unsigned LAST_OUTPUT_PIN = PIN_DSKCHG;
constexpr unsigned BUS_FIRST_PIN = FIRST_INPUT_PIN;
constexpr unsigned BUS_LAST_PIN = LAST_OUTPUT_PIN;

// Trace ring between core 1 (producer) and core 0 (consumer):
// 4096 events x 16 bytes = 64 KB.
constexpr size_t TRACE_RING_EVENTS = 4096;

// Steps in the same direction merge into one STEP line unless this far apart.
// Hosts step every 3-6 ms in a seek; a gap this long starts a new seek.
constexpr uint32_t STEP_RUN_GAP_US = 50000;

// Core 0 prints a pending line once the trace has been quiet this long:
// longer than STEP_RUN_GAP_US, so a slow seek still prints as one line.
constexpr unsigned TRACE_FLUSH_MS = 60;

}  // namespace cfg
