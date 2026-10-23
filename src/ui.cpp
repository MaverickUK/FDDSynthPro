#include "ui.h"

namespace ui {

static size_t imageCount(const Status& s) { return s.images ? s.images->count() : 0; }

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
                screen_ = base_;
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
            Screen next = screen_ == Screen::Home        ? Screen::HeadTrace
                          : screen_ == Screen::HeadTrace ? Screen::Sounds
                          : screen_ == Screen::Sounds    ? Screen::Stats
                                                         : Screen::Home;
            screen_ = base_ = next;
            return;
        }
        case Input::LongAction:
            eject_toggle_ = true;
            return;
    }
}

void Model::tick(const Status& s, uint32_t now_ms) {
    uint32_t idle = now_ms - last_input_ms_;
    if (screen_ == Screen::Volume && idle >= OVERLAY_MS) screen_ = base_;
    if (screen_ == Screen::Disks && idle >= LIST_MS) screen_ = base_;
    if (screen_ == Screen::Swap) {
        // Shown until the swap the firmware started has finished.
        if (s.swapping) saw_swap_ = true;
        else if (saw_swap_ || (!swap_pending_ && idle >= 2000)) screen_ = base_;
    }
}

}  // namespace ui
