// The head carriage: which cylinder the head is over, and what a step does.
// Pure C++, host-tested.
//
// A real drive's head moves between two mechanical stops. Stepping outwards
// at cylinder 0 knocks the carriage against the outer stop (the classic
// recalibrate "grind"); the head stays put. The FDD Synth plays a different
// sound for that, so the outcome is reported, not just the new position.
#pragma once

#include <cstdint>

namespace head {

// The inner stop. 3.5-inch drives use cylinders 0-79 but can reach a few
// more; hosts probing for 80+ cylinders rely on that.
constexpr uint8_t DEFAULT_LAST_CYLINDER = 83;

enum class Step : uint8_t {
    In,           // moved one cylinder inwards (towards higher cylinders)
    Out,          // moved one cylinder outwards
    AtOuterStop,  // stepped outwards at cylinder 0: no movement, a knock
    AtInnerStop,  // stepped inwards at the last cylinder: no movement
};

const char* stepName(Step s);

class Head {
public:
    explicit Head(uint8_t last_cylinder = DEFAULT_LAST_CYLINDER, uint8_t start_cylinder = 0)
        : last_(last_cylinder), cylinder_(start_cylinder > last_cylinder ? last_cylinder : start_cylinder) {}

    Step step(bool inwards);

    // The inner stop moves (a disk with more cylinders than a 3.5-inch drive
    // reaches: the emulated drive follows the disk). The head stays inside it.
    void setLastCylinder(uint8_t last) {
        last_ = last;
        if (cylinder_ > last_) cylinder_ = last_;
    }

    uint8_t cylinder() const { return cylinder_; }
    uint8_t lastCylinder() const { return last_; }
    bool atTrack0() const { return cylinder_ == 0; }

private:
    uint8_t last_;
    uint8_t cylinder_;
};

}  // namespace head
