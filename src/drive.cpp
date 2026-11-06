#include "drive.h"

#include "fdd_signals.h"

namespace drive {

Drive::Drive(Config c) : cfg_(c), head_(c.last_cylinder, c.start_cylinder), spindle_(c.spindle) {}

StepEvent Drive::onSample(uint32_t now_us, uint32_t pins, bool step_edge) {
    bool selected = fdd::asserted(pins, cfg_.lines.select);
    bool motor = fdd::asserted(pins, cfg_.lines.motor);
    if (cfg_.interface == bus::Interface::Amiga) {
        // Each drive keeps its own; with edge samples, onSelectEdge latches it
        if (!edge_samples_ && selected && !selected_) motor_latched_ = motor;
        motor = motor_latched_;
    }
    selected_ = selected;
    spindle_.setMotor(motor, now_us);
    side_ = fdd::asserted(pins, cfg::PIN_SIDE1) ? 1 : 0;

    StepEvent e;
    bool step_now = fdd::asserted(pins, cfg::PIN_STEP);
    bool new_step = step_latch_ ? step_edge : step_now && !step_was_asserted_;
    if (!edge_samples_ && new_step && selected_) e = step(pins);
    step_was_asserted_ = step_now;
    return e;
}

StepEvent Drive::step(uint32_t pins) {
    StepEvent e;
    e.stepped = true;
    e.outcome = head_.step(fdd::asserted(pins, cfg::PIN_DIR));
    e.cylinder = head_.cylinder();
    if (disk_present_) disk_changed_ = false;
    return e;
}

StepEvent Drive::onStepEdge(uint32_t, uint32_t pins) {
    if (!fdd::asserted(pins, cfg_.lines.select)) return StepEvent{};  // another drive's
    return step(pins);
}

void Drive::onSelectEdge(uint32_t now_us, uint32_t pins) {
    if (cfg_.interface != bus::Interface::Amiga) return;
    motor_latched_ = fdd::asserted(pins, cfg_.lines.motor);
    spindle_.setMotor(motor_latched_, now_us);
}

Outputs Drive::outputs(uint32_t now_us) {
    if (!selected_) return Outputs{};
    return outputsSelected(now_us);
}

Outputs Drive::outputsSelected(uint32_t now_us) {
    Outputs o;
    o.index = disk_present_ && spindle_.index(now_us);
    o.trk00 = head_.atTrack0();
    o.wpt = !disk_present_ || write_protected_;
    o.dskchg = disk_changed_;
    if (cfg_.interface == bus::Interface::Amiga)
        o.ready = spindle_.motorOn() ? spindle_.atSpeed(now_us) : true;  // at speed; or the ID: all ones
    else
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
