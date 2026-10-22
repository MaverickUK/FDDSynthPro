// FDDSynthPro floppy drive emulator prototype.
//
// One build so far (FIRMWARE_SLICE, set in platformio.ini):
//   bb48  slice 0, listen only: core 1 polls the interface pins and records
//         select, motor, side, write gate and density changes, and every
//         step. Every pin stays an input; the SN7407 outputs stay released.
// Core 1 records events into the trace ring and core 0 owns USB serial: it
// prints the trace and runs the console.
#include <Arduino.h>

#include <hardware/gpio.h>

#include <atomic>
#include <cstdio>

#include "config.h"
#include "fdd_signals.h"
#include "pin_watch.h"
#include "short_test.h"
#include "spsc_ring.h"
#include "trace_format.h"
#include "version.h"

static SpscRing<TraceEvent, cfg::TRACE_RING_EVENTS> g_trace;
static std::atomic<uint32_t> g_events{0};   // recorded by core 1
static std::atomic<uint32_t> g_dropped{0};  // lost because the ring was full
static std::atomic<uint32_t> g_steps{0};    // STEP assertions seen

// Pins in sample layout (bit n = GPIO BUS_FIRST_PIN + n).
static uint32_t pinsAsSample() { return gpio_get_all() >> cfg::BUS_FIRST_PIN; }

static void record(const TraceEvent& e) {
    g_events.fetch_add(1, std::memory_order_relaxed);
    if (!g_trace.push(e)) g_dropped.fetch_add(1, std::memory_order_relaxed);
}

// ---- Core 1: slice 0, listen only ---------------------------------------------

void setup1() {
    for (unsigned pin = cfg::FIRST_INPUT_PIN; pin <= cfg::LAST_INPUT_PIN; ++pin) {
        gpio_init(pin);           // SIO input
        gpio_disable_pulls(pin);  // the 1k pull-ups are on the board
    }
    for (unsigned pin = cfg::FIRST_OUTPUT_PIN; pin <= cfg::LAST_OUTPUT_PIN; ++pin) {
        // Inputs too in slice 0. The pull-up holds the SN7407's input high,
        // so its output stays released. Never pull-downs (erratum E9).
        gpio_init(pin);
        gpio_set_pulls(pin, true, false);
    }
}

void loop1() {
    // Never returns: one tight polling loop. A pass takes well under a
    // microsecond, and the shortest pulse watched (STEP) lasts at least one.
    PinWatch watch(pinsAsSample());
    for (;;) {
        TraceEvent out[2];
        unsigned n = watch.onSample(time_us_32(), pinsAsSample(), out);
        for (unsigned i = 0; i < n; ++i) {
            if (out[i].kind == TraceKind::Step) g_steps.fetch_add(1, std::memory_order_relaxed);
            record(out[i]);
        }
    }
}

// ---- Core 0 ---------------------------------------------------------------------

static TraceCoalescer g_coalescer;
static uint32_t g_last_event_ms = 0;
static uint32_t g_reported_drops = 0;

// Prints a line only if USB serial can take it now; lines that can't be
// printed wait in the trace ring (and are counted as dropped if it fills).
static bool printLine(const char* line, size_t n) {
    if (Serial && Serial.availableForWrite() < int(n)) return false;
    Serial.write(line, n);
    return true;
}

static void printReady() {
    Serial.printf("READY FDDSynthPro %s slice%d\n", FIRMWARE_VERSION, FIRMWARE_SLICE);
}

// Current level of every input line: a wiring check. With the host connected
// and idle, every line should be OFF (high, on its pull-up).
static void printPins() {
    char line[160];
    size_t n = formatPins(pinsAsSample(), line, sizeof line);
    Serial.write(line, n);
}

static void printStats() {
    Serial.printf("STATS events=%lu dropped=%lu steps=%lu ring=%u\n", (unsigned long)g_events.load(),
                  (unsigned long)g_dropped.load(), (unsigned long)g_steps.load(), (unsigned)g_trace.size());
}

// ---- Solder-bridge test (console `x`, then `Y`) -----------------------------
//
// Pulls each interface pin low in turn, with the others pulled up, to find
// solder bridges (short_test.h). Only with the floppy cable unplugged: it
// drives the pins. Core 1 is paused while it runs, and the board reboots
// afterwards so the normal pin setup comes back.

constexpr uint32_t SHORT_TEST_CONFIRM_MS = 10000;
static uint32_t g_short_test_armed_ms = 0;  // 0 = not armed

static void emitLine(const char* line, void*) { Serial.println(line); }

static void runShortTest() {
    Serial.println("# short test: running, core 1 paused");
    Serial.flush();
    rp2040.idleOtherCore();

    uint32_t observed = short_test::observedMask();
    uint32_t driven = short_test::drivenMask();
    for (unsigned g = 0; g < 32; ++g) {
        if ((observed >> g) & 1) {
            gpio_init(g);                    // SIO input
            gpio_set_pulls(g, true, false);  // pull-up only: never pull-downs (E9)
        }
    }
    busy_wait_us(500);
    uint32_t baseline_low = ~gpio_get_all() & observed;

    uint32_t low_when_driven[32] = {};
    for (unsigned g = 0; g < 32; ++g) {
        if (!((driven >> g) & 1) || ((baseline_low >> g) & 1)) continue;  // stuck pins: never driven
        gpio_put(g, 0);
        gpio_set_dir(g, GPIO_OUT);  // 50 us at the default 4 mA drive
        busy_wait_us(50);
        low_when_driven[g] = ~gpio_get_all() & observed;
        gpio_set_dir(g, GPIO_IN);
        busy_wait_us(100);  // pull-ups bring the line back
    }

    short_test::report(short_test::analyse(baseline_low, low_when_driven), emitLine, nullptr);
    Serial.println("# rebooting to restore the pin setup");
    Serial.flush();
    delay(100);
    rp2040.reboot();
}

static void handleConsole() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c != 'x' && c != 'Y' && c != '\r' && c != '\n') g_short_test_armed_ms = 0;
        switch (c) {
            case 'h':
                printReady();
                Serial.println("# h help, p pins, s stats, m mark, x short test (cable unplugged), "
                               "b reboot to bootloader");
                break;
            case 'p': printPins(); break;
            case 's': printStats(); break;
            case 'm':  // a timestamp, so tests can line phases up with the trace
                Serial.printf("MARK %lu\n", (unsigned long)time_us_32());
                break;
            case 'x':
                Serial.println("# SHORT TEST drives the interface pins. UNPLUG THE FLOPPY CABLE from the BB48 first,");
                Serial.println("# then type Y (capital) within 10 s to run it. Anything else cancels.");
                g_short_test_armed_ms = millis() | 1;
                break;
            case 'Y':
                if (g_short_test_armed_ms && millis() - g_short_test_armed_ms < SHORT_TEST_CONFIRM_MS) {
                    runShortTest();  // reboots
                }
                Serial.println("# short test not armed: type x first");
                break;
            case 'b':
                Serial.println("# rebooting to bootloader");
                Serial.flush();
                delay(50);
                rp2040.rebootToBootloader();
                break;
            default: break;
        }
    }
}

void setup() {
    Serial.begin(115200);
    printReady();
}

void loop() {
    handleConsole();

    // Print what core 1 recorded; a bounded batch so the console stays
    // responsive. A run that can't be printed yet is kept and retried.
    static TraceRun pending;
    static bool has_pending = false;
    if (has_pending) {
        char line[160];
        size_t n = formatRun(pending, line, sizeof line);
        if (!printLine(line, n)) return;
        has_pending = false;
    }
    TraceEvent e;
    for (int i = 0; i < 64 && g_trace.pop(e); ++i) {
        g_last_event_ms = millis();
        if (g_coalescer.add(e, pending)) {
            char line[160];
            size_t n = formatRun(pending, line, sizeof line);
            if (!printLine(line, n)) {
                has_pending = true;
                return;
            }
        }
    }
    if (millis() - g_last_event_ms >= cfg::TRACE_FLUSH_MS && g_coalescer.flush(pending)) {
        char line[160];
        size_t n = formatRun(pending, line, sizeof line);
        if (!printLine(line, n)) has_pending = true;
    }

    uint32_t drops = g_dropped.load(std::memory_order_relaxed);
    if (drops != g_reported_drops) {
        Serial.printf("DROP %lu\n", (unsigned long)(drops - g_reported_drops));
        g_reported_drops = drops;
    }
}
