// Host-side unit tests for the platform-independent firmware code: the pin
// layout and signal decoding, the pin watcher that core 1 runs, the trace
// format and coalescer, the SPSC ring (including a two-thread test that
// mirrors core 1 / core 0), (in test_short_test.cpp) the solder-bridge test,
// (in test_drive.cpp) the head, spindle and drive model, and (in
// test_track.cpp) CRC-16, MFM, the IBM track layout and the generated disk,
// (in test_loader.cpp) image geometry and the cylinder loader, and (in
// test_activity.cpp) the sound events, and (in test_write_decode.cpp) reading
// writes back: the data separator, field scanner and write interpreter, and
// (in test_virtual_pc.cpp) a virtual PC driving the whole drive, and (in
// test_synth.cpp) the FDD Synth's mixer, and (in test_ui.cpp) the front
// panel's controls and OLED screens.
//
// Run: test/run_native_tests.sh
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "fdd_signals.h"
#include "pin_watch.h"
#include "spsc_ring.h"
#include "trace_format.h"

#include "check.h"

int failures = 0;
int checks = 0;

// ---- Helpers --------------------------------------------------------------

// A sample (bit n = GPIO BUS_FIRST_PIN + n) with the given pins asserted
// (low) and every other line high, as an idle host leaves them.
static uint32_t sample(std::initializer_list<unsigned> asserted_pins) {
    uint32_t s = fdd::INPUT_MASK | fdd::OUTPUT_MASK;
    for (unsigned pin : asserted_pins) s &= ~fdd::maskOf(pin);
    return s;
}

static std::string format(const TraceRun& run) {
    char buf[160];
    size_t n = formatRun(run, buf, sizeof buf);
    return std::string(buf, n);
}

static TraceEvent step(uint32_t t, bool in) {
    return TraceEvent{t, in ? sample({cfg::PIN_STEP, cfg::PIN_DIR}) : sample({cfg::PIN_STEP}), 0, TraceKind::Step};
}

// ---- Pin layout ------------------------------------------------------------

TEST(test_pin_layout) {
    // Inputs then outputs, on consecutive pins, inside one PIO window and
    // clear of GPIO 0-3 (GPIO 2/3 are pulled up) and the LED.
    CHECK_EQ(cfg::LAST_INPUT_PIN + 1, cfg::FIRST_OUTPUT_PIN);
    CHECK_EQ(cfg::LAST_INPUT_PIN - cfg::FIRST_INPUT_PIN + 1, 10u);
    CHECK_EQ(cfg::LAST_OUTPUT_PIN - cfg::FIRST_OUTPUT_PIN + 1, 5u);
    CHECK(cfg::BUS_FIRST_PIN >= 4);
    CHECK(cfg::BUS_LAST_PIN <= 31);
    CHECK(cfg::PIN_BOARD_LED > cfg::BUS_LAST_PIN);
    CHECK_EQ(fdd::INPUT_MASK, 0x3FFu);
    CHECK_EQ(fdd::OUTPUT_MASK, 0x7C00u);
    CHECK(!(fdd::WATCHED_MASK & fdd::maskOf(cfg::PIN_WDATA)));
    CHECK(fdd::WATCHED_MASK & fdd::maskOf(cfg::PIN_STEP));
}

TEST(test_signal_names) {
    for (unsigned pin = cfg::BUS_FIRST_PIN; pin <= cfg::BUS_LAST_PIN; ++pin)
        CHECK(std::string(fdd::signalName(pin)) != "?");
    CHECK_STR(fdd::signalName(cfg::PIN_TRK00), "TRK00");
    CHECK_STR(fdd::signalName(cfg::PIN_BOARD_LED), "?");
}

TEST(test_active_low) {
    uint32_t s = sample({cfg::PIN_MOTEB});
    CHECK(fdd::asserted(s, cfg::PIN_MOTEB));
    CHECK(!fdd::asserted(s, cfg::PIN_MOTEA));
}

// ---- Pin watcher -----------------------------------------------------------

TEST(test_watch_idle_is_silent) {
    PinWatch w(sample({}));
    TraceEvent out[2];
    CHECK_EQ(w.onSample(1, sample({}), out), 0u);
}

TEST(test_watch_select_and_motor) {
    PinWatch w(sample({}));
    TraceEvent out[2];
    CHECK_EQ(w.onSample(10, sample({cfg::PIN_DRVSB, cfg::PIN_MOTEB}), out), 1u);
    CHECK(out[0].kind == TraceKind::Change);
    CHECK_EQ(out[0].changed, fdd::maskOf(cfg::PIN_DRVSB) | fdd::maskOf(cfg::PIN_MOTEB));
    CHECK_EQ(out[0].time_us, 10u);
    // No change, no event.
    CHECK_EQ(w.onSample(11, sample({cfg::PIN_DRVSB, cfg::PIN_MOTEB}), out), 0u);
}

TEST(test_watch_step_on_assertion_only) {
    PinWatch w(sample({cfg::PIN_DRVSB}));
    TraceEvent out[2];
    CHECK_EQ(w.onSample(5, sample({cfg::PIN_DRVSB, cfg::PIN_STEP}), out), 1u);
    CHECK(out[0].kind == TraceKind::Step);
    CHECK_EQ(w.onSample(7, sample({cfg::PIN_DRVSB}), out), 0u);  // release: nothing
    CHECK_EQ(w.onSample(9, sample({cfg::PIN_DRVSB, cfg::PIN_STEP}), out), 1u);
}

TEST(test_watch_step_with_direction_change) {
    // DIR and STEP changing in the same sample: the step, then the change.
    PinWatch w(sample({}));
    TraceEvent out[2];
    CHECK_EQ(w.onSample(5, sample({cfg::PIN_DIR, cfg::PIN_STEP}), out), 2u);
    CHECK(out[0].kind == TraceKind::Step);
    CHECK(out[1].kind == TraceKind::Change);
    CHECK_EQ(out[1].changed, fdd::maskOf(cfg::PIN_DIR));
}

TEST(test_watch_ignores_write_data_and_outputs) {
    PinWatch w(sample({}));
    TraceEvent out[2];
    CHECK_EQ(w.onSample(1, sample({cfg::PIN_WDATA}), out), 0u);
    CHECK_EQ(w.onSample(2, sample({cfg::PIN_INDEX, cfg::PIN_TRK00}), out), 0u);
    CHECK_EQ(w.onSample(3, sample({cfg::PIN_WGATE}), out), 1u);
}

// ---- Trace format and coalescer -------------------------------------------

TEST(test_format_change) {
    uint32_t pins = sample({cfg::PIN_DRVSB, cfg::PIN_MOTEB});
    uint32_t changed = fdd::maskOf(cfg::PIN_MOTEB) | fdd::maskOf(cfg::PIN_DRVSB) | fdd::maskOf(cfg::PIN_SIDE1);
    CHECK_STR(format(TraceRun{TraceEvent{1000, pins, changed, TraceKind::Change}, 1, 1000}),
              "SIG 1000 DRVSB=ON MOTEB=ON SIDE1=OFF\n");
}

TEST(test_format_steps) {
    CHECK_STR(format(TraceRun{step(500, false), 80, 237500}), "STEP 500 OUT x80 237000\n");
    CHECK_STR(format(TraceRun{step(7, true), 1, 7}), "STEP 7 IN x1 0\n");
}

TEST(test_format_pins) {
    char buf[160];
    size_t n = formatPins(sample({cfg::PIN_DENSEL}), buf, sizeof buf);
    CHECK_STR(std::string(buf, n),
              "PINS DENSEL=ON MOTEA=OFF DRVSB=OFF DRVSA=OFF MOTEB=OFF DIR=OFF STEP=OFF WDATA=OFF "
              "WGATE=OFF SIDE1=OFF\n");
}

TEST(test_format_truncates) {
    char buf[10];
    size_t n = formatRun(TraceRun{step(1, true), 1, 1}, buf, sizeof buf);
    CHECK_EQ(n, sizeof buf - 1);
    CHECK_EQ(buf[n], '\0');
}

TEST(test_coalescer_merges_a_seek) {
    TraceCoalescer c;
    TraceRun done;
    for (uint32_t i = 0; i < 80; ++i) CHECK(!c.add(step(1000 + i * 3000, false), done));
    CHECK(c.flush(done));
    CHECK_EQ(done.count, 80u);
    CHECK_EQ(done.first.time_us, 1000u);
    CHECK_EQ(done.last_us, 1000u + 79u * 3000u);
    CHECK(!c.flush(done));
}

TEST(test_coalescer_splits_on_direction_gap_and_change) {
    TraceCoalescer c;
    TraceRun done;
    CHECK(!c.add(step(0, true), done));
    CHECK(!c.add(step(3000, true), done));
    CHECK(c.add(step(6000, false), done));  // direction reversed
    CHECK_EQ(done.count, 2u);
    CHECK(c.add(step(6000 + cfg::STEP_RUN_GAP_US + 1, false), done));  // too long a gap
    CHECK_EQ(done.count, 1u);
    TraceEvent side{70000, sample({cfg::PIN_SIDE1}), fdd::maskOf(cfg::PIN_SIDE1), TraceKind::Change};
    CHECK(c.add(side, done));
    CHECK(done.first.kind == TraceKind::Step);
    // Changes are never merged, even when identical.
    CHECK(c.add(side, done));
    CHECK(done.first.kind == TraceKind::Change);
}

// ---- SPSC ring ------------------------------------------------------------

TEST(test_ring_basic) {
    SpscRing<int, 4> r;
    int v = 0;
    CHECK(!r.pop(v));
    for (int i = 0; i < 4; ++i) CHECK(r.push(i));
    CHECK(!r.push(99));  // full
    CHECK_EQ(r.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        CHECK(r.pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!r.pop(v));
}

TEST(test_ring_two_threads) {
    // Mirrors core 1 producing trace events and core 0 consuming them.
    static SpscRing<TraceEvent, 256> r;
    const uint32_t total = 200000;
    std::thread producer([&] {
        for (uint32_t i = 0; i < total;) {
            if (r.push(TraceEvent{i, i * 3u, 0, TraceKind::Step})) ++i;
        }
    });
    uint32_t expected = 0;
    bool in_order = true;
    while (expected < total) {
        TraceEvent e;
        if (!r.pop(e)) continue;
        if (e.time_us != expected || e.pins != expected * 3u) in_order = false;
        ++expected;
    }
    producer.join();
    CHECK(in_order);
    CHECK_EQ(expected, total);
}

void runShortTestTests();
void runDriveTests();
void runTrackTests();
void runLoaderTests();
void runActivityTests();
void runWriteDecodeTests();
void runVirtualPcTests();
void runSynthTests();
void runUiTests();

int main() {
    RUN(test_pin_layout);
    RUN(test_signal_names);
    RUN(test_active_low);
    RUN(test_watch_idle_is_silent);
    RUN(test_watch_select_and_motor);
    RUN(test_watch_step_on_assertion_only);
    RUN(test_watch_step_with_direction_change);
    RUN(test_watch_ignores_write_data_and_outputs);
    RUN(test_format_change);
    RUN(test_format_steps);
    RUN(test_format_pins);
    RUN(test_format_truncates);
    RUN(test_coalescer_merges_a_seek);
    RUN(test_coalescer_splits_on_direction_gap_and_change);
    RUN(test_ring_basic);
    RUN(test_ring_two_threads);
    runShortTestTests();
    runDriveTests();
    runTrackTests();
    runLoaderTests();
    runActivityTests();
    runWriteDecodeTests();
    runVirtualPcTests();
    runSynthTests();
    runUiTests();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
