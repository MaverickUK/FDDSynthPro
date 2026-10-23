// FDDSynthPro floppy drive emulator prototype.
//
// Two builds (FIRMWARE_SLICE, set in platformio.ini):
//   bb48        slice 0, listen only: core 1 polls the interface pins and
//               records select, motor, side, write gate and density changes,
//               and every step. Every pin stays an input; the SN7407 outputs
//               stay released.
//   bb48_drive  slices 1-2, the drive: core 1 also runs the drive model
//               (drive.h) and puts INDEX, TRK00, WPT and DSKCHG on the
//               SN7407, and the RDATA streamer (rdata_pio.h) sends the
//               current cylinder of the generated 1.44 MB disk, which core 0
//               builds whenever the head moves. A read-only disk is in from
//               power-on.
// In both, core 1 records events into the trace ring and core 0 owns USB
// serial: it prints the trace and runs the console.
#include <Arduino.h>

#include <hardware/gpio.h>

#include <atomic>
#include <cstdio>

#include "config.h"
#include "drive.h"
#include "fdd_signals.h"
#include "pin_watch.h"
#include "short_test.h"
#include "spsc_ring.h"
#include "trace_format.h"
#include "version.h"

#if FIRMWARE_SLICE != 0
#include "gen_disk.h"
#include "ibm_track.h"
#include "rdata_pio.h"
#include "rdata_stream.h"
#endif

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

// ---- Core 1 ---------------------------------------------------------------------

static void setUpInputs() {
    for (unsigned pin = cfg::FIRST_INPUT_PIN; pin <= cfg::LAST_INPUT_PIN; ++pin) {
        gpio_init(pin);           // SIO input
        gpio_disable_pulls(pin);  // the pull-ups are on the board
    }
}

// Records what the pin watcher saw: select/motor/side changes and steps.
static void watch(PinWatch& w, uint32_t now, uint32_t pins) {
    TraceEvent out[2];
    unsigned n = w.onSample(now, pins, out);
    for (unsigned i = 0; i < n; ++i) {
        if (out[i].kind == TraceKind::Step) g_steps.fetch_add(1, std::memory_order_relaxed);
        record(out[i]);
    }
}

#if FIRMWARE_SLICE == 0

void setup1() {
    setUpInputs();
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
    PinWatch w(pinsAsSample());
    for (;;) watch(w, time_us_32(), pinsAsSample());
}

#else

// Shared with core 0 for the console: written by core 1 only, except the
// disk request, which core 0 sets and core 1 takes.
static std::atomic<uint8_t> g_cylinder{0};
static std::atomic<uint8_t> g_side{0};
static std::atomic<bool> g_selected{false};
static std::atomic<bool> g_disk_in{false};
static std::atomic<uint32_t> g_outer_knocks{0};  // steps out at cylinder 0
static std::atomic<uint32_t> g_inner_knocks{0};  // steps in at the last cylinder
static std::atomic<int> g_disk_request{0};       // console `d`: +1 insert, -1 eject

constexpr uint32_t OUTPUT_GPIO_MASK = fdd::OUTPUT_MASK << cfg::BUS_FIRST_PIN;

// The disk's format, and the cylinder the RDATA streamer sends: both heads,
// interleaved (rdata_stream.h). Core 0 rebuilds it in place when the head
// moves; the DMA reads it all the time.
static const ibm::Format& FORMAT = ibm::HD_1440;
static uint32_t g_cylinder_slots[ibm::HD_1440.track_bytes];

void setup1() {
    setUpInputs();
    // Outputs released (high) before they become outputs, so the cable never
    // sees a glitch. They only ever drive the SN7407's inputs.
    gpio_init_mask(OUTPUT_GPIO_MASK);
    gpio_put_masked(OUTPUT_GPIO_MASK, OUTPUT_GPIO_MASK);
    gpio_set_dir_out_masked(OUTPUT_GPIO_MASK);
    // RDATA then moves to the PIO, held released until the drive is ready.
    rdata_pio::begin(g_cylinder_slots, FORMAT.track_bytes, FORMAT.dataRate());
}

void loop1() {
    // Never returns. Each pass: sample the inputs, update the drive, put its
    // outputs on the pins. The index pulse is timed from this loop, so its
    // edges are late by at most one pass (well under a microsecond).
    static drive::Drive d;
    d.insertDisk(true);  // read-only until slice 5
    g_disk_in.store(true);
    PinWatch w(pinsAsSample());
    bool index_was = false;
    bool streaming = false;
    for (;;) {
        uint32_t now = time_us_32();
        uint32_t pins = pinsAsSample();
        watch(w, now, pins);

        drive::StepEvent e = d.onSample(now, pins);
        if (e.stepped) {
            g_cylinder.store(e.cylinder, std::memory_order_relaxed);
            if (e.outcome == head::Step::AtOuterStop) g_outer_knocks.fetch_add(1, std::memory_order_relaxed);
            if (e.outcome == head::Step::AtInnerStop) g_inner_knocks.fetch_add(1, std::memory_order_relaxed);
        }
        g_side.store(d.side(), std::memory_order_relaxed);
        g_selected.store(d.selected(), std::memory_order_relaxed);

        int request = g_disk_request.exchange(0, std::memory_order_relaxed);
        if (request > 0) d.insertDisk(true);
        if (request < 0) d.ejectDisk();
        if (request) g_disk_in.store(d.diskPresent(), std::memory_order_relaxed);

        gpio_put_masked(OUTPUT_GPIO_MASK, drive::outputPins(d.outputs(now)) << cfg::BUS_FIRST_PIN);

        // The track starts at each index pulse, and RDATA only carries it
        // while the host could be reading.
        bool index = d.spindle().index(now);
        if (index && !index_was) rdata_pio::restartAtIndex();
        index_was = index;
        bool stream = d.selected() && d.diskPresent() && d.spindle().atSpeed(now);
        if (stream != streaming) {
            rdata_pio::enable(stream);
            streaming = stream;
        }
    }
}

// Core 0's share of the drive: rebuild the cylinder buffer when the head
// moves. The host waits for the head to settle (15 ms or more) before it
// reads, which covers the build.
static uint8_t g_track_data[2][ibm::HD_1440.sectors * gen_disk::SECTOR_BYTES];
static uint16_t g_track_scratch[ibm::HD_1440.track_bytes];
static int g_built_cylinder = -1;
static uint32_t g_build_us = 0;  // the last build's duration (console `s`)

static void buildCylinderIfMoved() {
    uint8_t cyl = g_cylinder.load(std::memory_order_relaxed);
    if (cyl == g_built_cylinder) return;
    uint32_t started = time_us_32();
    for (uint8_t head = 0; head < 2; ++head) gen_disk::track(FORMAT, cyl, head, g_track_data[head]);
    rdata::buildCylinder(FORMAT, cyl, g_track_data[0], g_track_data[1], g_track_scratch, g_cylinder_slots);
    g_build_us = time_us_32() - started;
    g_built_cylinder = cyl;
}

#endif

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
    Serial.printf("STATS events=%lu dropped=%lu steps=%lu ring=%u", (unsigned long)g_events.load(),
                  (unsigned long)g_dropped.load(), (unsigned long)g_steps.load(), (unsigned)g_trace.size());
#if FIRMWARE_SLICE != 0
    Serial.printf(" cyl=%u side=%u selected=%d disk=%d outer_knocks=%lu inner_knocks=%lu built=%d build_us=%lu",
                  (unsigned)g_cylinder.load(), (unsigned)g_side.load(), (int)g_selected.load(),
                  (int)g_disk_in.load(), (unsigned long)g_outer_knocks.load(),
                  (unsigned long)g_inner_knocks.load(), g_built_cylinder, (unsigned long)g_build_us);
#endif
    Serial.print("\n");
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
#if FIRMWARE_SLICE == 0
                Serial.println("# h help, p pins, s stats, m mark, x short test (cable unplugged), "
                               "b reboot to bootloader");
#else
                Serial.println("# h help, p pins, s stats, d disk in/out, m mark, "
                               "x short test (cable unplugged), b reboot to bootloader");
#endif
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
#if FIRMWARE_SLICE != 0
            case 'd': {  // the rig's way to eject and insert, for DSKCHG
                bool insert = !g_disk_in.load();
                g_disk_request.store(insert ? 1 : -1);
                Serial.printf("# disk %s\n", insert ? "inserted" : "ejected");
                break;
            }
#endif
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
#if FIRMWARE_SLICE != 0
    buildCylinderIfMoved();  // cylinder 0, before the host can ask
#endif
    printReady();
}

void loop() {
#if FIRMWARE_SLICE != 0
    buildCylinderIfMoved();
#endif
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
