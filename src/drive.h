// The drive as the host sees it: select, motor, steps and side in; index,
// track 0, write protect and disk change out. Pure C++, host-tested; core 1
// feeds it pin samples and puts its outputs on the SN7407.
//
// Behaviour, as a 3.5-inch PC drive:
//   - Outputs are only asserted while the drive is selected; deselected, it
//     releases every line so another drive can use the cable.
//   - Steps count only while selected. DIR is read when STEP is asserted.
//   - The motor runs whenever its line is asserted, selected or not.
//   - INDEX needs the motor at speed and a disk. TRK00 needs neither: the
//     sensor watches the head carriage.
//   - DSKCHG is asserted from power-on and after an eject, and cleared by a
//     step while a disk is in.
//   - WPT is asserted for a write-protected disk, or no disk.
//   - READY (Shugart's pin 34) needs a disk and the motor at speed. An
//     Amiga's /RDY is the motor at speed, and asserted while the motor is
//     off: the ID a standard drive gives (all ones).
//   - An Amiga drive latches its motor line as it is selected.
// Which lines select it, and what pin 34 carries, come from the bus setup
// (bus.h).
#pragma once

#include <cstdint>

#include "bus.h"
#include "config.h"
#include "fdd_signals.h"
#include "head.h"
#include "rotation.h"

namespace drive {

struct Config {
    // A PC drive answers on pins 12 and 16; the cable's twist routes the
    // host's drive A there. Greaseweazle: --drive B on a straight cable.
    bus::Lines lines = bus::lines(bus::Setup{});
    bus::Interface interface = bus::Interface::IbmPc;  // Amiga: the motor is latched at select
    uint8_t last_cylinder = head::DEFAULT_LAST_CYLINDER;
    uint8_t start_cylinder = 0;  // where the head is at power-on
    rotation::Config spindle;
};

// The output lines; true = asserted (pulled low on the cable).
struct Outputs {
    bool index = false;
    bool trk00 = false;
    bool wpt = false;
    bool dskchg = false;
    bool ready = false;
    bool operator==(const Outputs& o) const {
        return index == o.index && trk00 == o.trk00 && wpt == o.wpt && dskchg == o.dskchg && ready == o.ready;
    }
};

// The GPIO levels for the outputs, in sample layout (only OUTPUT_MASK bits
// set or clear). Asserted lines are low: the SN7407 then pulls the cable low.
// Pin 34 (PIN_DSKCHG's GPIO) carries DISK CHANGE on a PC, READY on Shugart
// and Amiga; pin 2 (PIN_CHNG) carries DISK CHANGE when `pin2_chng`.
inline uint32_t outputPins(const Outputs& o, bus::Interface interface = bus::Interface::IbmPc,
                           bool pin2_chng = false) {
    uint32_t s = fdd::OUTPUT_MASK;
    if (o.index) s &= ~fdd::maskOf(cfg::PIN_INDEX);
    if (o.trk00) s &= ~fdd::maskOf(cfg::PIN_TRK00);
    if (o.wpt) s &= ~fdd::maskOf(cfg::PIN_WPT);
    if (interface == bus::Interface::IbmPc ? o.dskchg : o.ready) s &= ~fdd::maskOf(cfg::PIN_DSKCHG);
    if (pin2_chng && interface != bus::Interface::IbmPc && o.dskchg) s &= ~fdd::maskOf(cfg::PIN_CHNG);
    return s;  // RDATA is the streamer's
}

// What one sample did to the head, for the trace and (slice 4) the sounds.
struct StepEvent {
    bool stepped = false;
    head::Step outcome = head::Step::In;
    uint8_t cylinder = 0;  // after the step
};

class Drive {
public:
    explicit Drive(Config c = {});

    // Takes a pin sample (fdd_signals.h layout). Call on every change of the
    // inputs, or simply on every pass of the loop.
    StepEvent onSample(uint32_t now_us, uint32_t pins);

    // The output lines at this moment.
    Outputs outputs(uint32_t now_us);

    // Whether RDATA may carry the track now: selected, a disk in, the motor
    // at speed, and the streamer's buffer holding the cylinder under the
    // head (`ready_cylinder`, from the loader; stale IDs would look like a
    // seek error).
    bool streams(uint32_t now_us, int ready_cylinder) const {
        return selected_ && disk_present_ && spindle_.atSpeed(now_us) && ready_cylinder == head_.cylinder();
    }

    // The disk's speed and how far the head can go (ibm::Format): the inner
    // stop never comes nearer than DEFAULT_LAST_CYLINDER, as on a real drive.
    void setGeometry(uint32_t period_us, unsigned last_cylinder) {
        spindle_.setPeriod(period_us);
        head_.setLastCylinder(uint8_t(last_cylinder > cfg_.last_cylinder ? (last_cylinder > 255 ? 255 : last_cylinder)
                                                                        : cfg_.last_cylinder));
    }

    void insertDisk(bool write_protected);
    void ejectDisk();
    // Protects the disk that's in, after a lost write (write_burst.h: lostWrite).
    void setWriteProtected(bool write_protected) { write_protected_ = write_protected; }

    const Config& config() const { return cfg_; }
    bool selected() const { return selected_; }
    bool diskPresent() const { return disk_present_; }
    bool diskChanged() const { return disk_changed_; }
    bool writeProtected() const { return write_protected_; }
    uint8_t side() const { return side_; }
    const head::Head& head() const { return head_; }
    rotation::Spindle& spindle() { return spindle_; }

private:
    Config cfg_;
    head::Head head_;
    rotation::Spindle spindle_;
    bool selected_ = false;
    bool step_was_asserted_ = false;
    bool motor_latched_ = false;  // Amiga: the motor line as it was at select
    uint8_t side_ = 0;
    bool disk_present_ = false;
    bool write_protected_ = true;
    bool disk_changed_ = true;  // a PC drive reports a change from power-on
};

}  // namespace drive
