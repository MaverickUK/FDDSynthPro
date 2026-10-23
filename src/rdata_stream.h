// What the RDATA streamer (fdd_rdata.pio) is fed: both heads of the current
// cylinder, interleaved cell by cell, and the clock divider that sets the
// cell rate. Pure C++, host-tested; the PIO program itself is tested in the
// simulator (host/tests/test_rdata_sim.py).
//
// A cylinder is fmt.track_bytes 32-bit words. Word i holds byte i of both
// tracks: 16 slots of (head 0 cell, head 1 cell), first slot in the top two
// bits, which is the order the program shifts them out.
#pragma once

#include <cstdint>

#include "ibm_track.h"

namespace rdata {

// Must match fdd_rdata.pio (the firmware checks with static_assert).
constexpr uint32_t CELL_CYCLES = 16;

// The slots for one byte of each track.
uint32_t interleave(uint16_t head0_cells, uint16_t head1_cells);

// One head's cells back out of a slot word (for tests).
uint16_t headCells(uint32_t slots, uint8_t head);

// Writes one head's cells into a cylinder buffer (track_bytes words),
// leaving the other head's as they are.
void setHead(uint8_t head, const uint16_t* cells, uint32_t words, uint32_t* out);

// Builds both tracks of a cylinder into `out` (fmt.track_bytes words).
// `head0_data` and `head1_data` are each track's sector data, as
// ibm::buildTrack takes it. `scratch` holds fmt.track_bytes cell words.
// Returns false if the format doesn't fit a track.
bool buildCylinder(const ibm::Format& fmt, uint8_t cylinder, const uint8_t* head0_data, const uint8_t* head1_data,
                   uint16_t* scratch, uint32_t* out);

// The PIO clock divider for a format's cell rate (two cells per data bit,
// CELL_CYCLES PIO cycles per cell), in the hardware's form: an integer part
// and a fraction in 256ths. `exact` is false if the rate can't be hit exactly.
struct Divider {
    uint16_t integer;
    uint8_t frac;
    bool exact;
};
// `cycles_per_cell` is the program's: CELL_CYCLES for the streamer, 32 for
// the WDATA capture (16 ticks of 2 cycles).
Divider clockDivider(uint32_t sys_hz, uint32_t data_rate_bps, uint32_t cycles_per_cell = CELL_CYCLES);

}  // namespace rdata
