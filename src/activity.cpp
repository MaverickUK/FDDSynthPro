#include "activity.h"

#include <cstdio>

namespace activity {

// ---- DriveObserver -------------------------------------------------------------------

unsigned DriveObserver::observe(uint32_t now_us, drive::Drive& d, const drive::StepEvent& step, DriveEvent out[3]) {
    unsigned n = 0;
    bool motor = d.spindle().motorOn();
    bool disk = d.diskPresent();
    if (!started_) {
        // The state at power-on isn't an event, except a motor already on.
        started_ = true;
        disk_ = disk;
    }
    if (motor != motor_) {
        out[n++] = DriveEvent{now_us, motor ? Input::MotorOn : Input::MotorOff, head::Step::In, 0};
        motor_ = motor;
    }
    if (disk != disk_) {
        out[n++] = DriveEvent{now_us, disk ? Input::Insert : Input::Eject, head::Step::In, 0};
        disk_ = disk;
    }
    if (step.stepped) out[n++] = DriveEvent{now_us, Input::Step, step.outcome, step.cylinder};
    return n;
}

// ---- Tracker -------------------------------------------------------------------------

const char* soundName(Sound s) {
    switch (s) {
        case Sound::MotorOn: return "MOTOR_ON";
        case Sound::MotorOff: return "MOTOR_OFF";
        case Sound::Step: return "STEP";
        case Sound::SeekStart: return "SEEK_START";
        case Sound::SeekEnd: return "SEEK_END";
        case Sound::Bump: return "BUMP";
        case Sound::Insert: return "INSERT";
        case Sound::Eject: return "EJECT";
    }
    return "?";
}

void Tracker::emit(const Event& e) {
    if (count_ == QUEUE) {
        // Full: drop the oldest. Core 0 prints far faster than steps come.
        head_ = (head_ + 1) % QUEUE;
        --count_;
        ++dropped_;
    }
    queue_[(head_ + count_) % QUEUE] = e;
    ++count_;
}

bool Tracker::pop(Event& e) {
    if (!count_) return false;
    e = queue_[head_];
    head_ = (head_ + 1) % QUEUE;
    --count_;
    return true;
}

void Tracker::endSeek(uint32_t time_us) {
    emit(Event{time_us, Sound::SeekEnd, last_inwards_, cylinder_, seek_steps_});
    seeking_ = false;
}

void Tracker::onDriveEvent(const DriveEvent& e) {
    switch (e.input) {
        case Input::MotorOn:
        case Input::MotorOff: {
            bool on = e.input == Input::MotorOn;
            if (on == motor_) return;
            motor_ = on;
            emit(Event{e.time_us, on ? Sound::MotorOn : Sound::MotorOff, false, 0, 0});
            return;
        }
        case Input::Insert:
            emit(Event{e.time_us, Sound::Insert, false, 0, 0});
            return;
        case Input::Eject:
            emit(Event{e.time_us, Sound::Eject, false, 0, 0});
            return;
        case Input::Step:
            break;
    }

    update(e.time_us);  // a seek that has run its course ends before this step
    if (e.outcome == head::Step::AtOuterStop || e.outcome == head::Step::AtInnerStop) {
        // A knock, not a move: its own sound. It ends a seek: the head stopped.
        if (seeking_) endSeek(e.time_us);
        have_last_ = false;
        ++bumps_;
        emit(Event{e.time_us, Sound::Bump, e.outcome == head::Step::AtInnerStop, e.cylinder, 0});
        return;
    }

    bool inwards = e.outcome == head::Step::In;
    bool close = have_last_ && e.time_us - last_step_us_ <= cfg_.seek_gap_us;
    if (seeking_ && (!close || inwards != last_inwards_)) endSeek(last_step_us_);

    if (seeking_) {
        ++seek_steps_;
    } else if (close && inwards == last_inwards_) {
        // The second close step: the first already clicked, so a seek
        // starts from here and counts both.
        seeking_ = true;
        seek_steps_ = 2;
        ++seeks_;
        emit(Event{e.time_us, Sound::SeekStart, inwards, e.cylinder, 0});
    } else {
        ++clicks_;
        emit(Event{e.time_us, Sound::Step, inwards, e.cylinder, 0});
    }
    have_last_ = true;
    last_step_us_ = e.time_us;
    last_inwards_ = inwards;
    cylinder_ = e.cylinder;
}

void Tracker::update(uint32_t now_us) {
    if (seeking_ && now_us - last_step_us_ > cfg_.seek_gap_us) endSeek(last_step_us_);
}

size_t format(const Event& e, char* buf, size_t size) {
    int n;
    const char* dir = e.inwards ? "IN" : "OUT";
    switch (e.sound) {
        case Sound::Step:
        case Sound::SeekStart:
            n = std::snprintf(buf, size, "ACT %lu %s %s %u\n", (unsigned long)e.time_us, soundName(e.sound), dir,
                              (unsigned)e.cylinder);
            break;
        case Sound::SeekEnd:
            n = std::snprintf(buf, size, "ACT %lu SEEK_END %u %u\n", (unsigned long)e.time_us, (unsigned)e.cylinder,
                              (unsigned)e.steps);
            break;
        case Sound::Bump:
            n = std::snprintf(buf, size, "ACT %lu BUMP %s\n", (unsigned long)e.time_us, e.inwards ? "INNER" : "OUTER");
            break;
        default:
            n = std::snprintf(buf, size, "ACT %lu %s\n", (unsigned long)e.time_us, soundName(e.sound));
            break;
    }
    if (n < 0 || size == 0) return 0;
    return size_t(n) >= size ? size - 1 : size_t(n);
}

}  // namespace activity
