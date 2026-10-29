// The spindle: whether the disk is turning, and where in the revolution it
// is. Pure C++, host-tested.
//
// Time is the 32-bit microsecond counter (time_us_32()), which wraps every
// 71 minutes. Differences are taken in unsigned arithmetic, so the wrap is
// harmless as long as phaseUs() or index() is called at least once per wrap,
// which core 1's loop does millions of times a second.
#pragma once

#include <cstdint>

namespace rotation {

struct Config {
    uint32_t period_us = 200000;     // 300 RPM
    uint32_t index_pulse_us = 2000;  // to be matched to the reference drive (bringup.md step 6)
    uint32_t spin_up_us = 0;         // motor on until the first index pulse
};

class Spindle {
public:
    explicit Spindle(Config c = {}) : cfg_(c) {}

    // Motor on starts a new revolution once spun up: the index pulse begins
    // spin_up_us later. Motor off stops it at once.
    void setMotor(bool on, uint32_t now_us);
    bool motorOn() const { return on_; }

    // True once the motor has been on for spin_up_us.
    bool atSpeed(uint32_t now_us) const;

    // Time since the current revolution's index pulse began, 0..period-1.
    // 0 when not at speed.
    uint32_t phaseUs(uint32_t now_us);

    // The index line: asserted for index_pulse_us at the start of each
    // revolution, while at speed.
    bool index(uint32_t now_us) { return atSpeed(now_us) && phaseUs(now_us) < cfg_.index_pulse_us; }

    // Another speed: 300 or 360 RPM (a disk's). Takes effect from the
    // current revolution.
    void setPeriod(uint32_t period_us) { cfg_.period_us = period_us; }

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    bool on_ = false;
    uint32_t anchor_us_ = 0;  // when the current revolution began (in the future while spinning up)
};

}  // namespace rotation
