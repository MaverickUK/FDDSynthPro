// FDDSynthPro floppy drive emulator prototype.
//
// Two builds (FIRMWARE_SLICE, set in platformio.ini):
//   bb48        slice 0, listen only: core 1 polls the interface pins and
//               records select, motor, side, write gate and density changes,
//               and every step. Every pin stays an input; the SN7407 outputs
//               stay released.
//   bb48_drive  slices 1-5, the drive: core 1 also runs the drive model
//               (drive.h) and puts INDEX, TRK00, WPT and DSKCHG on the
//               SN7407, and the RDATA streamer (rdata_pio.h) sends the
//               cylinder under the head. Core 0 loads it (cylinder_loader.h)
//               from the first image on the SD card, or from the generated
//               1.44 MB disk without one. A read-only disk is in from
//               power-on, writable if the image is. Console `[` and `]`
//               swap to the previous or next image on the card, GoTek
//               style. What the drive does
//               becomes sound events (activity.h), printed as ACT lines for
//               the FDD Synth. Writes: core 1 captures WDATA while WGATE is
//               asserted (wdata_pio.h); core 0 decodes each burst and puts
//               the sectors in the live track and on the card
//               (write_burst.h).
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
#include "activity.h"
#include "cylinder_loader.h"
#include "ibm_track.h"
#include "rdata_pio.h"
#include "sd_image.h"
#include "track_source.h"
#include "wdata_pio.h"
#include "write_burst.h"
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

// A disk swap (console `[` / `]`): core 0 asks for an eject, core 1 ejects,
// core 0 opens the new image and loads the cylinder under the head, core 1
// retunes the PIO programs if the data rate changed and inserts it.
enum SwapState : int { SWAP_NONE, SWAP_EJECT, SWAP_EJECTED, SWAP_INSERT };
static std::atomic<int> g_swap{SWAP_NONE};

// What the drive did, from core 1 to core 0's activity tracker.
static SpscRing<activity::DriveEvent, 256> g_drive_events;
static std::atomic<uint32_t> g_drive_events_dropped{0};

constexpr uint32_t OUTPUT_GPIO_MASK = fdd::OUTPUT_MASK << cfg::BUS_FIRST_PIN;

// The cylinder the RDATA streamer sends: both heads, interleaved
// (rdata_stream.h), sized for the largest format. Core 0's loader rebuilds it
// in place when the head moves; the DMA reads it all the time.
static uint32_t g_cylinder_slots[ibm::HD_1440.track_bytes];

// Set by core 0 once it has chosen the disk (setup()); core 1 waits for them.
static std::atomic<const ibm::Format*> g_format{nullptr};
static std::atomic<bool> g_writable{false};
static std::atomic<CylinderLoader*> g_loader{nullptr};

// One write burst at a time, from core 1's capture to core 0's decoder. Big
// enough for a whole HD track (about 75,000 flux transitions).
enum BurstState : int { BURST_FREE, BURST_CAPTURING, BURST_READY };
static uint8_t g_capture[80 * 1024] __attribute__((aligned(4)));
static WriteBurst g_burst;
static std::atomic<int> g_burst_state{BURST_FREE};
static std::atomic<uint32_t> g_bursts_dropped{0};  // WGATE while core 0 was still on the last burst

void setup1() {
    setUpInputs();
    // Outputs released (high) before they become outputs, so the cable never
    // sees a glitch. They only ever drive the SN7407's inputs.
    gpio_init_mask(OUTPUT_GPIO_MASK);
    gpio_put_masked(OUTPUT_GPIO_MASK, OUTPUT_GPIO_MASK);
    gpio_set_dir_out_masked(OUTPUT_GPIO_MASK);
    // RDATA then moves to the PIO, held released until the drive is ready.
    // The data rate depends on the disk, which core 0 chooses after mounting
    // the SD card.
    while (!g_loader.load(std::memory_order_acquire)) tight_loop_contents();
    const ibm::Format* fmt = g_format.load();
    rdata_pio::begin(g_cylinder_slots, fmt->track_bytes, fmt->dataRate());
    wdata_pio::begin(fmt->dataRate());
}

void loop1() {
    // Never returns. Each pass: sample the inputs, update the drive, put its
    // outputs on the pins. The index pulse is timed from this loop, so its
    // edges are late by at most one pass (well under a microsecond).
    static drive::Drive d;
    const CylinderLoader& loader = *g_loader.load();
    const ibm::Format* fmt = g_format.load();
    uint32_t data_rate = fmt->dataRate();
    bool write_protected = !g_writable.load();
    d.insertDisk(write_protected);
    g_disk_in.store(true);
    PinWatch w(pinsAsSample());
    activity::DriveObserver observer;
    bool index_was = false;
    bool streaming = false;
    WriteGate write_gate;
    g_burst.intervals = g_capture;
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
        if (request > 0) d.insertDisk(write_protected);
        if (request < 0) d.ejectDisk();
        if (request) g_disk_in.store(d.diskPresent(), std::memory_order_relaxed);

        int swap = g_swap.load(std::memory_order_acquire);
        if (swap == SWAP_EJECT && !write_gate.capturing()) {
            d.ejectDisk();
            g_disk_in.store(false, std::memory_order_relaxed);
            g_swap.store(SWAP_EJECTED, std::memory_order_release);
        } else if (swap == SWAP_INSERT) {
            const ibm::Format* next = g_format.load();
            if (next != fmt) {  // 720 KB <-> 1.44 MB: another data rate and track length
                fmt = next;
                data_rate = fmt->dataRate();
                rdata_pio::setFormat(fmt->track_bytes, data_rate);
                wdata_pio::setRate(data_rate);
            }
            write_protected = !g_writable.load();
            d.insertDisk(write_protected);
            g_disk_in.store(true, std::memory_order_relaxed);
            g_swap.store(SWAP_NONE, std::memory_order_release);
        }

        activity::DriveEvent changes[3];
        unsigned n = observer.observe(now, d, e, changes);
        for (unsigned i = 0; i < n; ++i)
            if (!g_drive_events.push(changes[i])) g_drive_events_dropped.fetch_add(1, std::memory_order_relaxed);

        gpio_put_masked(OUTPUT_GPIO_MASK, drive::outputPins(d.outputs(now)) << cfg::BUS_FIRST_PIN);

        // The track starts at each index pulse, and RDATA only carries it
        // while the host could be reading and the buffer holds the cylinder
        // under the head (stale IDs would look like a seek error).
        bool index = d.spindle().index(now);
        if (index && !index_was) rdata_pio::restartAtIndex();
        index_was = index;
        bool stream = d.streams(now, loader.ready());
        if (stream != streaming) {
            rdata_pio::enable(stream);
            streaming = stream;
        }

        // Writes: capture WDATA from WGATE asserted to released, then hand
        // the burst to core 0 (write_burst.h).
        bool buffer_free = g_burst_state.load(std::memory_order_acquire) == BURST_FREE;
        switch (write_gate.onSample(now, pins, d, buffer_free, data_rate, g_burst)) {
            case WriteGate::Action::Start:
                g_burst_state.store(BURST_CAPTURING, std::memory_order_relaxed);
                wdata_pio::start(g_capture, sizeof g_capture);
                break;
            case WriteGate::Action::Stop:
                g_burst.count = wdata_pio::stop();
                g_burst_state.store(BURST_READY, std::memory_order_release);
                break;
            case WriteGate::Action::Missed:
                g_bursts_dropped.fetch_add(1, std::memory_order_relaxed);
                break;
            case WriteGate::Action::None:
                break;
        }
    }
}

// Core 0's share of the drive: choose the disk at power-on, then keep the
// cylinder buffer loaded with the cylinder under the head, a step at a time.
static GeneratedSource g_generated(ibm::HD_1440);
static DiskSlot* g_slot = nullptr;  // the disk in the drive
static bool g_from_card = false;    // serving images from the card, not the generated disk
static size_t g_image_index = 0;
static uint8_t g_track_data[2][ibm::HD_1440.sectors * gen_disk::SECTOR_BYTES];
static uint16_t g_track_scratch[ibm::HD_1440.track_bytes];
static uint32_t g_read_us_max = 0;    // slowest track read (console `s`)
static uint32_t g_encode_us_max = 0;  // slowest track encode

// Decoding writes: a full track's data fields, plus room for strays.
static uint8_t g_write_store[12 * 1024];
static mfm::Field g_write_fields[64];
static BurstWriter* g_writer = nullptr;
static uint32_t g_sectors_written = 0;  // console `s`
static uint32_t g_write_bad_crc = 0;
static uint32_t g_write_unplaced = 0;
static uint32_t g_write_us_max = 0;

static void chooseDisk() {
    TrackSource* image = sd_image::mount() ? sd_image::open(0) : nullptr;
    g_from_card = image != nullptr;
    static DiskSlot slot(image ? *image : static_cast<TrackSource&>(g_generated));
    g_slot = &slot;
    static CylinderLoader loader(slot, g_cylinder_slots, g_track_scratch, g_track_data[0], g_track_data[1]);
    static BurstWriter writer(slot, loader, g_write_store, sizeof g_write_store, g_write_fields, 64);
    g_writer = &writer;
    while (loader.service() != CylinderLoader::Step::Idle) {}  // cylinder 0, before the host can ask
    g_format.store(&slot.format());
    g_writable.store(slot.writable());
    g_loader.store(&loader, std::memory_order_release);
}

// The disk is out for this long during a swap, as when a person changes
// disks: the host sees DSKCHG, and the synth plays EJECT then INSERT.
constexpr uint32_t SWAP_EJECTED_MS = 500;
static uint32_t g_ejected_ms = 0;

static void requestSwap(bool next) {
    const ImageList& images = sd_image::images();
    if (!g_from_card || images.count() < 2) {
        Serial.println("# no other image on the card");
        return;
    }
    if (g_swap.load() != SWAP_NONE) {
        Serial.println("# a swap is already under way");
        return;
    }
    g_image_index = next ? images.next(g_image_index) : images.prev(g_image_index);
    Serial.printf("# swapping to %s\n", images.name(g_image_index));
    g_ejected_ms = 0;
    g_swap.store(SWAP_EJECT, std::memory_order_release);
}

static void serviceSwap() {
    if (g_swap.load(std::memory_order_acquire) != SWAP_EJECTED) return;
    if (!g_ejected_ms) g_ejected_ms = millis() | 1;
    // Wait out the eject, and any write burst still being decoded: it
    // belongs to the old disk.
    if (millis() - g_ejected_ms < SWAP_EJECTED_MS || g_burst_state.load() != BURST_FREE) return;
    TrackSource* image = sd_image::open(g_image_index);
    if (!image) {
        Serial.printf("# can't open %s; keeping the last disk out\n", sd_image::images().name(g_image_index));
        g_swap.store(SWAP_NONE, std::memory_order_release);
        return;
    }
    g_slot->insert(*image);
    CylinderLoader& loader = *g_loader.load();
    loader.reset();
    loader.request(g_cylinder.load(std::memory_order_relaxed));
    while (loader.service() != CylinderLoader::Step::Idle) {}
    g_format.store(&image->format());
    g_writable.store(image->writable());
    g_swap.store(SWAP_INSERT, std::memory_order_release);
}

static void serviceWrites() {
    if (g_burst_state.load(std::memory_order_acquire) != BURST_READY) return;
    uint32_t started = time_us_32();
    BurstResult r = g_writer->apply(g_burst, wdata_pio::ticksPerCell(), wdata_pio::edgeTicks());
    uint32_t took = time_us_32() - started;
    if (took > g_write_us_max) g_write_us_max = took;
    g_sectors_written += r.persisted;
    g_write_bad_crc += r.decoded.bad_crc;
    g_write_unplaced += r.decoded.unplaced;
    g_burst_state.store(BURST_FREE, std::memory_order_release);
}

static void serviceLoader() {
    CylinderLoader& loader = *g_loader.load();
    loader.request(g_cylinder.load(std::memory_order_relaxed));
    uint32_t started = time_us_32();
    CylinderLoader::Step step = loader.service();
    uint32_t took = time_us_32() - started;
    if (step == CylinderLoader::Step::ReadHead0 || step == CylinderLoader::Step::ReadHead1) {
        if (took > g_read_us_max) g_read_us_max = took;
    } else if (step != CylinderLoader::Step::Idle) {
        if (took > g_encode_us_max) g_encode_us_max = took;
    }
}

// The sounds: drive events in, ACT lines out. A line that can't be printed
// yet waits, and the tracker's queue holds the rest.
static activity::Tracker g_activity;
static uint32_t g_activity_checked_us = 0;

static bool printLine(const char* line, size_t n);

static void serviceActivity() {
    activity::DriveEvent in;
    while (g_drive_events.pop(in)) g_activity.onDriveEvent(in);
    uint32_t now = time_us_32();
    if (now - g_activity_checked_us >= 1000) {
        g_activity_checked_us = now;
        g_activity.update(now);
    }
    static activity::Event pending;
    static bool has_pending = false;
    for (int i = 0; i < 8; ++i) {
        if (!has_pending && !g_activity.pop(pending)) return;
        char line[48];
        size_t len = activity::format(pending, line, sizeof line);
        has_pending = !printLine(line, len);
        if (has_pending) return;
    }
}

static void printImage() {
    const ibm::Format* fmt = g_format.load();
    unsigned kb = fmt ? unsigned(fmt->trackDataBytes() * IMAGE_CYLINDERS * IMAGE_HEADS / 1024) : 0;
    if (!g_from_card)
        Serial.printf("IMAGE generated %u\n", kb);
    else
        Serial.printf("IMAGE %s %u contiguous=%d writable=%d index=%u/%u\n", sd_image::name(), kb,
                      (int)sd_image::contiguous(), (int)g_writable.load(), unsigned(g_image_index + 1),
                      unsigned(sd_image::images().count()));
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
    const CylinderLoader& loader = *g_loader.load();
    Serial.printf(" cyl=%u side=%u selected=%d disk=%d outer_knocks=%lu inner_knocks=%lu", (unsigned)g_cylinder.load(),
                  (unsigned)g_side.load(), (int)g_selected.load(), (int)g_disk_in.load(),
                  (unsigned long)g_outer_knocks.load(), (unsigned long)g_inner_knocks.load());
    Serial.printf(" ready=%d loads=%lu restarts=%lu read_us_max=%lu encode_us_max=%lu", loader.ready(),
                  (unsigned long)loader.loads(), (unsigned long)loader.restarts(), (unsigned long)g_read_us_max,
                  (unsigned long)g_encode_us_max);
    Serial.printf(" writable=%d written=%lu write_bad_crc=%lu write_unplaced=%lu bursts_dropped=%lu write_us_max=%lu",
                  (int)g_writable.load(), (unsigned long)g_sectors_written, (unsigned long)g_write_bad_crc,
                  (unsigned long)g_write_unplaced, (unsigned long)g_bursts_dropped.load(),
                  (unsigned long)g_write_us_max);
    Serial.printf(" clicks=%lu seeks=%lu bumps=%lu act_dropped=%lu", (unsigned long)g_activity.clicks(),
                  (unsigned long)g_activity.seeks(), (unsigned long)g_activity.bumps(),
                  (unsigned long)(g_activity.dropped() + g_drive_events_dropped.load()));
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
                Serial.println("# h help, p pins, s stats, i image, [ ] previous/next image, d disk in/out, m mark, "
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
            case 'i': printImage(); break;
            case '[': requestSwap(false); break;
            case ']': requestSwap(true); break;
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
    chooseDisk();
#endif
    printReady();
}

void loop() {
#if FIRMWARE_SLICE != 0
    serviceWrites();  // before loading: a write may be to the cylinder being left
    serviceSwap();
    serviceLoader();
    serviceActivity();
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
