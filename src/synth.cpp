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
            if (seeking_ >= 0 || !pack_.seek.length) return;
            for (size_t i = 0; i < SEEK_LOOPS; ++i) {
                if (seeks_[i].on) continue;
                Clicks& c = seeks_[i];
                c.on = true;
                c.start = pos_;
                c.stop = UINT32_MAX;
                const uint32_t own = pack_.seek.length;
                uint32_t period = uint32_t(uint64_t(e.interval_us) * pack_.rate / 1000000);
                c.period = e.interval_us ? (period ? period : 1) : own;
                c.length = c.period < own ? c.period : own;
                uint32_t ms = pack_.rate / 1000;
                c.fade = c.period < own ? (c.length / 4 < ms ? c.length / 4 : ms) : 0;
                seeking_ = int(i);
                return;
            }
            return;  // all four still sounding their tails: this seek stays silent
        }
        case Sound::SeekEnd: {
            if (seeking_ < 0) return;
            // The clicks have run until now (this event comes 20 ms after the
            // last step); the one sounding finishes, unless the step count
            // says the last step's click is already over.
            Clicks& c = seeks_[seeking_];
            uint32_t stop = pos_ + c.length;
            if (e.steps >= 2) {
                uint32_t last = c.start + uint32_t(e.steps - 2) * c.period + c.length;
                if (last < stop) stop = last > pos_ ? last : pos_;
            }
            c.stop = stop;
            seeking_ = -1;
            return;
        }
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

// Adds a seek's clicks for [from, from + n) to `mix`.
static void addClicks(int32_t* mix, size_t n, uint32_t from, uint32_t start, uint32_t stop, uint32_t period,
                      uint32_t length, uint32_t fade, const Sample& s) {
    for (size_t k = 0; k < n; ++k) {
        uint32_t i = from + uint32_t(k);
        if (i < start || i >= stop) continue;
        uint32_t phase = (i - start) % period;
        if (phase >= length) continue;
        int32_t v = s.data[phase];
        if (fade && phase >= length - fade) v = v * int32_t(length - phase) / int32_t(fade);  // towards zero
        mix[k] += v;
    }
}

void Mixer::reset() {
    for (OneShot& o : shots_) o = OneShot{};
    for (Clicks& c : seeks_) c = Clicks{};
    seeking_ = -1;
    spin_ = motor_ ? Loop{true, pos_, UINT32_MAX} : Loop{};
}

void Mixer::setVolume(int percent) {
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    target_gain_ = UNITY * uint32_t(percent * percent) / 10000;
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
        for (Clicks& c : seeks_) {
            if (!c.on) continue;
            addClicks(mix, n, pos_, c.start, c.stop, c.period, c.length, c.fade, pack_.seek);
            if (pos_ + n >= c.stop) c.on = false;
        }

        for (size_t k = 0; k < n; ++k) {
            int32_t v = mix[k];
            v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            if (gain_ != target_gain_) {
                constexpr uint32_t step = UNITY / VOLUME_RAMP;
                if (gain_ < target_gain_) gain_ = target_gain_ - gain_ < step ? target_gain_ : gain_ + step;
                else gain_ = gain_ - target_gain_ < step ? target_gain_ : gain_ - step;
            }
            // At unity this is v exactly. (Shifting a negative right rounds
            // down, on every compiler the firmware and tests use.)
            out[k] = int16_t((v * int32_t(gain_)) >> 15);
        }
        out += n;
        frames -= n;
        pos_ += uint32_t(n);
    }
}

}  // namespace synth
