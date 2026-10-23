// Tests for slice 4: the drive observer and the sound-event tracker, first
// on their own, then fed by the virtual hosts through the drive model.
#include <cstdint>
#include <string>
#include <vector>

#include "activity.h"
#include "check.h"
#include "drive.h"
#include "virtual_hosts.h"

using activity::DriveEvent;
using activity::Input;
using activity::Sound;

namespace {  // this file's own helpers and tests

// ---- Helpers ----------------------------------------------------------------------

static DriveEvent stepAt(uint32_t t, head::Step outcome, uint8_t cylinder) {
    return DriveEvent{t, Input::Step, outcome, cylinder};
}

static std::vector<activity::Event> drain(activity::Tracker& tr) {
    std::vector<activity::Event> out;
    activity::Event e;
    while (tr.pop(e)) out.push_back(e);
    return out;
}

static std::vector<std::string> lines(const std::vector<activity::Event>& events) {
    std::vector<std::string> out;
    for (const auto& e : events) {
        char buf[64];
        size_t n = activity::format(e, buf, sizeof buf);
        out.emplace_back(buf, n ? n - 1 : 0);  // without the newline
    }
    return out;
}

// The moving steps the events account for. A seek's first step is also a
// click, and its SEEK_END count includes that step, so it counts once.
static unsigned movingSteps(const std::vector<activity::Event>& events) {
    unsigned n = 0;
    for (const auto& e : events) {
        if (e.sound == Sound::Step) ++n;
        if (e.sound == Sound::SeekEnd) n += e.steps - 1u;
    }
    return n;
}

// ---- Tracker on its own -----------------------------------------------------------

TEST(test_tracker_single_steps_click) {
    activity::Tracker tr;
    tr.onDriveEvent(stepAt(1000, head::Step::In, 1));
    tr.onDriveEvent(stepAt(31000, head::Step::In, 2));  // 30 ms later: separate
    tr.update(100000);
    auto ev = drain(tr);
    CHECK_EQ(ev.size(), 2u);
    CHECK(ev[0].sound == Sound::Step && ev[1].sound == Sound::Step);
    CHECK_EQ(tr.clicks(), 2u);
    CHECK(!tr.seeking());
}

TEST(test_tracker_close_steps_are_a_seek) {
    activity::Tracker tr;
    for (uint32_t i = 0; i < 10; ++i) tr.onDriveEvent(stepAt(1000 + i * 3000, head::Step::Out, uint8_t(9 - i)));
    CHECK(tr.seeking());
    tr.update(1000 + 9 * 3000 + 20000);  // exactly the gap: not over yet
    CHECK(tr.seeking());
    tr.update(1000 + 9 * 3000 + 20001);
    CHECK(!tr.seeking());
    auto ev = lines(drain(tr));
    CHECK_EQ(ev.size(), 3u);
    CHECK_STR(ev[0], "ACT 1000 STEP OUT 9");
    CHECK_STR(ev[1], "ACT 4000 SEEK_START OUT 8");
    CHECK_STR(ev[2], "ACT 28000 SEEK_END 0 10");  // timed at its last step
}

TEST(test_tracker_reversal_ends_a_seek) {
    activity::Tracker tr;
    tr.onDriveEvent(stepAt(0, head::Step::In, 1));
    tr.onDriveEvent(stepAt(3000, head::Step::In, 2));
    tr.onDriveEvent(stepAt(6000, head::Step::In, 3));
    tr.onDriveEvent(stepAt(9000, head::Step::Out, 2));  // close, but back the other way
    auto ev = lines(drain(tr));
    CHECK_EQ(ev.size(), 4u);
    CHECK_STR(ev[2], "ACT 6000 SEEK_END 3 3");
    CHECK_STR(ev[3], "ACT 9000 STEP OUT 2");
}

TEST(test_tracker_bumps) {
    activity::Tracker tr;
    tr.onDriveEvent(stepAt(0, head::Step::Out, 1));
    tr.onDriveEvent(stepAt(3000, head::Step::Out, 0));
    tr.onDriveEvent(stepAt(6000, head::Step::AtOuterStop, 0));  // ends the seek
    tr.onDriveEvent(stepAt(9000, head::Step::AtOuterStop, 0));
    tr.onDriveEvent(stepAt(500000, head::Step::AtInnerStop, 83));
    auto ev = lines(drain(tr));
    CHECK_EQ(ev.size(), 6u);
    CHECK_STR(ev[2], "ACT 6000 SEEK_END 0 2");
    CHECK_STR(ev[3], "ACT 6000 BUMP OUTER");
    CHECK_STR(ev[4], "ACT 9000 BUMP OUTER");
    CHECK_STR(ev[5], "ACT 500000 BUMP INNER");
    CHECK_EQ(tr.bumps(), 3u);
    // A move after bumps clicks: bumps don't start a seek.
    tr.onDriveEvent(stepAt(503000, head::Step::Out, 82));
    CHECK(drain(tr)[0].sound == Sound::Step);
}

TEST(test_tracker_motor_and_disk) {
    activity::Tracker tr;
    tr.onDriveEvent(DriveEvent{10, Input::MotorOn, head::Step::In, 0});
    tr.onDriveEvent(DriveEvent{20, Input::MotorOn, head::Step::In, 0});  // no change: no event
    tr.onDriveEvent(DriveEvent{30, Input::Eject, head::Step::In, 0});
    tr.onDriveEvent(DriveEvent{40, Input::Insert, head::Step::In, 0});
    tr.onDriveEvent(DriveEvent{50, Input::MotorOff, head::Step::In, 0});
    auto ev = lines(drain(tr));
    CHECK_EQ(ev.size(), 4u);
    CHECK_STR(ev[0], "ACT 10 MOTOR_ON");
    CHECK_STR(ev[1], "ACT 30 EJECT");
    CHECK_STR(ev[2], "ACT 40 INSERT");
    CHECK_STR(ev[3], "ACT 50 MOTOR_OFF");
    CHECK(!tr.motorOn());
}

TEST(test_tracker_queue_keeps_the_newest) {
    activity::Tracker tr;
    for (uint32_t i = 0; i < 40; ++i) tr.onDriveEvent(stepAt(i * 100000, head::Step::In, uint8_t(i)));
    auto ev = drain(tr);
    CHECK_EQ(ev.size(), 32u);
    CHECK_EQ(tr.dropped(), 8u);
    CHECK_EQ(ev.back().cylinder, 39);
}

TEST(test_format_truncates) {
    char buf[8];
    size_t n = activity::format(activity::Event{123456, Sound::MotorOn, false, 0, 0}, buf, sizeof buf);
    CHECK_EQ(n, 7u);
    CHECK_EQ(buf[7], '\0');
}

// ---- The observer --------------------------------------------------------------------

TEST(test_observer_reports_changes_only) {
    drive::Drive d;
    d.insertDisk(true);
    activity::DriveObserver obs;
    DriveEvent out[3];
    CHECK_EQ(obs.observe(0, d, drive::StepEvent{}, out), 0u);  // power-on state: nothing
    d.onSample(10, fdd::INPUT_MASK & ~fdd::maskOf(cfg::PIN_MOTEB));
    CHECK_EQ(obs.observe(10, d, drive::StepEvent{}, out), 1u);
    CHECK(out[0].input == Input::MotorOn);
    CHECK_EQ(obs.observe(11, d, drive::StepEvent{}, out), 0u);
    d.ejectDisk();
    drive::StepEvent step{true, head::Step::In, 1};
    CHECK_EQ(obs.observe(12, d, step, out), 2u);
    CHECK(out[0].input == Input::Eject);
    CHECK(out[1].input == Input::Step);
    CHECK_EQ(out[1].cylinder, 1);
}

// ---- Fed by the virtual hosts ----------------------------------------------------------

// A drive, a host and the activity pipeline, wired as core 1 and core 0 are:
// the events are taken off the tracker's queue as they come.
template <typename Host>
struct Pipeline {
    explicit Pipeline(drive::Config c = {}) : d(c), host(d) {
        d.insertDisk(true);
        host.on_sample = [this](uint32_t now, const drive::StepEvent& e) {
            DriveEvent out[3];
            unsigned n = obs.observe(now, d, e, out);
            for (unsigned i = 0; i < n; ++i) tr.onDriveEvent(out[i]);
            for (const auto& ev : drain(tr)) events.push_back(ev);
        };
    }
    std::vector<activity::Event> finish() {
        tr.update(host.now + 1000000);
        for (const auto& ev : drain(tr)) events.push_back(ev);
        CHECK_EQ(tr.dropped(), 0u);
        return events;
    }

    drive::Drive d;
    Host host;
    activity::DriveObserver obs;
    activity::Tracker tr;
    std::vector<activity::Event> events;
};

static unsigned countOf(const std::vector<activity::Event>& ev, Sound s) {
    unsigned n = 0;
    for (const auto& e : ev) n += e.sound == s;
    return n;
}

TEST(test_greaseweazle_seek_is_one_seek) {
    // 10 ms steps: a seek across the disk is one hum.
    Pipeline<VirtualGreaseweazle> p;
    p.host.select(true);
    CHECK(p.host.seek(79));
    auto ev = p.finish();
    CHECK_EQ(ev.size(), 3u);
    auto l = lines(ev);
    CHECK(l[0].find("STEP IN 1") != std::string::npos);
    CHECK(l[1].find("SEEK_START IN 2") != std::string::npos);
    CHECK(l[2].find("SEEK_END 79 79") != std::string::npos);
    CHECK_EQ(movingSteps(ev), 79u);
}

TEST(test_feather_steps_click) {
    // 26 ms steps: slow enough for every step to be its own click.
    Pipeline<VirtualAdafruitFloppy> p;
    p.host.select(true);
    CHECK(p.host.gotoTrack(10));
    auto ev = p.finish();
    CHECK_EQ(countOf(ev, Sound::Step), 10u);
    CHECK_EQ(countOf(ev, Sound::SeekStart), 0u);
    CHECK_EQ(movingSteps(ev), 10u);
}

TEST(test_feather_recalibrate_from_mid_disk) {
    drive::Config c;
    c.start_cylinder = 50;
    Pipeline<VirtualAdafruitFloppy> p(c);
    p.host.select(true);
    CHECK(p.host.gotoTrack(10));
    auto ev = p.finish();
    CHECK_EQ(movingSteps(ev), 60u);  // 50 out, 10 in
    CHECK_EQ(countOf(ev, Sound::Bump), 0u);
}

TEST(test_noclick_step_bumps) {
    Pipeline<VirtualGreaseweazle> p;
    p.host.select(true);
    p.host.noclickStep();
    auto ev = p.finish();
    CHECK_EQ(ev.size(), 1u);
    CHECK_STR(lines(ev)[0].substr(lines(ev)[0].find("BUMP")), "BUMP OUTER");
}

TEST(test_motor_on_and_off) {
    Pipeline<VirtualGreaseweazle> p;
    p.host.select(true);
    p.host.motor(true);
    p.host.motor(false);
    auto ev = p.finish();
    CHECK_EQ(ev.size(), 2u);
    CHECK(ev[0].sound == Sound::MotorOn);
    CHECK(ev[1].sound == Sound::MotorOff);
    CHECK_EQ(ev[1].time_us - ev[0].time_us, VirtualGreaseweazle::MOTOR_DELAY_US);
}

TEST(test_blind_recalibrate_grinds) {
    // A host that steps out 80 times without looking at TRK00, from
    // cylinder 5, at 3 ms: a short seek, then the head hammering the stop.
    drive::Config c;
    c.start_cylinder = 5;
    drive::Drive d(c);
    activity::DriveObserver obs;
    activity::Tracker tr;
    std::vector<activity::Event> ev;
    uint32_t t = 0;
    uint32_t pins = fdd::INPUT_MASK & ~fdd::maskOf(cfg::PIN_DRVSB);
    auto sample = [&](uint32_t p) {
        drive::StepEvent e = d.onSample(t, p);
        DriveEvent out[3];
        unsigned n = obs.observe(t, d, e, out);
        for (unsigned i = 0; i < n; ++i) tr.onDriveEvent(out[i]);
        for (const auto& x : drain(tr)) ev.push_back(x);
    };
    sample(pins);
    for (int i = 0; i < 80; ++i) {
        sample(pins & ~fdd::maskOf(cfg::PIN_STEP));
        t += 15;
        sample(pins);
        t += 3000;
    }
    tr.update(t + 1000000);
    for (const auto& x : drain(tr)) ev.push_back(x);
    CHECK_EQ(movingSteps(ev), 5u);
    CHECK_EQ(countOf(ev, Sound::SeekEnd), 1u);
    CHECK_EQ(countOf(ev, Sound::Bump), 75u);
    CHECK_EQ(tr.dropped(), 0u);
    // The seek ends at the first bump, where the head stopped moving.
    CHECK(ev[2].sound == Sound::SeekEnd && ev[3].sound == Sound::Bump);
    CHECK_EQ(ev[2].time_us, ev[3].time_us);
}

}  // namespace

void runActivityTests() {
    RUN(test_tracker_single_steps_click);
    RUN(test_tracker_close_steps_are_a_seek);
    RUN(test_tracker_reversal_ends_a_seek);
    RUN(test_tracker_bumps);
    RUN(test_tracker_motor_and_disk);
    RUN(test_tracker_queue_keeps_the_newest);
    RUN(test_format_truncates);
    RUN(test_observer_reports_changes_only);
    RUN(test_greaseweazle_seek_is_one_seek);
    RUN(test_feather_steps_click);
    RUN(test_feather_recalibrate_from_mid_disk);
    RUN(test_noclick_step_bumps);
    RUN(test_motor_on_and_off);
    RUN(test_blind_recalibrate_grinds);
}
