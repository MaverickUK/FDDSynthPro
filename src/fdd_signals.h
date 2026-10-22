// Decoding a pin sample of the floppy interface. Pure C++, host-tested.
//
// A sample is the GPIO levels shifted so bit n is GPIO (BUS_FIRST_PIN + n):
// `gpio_get_all() >> cfg::BUS_FIRST_PIN`. Every floppy line is active low,
// so a signal is asserted ("ON") when its bit is 0.
#pragma once

#include <cstdint>

#include "config.h"

namespace fdd {

constexpr unsigned bitOf(unsigned pin) { return pin - cfg::BUS_FIRST_PIN; }
constexpr uint32_t maskOf(unsigned pin) { return 1u << bitOf(pin); }
constexpr bool high(uint32_t sample, unsigned pin) { return (sample >> bitOf(pin)) & 1u; }
constexpr bool asserted(uint32_t sample, unsigned pin) { return !high(sample, pin); }

constexpr uint32_t rangeMask(unsigned first, unsigned last) {
    return ((last - first + 1 >= 32) ? ~0u : ((1u << (last - first + 1)) - 1u)) << bitOf(first);
}

constexpr uint32_t INPUT_MASK = rangeMask(cfg::FIRST_INPUT_PIN, cfg::LAST_INPUT_PIN);
constexpr uint32_t OUTPUT_MASK = rangeMask(cfg::FIRST_OUTPUT_PIN, cfg::LAST_OUTPUT_PIN);

// The inputs the listener records changes on. WDATA is left out: during a
// write it toggles every few microseconds, which would flood the trace.
// STEP is recorded as its own event (one per assertion), not as a change.
constexpr uint32_t WATCHED_MASK = INPUT_MASK & ~maskOf(cfg::PIN_WDATA);

// Short name of a floppy line ("?" for a pin that isn't one).
const char* signalName(unsigned pin);

}  // namespace fdd
