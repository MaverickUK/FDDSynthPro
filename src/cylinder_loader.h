// Loads the cylinder under the head into the RDATA streamer's buffer
// (rdata_stream.h), a step at a time. Pure C++, host-tested; core 0 runs it.
//
// A load is four steps: read head 0's track, read head 1's, encode head 0
// into the buffer, encode head 1. Core 0 calls service() on every pass of
// its loop; each call does at most one step, so the console and the trace
// keep going between them.
//
// If the head moves during a load, the load starts again for the new
// cylinder at the next step: during a seek only the last cylinder is ever
// finished, so it's ready one load after the last step, not two.
//
// The buffer is built in place while the DMA streams it. ready() is -1 from
// the first write into it until the load finishes; the drive keeps RDATA
// released meanwhile, so the host never reads a half-built or stale
// cylinder (stale IDs would look like a seek error).
#pragma once

#include <atomic>
#include <cstdint>

#include "track_source.h"

class CylinderLoader {
public:
    enum class Step : uint8_t { Idle, ReadHead0, ReadHead1, EncodeHead0, EncodeHead1 };

    // `slots`: the streamer's buffer, format().track_bytes words.
    // `scratch`: format().track_bytes cell words.
    // `data0`, `data1`: format().trackDataBytes() bytes each.
    CylinderLoader(TrackSource& source, uint32_t* slots, uint16_t* scratch, uint8_t* data0, uint8_t* data1)
        : source_(source), slots_(slots), scratch_(scratch), data_{data0, data1} {}

    // The cylinder the head is on now. Cheap; call as often as you like.
    void request(uint8_t cylinder) { wanted_ = cylinder; }

    // Does the next step of a load, if there is one. Returns the step done
    // (Idle: nothing to do).
    Step service();

    // The cylinder the buffer holds, or -1 while it is being built. Safe to
    // read from the other core: it turns -1 before the first write into the
    // buffer, and to the cylinder only after the last.
    int ready() const { return ready_.load(std::memory_order_acquire); }
    // Loads finished, and loads abandoned because the head moved.
    uint32_t loads() const { return loads_; }
    uint32_t restarts() const { return restarts_; }

private:
    TrackSource& source_;
    uint32_t* slots_;
    uint16_t* scratch_;
    uint8_t* data_[2];
    bool present_[2] = {false, false};

    int wanted_ = 0;
    std::atomic<int> ready_{-1};
    int loading_ = -1;  // the cylinder being loaded, or -1
    Step next_ = Step::Idle;
    uint32_t loads_ = 0;
    uint32_t restarts_ = 0;
};
