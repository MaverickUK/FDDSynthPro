// The RDATA streamer on the hardware: the fdd_rdata PIO program, fed by two
// DMA channels that loop over the cylinder buffer for as long as the disk
// turns. Core 1 only (begin, restartAtIndex, enable).
//
// The buffer (rdata_stream.h) may be rebuilt in place while it streams: the
// host is waiting for the head to settle then, as on a real drive, where the
// head lands mid-track.
#pragma once

#include <cstdint>

namespace rdata_pio {

// Claims a state machine and two DMA channels, and starts streaming
// `cylinder` (`words` slot words) at the format's data rate. RDATA starts
// released.
void begin(const uint32_t* cylinder, uint32_t words, uint32_t data_rate_bps);

// Starts the stream again from the first slot. Called as each index pulse
// begins, so the track is always in step with the index, whatever happened
// before. The pause is a few microseconds, inside gap 4a.
void restartAtIndex();

// A disk of another format: the buffer's length and the data rate change.
// RDATA should be held released (no disk) while it happens.
void setFormat(uint32_t words, uint32_t data_rate_bps);

// Lets the program drive RDATA, or holds it released (deselected, no disk,
// motor not at speed). The program keeps running either way.
void enable(bool on);

}  // namespace rdata_pio
