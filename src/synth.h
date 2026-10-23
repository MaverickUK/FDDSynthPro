// The FDD Synth's mixer: sound events in, samples out. Pure C++,
// host-tested, and checked sample for sample against the Python player
// (host/fddproto/player.py, which is the spec: docs/activity.md).
//
// The rules:
//   MOTOR_ON    spinup plays once; the spin loop starts where it ends
//   MOTOR_OFF   the spin loop stops; spindown plays once
//   STEP        step plays once
//   SEEK_START  the seek loop starts
//   SEEK_END    the seek loop stops one step-sample later (the last click's tail)
//   BUMP        bump plays once;  INSERT, EJECT  insert, eject play once
// Loops play from their own start, so each begins at its first sample. The
// mix is the plain sum of everything playing, saturated to 16 bits. The
// volume then scales it, like an amp's volume knob: at 100% (the default)
// the output is the mix itself, which the player is the spec for.
//
// An event applies at the mixer's current position: the caller renders up to
// where the event belongs, then hands it over. On the board, events arrive
// as they happen and the output buffer adds a fixed delay.
#pragma once

#include <cstddef>
#include <cstdint>

#include "activity.h"

namespace synth {

struct Sample {
    const int16_t* data = nullptr;
    uint32_t length = 0;  // samples (mono)
};

struct Pack {
    uint32_t rate = 0;  // every sample shares it
    Sample spinup, spin, spindown, step, seek, bump, insert, eject;
    // The sounds the rules can't do without (insert and eject are optional).
    bool complete() const {
        return spinup.length && spin.length && spindown.length && step.length && seek.length && bump.length;
    }
    // Bit n: the nth sound above (spinup first) is loaded; ui::SOUND_NAMES order.
    uint8_t loadedMask() const {
        const Sample* all[] = {&spinup, &spin, &spindown, &step, &seek, &bump, &insert, &eject};
        uint8_t mask = 0;
        for (int i = 0; i < 8; ++i)
            if (all[i]->length) mask |= uint8_t(1u << i);
        return mask;
    }
    uint32_t bytes() const {
        return 2 * (spinup.length + spin.length + spindown.length + step.length + seek.length + bump.length +
                    insert.length + eject.length);
    }
};

class Mixer {
public:
    // Sounds playing at once. A grind of bumps 3 ms apart can need more; the
    // oldest is then cut short (voicesStolen()).
    static constexpr size_t ONE_SHOTS = 32;
    static constexpr size_t SEEK_LOOPS = 4;  // a new seek can start while the last one's tail plays

    explicit Mixer(const Pack& pack) : pack_(pack) {}

    void event(const activity::Event& e);
    void render(int16_t* out, size_t frames);

    // 0-100. Loudness follows the square of the setting, roughly as the ear
    // hears it. A change glides over VOLUME_RAMP samples, so it never clicks.
    void setVolume(int percent);
    static constexpr uint32_t UNITY = 32768;      // the gain at 100%, Q15
    static constexpr uint32_t VOLUME_RAMP = 256;  // 12 ms at 22.05 kHz
    uint32_t gain() const { return gain_; }

    // Samples rendered since the start.
    uint32_t position() const { return pos_; }
    uint32_t voicesStolen() const { return stolen_; }

private:
    struct OneShot {
        const Sample* sample = nullptr;  // nullptr: free
        uint32_t start = 0;
    };
    struct Loop {
        bool on = false;
        uint32_t start = 0;
        uint32_t stop = UINT32_MAX;  // the first sample not played
    };

    void play(const Sample& s);

    const Pack& pack_;
    uint32_t pos_ = 0;
    OneShot shots_[ONE_SHOTS];
    Loop spin_;
    bool motor_ = false;
    Loop seeks_[SEEK_LOOPS];
    int seeking_ = -1;  // the seek loop a SEEK_END will stop
    uint32_t stolen_ = 0;
    uint32_t gain_ = UNITY, target_gain_ = UNITY;
};

}  // namespace synth
