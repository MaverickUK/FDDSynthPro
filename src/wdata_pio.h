// The WDATA capture on the hardware: the fdd_wdata PIO program, its RX FIFO
// drained by DMA into a buffer. Core 1 only: it starts a capture when the
// host asserts WGATE, and stops it when the host releases it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wdata_pio {

// Claims a state machine and a DMA channel, clocked for the format's data
// rate (16 ticks a cell). Nothing runs until start().
void begin(uint32_t data_rate_bps);

// A disk of another data rate. Call only while no capture is running.
void setRate(uint32_t data_rate_bps);

// Captures intervals into `buffer` (`capacity` bytes, a multiple of 4).
void start(uint8_t* buffer, size_t capacity);

// Stops, and returns how many intervals (bytes) the buffer holds. Up to
// three intervals still in the program's shift register are lost: they're
// the gap bytes a host writes after the CRC.
size_t stop();

// Ticks the program doesn't count at each edge (fdd_wdata.pio): add them to
// every interval.
uint32_t edgeTicks();
uint32_t ticksPerCell();

}  // namespace wdata_pio
