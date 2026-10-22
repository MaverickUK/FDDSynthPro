// Turns a stream of pin samples into trace events: what core 1 does in the
// listen build, kept free of SDK headers so it can be host-tested.
#pragma once

#include <cstdint>

#include "config.h"
#include "fdd_signals.h"
#include "trace_format.h"

class PinWatch {
public:
    explicit PinWatch(uint32_t initial_pins) : prev_(initial_pins & fdd::WATCHED_MASK) {}

    // Takes the next sample. Writes up to two events to `out` (a Step, then a
    // Change) and returns how many. A step is the assertion of STEP; its
    // release is not an event.
    unsigned onSample(uint32_t time_us, uint32_t pins, TraceEvent out[2]) {
        uint32_t now = pins & fdd::WATCHED_MASK;
        uint32_t changed = now ^ prev_;
        if (!changed) return 0;
        prev_ = now;
        unsigned n = 0;
        constexpr uint32_t step = fdd::maskOf(cfg::PIN_STEP);
        if ((changed & step) && fdd::asserted(pins, cfg::PIN_STEP))
            out[n++] = TraceEvent{time_us, pins, 0, TraceKind::Step};
        if (changed & ~step) out[n++] = TraceEvent{time_us, pins, changed & ~step, TraceKind::Change};
        return n;
    }

private:
    uint32_t prev_;
};
