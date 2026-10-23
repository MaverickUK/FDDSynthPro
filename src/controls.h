// The front panel's controls as ui::Inputs. Pure C++, host-tested; the
// firmware (panel.cpp) feeds it the encoder's detents and the buttons' raw
// levels, and passes what comes out to the ui::Model.
//
//   turn           one TurnUp or TurnDown per detent (clockwise is up)
//   encoder press  Press, as soon as it's down (debounced)
//   action         Action on release, if let go before a long press
//   long action    LongAction while still held, at cfg::BUTTON_LONG_MS
#pragma once

#include <cstddef>
#include <cstdint>

#include "button.h"
#include "config.h"
#include "ui.h"

namespace controls {

class Controls {
public:
    // Inputs from one update(): a fast turn is cut to this many detents.
    static constexpr size_t MAX_INPUTS = 8;

    // `detents` since the last call (+ clockwise), the buttons' pressed
    // state, and the time. Writes the inputs, in order, and returns how many.
    size_t update(int detents, bool encoder_pressed, bool action_pressed, uint32_t now_ms,
                  ui::Input (&out)[MAX_INPUTS]) {
        size_t n = 0;
        if (encoder_.update(encoder_pressed, now_ms) == Button::Event::Pressed) out[n++] = ui::Input::Press;
        switch (action_.update(action_pressed, now_ms)) {
            case Button::Event::ShortPress: out[n++] = ui::Input::Action; break;
            case Button::Event::LongPress: out[n++] = ui::Input::LongAction; break;
            default: break;
        }
        if (cfg::ENCODER_REVERSED) detents = -detents;
        for (; detents > 0 && n < MAX_INPUTS; --detents) out[n++] = ui::Input::TurnUp;
        for (; detents < 0 && n < MAX_INPUTS; ++detents) out[n++] = ui::Input::TurnDown;
        return n;
    }

private:
    // The encoder's button only uses Pressed, so its press-length limits
    // don't matter. A press of the action button shorter than the long one
    // always counts as short.
    Button encoder_{cfg::BUTTON_DEBOUNCE_MS, UINT32_MAX, UINT32_MAX};
    Button action_{cfg::BUTTON_DEBOUNCE_MS, cfg::BUTTON_LONG_MS, cfg::BUTTON_LONG_MS};
};

}  // namespace controls
