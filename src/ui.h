// The front panel: which OLED screen shows, and what the encoder and the
// action button do. Pure C++, host-tested; screens.h draws it, and the
// firmware feeds it inputs and acts on its requests.
//
// Controls, as on the HDD Synth: a rotary encoder with a push button, and an
// action button.
//   Turn          volume (the overlay stays OLED_OVERLAY_MS after the last
//                 turn); on the disk list, move the selection
//   Press         open the disk list; on the list, insert the selected disk
//   Action        next screen: home, head trace, sounds, stats; on the disk
//                 list, back
//   Long action   eject the disk, or insert it again
#pragma once

#include <cstddef>
#include <cstdint>

#include "image_list.h"

namespace ui {

enum class Screen : uint8_t { Home, Disks, Swap, Volume, HeadTrace, Sounds, Stats };
enum class Input : uint8_t { TurnUp, TurnDown, Press, Action, LongAction };
enum class Activity : uint8_t { Idle, Read, Write, Seek };

// What the drive is doing, for the screens. The firmware fills it each frame.
struct Status {
    const ImageList* images = nullptr;  // nullptr or empty: the generated disk
    size_t image = 0;                   // which one is in (or last was)
    const char* image_name = "";
    unsigned kb = 1440;                 // 1440 or 720
    bool disk_in = true;
    bool write_protected = false;
    bool swapping = false;              // a swap under way (Swap screen)
    bool motor = false;
    uint8_t cylinder = 0;
    uint8_t head = 0;
    Activity activity = Activity::Idle;
    uint8_t rotation = 0;               // 0-255 round the disk, for the spinning icon
    // Sounds
    const char* pack = "";              // "" if none loaded
    uint8_t sounds_loaded = 0;          // bit n: sound n of SOUND_NAMES
    unsigned rate = 0;
    unsigned pack_kb = 0;
    // Stats
    uint32_t loads = 0, written = 0, bad_crc = 0, overruns = 0, underruns = 0;
};

constexpr const char* SOUND_NAMES[8] = {"spinup", "spin", "spindown", "step", "seek", "bump", "insert", "eject"};

// The head's cylinder over the last few seconds, for the head trace screen:
// one sample per column, newest on the right.
class HeadTrace {
public:
    static constexpr int SAMPLES = 128;
    void add(uint8_t cylinder) {
        samples_[next_] = cylinder;
        next_ = (next_ + 1) % SAMPLES;
        if (count_ < SAMPLES) ++count_;
    }
    int count() const { return count_; }
    // i = 0 is the oldest kept.
    uint8_t at(int i) const { return samples_[(next_ - count_ + i + SAMPLES) % SAMPLES]; }

private:
    uint8_t samples_[SAMPLES] = {};
    int next_ = 0, count_ = 0;
};

class Model {
public:
    static constexpr uint32_t OVERLAY_MS = 4000;  // volume
    static constexpr uint32_t LIST_MS = 15000;    // the disk list closes if left alone

    void input(Input in, const Status& s, uint32_t now_ms);
    // Timeouts. Call every frame.
    void tick(const Status& s, uint32_t now_ms);

    Screen screen() const { return screen_; }
    size_t selection() const { return selection_; }
    int volume() const { return volume_; }  // 0-100

    // Requests for the firmware; each is handed over once.
    bool takeSwap(size_t& image) {
        if (!swap_pending_) return false;
        swap_pending_ = false;
        image = swap_to_;
        return true;
    }
    bool takeEjectToggle() {
        bool t = eject_toggle_;
        eject_toggle_ = false;
        return t;
    }

private:
    Screen screen_ = Screen::Home;
    Screen base_ = Screen::Home;  // where the volume overlay and the list return to
    size_t selection_ = 0;
    int volume_ = 75;
    uint32_t last_input_ms_ = 0;
    bool swap_pending_ = false;
    size_t swap_to_ = 0;
    bool eject_toggle_ = false;
    bool saw_swap_ = false;
};

}  // namespace ui
