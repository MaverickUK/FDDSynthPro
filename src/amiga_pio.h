// An Amiga's bus at hardware speed: two edge samplers and an output gate,
// in PIO. Core 1 only.
//
// An Amiga selects its drive for a few microseconds at a time: 32 selects of
// about 4 us to read the drive ID from /RDY, and about 20 us around each
// step, with DIR set and cleared alongside the select (seen on an A1200, 6 Nov
// 2026). Core 1's once-a-pass sample can miss a select altogether, see DIR as
// it was after, or put the outputs on the cable after the Amiga has read
// them. So:
//   - fdd_edge, twice: the ten inputs as they were within two cycles of STEP
//     falling, and of the drive's select falling (drive.h: onStepEdge and
//     onSelectEdge: whether a step was this drive's and which way, and the
//     motor line at select);
//   - fdd_gate (fdd_gate_od in the direct builds): the outputs core 1 works
//     out as if the drive were selected (Drive::outputsSelected), on the
//     cable only while the select is asserted, within a few cycles.
// One drive only (drive=df0 or drive=df1): two drives' gates would need two
// selects in one program.
#pragma once

#include <cstdint>

namespace amiga_pio {

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
