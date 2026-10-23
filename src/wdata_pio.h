// The WDATA capture on the hardware: the fdd_wdata PIO program, its RX FIFO
// drained by an endless DMA round a ring (CaptureStream in write_burst.h
// reads it on core 0). Core 1 only: it starts the program when the host
// asserts WGATE, stops it when the host releases it, and tracks how far the
// DMA has written.
#pragma once

#include <cstddef>
#include <cstdint>

namespace wdata_pio {

// Claims a state machine and a DMA channel, clocked for the format's data
// rate (16 ticks a cell), and starts the DMA round `ring`: `ring_bytes`, a
// power of two, with the buffer aligned to its size. The program doesn't
// run until start().
void begin(uint8_t* ring, uint32_t ring_bytes, uint32_t data_rate_bps);

// A disk of another data rate. Call only while no capture is running.
void setRate(uint32_t data_rate_bps);

// Starts and stops the program. stop() waits for the FIFO to drain into
// the ring. Up to three intervals still in the program's shift register
// are lost: they're the gap bytes a host writes after the CRC.
void start();
void stop();

// Bytes written into the ring since begin(), free-running (the ring index
// is this modulo the ring size). Call on every pass of core 1's loop: it
// counts the ring's wraps from the DMA's write address, so it must see each
// one (16 KB is 44 ms of writing at 1.44 MB).
uint32_t produced();

// Ticks the program doesn't count at each edge (fdd_wdata.pio): add them to
// every interval.
uint32_t edgeTicks();
uint32_t ticksPerCell();

}  // namespace wdata_pio
