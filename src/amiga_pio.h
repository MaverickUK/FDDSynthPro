// An Amiga's bus at hardware speed: two edge samplers and an output gate,
// in PIO. Core 1 only.
//
// An Amiga selects its drive for a few microseconds at a time: 32 selects of
// about 4 us to read the drive ID from /RDY, and about 20 us around each
// step, with DIR set and cleared alongside the select (seen on an A1200, 6 Nov
// 2026). Core 1's once-a-pass sample can miss a select altogether, see DIR as
// it was after, or put the outputs on the cable after the Amiga has read
// them. So:
//   - fdd_step: the ten inputs as STEP fell (whether the step was this
//     drive's) and as it rose (DIR: a game's loader may change it in the same
//     write as STEP falls); fdd_edge: the inputs as the drive's select fell
//     (the motor line at select). drive.h: onStepEdge and onSelectEdge;
//   - fdd_gate (fdd_gate_od in the direct builds): the outputs core 1 works
//     out as if the drive were selected (Drive::outputsSelected), on the
//     cable only while the select is asserted, within a few cycles.
// One drive only (drive=df0 or drive=df1): two drives' gates would need two
// selects in one program.
#pragma once

#include <cstdint>

#include "config.h"
#include "fdd_signals.h"

namespace amiga_pio {

// A sampler's ten inputs (GPIO 12-21 in bits 0-9) in sample layout.
constexpr uint32_t samplePins(uint32_t raw) {
    return ((raw & 0x3FFu) << (cfg::FIRST_INPUT_PIN - cfg::BUS_FIRST_PIN)) | fdd::OUTPUT_MASK;
}

// fdd_step's word (the inputs as STEP fell in bits 19-10, as it rose in
// 9-0) as one sample: everything as it fell, but DIR as it rose. A loader
// may set DIR in the same write that pulls STEP low.
constexpr uint32_t stepPins(uint32_t word) {
    constexpr uint32_t DIR = 1u << (cfg::PIN_DIR - cfg::FIRST_INPUT_PIN);
    return samplePins(((word >> 10) & ~DIR) | (word & DIR));
}

// Claims three state machines (the gate in the PIO block RDATA doesn't use,
// so its OUT range can cover GPIO 4-23 without touching RDATA's pin) and
// starts them. `select_pin` is the drive's select GPIO. After this the
// output pins belong to the gate: core 1's SIO writes to them do nothing.
// False if the PIO blocks are full (the drive then runs on core 1 alone).
bool begin(unsigned select_pin);

// The next edge sample (sample layout, fdd_signals.h), if there is one.
bool takeStepSample(uint32_t& pins);
bool takeSelectSample(uint32_t& pins);

// The output levels as if selected (sample layout, OUTPUT_MASK bits), for
// the gate. Cheap when they haven't changed.
void setLevels(uint32_t levels);

}  // namespace amiga_pio
