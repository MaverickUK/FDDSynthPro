#include "ui.h"

namespace ui {

static size_t imageCount(const Status& s) { return s.images ? s.images->count() : 0; }
static size_t packCount(const Status& s) { return s.packs ? s.packs->count() : 0; }

void Model::input(Input in, const Status& s, uint32_t now_ms) {
    last_input_ms_ = now_ms;
    if (screen_ == Screen::Disks) {
        size_t n = imageCount(s);
        switch (in) {
            case Input::TurnUp:
                if (n) selection_ = s.images->prev(selection_);
                return;
            case Input::TurnDown:
                if (n) selection_ = s.images->next(selection_);
                return;
            case Input::Press:
                if (n && selection_ != s.image) {
                    swap_to_ = selection_;
                    swap_drive_ = drive_;
                    swap_pending_ = true;
                    screen_ = Screen::Swap;
                    saw_swap_ = false;
                } else {
                    screen_ = base_;  // the disk that's already in: nothing to do
                }
                return;
            case Input::Action:
                screen_ = base_;
                return;
            case Input::LongAction:
                eject_toggle_ = true;
                eject_drive_ = drive_;
                screen_ = base_;
                return;
        }
    }
    if (screen_ == Screen::Packs) {
        size_t n = packCount(s);
        switch (in) {
            case Input::TurnUp:
                if (n) selection_ = s.packs->prev(selection_);
                return;
            case Input::TurnDown:
                if (n) selection_ = s.packs->next(selection_);
                return;
            case Input::Press:
                if (n && selection_ != s.pack_index) {
                    pack_to_ = selection_;
                    pack_pending_ = true;
                }
                screen_ = Screen::Sounds;  // shows it loading, then the new pack
                return;
            case Input::Action:
                screen_ = Screen::Sounds;
                return;
            case Input::LongAction:
                eject_toggle_ = true;
                eject_drive_ = drive_;
                screen_ = Screen::Sounds;
                return;
        }
    }
    if (screen_ == Screen::Swap) return;  // wait for it to finish

    switch (in) {
        case Input::TurnUp:
        case Input::TurnDown:
            volume_ += in == Input::TurnUp ? 5 : -5;
            volume_ = volume_ < 0 ? 0 : volume_ > 100 ? 100 : volume_;
            screen_ = Screen::Volume;
            return;
        case Input::Press:
            if (screen_ == Screen::Sounds) {
                if (!packCount(s)) return;
                selection_ = s.pack_index;
                screen_ = Screen::Packs;
                return;
            }
            if (!imageCount(s)) return;  // the generated disk: nothing to choose
            selection_ = s.image;
            screen_ = Screen::Disks;
            return;
        case Input::Action: {
            // Home -> head trace -> sounds -> stats -> home. From the volume
            // overlay, back to where it was.
            if (screen_ == Screen::Volume) {
                screen_ = base_;
                return;
            }
            // With two drives, home is A's and then B's.
            if (screen_ == Screen::Home && drive_ + 1 < s.drives) {
                ++drive_;
                return;
            }
            Screen next = screen_ == Screen::Home        ? Screen::HeadTrace
                          : screen_ == Screen::HeadTrace ? Screen::Sounds
                          : screen_ == Screen::Sounds    ? Screen::Stats
                                                         : Screen::Home;
            if (next == Screen::Home) drive_ = 0;
            screen_ = base_ = next;
            return;
        }
        case Input::LongAction:
            eject_toggle_ = true;
            eject_drive_ = drive_;
            return;
    }
}

void Model::tick(const Status& s, uint32_t now_ms) {
    uint32_t idle = now_ms - last_input_ms_;
    if (screen_ == Screen::Volume && idle >= OVERLAY_MS) screen_ = base_;
    if (screen_ == Screen::Disks && idle >= LIST_MS) screen_ = base_;
    if (screen_ == Screen::Packs && idle >= LIST_MS) screen_ = Screen::Sounds;
    if (screen_ == Screen::Swap) {
        // Shown until the swap the firmware started has finished.
        if (s.swapping) saw_swap_ = true;
        else if (saw_swap_ || (!swap_pending_ && idle >= 2000)) screen_ = base_;
    }
}

}  // namespace ui
