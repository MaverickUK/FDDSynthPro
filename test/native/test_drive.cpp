// Tests for slice 1's logic: the head carriage, the spindle and the drive
// model, finished off by virtual hosts that seek and spin up the way their
// firmware does: a Greaseweazle (greaseweazle-firmware src/floppy.c) and the
// rig's Feather RP2040 + Floppy FeatherWing (Adafruit_Floppy).
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "check.h"
#include "drive.h"
#include "fdd_signals.h"
#include "head.h"
#include "rotation.h"
#include "virtual_hosts.h"

namespace {  // this file's own helpers and tests

// ---- Head -----------------------------------------------------------------

TEST(test_head_steps_and_stops) {
    head::Head h(83);
    CHECK(h.atTrack0());
    CHECK(h.step(false) == head::Step::AtOuterStop);
    CHECK_EQ(h.cylinder(), 0);
    CHECK(h.step(true) == head::Step::In);
    CHECK_EQ(h.cylinder(), 1);
    CHECK(!h.atTrack0());
    CHECK(h.step(false) == head::Step::Out);
    CHECK(h.atTrack0());
    for (int i = 0; i < 83; ++i) CHECK(h.step(true) == head::Step::In);
    CHECK_EQ(h.cylinder(), 83);
    CHECK(h.step(true) == head::Step::AtInnerStop);
    CHECK_EQ(h.cylinder(), 83);
}

TEST(test_head_start_position) {
    CHECK_EQ(head::Head(83, 40).cylinder(), 40);
    CHECK_EQ(head::Head(83, 200).cylinder(), 83);  // clamped to the inner stop
    CHECK_STR(head::stepName(head::Step::AtOuterStop), "OUTER_STOP");
}

// ---- Spindle ----------------------------------------------------------------

TEST(test_spindle_off_has_no_index) {
    rotation::Spindle s;
    CHECK(!s.motorOn());
    CHECK(!s.index(0));
    CHECK(!s.index(123456));
    CHECK_EQ(s.phaseUs(5000), 0u);
}

TEST(test_spindle_index_timing) {
    rotation::Spindle s;  // 200 ms period, 2 ms pulse, no spin-up
    s.setMotor(true, 1000);
    CHECK(s.index(1000));
    CHECK(s.index(2999));
    CHECK(!s.index(3000));
    CHECK(!s.index(200999));
    CHECK(s.index(201000));
    CHECK_EQ(s.phaseUs(201000 + 12345), 12345u);
    // Many revolutions later, still locked to the same phase.
    CHECK_EQ(s.phaseUs(1000 + 50 * 200000 + 7), 7u);
    // Setting the motor on again does not restart the revolution.
    s.setMotor(true, 1000 + 50 * 200000 + 100);
    CHECK_EQ(s.phaseUs(1000 + 50 * 200000 + 200), 200u);
}

TEST(test_spindle_spin_up_and_stop) {
    rotation::Config c;
    c.spin_up_us = 400000;
    rotation::Spindle s(c);
    s.setMotor(true, 0);
    CHECK(s.motorOn());
    CHECK(!s.atSpeed(399999));
    CHECK(!s.index(399999));
    CHECK(s.atSpeed(400000));
    CHECK(s.index(400000));
    s.setMotor(false, 500000);
    CHECK(!s.index(600000));
    // On again: a new spin-up, and the revolution starts afresh.
    s.setMotor(true, 700000);
    CHECK(!s.atSpeed(1099999));
    CHECK(s.index(1100000));
    CHECK_EQ(s.phaseUs(1100000 + 3000), 3000u);
}

TEST(test_spindle_across_the_timer_wrap) {
    rotation::Spindle s;
    uint32_t start = 0xFFFFFFFFu - 1000000u;  // a second before the 32-bit counter wraps
    s.setMotor(true, start);
    bool steady = true;
    // Two seconds in 50 ms steps, through the wrap.
    for (uint32_t i = 1; i <= 40; ++i) {
        uint32_t now = start + i * 50000u;
        if (s.phaseUs(now) != (i * 50000u) % 200000u) steady = false;
    }
    CHECK(steady);
}

TEST(test_spindle_runs_longer_than_the_timer) {
    // 75 minutes of motor-on, longer than the 71.6 minutes the 32-bit
    // counter covers, checked every 50 ms as the loop would (far more often).
    rotation::Spindle s;
    s.setMotor(true, 0);
    bool steady = true;
    uint64_t t = 0;
    for (uint32_t i = 0; i < 75u * 60u * 20u; ++i) {
        t += 50000;
        if (s.phaseUs(uint32_t(t)) != t % 200000u) steady = false;
    }
    CHECK(steady);
}

// ---- Drive ----------------------------------------------------------------

static uint32_t sample(std::initializer_list<unsigned> asserted_pins) {
    uint32_t s = fdd::INPUT_MASK;
    for (unsigned pin : asserted_pins) s &= ~fdd::maskOf(pin);
    return s;
}

TEST(test_drive_power_on_state) {
    drive::Drive d;
    d.onSample(0, sample({}));
    CHECK(!d.selected());
    CHECK(d.outputs(0) == drive::Outputs{});  // deselected: every line released
    d.onSample(10, sample({cfg::PIN_DRVSB}));
    drive::Outputs o = d.outputs(10);
    CHECK(o.trk00);
    CHECK(o.dskchg);
    CHECK(o.wpt);  // no disk
    CHECK(!o.index);
}

TEST(test_drive_answers_on_its_own_select_only) {
    drive::Drive d;
    d.onSample(0, sample({cfg::PIN_DRVSA, cfg::PIN_MOTEA}));
    CHECK(!d.selected());
    CHECK(!d.spindle().motorOn());
    drive::Config c;
    c.lines = {cfg::PIN_DRVSA, cfg::PIN_MOTEA};
    drive::Drive a(c);
    a.onSample(0, sample({cfg::PIN_DRVSA, cfg::PIN_MOTEA}));
    CHECK(a.selected());
    CHECK(a.spindle().motorOn());
}

TEST(test_bus_lines) {
    using bus::Cable;
    using bus::Interface;
    auto lines = [](Interface i, Cable c, uint8_t unit) { return bus::unitLines(bus::Setup{i, c, 1}, unit); };
    // A PC drive past the twist is the host's A on 12/16; B on a straight
    // cable is the same pair. The other pair (14/10) is A on a straight cable.
    bus::Lines pair_12_16{cfg::PIN_DRVSB, cfg::PIN_MOTEB}, pair_14_10{cfg::PIN_DRVSA, cfg::PIN_MOTEA};
    CHECK(bus::lines(bus::Setup{}) == pair_12_16);
    CHECK(lines(Interface::IbmPc, Cable::Twisted, 0) == pair_12_16);
    CHECK(lines(Interface::IbmPc, Cable::Twisted, 1) == pair_14_10);
    CHECK(lines(Interface::IbmPc, Cable::Straight, 0) == pair_14_10);
    CHECK(lines(Interface::IbmPc, Cable::Straight, 1) == pair_12_16);
    // Two drives on a PC: one on each pair.
    bus::Setup both{Interface::IbmPc, Cable::Twisted, 0b11};
    CHECK(bus::lines(both, 0) == pair_12_16);
    CHECK(bus::lines(both, 1) == pair_14_10);
    both.cable = Cable::Straight;
    CHECK(bus::lines(both, 0) == pair_14_10);
    CHECK(bus::lines(both, 1) == pair_12_16);
    // Shugart: DS0-DS2 on pins 10, 12 and 14, the motor on 16 for all; the cable doesn't matter.
    CHECK(lines(Interface::Shugart, Cable::Twisted, 0) == (bus::Lines{cfg::PIN_MOTEA, cfg::PIN_MOTEB}));
    CHECK(lines(Interface::Shugart, Cable::Straight, 1) == (bus::Lines{cfg::PIN_DRVSB, cfg::PIN_MOTEB}));
    CHECK(lines(Interface::Shugart, Cable::Twisted, 2) == (bus::Lines{cfg::PIN_DRVSA, cfg::PIN_MOTEB}));
    CHECK_STR(bus::unitName(bus::Setup{Interface::Shugart, Cable::Twisted, 1}, 2), "ds2");
    CHECK_STR(bus::unitName(bus::Setup{Interface::IbmPc, Cable::Twisted, 1}, 1), "b");
    char names[12];
    CHECK_STR(bus::unitsName(bus::Setup{Interface::Shugart, Cable::Twisted, 0b101}, names), "ds0,ds2");
}

TEST(test_shugart_drive_answers_on_its_ds_line) {
    // DS0 is pin 10, which a PC calls MOTEA: as a Shugart drive, that line selects.
    drive::Config c;
    c.lines = bus::lines(bus::Setup{bus::Interface::Shugart, bus::Cable::Twisted, 0b001});
    c.interface = bus::Interface::Shugart;
    drive::Drive d(c);
    d.insertDisk(false);
    d.onSample(0, sample({cfg::PIN_DRVSB, cfg::PIN_MOTEB}));  // DS1 selected: not us
    CHECK(!d.selected());
    CHECK(d.spindle().motorOn());  // MOTOR ON runs every drive's motor
    d.onSample(10, sample({cfg::PIN_MOTEA, cfg::PIN_MOTEB, cfg::PIN_STEP, cfg::PIN_DIR}));
    CHECK(d.selected());
    CHECK_EQ(d.head().cylinder(), 1);
}

TEST(test_shugart_pin_34_is_ready) {
    drive::Config c;
    c.lines = bus::lines(bus::Setup{bus::Interface::Shugart, bus::Cable::Twisted, 0b010});
    c.interface = bus::Interface::Shugart;
    c.spindle.spin_up_us = 1000;
    drive::Drive d(c);
    d.onSample(0, sample({cfg::PIN_DRVSB, cfg::PIN_MOTEB}));
    // No disk: not ready, though DISK CHANGE is set, which a PC would see on pin 34.
    drive::Outputs o = d.outputs(5000);
    CHECK(!o.ready);
    CHECK(o.dskchg);
    CHECK(!fdd::asserted(drive::outputPins(o, bus::Interface::Shugart), cfg::PIN_DSKCHG));
    CHECK(fdd::asserted(drive::outputPins(o, bus::Interface::IbmPc), cfg::PIN_DSKCHG));
    d.insertDisk(false);
    d.onSample(6000, sample({cfg::PIN_DRVSB}));  // motor off
    d.onSample(7000, sample({cfg::PIN_DRVSB, cfg::PIN_MOTEB}));
    CHECK(!d.outputs(7500).ready);  // spinning up
    o = d.outputs(8000);
    CHECK(o.ready);
    CHECK(fdd::asserted(drive::outputPins(o, bus::Interface::Shugart), cfg::PIN_DSKCHG));
    d.onSample(9000, sample({cfg::PIN_MOTEB}));  // deselected: released
    CHECK(!fdd::asserted(drive::outputPins(d.outputs(9000), bus::Interface::Shugart), cfg::PIN_DSKCHG));
}

TEST(test_drive_steps_once_per_assertion_and_only_when_selected) {
    drive::Drive d;
    // Not selected: ignored.
    CHECK(!d.onSample(0, sample({cfg::PIN_STEP, cfg::PIN_DIR})).stepped);
    d.onSample(10, sample({}));
    // Selected, DIR asserted: one step in, however long STEP is held.
    drive::StepEvent e = d.onSample(20, sample({cfg::PIN_DRVSB, cfg::PIN_DIR, cfg::PIN_STEP}));
    CHECK(e.stepped);
    CHECK(e.outcome == head::Step::In);
    CHECK_EQ(e.cylinder, 1);
    CHECK(!d.onSample(25, sample({cfg::PIN_DRVSB, cfg::PIN_DIR, cfg::PIN_STEP})).stepped);
    d.onSample(35, sample({cfg::PIN_DRVSB, cfg::PIN_DIR}));
    CHECK(!d.outputs(35).trk00);
    // DIR released: outwards.
    e = d.onSample(50, sample({cfg::PIN_DRVSB, cfg::PIN_STEP}));
    CHECK(e.outcome == head::Step::Out);
    CHECK(d.outputs(50).trk00);
    // Selecting while STEP is already held is not a step.
    drive::Drive d2;
    d2.onSample(0, sample({cfg::PIN_STEP}));
    CHECK(!d2.onSample(5, sample({cfg::PIN_DRVSB, cfg::PIN_STEP})).stepped);
}

TEST(test_drive_side) {
    drive::Drive d;
    d.onSample(0, sample({cfg::PIN_DRVSB}));
    CHECK_EQ(d.side(), 0);
    d.onSample(1, sample({cfg::PIN_DRVSB, cfg::PIN_SIDE1}));
    CHECK_EQ(d.side(), 1);
}

TEST(test_drive_disk_change) {
    drive::Drive d;
    uint32_t sel = sample({cfg::PIN_DRVSB});
    uint32_t step = sample({cfg::PIN_DRVSB, cfg::PIN_STEP});
    d.onSample(0, sel);
    // No disk: a step doesn't clear it.
    d.onSample(10, step);
    d.onSample(20, sel);
    CHECK(d.outputs(20).dskchg);
    // Disk in: still set until the next step.
    d.insertDisk(true);
    CHECK(d.outputs(30).dskchg);
    d.onSample(40, step);
    d.onSample(50, sel);
    CHECK(!d.outputs(50).dskchg);
    CHECK(d.outputs(50).wpt);
    // Eject: set again, and WPT asserted with no disk.
    d.ejectDisk();
    CHECK(d.outputs(60).dskchg);
    d.insertDisk(false);
    CHECK(!d.outputs(70).wpt);
}

TEST(test_drive_index_needs_motor_disk_and_select) {
    drive::Drive d;
    d.insertDisk(true);
    // Motor on while deselected: the spindle turns, but the line is released.
    d.onSample(1000, sample({cfg::PIN_MOTEB}));
    CHECK(d.spindle().motorOn());
    CHECK(!d.outputs(1000).index);
    d.onSample(200500, sample({cfg::PIN_MOTEB, cfg::PIN_DRVSB}));
    CHECK(d.outputs(201000).index);   // the next revolution's pulse
    CHECK(!d.outputs(204000).index);
    d.ejectDisk();
    CHECK(!d.outputs(401000).index);  // no disk, no index hole
    d.insertDisk(true);
    d.onSample(500000, sample({cfg::PIN_DRVSB}));  // motor off
    CHECK(!d.outputs(601000).index);
}

TEST(test_output_pins_are_active_low) {
    CHECK_EQ(drive::outputPins(drive::Outputs{}), fdd::OUTPUT_MASK);  // all released
    drive::Outputs o;
    o.trk00 = true;
    o.dskchg = true;
    uint32_t s = drive::outputPins(o);
    CHECK(fdd::asserted(s, cfg::PIN_TRK00));
    CHECK(fdd::asserted(s, cfg::PIN_DSKCHG));
    CHECK(!fdd::asserted(s, cfg::PIN_INDEX));
    CHECK(!fdd::asserted(s, cfg::PIN_WPT));
    CHECK(!fdd::asserted(s, cfg::PIN_RDATA));
    CHECK_EQ(s & ~fdd::OUTPUT_MASK, 0u);
}

static unsigned count(const std::vector<drive::StepEvent>& steps, head::Step outcome) {
    unsigned n = 0;
    for (const auto& e : steps) n += e.outcome == outcome;
    return n;
}

TEST(test_gw_first_seek_from_track0) {
    drive::Drive d;
    VirtualGreaseweazle gw(d);
    gw.select(true);
    CHECK(gw.seek(2));
    CHECK_EQ(d.head().cylinder(), 2);
    CHECK_EQ(gw.steps.size(), 2u);  // TRK00 seen at once: no steps out
    CHECK_EQ(count(gw.steps, head::Step::AtOuterStop), 0u);
}

TEST(test_gw_first_seek_from_mid_disk) {
    drive::Config c;
    c.start_cylinder = 50;
    drive::Drive d(c);
    VirtualGreaseweazle gw(d);
    gw.select(true);
    CHECK(gw.seek(10));
    CHECK_EQ(d.head().cylinder(), 10);
    CHECK_EQ(count(gw.steps, head::Step::Out), 50u);
    CHECK_EQ(count(gw.steps, head::Step::In), 10u);
    CHECK_EQ(count(gw.steps, head::Step::AtOuterStop), 0u);  // stopped as soon as TRK00 came
}

TEST(test_gw_seeks_every_cylinder_and_back) {
    drive::Drive d;
    VirtualGreaseweazle gw(d);
    gw.select(true);
    bool agree = true;
    for (int round = 0; round < 3; ++round) {
        for (int cyl = 0; cyl < 80; ++cyl) {
            CHECK(gw.seek(cyl));
            if (d.head().cylinder() != cyl || gw.trk0() != (cyl == 0)) agree = false;
        }
        for (int cyl = 79; cyl >= 0; --cyl) {
            gw.seek(cyl);
            if (d.head().cylinder() != cyl || gw.trk0() != (cyl == 0)) agree = false;
        }
    }
    CHECK(agree);
}

TEST(test_gw_ignored_when_it_selects_the_other_drive) {
    drive::Drive d;  // answers as B
    VirtualGreaseweazle gw(d, cfg::PIN_DRVSA, cfg::PIN_MOTEA);
    gw.select(true);
    CHECK(!gw.seek(5));  // no TRK00 seen in 256 steps: ACK_NO_TRK0
    CHECK(gw.steps.empty());
}

TEST(test_gw_noclick_step_knocks_once_at_track0) {
    drive::Drive d;
    VirtualGreaseweazle gw(d);
    gw.select(true);
    gw.noclickStep();
    CHECK_EQ(gw.steps.size(), 1u);
    CHECK(gw.steps[0].outcome == head::Step::AtOuterStop);
    CHECK(gw.trk0());
}

TEST(test_gw_rpm) {
    drive::Drive d;
    d.insertDisk(true);
    VirtualGreaseweazle gw(d);
    gw.select(true);
    gw.motor(true);
    std::vector<uint32_t> periods = gw.indexPeriods(5);
    CHECK_EQ(periods.size(), 5u);
    bool exact = true;
    for (uint32_t p : periods) exact = exact && p == 200000u;  // 300.0 RPM
    CHECK(exact);
}

TEST(test_gw_rpm_without_disk_sees_no_index) {
    drive::Drive d;
    VirtualGreaseweazle gw(d);
    gw.select(true);
    gw.motor(true);
    CHECK(gw.indexPeriods(2).empty());
}

TEST(test_adafruit_first_seek_from_mid_disk) {
    drive::Config c;
    c.start_cylinder = 50;
    drive::Drive d(c);
    VirtualAdafruitFloppy host(d);
    host.select(true);
    CHECK(host.gotoTrack(10));
    CHECK_EQ(d.head().cylinder(), 10);
    CHECK_EQ(count(host.steps, head::Step::Out), 50u);
    CHECK_EQ(count(host.steps, head::Step::In), 10u);
    CHECK_EQ(count(host.steps, head::Step::AtOuterStop), 0u);
}

TEST(test_adafruit_long_step_pulses_count_once) {
    // 13 ms assertions: one step each, not one per sample while held.
    drive::Drive d;
    VirtualAdafruitFloppy host(d);
    host.select(true);
    CHECK(host.gotoTrack(79));
    CHECK_EQ(host.steps.size(), 79u);
    CHECK_EQ(d.head().cylinder(), 79);
}

TEST(test_adafruit_seeks_every_cylinder_and_back) {
    drive::Drive d;
    VirtualAdafruitFloppy host(d);
    host.select(true);
    bool agree = true;
    for (int cyl = 0; cyl < 80; ++cyl) {
        CHECK(host.gotoTrack(cyl));
        if (d.head().cylinder() != cyl || host.trk0() != (cyl == 0)) agree = false;
    }
    for (int cyl = 79; cyl >= 0; --cyl) {
        host.gotoTrack(cyl);
        if (d.head().cylinder() != cyl || host.trk0() != (cyl == 0)) agree = false;
    }
    CHECK(agree);
    // Seeking to 0 recalibrates, but checks TRK00 first: never a knock.
    CHECK_EQ(count(host.steps, head::Step::AtOuterStop), 0u);
    // It never asks for more than 79: no knock at the inner stop either.
    CHECK_EQ(count(host.steps, head::Step::AtInnerStop), 0u);
}

TEST(test_adafruit_cannot_reach_a_drive_on_other_pins) {
    drive::Config c;
    c.lines = {cfg::PIN_DRVSA, cfg::PIN_MOTEA};
    drive::Drive d(c);
    VirtualAdafruitFloppy host(d);  // only ever pins 12/16
    host.select(true);
    CHECK(!host.gotoTrack(5));
    CHECK(host.steps.empty());
}

TEST(test_adafruit_motor_sees_index) {
    drive::Drive d;
    d.insertDisk(true);
    VirtualAdafruitFloppy host(d);
    host.select(true);
    CHECK(host.motorOn());
    std::vector<uint32_t> periods = host.indexPeriods(3);
    CHECK_EQ(periods.size(), 3u);
    drive::Drive empty;
    VirtualAdafruitFloppy host2(empty);
    host2.select(true);
    CHECK(!host2.motorOn());  // no disk: no index, so it reports a timeout
}

}  // namespace

void runDriveTests() {
    RUN(test_head_steps_and_stops);
    RUN(test_head_start_position);
    RUN(test_spindle_off_has_no_index);
    RUN(test_spindle_index_timing);
    RUN(test_spindle_spin_up_and_stop);
    RUN(test_spindle_across_the_timer_wrap);
    RUN(test_spindle_runs_longer_than_the_timer);
    RUN(test_drive_power_on_state);
    RUN(test_drive_answers_on_its_own_select_only);
    RUN(test_bus_lines);
    RUN(test_shugart_drive_answers_on_its_ds_line);
    RUN(test_shugart_pin_34_is_ready);
    RUN(test_drive_steps_once_per_assertion_and_only_when_selected);
    RUN(test_drive_side);
    RUN(test_drive_disk_change);
    RUN(test_drive_index_needs_motor_disk_and_select);
    RUN(test_output_pins_are_active_low);
    RUN(test_gw_first_seek_from_track0);
    RUN(test_gw_first_seek_from_mid_disk);
    RUN(test_gw_seeks_every_cylinder_and_back);
    RUN(test_gw_ignored_when_it_selects_the_other_drive);
    RUN(test_gw_noclick_step_knocks_once_at_track0);
    RUN(test_gw_rpm);
    RUN(test_gw_rpm_without_disk_sees_no_index);
    RUN(test_adafruit_first_seek_from_mid_disk);
    RUN(test_adafruit_long_step_pulses_count_once);
    RUN(test_adafruit_seeks_every_cylinder_and_back);
    RUN(test_adafruit_cannot_reach_a_drive_on_other_pins);
    RUN(test_adafruit_motor_sees_index);
}
