#include "drive.h"

#include "fdd_signals.h"

namespace drive {

Drive::Drive(Config c) : cfg_(c), head_(c.last_cylinder, c.start_cylinder), spindle_(c.spindle) {}

StepEvent Drive::onSample(uint32_t now_us, uint32_t pins) {
    selected_ = fdd::asserted(pins, cfg_.lines.select);
    spindle_.setMotor(fdd::asserted(pins, cfg_.lines.motor), now_us);
    side_ = fdd::asserted(pins, cfg::PIN_SIDE1) ? 1 : 0;

    StepEvent e;
    bool step = fdd::asserted(pins, cfg::PIN_STEP);
    if (step && !step_was_asserted_ && selected_) {
        e.stepped = true;
        e.outcome = head_.step(fdd::asserted(pins, cfg::PIN_DIR));
        e.cylinder = head_.cylinder();
        if (disk_present_) disk_changed_ = false;
    }
    step_was_asserted_ = step;
    return e;
}

Outputs Drive::outputs(uint32_t now_us) {
    Outputs o;
    if (!selected_) return o;
    o.index = disk_present_ && spindle_.index(now_us);
    o.trk00 = head_.atTrack0();
    o.wpt = !disk_present_ || write_protected_;
    o.dskchg = disk_changed_;
    o.ready = disk_present_ && spindle_.atSpeed(now_us);
    return o;
}

void Drive::insertDisk(bool write_protected) {
    disk_present_ = true;
    write_protected_ = write_protected;
}

void Drive::ejectDisk() {
    disk_present_ = false;
    disk_changed_ = true;
}

}  // namespace drive
