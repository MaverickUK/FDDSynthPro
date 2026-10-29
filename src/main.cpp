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
//               from the first image on the SD card, copied into PSRAM
//               before the disk goes in (image_cache.h), or from the
//               generated 1.44 MB disk without one. A read-only disk is in from
//               power-on, writable if the image is. Console `[` and `]`
//               swap to the previous or next image on the card, GoTek
//               style. What the drive does
//               becomes sound events (activity.h), printed as ACT lines for
//               the FDD Synth, and played: the mixer (synth.h) sounds them
//               with the card's sample pack through an I2S amp (audio.h).
//               The front panel (panel.h) shows the drive on an OLED; its
//               encoder sets the volume and picks the disk.
//               Writes: core 1 captures WDATA while WGATE is
//               asserted (wdata_pio.h); core 0 decodes each burst and puts
//               the sectors in the live track and on the card
//               (write_burst.h).
// In both, core 1 records events into the trace ring and core 0 owns USB
// serial: it prints the trace and runs the console.
#include <Arduino.h>

#include <hardware/gpio.h>
#include <hardware/regs/addressmap.h>

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
#include "audio.h"
#include "cylinder_loader.h"
#include "ibm_track.h"
#include "image_cache.h"
#include "panel.h"
#include "rdata_pio.h"
#include "sd_image.h"
#include "settings.h"
#include "synth.h"
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

constexpr uint32_t OUTPUT_GPIO_MASK = fdd::OUTPUT_MASK << cfg::BUS_FIRST_PIN;

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
    for (unsigned pin : cfg::OUTPUT_PINS) {
        // Inputs too in slice 0. Through the SN7407, the pull-up holds its
        // input high, so its output stays released. Wired straight to the
        // header, the host pulls them up (to 5 V), so ours stay off. Never
        // pull-downs (erratum E9).
        gpio_init(pin);
        gpio_set_pulls(pin, !cfg::OUTPUTS_OPEN_DRAIN, false);
    }
}

void loop1() {
    // Never returns: one tight polling loop. A pass takes well under a
    // microsecond, and the shortest pulse watched (STEP) lasts at least one.
    PinWatch w(pinsAsSample());
    for (;;) watch(w, time_us_32(), pinsAsSample());
}

#else

// Each drive the emulator is (bus.h: one or two): what core 1 shares with
// core 0. Written by core 1, except the requests (disk_request, swap),
// which core 0 sets and core 1 takes, and what core 0 chooses before core 1
// starts (format, writable).
enum SwapState : int { SWAP_NONE, SWAP_EJECT, SWAP_EJECTED, SWAP_INSERT };

struct DriveShared {
    std::atomic<uint8_t> cylinder{0};
    std::atomic<bool> selected{false};
    std::atomic<bool> disk_in{false};
    std::atomic<uint32_t> outer_knocks{0};  // steps out at cylinder 0
    std::atomic<uint32_t> inner_knocks{0};  // steps in at the last cylinder
    std::atomic<int> disk_request{0};       // console `d`, the panel: +1 insert, -1 eject
    // For the front panel.
    std::atomic<bool> write_protected{false};
    std::atomic<bool> writing{false};    // a write burst being captured
    std::atomic<bool> streaming{false};  // RDATA carrying its track
    // A disk swap (console `[` / `]`, the panel): core 0 asks for an eject,
    // core 1 ejects, core 0 opens the new image and loads the cylinder under
    // the head, core 1 retunes the PIO programs if the data rate changed and
    // inserts it.
    std::atomic<int> swap{SWAP_NONE};
    // A lost write (write_burst.h: lostWrite): core 0 sets it, core 1 then
    // reports write-protect until the disk is inserted again.
    std::atomic<bool> write_fault{false};
    // Set by core 0 once it has chosen the disk (and at each swap).
    std::atomic<const ibm::Format*> format{nullptr};
    std::atomic<bool> writable{false};
};
static DriveShared g_shared[bus::MAX_DRIVES];
static std::atomic<uint8_t> g_side{0};  // the host's SIDE1, the same for both drives

// What the drive did, from core 1 to core 0's activity tracker.
static SpscRing<activity::DriveEvent, 256> g_drive_events;
static std::atomic<uint32_t> g_drive_events_dropped{0};

// Each drive's cylinder, as the RDATA streamer sends it: both heads,
// interleaved (rdata_stream.h), sized for the largest format. Core 0's
// loader rebuilds it in place when the head moves; the DMA streams the
// selected drive's.
static uint32_t g_cylinder_slots[bus::MAX_DRIVES][ibm::HD_1440.track_bytes];

// Set by core 0 in setup() (chooseDisk): the drives, how they sit on the
// cable, and their loaders. Core 1 waits for g_started.
static unsigned g_drive_count = 1;
static drive::Config g_drive_config[bus::MAX_DRIVES];
static bus::Interface g_interface = bus::Interface::IbmPc;
static CylinderLoader* g_loaders[bus::MAX_DRIVES];
static std::atomic<bool> g_started{false};

// Writes stream from core 1's capture to core 0's decoder through a ring
// (CaptureStream, write_burst.h): the DMA fills it, core 1 marks where each
// burst starts and ends and how far the DMA has got, core 0 decodes behind.
// Only the selected drive writes, so one capture serves both.
static uint8_t g_capture_ring[cfg::CAPTURE_RING_BYTES] __attribute__((aligned(cfg::CAPTURE_RING_BYTES)));
static SpscRing<CaptureStream::Mark, 16> g_capture_marks;
static std::atomic<uint32_t> g_capture_produced{0};
static std::atomic<uint32_t> g_bursts_dropped{0};  // WGATE with no room for its marks

static void setUpOutputs() {
    gpio_init_mask(OUTPUT_GPIO_MASK);
    if (cfg::OUTPUTS_OPEN_DRAIN) {
        // Straight to the header: the level is always low, and a line is
        // asserted by enabling the output. All start as inputs (released),
        // with no pulls: the host pulls the lines up. 12 mA for the lowest
        // low level against the host's pull-ups.
        gpio_put_masked(OUTPUT_GPIO_MASK, 0);
        gpio_set_dir_in_masked(OUTPUT_GPIO_MASK);
        for (unsigned pin : cfg::OUTPUT_PINS) {
            gpio_disable_pulls(pin);
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        }
    } else {
        // Released (high) before they become outputs, so the cable never sees
        // a glitch. They only ever drive the SN7407's inputs.
        gpio_put_masked(OUTPUT_GPIO_MASK, OUTPUT_GPIO_MASK);
        gpio_set_dir_out_masked(OUTPUT_GPIO_MASK);
    }
}

// `levels`: the output pins in sample layout, high = released (drive.h).
static void putOutputs(uint32_t levels) {
    uint32_t gpio = levels << cfg::BUS_FIRST_PIN;
    if (cfg::OUTPUTS_OPEN_DRAIN)
        gpio_set_dir_masked(OUTPUT_GPIO_MASK, ~gpio & OUTPUT_GPIO_MASK);  // output = asserted (low)
    else
        gpio_put_masked(OUTPUT_GPIO_MASK, gpio);
}

void setup1() {
    setUpInputs();
    setUpOutputs();
    // RDATA then moves to the PIO, held released until the drive is ready.
    // The data rate depends on the disk, which core 0 chooses after mounting
    // the SD card. The streamer starts on drive 0's.
    while (!g_started.load(std::memory_order_acquire)) tight_loop_contents();
    const ibm::Format* fmt = g_shared[0].format.load();
    rdata_pio::begin(g_cylinder_slots[0], fmt->track_bytes, fmt->dataRate());
    wdata_pio::begin(g_capture_ring, cfg::CAPTURE_RING_BYTES, fmt->dataRate());
}

// Where drive `d`'s disk is under its head, in streamer words (one per
// encoded byte) from its index.
static uint32_t wordUnderHead(drive::Drive& d, uint32_t now, uint32_t data_rate) {
    return uint32_t(uint64_t(d.spindle().phaseUs(now)) * data_rate / 8000000u);
}

void loop1() {
    // Never returns. Each pass: sample the inputs, update the drives, put
    // their outputs on the pins. The index pulse is timed from this loop, so
    // its edges are late by at most one pass (well under a microsecond).
    const unsigned n = g_drive_count;
    static drive::Drive d0(g_drive_config[0]), d1(g_drive_config[1]);
    drive::Drive* drives[bus::MAX_DRIVES] = {&d0, &d1};
    const ibm::Format* fmt[bus::MAX_DRIVES];
    bool write_protected[bus::MAX_DRIVES];
    for (unsigned i = 0; i < n; ++i) {
        fmt[i] = g_shared[i].format.load();
        write_protected[i] = !g_shared[i].writable.load();
        drives[i]->insertDisk(write_protected[i]);
        g_shared[i].disk_in.store(true);
    }
    // The drive the streamer and the capture serve: the selected one, else
    // the last selected.
    unsigned active = 0;
    uint32_t data_rate = fmt[0]->dataRate();
    PinWatch w(pinsAsSample());
    activity::DriveObserver observer;
    bool index_was = false;
    bool streaming = false;
    WriteGate write_gate;
    WriteBurst burst{};
    for (;;) {
        uint32_t now = time_us_32();
        uint32_t pins = pinsAsSample();
        watch(w, now, pins);

        drive::StepEvent e;
        for (unsigned i = 0; i < n; ++i) {
            DriveShared& s = g_shared[i];
            drive::StepEvent each = drives[i]->onSample(now, pins);
            if (each.stepped) {  // only a selected drive steps
                e = each;
                s.cylinder.store(each.cylinder, std::memory_order_relaxed);
                if (each.outcome == head::Step::AtOuterStop) s.outer_knocks.fetch_add(1, std::memory_order_relaxed);
                if (each.outcome == head::Step::AtInnerStop) s.inner_knocks.fetch_add(1, std::memory_order_relaxed);
            }
            s.selected.store(drives[i]->selected(), std::memory_order_relaxed);

            int request = s.disk_request.exchange(0, std::memory_order_relaxed);
            if (request > 0) {
                s.write_fault.store(false, std::memory_order_relaxed);  // a fresh start
                drives[i]->insertDisk(write_protected[i]);
            }
            if (request < 0) drives[i]->ejectDisk();
            if (request) s.disk_in.store(drives[i]->diskPresent(), std::memory_order_relaxed);

            int swap = s.swap.load(std::memory_order_acquire);
            if (swap == SWAP_EJECT && !(i == active && write_gate.capturing())) {
                drives[i]->ejectDisk();
                s.disk_in.store(false, std::memory_order_relaxed);
                s.swap.store(SWAP_EJECTED, std::memory_order_release);
            } else if (swap == SWAP_INSERT) {
                const ibm::Format* next = s.format.load();
                if (next != fmt[i]) {  // 720 KB <-> 1.44 MB: another data rate and track length
                    fmt[i] = next;
                    if (i == active) {
                        data_rate = fmt[i]->dataRate();
                        rdata_pio::setFormat(fmt[i]->track_bytes, data_rate);
                        wdata_pio::setRate(data_rate);
                    }
                }
                write_protected[i] = !s.writable.load();
                s.write_fault.store(false, std::memory_order_relaxed);
                drives[i]->insertDisk(write_protected[i]);
                s.disk_in.store(true, std::memory_order_relaxed);
                s.swap.store(SWAP_NONE, std::memory_order_release);
            }

            // After a lost write the disk reads as write-protected, so the
            // host's next write fails where the user can see it.
            if (s.write_fault.load(std::memory_order_relaxed) && drives[i]->diskPresent() &&
                !drives[i]->writeProtected())
                drives[i]->setWriteProtected(true);
            s.write_protected.store(drives[i]->writeProtected(), std::memory_order_relaxed);
        }
        g_side.store(d0.side(), std::memory_order_relaxed);

        activity::DriveEvent changes[activity::DriveObserver::MAX_EVENTS];
        unsigned count = observer.observe(now, drives, n, e, changes);
        for (unsigned i = 0; i < count; ++i)
            if (!g_drive_events.push(changes[i])) g_drive_events_dropped.fetch_add(1, std::memory_order_relaxed);

        // The cable's lines: each drive releases its own while deselected.
        uint32_t levels = fdd::OUTPUT_MASK;
        for (unsigned i = 0; i < n; ++i) levels &= drive::outputPins(drives[i]->outputs(now), g_interface);
        putOutputs(levels);

        // The host selected the other drive: the streamer moves to its
        // buffer, at the word under its head, and the capture to its rate.
        // Not mid-write: the gate below ends the burst first (the old drive
        // is deselected), and the switch follows on the next pass.
        for (unsigned i = 0; i < n; ++i) {
            if (i == active || !drives[i]->selected() || write_gate.capturing()) continue;
            if (streaming) {
                rdata_pio::enable(false);
                streaming = false;
                g_shared[active].streaming.store(false, std::memory_order_relaxed);
            }
            active = i;
            data_rate = fmt[i]->dataRate();
            rdata_pio::switchTo(g_cylinder_slots[i], fmt[i]->track_bytes, data_rate,
                                wordUnderHead(*drives[i], now, data_rate));
            wdata_pio::setRate(data_rate);
            index_was = drives[i]->spindle().index(now);
            break;
        }
        drive::Drive& d = *drives[active];

        // The track starts at each index pulse, and RDATA only carries it
        // while the host could be reading and the buffer holds the cylinder
        // under the head (stale IDs would look like a seek error).
        bool index = d.spindle().index(now);
        if (index && !index_was) rdata_pio::restartAtIndex();
        index_was = index;
        bool stream = d.streams(now, g_loaders[active]->ready());
        if (stream != streaming) {
            rdata_pio::enable(stream);
            streaming = stream;
            g_shared[active].streaming.store(stream, std::memory_order_relaxed);
        }

        // Writes: run the capture from WGATE asserted to released, marking
        // each burst's start and end in the stream for core 0 (write_burst.h).
        // A burst needs room for both its marks.
        bool room = g_capture_marks.size() + 2 <= 16;
        switch (write_gate.onSample(now, pins, d, room, data_rate, burst)) {
            case WriteGate::Action::Start:
                burst.drive = uint8_t(active);
                g_capture_marks.push(CaptureStream::Mark{true, wdata_pio::produced(), burst});
                wdata_pio::start();
                break;
            case WriteGate::Action::Stop:
                wdata_pio::stop();
                g_capture_marks.push(CaptureStream::Mark{false, wdata_pio::produced(), WriteBurst{}});
                break;
            case WriteGate::Action::Missed:
                g_bursts_dropped.fetch_add(1, std::memory_order_relaxed);
                break;
            case WriteGate::Action::None:
                break;
        }
        g_capture_produced.store(wdata_pio::produced(), std::memory_order_release);
        g_shared[active].writing.store(write_gate.capturing(), std::memory_order_relaxed);
    }
}

// Core 0's share of each drive: its disk (the card's image in PSRAM, or the
// generated disk), the loader that keeps its cylinder buffer filled with the
// cylinder under its head, its writer, and its swaps.
struct DriveState {
    ImageCache* cache = nullptr;  // in PSRAM: the slot holds this (or the generated disk)
    DiskSlot* slot = nullptr;     // the disk in the drive
    CylinderLoader* loader = nullptr;
    BurstWriter* writer = nullptr;
    bool from_card = false;  // serving images from the card, not the generated disk
    size_t image_index = 0;
    // A swap under way.
    uint32_t ejected_ms = 0;
    bool filling = false;  // the new image is going into the cache
    uint32_t fill_started_ms = 0;
    size_t swap_from = 0;  // the image to go back to if the new one won't open
    bool no_disk = false;  // neither would open (the card failed?): nothing to insert
};
static DriveState g_drv[bus::MAX_DRIVES];

static GeneratedSource g_generated(ibm::HD_1440);
static uint8_t g_track_data[bus::MAX_DRIVES][2][ibm::HD_1440.sectors * gen_disk::SECTOR_BYTES];
static uint16_t g_track_scratch[ibm::HD_1440.track_bytes];  // used within one loader step: shared
static uint32_t g_read_us_max = 0;    // slowest track read (console `s`): from the cache
static uint32_t g_encode_us_max = 0;  // slowest track encode
static uint32_t g_card_read_us_max = 0;  // slowest track read from the card, filling the cache
static uint32_t g_fill_ms = 0;           // how long the last fill took

// One step of filling a drive's cache: a track from the card.
static bool fillStep(DriveState& s) {
    uint32_t started = time_us_32();
    bool more = s.cache->fill();
    uint32_t took = time_us_32() - started;
    if (more && took > g_card_read_us_max) g_card_read_us_max = took;
    return more;
}

static void reportFill(DriveState& s, uint32_t started_ms) {
    g_fill_ms = millis() - started_ms;
    if (s.cache->fits())
        Serial.printf("# cached %u/%u tracks in %lu ms\n", s.cache->cachedTracks(), ImageCache::TRACKS,
                      (unsigned long)g_fill_ms);
    else
        Serial.println("# no PSRAM for the image cache: reading tracks from the card (slow seeks)");
}

// Decoding writes: a full track's data fields, plus room for strays. One
// burst at a time, so the drives' writers share it.
static uint8_t g_write_store[12 * 1024];
static mfm::Field g_write_fields[64];
static CaptureStream* g_capture = nullptr;
static uint32_t g_sectors_written = 0;  // console `s`
static uint32_t g_write_bad_crc = 0;
static uint32_t g_write_unplaced = 0;
static uint32_t g_write_us_max = 0;  // the longest chunk of decoding
static uint32_t g_write_faults = 0;

// The FDD Synth: a sample pack from the card, in RAM, played by the mixer.
// The mixer hears every drive event even with no pack loaded (it's silent
// then), so a pack loaded later knows the motor is running.
static synth::Pack g_pack;
static synth::Mixer g_mixer(g_pack);
static bool g_pack_ok = false;  // a complete pack is loaded
static size_t g_pack_index = 0;
static int g_pack_request = -1;  // a pack chosen on the panel, loaded once the drive is idle

static const char* packName() { return g_pack_ok ? sd_image::packs().name(g_pack_index) : ""; }

// Loads pack `index` in place of the last; the mixer drops what was playing
// (it pointed into the old pack) and carries on.
static void loadPack(size_t index) {
    g_pack_ok = sd_image::loadPack(g_pack, index, cfg::SAMPLE_POOL_MAX_BYTES);
    g_pack_index = index;
    g_mixer.reset();
    if (!g_pack_ok) {
        Serial.printf("# pack %s is incomplete or unreadable: no sound\n", sd_image::packs().name(index));
        return;
    }
    if (audio::running())
        audio::setRate(g_pack.rate);
    else
        audio::begin(g_mixer, g_pack.rate);
    Serial.printf("# pack %s\n", packName());
}

// What the drive remembers across power-off (settings.h): read at power-on,
// written a few seconds after a change once the drive is idle.
static settings::Settings g_settings;  // as on the card
static settings::Settings g_latest;    // as now
static settings::SaveTimer g_save;
static bool g_card = false;

// A drive's name for the panel and the console: "A", "B", "DS0"...
static const char* driveName(unsigned drive) {
    static const char* const PC[] = {"A", "B"};
    static const char* const SHUGART[] = {"DS0", "DS1", "DS2"};
    uint8_t unit = g_settings.bus.unit(drive);
    return g_settings.bus.interface == bus::Interface::Shugart ? SHUGART[unit % 3] : PC[unit % 2];
}

// Drive `i`'s disk at power-on: the one that was in, else the card's i-th
// (so two drives start with different disks), else the generated disk.
static void chooseDisk(unsigned i) {
    DriveState& s = g_drv[i];
    // PSRAM, if the board has it (the BB48R does): pmalloc fails without.
    // Used through the uncached alias: PSRAM shares the XIP cache with the
    // flash core 1 runs from, and copying a 9 KB track through it would
    // evict core 1's loop, stalling it on misses long enough to miss a
    // short STEP pulse. Uncached, core 1's code stays put.
    uint8_t* ram = static_cast<uint8_t*>(pmalloc(cfg::IMAGE_CACHE_BYTES));
    if (ram) ram += XIP_NOCACHE_NOALLOC_BASE - XIP_BASE;
    s.cache = new ImageCache(ram, ram ? cfg::IMAGE_CACHE_BYTES : 0);
    const ImageList& images = sd_image::images();
    size_t index = images.count() ? images.find(g_settings.image[i]) : 0;
    if (g_card && index >= images.count()) index = images.count() ? i % images.count() : 0;
    TrackSource* image = g_card ? sd_image::open(index, i) : nullptr;
    if (!image && index) image = sd_image::open(index = 0, i);
    s.image_index = index;
    s.from_card = image != nullptr;
    if (image) {  // all of it into PSRAM before the drive starts: about 0.6 s
        uint32_t started = millis();
        s.cache->attach(*image);
        while (fillStep(s)) {}
        reportFill(s, started);
    }
    s.slot = new DiskSlot(image ? static_cast<TrackSource&>(*s.cache) : static_cast<TrackSource&>(g_generated));
    s.loader = new CylinderLoader(*s.slot, g_cylinder_slots[i], g_track_scratch, g_track_data[i][0], g_track_data[i][1]);
    s.writer = new BurstWriter(*s.slot, *s.loader, g_write_store, sizeof g_write_store, g_write_fields, 64);
    while (s.loader->service() != CylinderLoader::Step::Idle) {}  // cylinder 0, before the host can ask
    g_loaders[i] = s.loader;
    g_shared[i].format.store(&s.slot->format());
    g_shared[i].writable.store(s.slot->writable());
}

static void chooseDisks() {
    bool card = sd_image::mount();
    g_card = card;
    if (card) {
        char text[settings::FILE_MAX];
        g_settings = settings::parse(text, sd_image::readSettings(text, sizeof text));
        g_latest = g_settings;
        const PackList& packs = sd_image::packs();
        if (packs.count()) {
            size_t pack = packs.find(g_settings.pack);
            loadPack(pack < packs.count() ? pack : 0);
        }
    }
    const bus::Setup& bus = g_settings.bus;
    g_drive_count = bus.drives();
    g_interface = bus.interface;
    for (unsigned i = 0; i < g_drive_count; ++i) {
        g_drive_config[i].lines = bus::lines(bus, i);
        g_drive_config[i].interface = bus.interface;
        chooseDisk(i);
    }
    static CaptureStream stream(g_capture_ring, cfg::CAPTURE_RING_BYTES, *g_drv[0].writer);
    for (unsigned i = 1; i < g_drive_count; ++i) stream.setWriter(uint8_t(i), *g_drv[i].writer);
    stream.ticks_per_cell = wdata_pio::ticksPerCell();
    stream.edge_ticks = wdata_pio::edgeTicks();
    g_capture = &stream;
    g_started.store(true, std::memory_order_release);
}

// The disk is out for at least this long during a swap, as when a person
// changes disks: the host sees DSKCHG, and the synth plays EJECT then
// INSERT. Longer if filling the cache takes longer (about 0.6 s).
constexpr uint32_t SWAP_EJECTED_MS = 500;

// The drive the console's `[`, `]`, `d` and `i` act on: `1` or `2` picks it.
static unsigned g_console_drive = 0;

// From the panel's disk list, or console `[` / `]`.
static void startSwap(unsigned drive, size_t index) {
    DriveState& s = g_drv[drive];
    if (g_shared[drive].swap.load() != SWAP_NONE) {
        Serial.println("# a swap is already under way");
        return;
    }
    s.swap_from = s.image_index;
    s.image_index = index;
    Serial.printf("# swapping %s to %s\n", driveName(drive), sd_image::images().name(index));
    s.ejected_ms = 0;
    s.filling = false;
    g_shared[drive].swap.store(SWAP_EJECT, std::memory_order_release);
}

static void requestSwap(bool next) {
    const ImageList& images = sd_image::images();
    DriveState& s = g_drv[g_console_drive];
    if (!s.from_card || images.count() < 2) {
        Serial.println("# no other image on the card");
        return;
    }
    startSwap(g_console_drive, next ? images.next(s.image_index) : images.prev(s.image_index));
}

// From the panel's long action, or console `d` (the rig's way to set DSKCHG).
static void toggleDisk(unsigned drive) {
    DriveShared& sh = g_shared[drive];
    if (sh.swap.load() != SWAP_NONE) {  // the swap ejects and inserts
        Serial.println("# a swap is under way");
        return;
    }
    bool insert = !sh.disk_in.load();
    if (insert && g_drv[drive].no_disk) {
        Serial.println("# no disk to insert: the last swap couldn't open an image");
        return;
    }
    sh.disk_request.store(insert ? 1 : -1);
    Serial.printf("# disk %s\n", insert ? "inserted" : "ejected");
}

// The swap's last step: the cached image in the slot, the cylinder under the
// head loaded, and core 1 told to insert it.
static void insertCached(unsigned drive) {
    DriveState& s = g_drv[drive];
    DriveShared& sh = g_shared[drive];
    s.slot->insert(*s.cache);
    s.loader->reset();
    s.loader->request(sh.cylinder.load(std::memory_order_relaxed));
    while (s.loader->service() != CylinderLoader::Step::Idle) {}
    sh.format.store(&s.cache->format());
    sh.writable.store(s.cache->writable());
    sh.swap.store(SWAP_INSERT, std::memory_order_release);
}

static void serviceSwap(unsigned drive) {
    DriveState& s = g_drv[drive];
    DriveShared& sh = g_shared[drive];
    if (sh.swap.load(std::memory_order_acquire) != SWAP_EJECTED) return;
    if (!s.ejected_ms) s.ejected_ms = millis() | 1;
    // Filling the cache with the new image: a track per call, so the rest of
    // core 0 (audio, the panel) carries on. Then the disk goes in, once it
    // has been out for SWAP_EJECTED_MS.
    if (s.filling) {
        if (fillStep(s)) return;
        if (millis() - s.ejected_ms < SWAP_EJECTED_MS) return;
        s.filling = false;
        reportFill(s, s.fill_started_ms);
        insertCached(drive);
        return;
    }
    // Wait for any write burst still being decoded: it may belong to the old
    // disk, and opening the new image closes the old one.
    if (g_capture->inBurst() || g_capture_marks.size()) return;
    const ImageList& images = sd_image::images();
    TrackSource* image = sd_image::open(s.image_index, drive);
    if (!image) {
        // Opening it closed the old image, so the slot can't keep that one:
        // put the old disk back in, freshly opened, as if the swap was undone.
        Serial.printf("# can't open %s; putting %s back\n", images.name(s.image_index), images.name(s.swap_from));
        s.image_index = s.swap_from;
        image = sd_image::open(s.image_index, drive);
    }
    if (!image) {
        Serial.println("# can't open that either; the drive stays empty");
        s.slot->insert(g_generated);  // never the closed file
        s.loader->reset();            // nor its cylinder: ready() is -1, so RDATA stays off
        s.no_disk = true;
        sh.swap.store(SWAP_NONE, std::memory_order_release);
        return;
    }
    s.no_disk = false;
    // The cache is free to refill: the disk is out (nothing reads it for the
    // host) and no write is pending. The loader resets once it's in.
    s.cache->attach(*image);
    s.filling = true;
    s.fill_started_ms = millis();
}

// Decodes what the capture has brought, up to a chunk per call, so audio
// and the rest of core 0 get a turn between chunks.
constexpr size_t WRITE_CHUNK_BYTES = 2048;

static void serviceWrites() {
    uint32_t started = time_us_32();
    BurstResult r;
    auto next_mark = [](CaptureStream::Mark& m) { return g_capture_marks.pop(m); };
    if (g_capture->service(g_capture_produced.load(std::memory_order_acquire), WRITE_CHUNK_BYTES, next_mark, r)) {
        g_sectors_written += r.persisted;
        g_write_bad_crc += r.decoded.bad_crc;
        g_write_unplaced += r.decoded.unplaced;
        if (lostWrite(r)) {
            unsigned drive = g_capture->burstDrive();
            ++g_write_faults;
            g_shared[drive].write_fault.store(true, std::memory_order_relaxed);
            Serial.printf("# WRITE FAULT on %s: %s; write-protected until the disk is inserted again\n",
                          driveName(drive), r.overrun ? "the capture overran" : "the card refused a sector");
        }
    }
    uint32_t took = time_us_32() - started;
    if (took > g_write_us_max) g_write_us_max = took;
}

// A step for each drive's loader that has one. Only a selected drive's head
// moves, so at most one is loading.
static void serviceLoaders() {
    for (unsigned i = 0; i < g_drive_count; ++i) {
        CylinderLoader& loader = *g_drv[i].loader;
        loader.request(g_shared[i].cylinder.load(std::memory_order_relaxed));
        uint32_t started = time_us_32();
        CylinderLoader::Step step = loader.service();
        uint32_t took = time_us_32() - started;
        if (step == CylinderLoader::Step::ReadHead0 || step == CylinderLoader::Step::ReadHead1) {
            if (took > g_read_us_max) g_read_us_max = took;
        } else if (step != CylinderLoader::Step::Idle) {
            if (took > g_encode_us_max) g_encode_us_max = took;
        }
    }
}

static bool loading() {
    for (unsigned i = 0; i < g_drive_count; ++i)
        if (g_drv[i].loader->loading()) return true;
    return false;
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
    // Every event to the mixer at once; the ACT line follows when the serial
    // link can take it, so a slow host delays the trace, never the sound.
    static SpscRing<activity::Event, 64> unprinted;
    activity::Event e;
    while (g_activity.pop(e)) {
        g_mixer.event(e);
        unprinted.push(e);  // dropped if the trace can't keep up; the sound isn't
    }
    static activity::Event pending;
    static bool has_pending = false;
    for (int i = 0; i < 8; ++i) {
        if (!has_pending && !unprinted.pop(pending)) return;
        char line[48];
        size_t len = activity::format(pending, line, sizeof line);
        has_pending = !printLine(line, len);
        if (has_pending) return;
    }
}

static unsigned imageKb(unsigned drive) {
    const ibm::Format* fmt = g_shared[drive].format.load();
    return fmt ? unsigned(fmt->trackDataBytes() * IMAGE_CYLINDERS * IMAGE_HEADS / 1024) : 0;
}

static void printImage(unsigned drive) {
    unsigned kb = imageKb(drive);
    const char* prefix = g_drive_count > 1 ? driveName(drive) : "";
    const char* gap = g_drive_count > 1 ? " " : "";
    if (!g_drv[drive].from_card)
        Serial.printf("IMAGE %s%sgenerated %u\n", prefix, gap, kb);
    else
        Serial.printf("IMAGE %s%s%s %u contiguous=%d writable=%d index=%u/%u\n", prefix, gap, sd_image::name(drive),
                      kb, (int)sd_image::contiguous(drive), (int)g_shared[drive].writable.load(),
                      unsigned(g_drv[drive].image_index + 1), unsigned(sd_image::images().count()));
}

// The front panel: the drive it shows as it is now in, swaps, ejects and
// the volume out.
static ui::Status panelStatus() {
    unsigned i = panel::model().drive() < g_drive_count ? panel::model().drive() : 0;
    const DriveState& d = g_drv[i];
    const DriveShared& sh = g_shared[i];
    ui::Status s;
    s.drives = uint8_t(g_drive_count);
    s.drive_name = driveName(i);
    s.images = d.from_card ? &sd_image::images() : nullptr;
    s.image = d.image_index;
    s.image_name = d.from_card ? sd_image::name(i) : "Generated";
    s.kb = imageKb(i);
    s.disk_in = sh.disk_in.load(std::memory_order_relaxed);
    s.write_protected = sh.write_protected.load(std::memory_order_relaxed);
    s.swapping = sh.swap.load(std::memory_order_relaxed) != SWAP_NONE;
    s.motor = g_activity.motorOn();
    s.cylinder = sh.cylinder.load(std::memory_order_relaxed);
    s.head = g_side.load(std::memory_order_relaxed);
    s.activity = sh.writing.load(std::memory_order_relaxed)                               ? ui::Activity::Write
                 : g_activity.seeking() && sh.selected.load(std::memory_order_relaxed) ? ui::Activity::Seek
                 : sh.streaming.load(std::memory_order_relaxed)                           ? ui::Activity::Read
                                                                                           : ui::Activity::Idle;
    // The spinning icon: 300 RPM while the motor runs, still otherwise.
    static uint8_t rotation = 0;
    if (s.motor) rotation = uint8_t((time_us_32() % 200000) * 256 / 200000);
    s.rotation = rotation;
    if (g_pack_ok) {
        s.pack = packName();
        s.sounds_loaded = g_pack.loadedMask();
        s.rate = g_pack.rate;
        s.pack_kb = g_pack.bytes() / 1024;
    }
    s.packs = g_card ? &sd_image::packs() : nullptr;
    s.pack_index = g_pack_index;
    s.pack_loading = g_pack_request >= 0;
    for (unsigned k = 0; k < g_drive_count; ++k) s.loads += g_drv[k].loader->loads();
    s.written = g_sectors_written;
    s.bad_crc = g_write_bad_crc;
    s.overruns = g_capture->overruns();
    s.underruns = audio::underruns();
    return s;
}

static void servicePanel() {
    panel::service(panelStatus(), millis(), !loading());  // never mid-load
    ui::Model& m = panel::model();
    size_t image;
    uint8_t drive;
    // Taken only when no swap is running, so a choice made during one (a
    // console swap) waits for it instead of being refused.
    bool idle = true;
    for (unsigned i = 0; i < g_drive_count; ++i) idle = idle && g_shared[i].swap.load() == SWAP_NONE;
    if (idle && m.takeSwap(image, drive) && drive < g_drive_count) startSwap(drive, image);
    if (m.takeEjectToggle(drive) && drive < g_drive_count) toggleDisk(drive);
    size_t pack;
    if (m.takePackChange(pack)) g_pack_request = int(pack);
    g_mixer.setVolume(m.volume());
}

// Nothing the host is doing could be held up: it has deselected the drives,
// and no swap or write is under way. Slow card work (a pack, the settings)
// waits for this.
static bool driveIdle() {
    for (unsigned i = 0; i < g_drive_count; ++i) {
        const DriveShared& sh = g_shared[i];
        if (sh.selected.load(std::memory_order_relaxed) || sh.swap.load() != SWAP_NONE ||
            sh.writing.load(std::memory_order_relaxed))
            return false;
    }
    return !g_capture->inBurst() && !g_capture_marks.size();
}

static void servicePack() {
    if (g_pack_request < 0 || !driveIdle()) return;
    size_t pack = size_t(g_pack_request);
    g_pack_request = -1;
    loadPack(pack);
}

static void serviceSettings() {
    if (!g_card) return;
    settings::Settings now;
    for (unsigned i = 0; i < g_drive_count; ++i)
        if (g_drv[i].from_card && !g_drv[i].no_disk) std::strncpy(now.image[i], sd_image::name(i), settings::NAME - 1);
    if (sd_image::packs().count()) std::strncpy(now.pack, sd_image::packs().name(g_pack_index), settings::NAME - 1);
    now.volume = panel::model().volume();
    now.bus = g_settings.bus;  // only an edit of the file changes it
    uint32_t ms = millis();
    if (now != g_latest) {
        g_latest = now;
        g_save.changed(ms);
    }
    if (!g_save.due(ms, driveIdle())) return;
    g_save.saved();
    if (g_latest == g_settings) return;  // changed back again
    char text[settings::FILE_MAX];
    if (sd_image::writeSettings(text, settings::format(g_latest, text, sizeof text)))
        g_settings = g_latest;
    else
        Serial.println("# couldn't save the settings to the card");
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
    // "direct": the outputs are wired straight to the header (config.h).
    Serial.printf("READY FDDSynthPro %s slice%d%s\n", FIRMWARE_VERSION, FIRMWARE_SLICE,
                  cfg::OUTPUTS_OPEN_DRAIN ? " direct" : "");
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
    char units[12];
    Serial.printf(" interface=%s cable=%s drive=%s", bus::interfaceName(g_settings.bus.interface),
                  bus::cableName(g_settings.bus.cable), bus::unitsName(g_settings.bus, units));
    // Each drive's: unsuffixed for the first, "2" after the second's names.
    for (unsigned i = 0; i < g_drive_count; ++i) {
        const DriveShared& sh = g_shared[i];
        const CylinderLoader& loader = *g_drv[i].loader;
        const char* n = i ? "2" : "";
        Serial.printf(" cyl%s=%u selected%s=%d disk%s=%d outer_knocks%s=%lu inner_knocks%s=%lu", n,
                      (unsigned)sh.cylinder.load(), n, (int)sh.selected.load(), n, (int)sh.disk_in.load(), n,
                      (unsigned long)sh.outer_knocks.load(), n, (unsigned long)sh.inner_knocks.load());
        Serial.printf(" ready%s=%d loads%s=%lu restarts%s=%lu cached%s=%u/%u writable%s=%d", n, loader.ready(), n,
                      (unsigned long)loader.loads(), n, (unsigned long)loader.restarts(), n,
                      g_drv[i].cache->cachedTracks(), ImageCache::TRACKS, n, (int)sh.writable.load());
    }
    Serial.printf(" side=%u read_us_max=%lu encode_us_max=%lu", (unsigned)g_side.load(), (unsigned long)g_read_us_max,
                  (unsigned long)g_encode_us_max);
    Serial.printf(" card_read_us_max=%lu fill_ms=%lu", (unsigned long)g_card_read_us_max, (unsigned long)g_fill_ms);
    Serial.printf(" written=%lu write_bad_crc=%lu write_unplaced=%lu bursts_dropped=%lu "
                  "capture_overruns=%lu write_faults=%lu write_us_max=%lu",
                  (unsigned long)g_sectors_written, (unsigned long)g_write_bad_crc,
                  (unsigned long)g_write_unplaced, (unsigned long)g_bursts_dropped.load(),
                  (unsigned long)g_capture->overruns(), (unsigned long)g_write_faults, (unsigned long)g_write_us_max);
    Serial.printf(" pack=%s audio_underruns=%lu voices_stolen=%lu", g_pack_ok ? packName() : "none",
                  (unsigned long)audio::underruns(), (unsigned long)g_mixer.voicesStolen());
    Serial.printf(" clicks=%lu seeks=%lu bumps=%lu act_dropped=%lu", (unsigned long)g_activity.clicks(),
                  (unsigned long)g_activity.seeks(), (unsigned long)g_activity.bumps(),
                  (unsigned long)(g_activity.dropped() + g_drive_events_dropped.load()));
    Serial.printf(" oled=%d frames=%lu draw_us_max=%lu oled_aborts=%lu volume=%d", (int)panel::displayFound(),
                  (unsigned long)panel::frames(), (unsigned long)panel::drawUsMax(), (unsigned long)panel::aborts(),
                  panel::model().volume());
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
                Serial.println("# h help, p pins, s stats, i image, [ ] previous/next image, d disk in/out, "
                               "1 2 which drive i [ ] d act on, k next sample pack, m mark, "
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
            case 'i': printImage(g_console_drive); break;
            case '1':
            case '2':
                if (unsigned(c - '1') < g_drive_count) g_console_drive = unsigned(c - '1');
                Serial.printf("# console drive %s\n", driveName(g_console_drive));
                break;
            case 'k':  // the next pack, as the panel's pack list does: when the drive is idle
                if (sd_image::packs().count()) g_pack_request = int(sd_image::packs().next(g_pack_index));
                break;
            case '[': requestSwap(false); break;
            case ']': requestSwap(true); break;
            case 'd': toggleDisk(g_console_drive); break;
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
    panel::begin();  // first: looking for the display can block briefly, before anything runs
    chooseDisks();
    if (g_card) panel::model().setVolume(g_settings.volume);
#endif
    printReady();
}

void loop() {
#if FIRMWARE_SLICE != 0
    // Audio between every slow step, so the I2S queue never runs dry.
    audio::service();
    serviceWrites();  // before loading: a write may be to the cylinder being left
    audio::service();
    for (unsigned i = 0; i < g_drive_count; ++i) serviceSwap(i);
    serviceLoaders();
    audio::service();
    serviceActivity();
    servicePanel();
    audio::service();
    servicePack();      // only when the drive is idle
    serviceSettings();  // likewise
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
