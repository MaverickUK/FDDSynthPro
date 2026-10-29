// Virtual hosts: they drive the drive model the way real firmware drives a
// cable, setting pins at given times and reading the outputs back. Each
// follows its firmware's source, so the drive meets their exact timings
// before any hardware does. Used by test_drive.cpp and test_activity.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "config.h"
#include "drive.h"
#include "fdd_signals.h"

// The cable can carry more than one drive (the emulator as A and B): each
// sees every pin change, and the host reads the lines they share, where
// each releases its own while deselected.
class VirtualHost {
public:
    VirtualHost(drive::Drive& d, unsigned select_pin, unsigned motor_pin)
        : VirtualHost(std::vector<drive::Drive*>{&d}, select_pin, motor_pin) {}
    VirtualHost(std::vector<drive::Drive*> drives, unsigned select_pin, unsigned motor_pin)
        : drives_(std::move(drives)), select_(select_pin), motor_(motor_pin) {
        for (drive::Drive* d : drives_) d->onSample(now, pins_);
    }

    uint32_t now = 1000;
    std::vector<drive::StepEvent> steps;  // every step the drive saw
    int cylinder = -1;                    // where the host thinks the head is (-1: unknown)
    // Called after every sample the drive takes, as core 1's loop would.
    std::function<void(uint32_t now, const drive::StepEvent&)> on_sample;

    bool trk0() { return cable().trk00; }
    uint32_t pins() const { return pins_; }
    bool index() { return cable().index; }

    // The output lines as the host sees them: asserted by whichever drive asserts them.
    drive::Outputs cable() {
        drive::Outputs o;
        for (drive::Drive* d : drives_) {
            drive::Outputs each = d->outputs(now);
            o.index = o.index || each.index;
            o.trk00 = o.trk00 || each.trk00;
            o.wpt = o.wpt || each.wpt;
            o.dskchg = o.dskchg || each.dskchg;
            o.ready = o.ready || each.ready;
        }
        return o;
    }

    // The time between index pulses, as `gw rpm` measures it, sampled every
    // 10 us for `revolutions` + 1 pulses. Empty if no pulses came.
    std::vector<uint32_t> indexPeriods(int revolutions) {
        std::vector<uint32_t> starts;
        bool was = index();
        for (uint32_t t = 0; t < uint32_t(revolutions + 2) * 220000u && int(starts.size()) <= revolutions;
             t += 10) {
            wait(10);
            bool is = index();
            if (is && !was) starts.push_back(now);
            was = is;
        }
        std::vector<uint32_t> periods;
        for (size_t i = 1; i < starts.size(); ++i) periods.push_back(starts[i] - starts[i - 1]);
        return periods;
    }

protected:
    void set(unsigned pin, bool asserted) {
        if (asserted)
            pins_ &= ~fdd::maskOf(pin);
        else
            pins_ |= fdd::maskOf(pin);
        drive::StepEvent e;
        for (drive::Drive* d : drives_) {
            drive::StepEvent each = d->onSample(now, pins_);
            if (each.stepped) e = each;  // only the selected drive steps
        }
        if (e.stepped) steps.push_back(e);
        if (on_sample) on_sample(now, e);
    }
    void wait(uint32_t us) { now += us; }

    // The drive that answers on the host's select line, for where its disk
    // is under the head (a test's shortcut: a host would time it from INDEX).
    drive::Drive& selectedDrive() {
        for (drive::Drive* d : drives_)
            if (d->config().lines.select == select_) return *d;
        return *drives_[0];
    }

    std::vector<drive::Drive*> drives_;
    unsigned select_, motor_;
    uint32_t pins_ = fdd::INPUT_MASK;
};

// A Greaseweazle: greaseweazle-firmware's src/floppy.c (factory delays) on
// the IBM PC bus. Drive A is pins 14/10, drive B pins 12/16. DIR is set 10 us
// before a step, STEP is asserted for 15 us, then the step delay (10 ms)
// passes. The first seek looks for track 0 by checking TRK00 before each
// step outwards, up to 256 steps.
class VirtualGreaseweazle : public VirtualHost {
public:
    explicit VirtualGreaseweazle(drive::Drive& d, unsigned select_pin = cfg::PIN_DRVSB,
                                 unsigned motor_pin = cfg::PIN_MOTEB)
        : VirtualHost(d, select_pin, motor_pin) {}

    static constexpr uint32_t STEP_DELAY_US = 10000;
    static constexpr uint32_t SETTLE_US = 15000;
    static constexpr uint32_t MOTOR_DELAY_US = 750000;

    void select(bool on) {
        set(select_, on);
        wait(10);
    }
    void motor(bool on) {
        set(motor_, on);
        if (on) wait(MOTOR_DELAY_US);
    }

    // floppy_seek(): returns false where the firmware returns ACK_NO_TRK0.
    bool seek(int cyl) {
        if (cylinder < 0) {
            dir(false);
            int nr = 0;
            for (; nr < 256 && !trk0(); ++nr) stepOnce();
            wait(SETTLE_US);
            if (!trk0()) return false;
            cylinder = 0;
        }
        if (cyl == cylinder) return true;
        dir(cyl > cylinder);
        for (int nr = cyl > cylinder ? cyl - cylinder : cylinder - cyl; nr > 0; --nr) stepOnce();
        wait(SETTLE_US);
        cylinder = cyl;
        return true;
    }

    // floppy_noclick_step(): seek to 0, step out once, and step back in if
    // TRK00 went away (a drive that really moved).
    void noclickStep() {
        seek(0);
        dir(false);
        stepOnce();
        if (!trk0()) {
            wait(SETTLE_US);
            dir(true);
            stepOnce();
            wait(SETTLE_US);
        }
    }

private:
    void dir(bool in) {
        set(cfg::PIN_DIR, in);
        wait(10);
    }
    void stepOnce() {
        set(cfg::PIN_STEP, true);
        wait(15);
        set(cfg::PIN_STEP, false);
        wait(STEP_DELAY_US);
    }
};

// A Feather RP2040 + Floppy FeatherWing running Adafruit's Greaseweazle-
// compatible firmware (Adafruit_Floppy 0.6.x, src/Adafruit_Floppy.cpp). It
// has one select and one motor line, on pins 12 and 16 (`gw --drive A`).
// A step holds STEP released 13 ms, asserted 13 ms, then waits another
// 13 ms after the last one. Every seek to cylinder 0, and the first seek of
// all, looks for track 0: up to 100 steps out checking TRK00 first, then up
// to 20 steps in. Seeks stop at cylinder 79. Motor on waits 1 s, then up to
// 1 s for an index pulse to come and go.
class VirtualAdafruitFloppy : public VirtualHost {
public:
    explicit VirtualAdafruitFloppy(drive::Drive& d) : VirtualHost(d, cfg::PIN_DRVSB, cfg::PIN_MOTEB) {}

    static constexpr uint32_t STEP_HALF_US = 13000;  // delay(step_delay_us / 1000 + 1) ms
    static constexpr uint32_t SETTLE_US = 15000;
    static constexpr uint32_t MOTOR_DELAY_US = 1000000;

    void select(bool on) {
        set(select_, on);
        wait(10);
    }

    // spin_motor(true): true if an index pulse came and went within 1 s.
    bool motorOn() {
        set(motor_, true);
        wait(MOTOR_DELAY_US);
        uint32_t start = now;
        while (!index()) {
            if (now - start > 1000000) return false;
            wait(10);
        }
        while (index()) {
            if (now - start > 1000000) return false;
            wait(10);
        }
        return true;
    }

    // goto_track(): false where it couldn't find track 0.
    bool gotoTrack(int track) {
        if (cylinder < 0 || track == 0) {
            int max_steps = 100;
            while (max_steps--) {
                if (trk0()) {
                    cylinder = 0;
                    break;
                }
                step(false, 1);
            }
            if (!trk0()) {
                max_steps = 20;
                while (max_steps--) {
                    if (trk0()) {
                        cylinder = 0;
                        break;
                    }
                    step(true, 1);
                }
                if (!trk0()) {
                    cylinder = -1;
                    return false;
                }
            }
        }
        wait(SETTLE_US);
        if (track > 79) track = 79;
        if (cylinder == track) return true;
        int n = track - cylinder;
        step(n > 0, n > 0 ? n : -n);
        wait(SETTLE_US);
        cylinder = track;
        return true;
    }

private:
    // step(): STEP rests released (high); each step asserts it for 13 ms.
    void step(bool in, int times) {
        set(cfg::PIN_DIR, in);
        wait(10);
        while (times--) {
            set(cfg::PIN_STEP, false);
            wait(STEP_HALF_US);
            set(cfg::PIN_STEP, true);
            wait(STEP_HALF_US);
            set(cfg::PIN_STEP, false);
        }
        wait(STEP_HALF_US);
    }
};
