#include "synth.h"

namespace synth {

void Mixer::play(const Sample& s) {
    if (!s.length) return;
    OneShot* slot = nullptr;
    for (OneShot& o : shots_)
        if (!o.sample) {
            slot = &o;
            break;
        }
    if (!slot) {
        // All busy: cut the one that started first.
        slot = &shots_[0];
        for (OneShot& o : shots_)
            if (o.start < slot->start) slot = &o;
        ++stolen_;
    }
    *slot = OneShot{&s, pos_};
}

void Mixer::event(const activity::Event& e) {
    using activity::Sound;
    switch (e.sound) {
        case Sound::MotorOn:
            if (motor_) return;
            motor_ = true;
            play(pack_.spinup);
            spin_ = Loop{true, pos_ + pack_.spinup.length, UINT32_MAX};
            return;
        case Sound::MotorOff:
            if (!motor_) return;
            motor_ = false;
            spin_.stop = pos_;
            play(pack_.spindown);
            return;
        case Sound::Step:
            play(pack_.step);
            return;
        case Sound::SeekStart: {
            if (seeking_ >= 0) return;
            for (size_t i = 0; i < SEEK_LOOPS; ++i) {
                if (!seeks_[i].on) {
                    seeks_[i] = Loop{true, pos_, UINT32_MAX};
                    seeking_ = int(i);
                    return;
                }
            }
            return;  // all four still sounding their tails: this seek stays silent
        }
        case Sound::SeekEnd:
            if (seeking_ < 0) return;
            seeks_[seeking_].stop = pos_ + pack_.step.length;
            seeking_ = -1;
            return;
        case Sound::Bump:
            play(pack_.bump);
            return;
        case Sound::Insert:
            play(pack_.insert);
            return;
        case Sound::Eject:
            play(pack_.eject);
            return;
    }
}

// Adds a loop's samples for [from, from + n) to `mix`.
static void addLoop(int32_t* mix, size_t n, uint32_t from, uint32_t start, uint32_t stop, const Sample& s) {
    if (!s.length) return;
    for (size_t k = 0; k < n; ++k) {
        uint32_t i = from + uint32_t(k);
        if (i >= start && i < stop) mix[k] += s.data[(i - start) % s.length];
    }
}

void Mixer::render(int16_t* out, size_t frames) {
    constexpr size_t BLOCK = 64;
    int32_t mix[BLOCK];
    while (frames) {
        size_t n = frames < BLOCK ? frames : BLOCK;
        for (size_t k = 0; k < n; ++k) mix[k] = 0;

        for (OneShot& o : shots_) {
            if (!o.sample) continue;
            for (size_t k = 0; k < n; ++k) {
                uint32_t at = pos_ + uint32_t(k) - o.start;
                if (at >= o.sample->length) break;
                mix[k] += o.sample->data[at];
            }
            if (pos_ + n - o.start >= o.sample->length) o.sample = nullptr;  // finished
        }
        if (spin_.on) {
            addLoop(mix, n, pos_, spin_.start, spin_.stop, pack_.spin);
            if (pos_ + n >= spin_.stop) spin_.on = false;
        }
        for (Loop& l : seeks_) {
            if (!l.on) continue;
            addLoop(mix, n, pos_, l.start, l.stop, pack_.seek);
            if (pos_ + n >= l.stop) l.on = false;
        }

        for (size_t k = 0; k < n; ++k) {
            int32_t v = mix[k];
            out[k] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        out += n;
        frames -= n;
        pos_ += uint32_t(n);
    }
}

}  // namespace synth
