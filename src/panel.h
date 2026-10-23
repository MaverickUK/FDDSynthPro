// The front panel (drive build): the OLED, the rotary encoder and the action
// button. Core 0 only. The logic is pure and host-tested: ui.h decides what
// shows and what the controls do, screens.h draws it, controls.h turns the
// pins into inputs and ssd1306.h makes the bus bytes. This is the hardware
// side.
//
// A frame never holds core 0 up: it goes into the I2C FIFO a few bytes per
// pass of the loop, and while the FIFO is empty the RP2350's I2C block holds
// the bus (SCL low) until more arrives, which the SSD1306 simply waits out.
#pragma once

#include <cstdint>

#include "ui.h"

namespace panel {

// Starts I2C1 and the display, and the controls. With no display answering
// it logs that and the OLED stays off; the controls work regardless.
void begin();

// Every pass of core 0's loop: applies the controls to the model, samples
// the head trace, draws a frame every cfg::OLED_FRAME_MS and feeds the bus.
// `s` is the drive as it is now.
void service(const ui::Status& s, uint32_t now_ms);

// The screens' state and the requests (swap, eject, volume) for main.cpp.
ui::Model& model();

// For console `s`.
bool displayFound();
uint32_t frames();
uint32_t drawUsMax();  // the slowest screens::draw
uint32_t aborts();     // frames the display didn't acknowledge

}  // namespace panel
