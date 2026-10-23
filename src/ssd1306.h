// The SSD1306 OLED's side of the I2C bus: the bytes that set it up, and the
// bytes of one frame, each marked with whether a STOP follows it. Pure C++,
// host-tested; panel.cpp hands them to the I2C FIFO a few at a time.
//
// Every transfer starts with a control byte: 0x00 means commands follow,
// 0x40 display data.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ssd1306 {

constexpr size_t FRAME_BYTES = 128 * 64 / 8;

// The set-up, as HDDSynth_Native sends it (one transfer, control byte first).
constexpr uint8_t INIT[] = {
    0x00,        // commands follow
    0xAE,        // display off while configuring
    0xD5, 0x80,  // clock divide / oscillator: the usual default
    0xA8, 0x3F,  // multiplex: 64 rows
    0xD3, 0x00,  // no vertical offset
    0x40,        // start line 0
    0x8D, 0x14,  // charge pump on (the module has no external VCC)
    0x20, 0x00,  // horizontal addressing: a frame is sent in one go
    0xA1,        // columns mirrored and
    0xC8,        // rows scanned bottom-up: so (0,0) is top-left on the module
    0xDA, 0x12,  // COM pin layout for 128x64
    0x81, 0xCF,  // contrast
    0xD9, 0xF1,  // pre-charge period
    0xDB, 0x40,  // VCOMH level
    0xA4,        // show RAM contents
    0xA6,        // not inverted
    0x2E,        // no scrolling
    0xAF,        // display on
};

// One frame as two transfers: the window (the whole screen), then the
// data. The canvas's layout (canvas.h) is the SSD1306's: 8 pages of 128
// columns, a byte per column of 8 rows, low bit on top.
class FrameStream {
public:
    struct Byte {
        uint8_t value;
        bool stop;  // the last byte of a transfer
    };
    static constexpr uint8_t WINDOW[] = {0x00, 0x21, 0, 127, 0x22, 0, 7};
    static constexpr size_t WINDOW_BYTES = sizeof WINDOW;
    static constexpr size_t TOTAL = WINDOW_BYTES + 1 + FRAME_BYTES;  // + the data's control byte

    // `frame` must stay unchanged until done().
    void start(const uint8_t* frame) {
        frame_ = frame;
        next_ = 0;
    }
    void cancel() { frame_ = nullptr; }
    bool done() const { return !frame_ || next_ == TOTAL; }

    Byte next() {
        size_t i = next_++;
        if (i < WINDOW_BYTES) return {WINDOW[i], i == WINDOW_BYTES - 1};
        if (i == WINDOW_BYTES) return {0x40, false};
        return {frame_[i - WINDOW_BYTES - 1], i == TOTAL - 1};
    }

private:
    const uint8_t* frame_ = nullptr;
    size_t next_ = 0;
};

}  // namespace ssd1306
