#include "panel.h"

#include <Arduino.h>
#include <hardware/gpio.h>
#include <hardware/i2c.h>

#include <atomic>

#include "canvas.h"
#include "config.h"
#include "controls.h"
#include "quadrature.h"
#include "screens.h"
#include "ssd1306.h"

namespace panel {

namespace {

i2c_inst_t* const BUS = i2c1;  // GPIO 2/3: the BB48's Qwiic/UEXT connector

bool found = false;
Canvas canvas;  // the frame on the bus; redrawn only once it has all gone
ssd1306::FrameStream stream;
ui::Model ui_model;
ui::HeadTrace trace;
controls::Controls panel_controls;
uint32_t last_frame_ms = 0, last_trace_ms = 0;
uint32_t frame_count = 0, draw_us_max = 0, abort_count = 0;

// The encoder: decoded in its pins' interrupts (attached from core 0, so
// they run there), so no transition is missed while the loop is busy.
QuadratureDecoder decoder;
std::atomic<int32_t> pending_detents{0};

uint8_t readAB() { return uint8_t((gpio_get(cfg::PIN_ENC_A) << 1) | gpio_get(cfg::PIN_ENC_B)); }

void onEncoderEdge() {
    int detent = decoder.update(readAB());
    if (detent) pending_detents.fetch_add(detent, std::memory_order_relaxed);
}

bool pressed(unsigned pin) { return !gpio_get(pin); }  // switches pull their pin to ground

// Puts as much of the frame into the I2C FIFO as it has room for (16 bytes).
// A byte marked stop ends its transfer; the next byte starts a new one at the
// same address.
void feed() {
    if (BUS->hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
        // Not acknowledged (the display unplugged?): the block has flushed
        // the FIFO. Drop the frame; the next one tries again.
        (void)BUS->hw->clr_tx_abrt;
        if (!stream.done()) ++abort_count;
        stream.cancel();
        return;
    }
    while (!stream.done() && i2c_get_write_available(BUS)) {
        ssd1306::FrameStream::Byte b = stream.next();
        BUS->hw->data_cmd = b.value | (b.stop ? I2C_IC_DATA_CMD_STOP_BITS : 0);
    }
}

}  // namespace

void begin() {
    pinMode(cfg::PIN_ENC_A, INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_B, INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_BUTTON, INPUT_PULLUP);
    pinMode(cfg::PIN_ACTION_BUTTON, INPUT_PULLUP);
    decoder = QuadratureDecoder(readAB());
    attachInterrupt(digitalPinToInterrupt(cfg::PIN_ENC_A), onEncoderEdge, CHANGE);
    attachInterrupt(digitalPinToInterrupt(cfg::PIN_ENC_B), onEncoderEdge, CHANGE);

    // The BB48 already pulls GPIO 2/3 up (2.2k), as I2C needs.
    i2c_init(BUS, cfg::OLED_I2C_HZ);
    gpio_set_function(cfg::PIN_OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(cfg::PIN_OLED_SCL, GPIO_FUNC_I2C);
    // Once, at power-on, so blocking is fine. This also sets the address
    // every later frame goes to.
    int sent = i2c_write_timeout_us(BUS, cfg::OLED_I2C_ADDR, ssd1306::INIT, sizeof ssd1306::INIT, false, 50000);
    found = sent == int(sizeof ssd1306::INIT);
    if (found)
        Serial.printf("# oled: found at 0x%02x\n", cfg::OLED_I2C_ADDR);
    else
        Serial.println("# oled: no display answering on the Qwiic connector (I2C1, 0x3C); OLED off");
}

void service(const ui::Status& s, uint32_t now_ms) {
    ui::Input inputs[controls::Controls::MAX_INPUTS];
    int detents = pending_detents.exchange(0, std::memory_order_relaxed);
    size_t n = panel_controls.update(detents, pressed(cfg::PIN_ENC_BUTTON), pressed(cfg::PIN_ACTION_BUTTON), now_ms,
                                     inputs);
    for (size_t i = 0; i < n; ++i) ui_model.input(inputs[i], s, now_ms);
    ui_model.tick(s, now_ms);

    if (now_ms - last_trace_ms >= cfg::HEAD_TRACE_SAMPLE_MS) {
        last_trace_ms = now_ms;
        trace.add(s.cylinder);
    }

    if (!found) return;
    feed();
    if (!stream.done() || now_ms - last_frame_ms < cfg::OLED_FRAME_MS) return;
    last_frame_ms = now_ms;
    uint32_t started = time_us_32();
    screens::draw(canvas, ui_model, s, trace);
    uint32_t took = time_us_32() - started;
    if (took > draw_us_max) draw_us_max = took;
    stream.start(canvas.data());
    ++frame_count;
    feed();
}

ui::Model& model() { return ui_model; }
bool displayFound() { return found; }
uint32_t frames() { return frame_count; }
uint32_t drawUsMax() { return draw_us_max; }
uint32_t aborts() { return abort_count; }

}  // namespace panel
