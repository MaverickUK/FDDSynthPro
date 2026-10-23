#include "rotation.h"

namespace rotation {

void Spindle::setMotor(bool on, uint32_t now_us) {
    if (on == on_) return;
    on_ = on;
    if (on) anchor_us_ = now_us + cfg_.spin_up_us;
}

bool Spindle::atSpeed(uint32_t now_us) const {
    return on_ && int32_t(now_us - anchor_us_) >= 0;
}

uint32_t Spindle::phaseUs(uint32_t now_us) {
    if (!atSpeed(now_us)) return 0;
    uint32_t elapsed = now_us - anchor_us_;
    if (elapsed >= cfg_.period_us) {
        // Move the anchor to the start of the current revolution, so the
        // difference stays small however long the motor runs.
        uint32_t phase = elapsed % cfg_.period_us;
        anchor_us_ += elapsed - phase;
        return phase;
    }
    return elapsed;
}

}  // namespace rotation
