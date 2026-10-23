// What the drive is doing, as the sound events the FDD Synth plays. Pure
// C++, host-tested.
//
// The original FDD Synth listens to the cable and has to guess. This drive
// knows: when the motor starts and stops, every step and where it took the
// head, when the head knocks against an end stop, and when a disk goes in or
// out. Two parts:
//   - DriveObserver (core 1) turns changes in the drive model into
//     DriveEvents, which cross to core 0 on a ring.
//   - Tracker (core 0) turns those into sound events: a click per step, or
//     a seek (a run of steps closer together than seek_gap_us) with a start
//     and an end, a bump at an end stop, and motor and disk events.
//
// Sound events, as the trace prints them (activity::format):
//   ACT <t_us> MOTOR_ON | MOTOR_OFF
//   ACT <t_us> STEP IN|OUT <cylinder>          one click
//   ACT <t_us> SEEK_START IN|OUT <cylinder>    a run of steps begins (its first step already clicked)
//   ACT <t_us> SEEK_END <cylinder> <steps>     ...and ended; <steps> includes the first
//   ACT <t_us> BUMP OUTER|INNER                a step against an end stop
//   ACT <t_us> INSERT | EJECT
// <cylinder> is where the head is after the step.
#pragma once

#include <cstddef>
#include <cstdint>

#include "drive.h"
#include "head.h"

namespace activity {

// ---- Core 1: what changed in the drive -------------------------------------------

enum class Input : uint8_t { MotorOn, MotorOff, Step, Insert, Eject };

struct DriveEvent {
    uint32_t time_us;
    Input input;
    head::Step outcome;  // Step only
    uint8_t cylinder;    // Step only: after the step
};

class DriveObserver {
public:
    // Call after every Drive::onSample (and after an insert or eject) with
    // what it returned. Writes up to three events to `out`; returns how many.
    unsigned observe(uint32_t now_us, drive::Drive& d, const drive::StepEvent& step, DriveEvent out[3]);

private:
    bool started_ = false;
    bool motor_ = false;
    bool disk_ = false;
};

// ---- Core 0: what it sounds like ---------------------------------------------------

struct Config {
    // Steps closer together than this are a seek (a hum or buzz); further
    // apart, separate clicks. PC BIOSes step every 3-6 ms, a Greaseweazle
    // every 10 ms, the Feather host every 26 ms (clicks, as on a real drive
    // stepping that slowly).
    uint32_t seek_gap_us = 20000;
};

enum class Sound : uint8_t { MotorOn, MotorOff, Step, SeekStart, SeekEnd, Bump, Insert, Eject };

struct Event {
    uint32_t time_us;
    Sound sound;
    bool inwards;      // Step, SeekStart; Bump: true for the inner stop
    uint8_t cylinder;  // Step, SeekStart, SeekEnd
    uint16_t steps;    // SeekEnd
};

const char* soundName(Sound s);

class Tracker {
public:
    explicit Tracker(Config c = {}) : cfg_(c) {}

    void onDriveEvent(const DriveEvent& e);
    // Ends a seek once no step has come for seek_gap_us. Call often (core 0
    // does once a millisecond).
    void update(uint32_t now_us);

    // Takes the next sound event, oldest first.
    bool pop(Event& e);

    bool motorOn() const { return motor_; }
    bool seeking() const { return seeking_; }
    uint32_t clicks() const { return clicks_; }
    uint32_t seeks() const { return seeks_; }
    uint32_t bumps() const { return bumps_; }
    uint32_t dropped() const { return dropped_; }

private:
    void emit(const Event& e);
    void endSeek(uint32_t time_us);

    Config cfg_;
    bool motor_ = false;
    bool seeking_ = false;
    bool have_last_ = false;   // a moving step has happened
    uint32_t last_step_us_ = 0;
    bool last_inwards_ = false;
    uint8_t cylinder_ = 0;
    uint16_t seek_steps_ = 0;
    uint32_t clicks_ = 0, seeks_ = 0, bumps_ = 0, dropped_ = 0;

    static constexpr size_t QUEUE = 32;
    Event queue_[QUEUE];
    size_t head_ = 0, count_ = 0;
};

// Formats an event as an ACT line with a trailing newline. Returns the length.
size_t format(const Event& e, char* buf, size_t size);

}  // namespace activity
