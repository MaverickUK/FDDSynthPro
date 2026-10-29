// What the drive remembers across power-off, GoTek style: the disk that was
// in, the sample pack and the volume; and how it sits on the cable (bus.h),
// which only an edit of the file changes. Pure C++, host-tested;
// sd_image.cpp reads and writes the file, main.cpp decides when.
//
// The file is /FDDSYNTH.CFG on the card, plain text, one setting a line
// (docs/config.md):
//
//   image=GAMES.IMG
//   pack=Recorded
//   volume=75
//   interface=ibmpc
//   cable=twisted
//   drive=a
//
// By name, not position, so adding files to the card doesn't change which
// disk comes back. Unknown lines are ignored and a bad value keeps the
// default, so a hand-edited file never stops the drive starting.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bus.h"

namespace settings {

constexpr const char* PATH = "/FDDSYNTH.CFG";
constexpr size_t NAME = 64;  // as ImageList::NAME
constexpr size_t FILE_MAX = 384;

struct Settings {
    char image[NAME] = "";  // "": the card's first
    char pack[NAME] = "";   // "": the first folder under /samples
    int volume = 75;        // 0-100, ui::Model's
    bus::Setup bus;         // read at power-on

    bool operator==(const Settings& o) const;
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

// Reads the file's text into `out`, starting from the defaults.
Settings parse(const char* text, size_t len);
// The file's text; returns its length (less than `cap`).
size_t format(const Settings& s, char* out, size_t cap);

// When to write a change to the card: once nothing has changed for
// SETTLE_MS (a turn of the volume is many changes) and the drive is idle
// (the host has it deselected and no swap or write is under way), so the
// write never holds up the host.
class SaveTimer {
public:
    static constexpr uint32_t SETTLE_MS = 3000;

    void changed(uint32_t now_ms) {
        dirty_ = true;
        changed_ms_ = now_ms;
    }
    bool due(uint32_t now_ms, bool drive_idle) const {
        return dirty_ && drive_idle && now_ms - changed_ms_ >= SETTLE_MS;
    }
    void saved() { dirty_ = false; }
    bool dirty() const { return dirty_; }

private:
    bool dirty_ = false;
    uint32_t changed_ms_ = 0;
};

}  // namespace settings
