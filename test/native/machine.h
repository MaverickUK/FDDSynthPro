// The drive as the firmware runs it, for tests that drive it end to end:
// the drive model, the cylinder loader, the write path and the sound
// events, with core 1's per-sample work and core 0's background work
// played out in simulated time. Pure C++ throughout; only the PIO programs
// are left out, and they're tested in the simulator.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "activity.h"
#include "bus.h"
#include "cylinder_loader.h"
#include "drive.h"
#include "gen_disk.h"
#include "image_cache.h"
#include "rdata_stream.h"
#include "track_source.h"
#include "virtual_hosts.h"
#include "write_burst.h"

// A writable disk in memory, starting as the generated disk.
class MemorySource : public TrackSource {
public:
    explicit MemorySource(const ibm::Format& fmt) : fmt_(fmt), image_(80 * 2 * fmt.trackDataBytes()) {
        for (uint8_t c = 0; c < 80; ++c)
            for (uint8_t h = 0; h < 2; ++h) gen_disk::track(fmt, c, h, &image_[imageTrackOffset(fmt, c, h)]);
    }
    // A real image (an IMG file's bytes), of the format its size gives.
    MemorySource(const ibm::Format& fmt, std::vector<uint8_t> image) : fmt_(fmt), image_(std::move(image)) {}
    const std::vector<uint8_t>& image() const { return image_; }
    const ibm::Format& format() const override { return fmt_; }
    bool readTrack(uint8_t c, uint8_t h, uint8_t* out) override {
        if (c >= 80) return false;
        std::memcpy(out, &image_[imageTrackOffset(fmt_, c, h)], fmt_.trackDataBytes());
        return true;
    }
    bool writeSector(uint8_t c, uint8_t h, uint8_t s, const uint8_t* data) override {
        if (!writable_ || failing) return false;
        std::memcpy(&image_[imageTrackOffset(fmt_, c, h) + s * fmt_.sectorBytes()], data, fmt_.sectorBytes());
        ++writes;
        return true;
    }
    bool writable() const override { return writable_; }
    const uint8_t* sector(uint8_t c, uint8_t h, uint8_t s) const {
        return &image_[imageTrackOffset(fmt_, c, h) + s * fmt_.sectorBytes()];
    }

    unsigned writes = 0;
    bool writable_ = true;
    bool failing = false;  // the card refuses every write (an error, or pulled out)

private:
    const ibm::Format& fmt_;
    std::vector<uint8_t> image_;
};

// Core 0's costs per loader step: track reads from the image cache in PSRAM
// (image_cache.h; test_loader.cpp's estimate), and an assumed encode
// time until the board measures it (test_loader.cpp uses the same). And the
// front panel's frame: screens::draw every 100 ms, which takes core 0 for
// draw_us between other steps, but never while a load is under way
// (panel.cpp). Frames come every frame_us plus up to 3 ms, as on the board,
// where a frame starts on whichever loop pass sees 100 ms have gone: so
// they drift against the disk's rotation, which the host's reads are locked
// to, and land at every point between its steps. 2 ms is pessimistic: the slowest screen draws in 10 us on an
// M1, and the board's Cortex-M33 is perhaps 100 times slower. The board
// reports its own as draw_us_max (console `s`). 0 leaves the panel out.
struct Core0Costs {
    uint32_t read_us = 1000;
    uint32_t encode_us = 2500;
    uint32_t draw_us = 2000;
    uint32_t frame_us = 100000;
};

class Machine {
public:
    // One drive's share of the firmware: its model on core 1, and on core 0
    // its image (the card's, then the cache in PSRAM), slot, loader and
    // writer, with the buffers each needs (sized for the largest format, as
    // on the board). The scratch and the write decoder's store are core 0's,
    // shared: it works on one drive at a time.
    struct Unit {
        Unit(const ibm::Format& fmt, const drive::Config& dc, uint16_t* scratch, uint8_t* store, size_t store_len,
             mfm::Field* fields, size_t max_fields)
            : slots(ibm::HD_1440.track_bytes),
              data0(ibm::HD_1440.trackDataBytes()),
              data1(ibm::HD_1440.trackDataBytes()),
              cache_ram(1474560),
              d(dc),
              source(fmt),
              cache(cache_ram.data(), cache_ram.size()),
              slot(cache),
              loader(slot, slots.data(), scratch, data0.data(), data1.data()),
              writer(slot, loader, store, store_len, fields, max_fields) {}

        std::vector<uint32_t> slots;  // the RDATA streamer's buffer
        std::vector<uint8_t> data0, data1;
        std::vector<uint8_t> cache_ram;  // PSRAM on the board
        drive::Drive d;
        MemorySource source;  // the disk in the drive at power-on: the card's image
        ImageCache cache;     // the inserted image, in memory; writes go through to the card
        DiskSlot slot;        // the cache, always
        CylinderLoader loader;
        BurstWriter writer;
    };

private:
    // Declared first: members are built in declaration order, and the units
    // and the capture below are built on these.
    Core0Costs costs_;
    std::vector<uint16_t> scratch_ = std::vector<uint16_t>(ibm::HD_1440.track_bytes);
    std::vector<uint8_t> store_ = std::vector<uint8_t>(12 * 1024);
    std::vector<mfm::Field> fields_ = std::vector<mfm::Field>(64);
    // The WDATA capture ring, as on the board (16 KB).
    std::vector<uint8_t> ring_ = std::vector<uint8_t>(16384);
    std::vector<std::unique_ptr<Unit>> units_;

public:
    // One drive, as the default bus setup has it: a PC drive past the twist.
    explicit Machine(const ibm::Format& fmt = ibm::HD_1440, Core0Costs costs = {}, uint8_t start_cylinder = 0,
                     bool writable = true)
        : Machine(bus::Setup{}, {&fmt}, costs, start_cylinder, writable) {}

    // One or two drives, as `bus` sets them up, each with a disk of its
    // format in (formats[i], all writable or not).
    Machine(const bus::Setup& bus, std::vector<const ibm::Format*> formats, Core0Costs costs = {},
            uint8_t start_cylinder = 0, bool writable = true)
        : costs_(costs),
          units_(makeUnits(bus, formats, start_cylinder)),
          d(units_[0]->d),
          source(units_[0]->source),
          cache(units_[0]->cache),
          slot(units_[0]->slot),
          loader(units_[0]->loader),
          writer(units_[0]->writer),
          stream(ring_.data(), uint32_t(ring_.size()), units_[0]->writer),
          interface_(bus.interface) {
        for (size_t i = 0; i < units_.size(); ++i) {
            Unit& u = *units_[i];
            u.source.writable_ = writable;
            stream.setWriter(uint8_t(i), u.writer);
            // As at power-on: the image is cached, then cylinder 0 is loaded
            // before the host can ask.
            fill(u, u.source);
            while (u.loader.service() != CylinderLoader::Step::Idle) {}
            u.d.insertDisk(!writable);
        }
    }

    size_t drives() const { return units_.size(); }
    Unit& unit(size_t i) { return *units_[i]; }
    // The drive models, for a virtual host's cable.
    std::vector<drive::Drive*> driveModels() {
        std::vector<drive::Drive*> out;
        for (auto& u : units_) out.push_back(&u->d);
        return out;
    }

    // The format of the drive the host last selected (drive 0 at first):
    // the streamer runs at its rate.
    const ibm::Format& format() const { return units_[current_]->slot.format(); }
    uint32_t usPerByte() const { return 8000000u / format().dataRate(); }

    // A disk swap, as the firmware does it: eject (DSKCHG set, RDATA off),
    // then later insert another disk. Its cylinders load in the background;
    // RDATA stays off until the one under the head is ready.
    void eject(size_t drive = 0) { units_[drive]->d.ejectDisk(); }
    void insert(TrackSource& disk, size_t drive = 0) {
        Unit& u = *units_[drive];
        fill(u, disk);  // during the ejected pause, as the firmware does
        u.slot.insert(u.cache);
        u.loader.reset();
        u.d.insertDisk(!disk.writable());  // a fresh start: any write fault is cleared
    }

    // Core 1's work after each sample the host's pin changes cause.
    void attach(VirtualHost& host) {
        host_ = &host;
        host.on_sample = [this, &host](uint32_t now, const drive::StepEvent& e) { sample(now, host.pins(), e); };
    }

    // Time passing with no pin changes: core 1's loop still runs (it notices
    // a disk going in or out), and core 0 works in the background.
    void tick(uint32_t now) {
        if (host_) sample(now, host_->pins(), drive::StepEvent{});
        runCore0(now);
        if (on_tick) on_tick(now);
    }

    // When the next panel frame is due (it starts once core 0 is free).
    uint32_t nextFrameUs() const { return next_frame_us_; }

    // Called after every tick: for watching the drive as time passes (the
    // showreel draws the front panel from it).
    std::function<void(uint32_t now)> on_tick;

    // Core 0's background work, up to `now`: one loader step or panel frame
    // at a time, each taking its cost. The loaders see their heads'
    // cylinders as each step starts.
    void runCore0(uint32_t now) {
        // Writes first, as on the board: decode whatever has arrived.
        BurstResult r;
        while (stream.service(produced_, SIZE_MAX, [this](CaptureStream::Mark& m) { return nextMark(m); }, r)) {
            writes.push_back(r);
            if (lostWrite(r)) units_[burst_drive_]->d.setWriteProtected(true);  // as core 1 does on the board
        }
        if (costs_.draw_us && !frames_started_) {
            next_frame_us_ = now;
            frames_started_ = true;
        }
        while (int32_t(now - core0_us_) > 0) {
            bool loading = false;
            for (auto& u : units_) {
                u->loader.request(u->d.head().cylinder());
                loading = loading || u->loader.loading();
            }
            bool frame_due = costs_.draw_us && int32_t(core0_us_ - next_frame_us_) >= 0;
            if (frame_due && !loading) {
                busy(costs_.draw_us);
                frame_seed_ = frame_seed_ * 1103515245 + 12345;
                next_frame_us_ += costs_.frame_us + (frame_seed_ >> 8) % 3000;
                ++frames;
                continue;
            }
            // A step for the first drive with one to do: only a selected
            // drive's head moves, so at most one is loading.
            CylinderLoader::Step s = CylinderLoader::Step::Idle;
            for (size_t i = 0; i < units_.size() && s == CylinderLoader::Step::Idle; ++i) {
                Unit& u = *units_[i];
                s = u.loader.service();
                if (s == CylinderLoader::Step::Idle) continue;
                busy((s == CylinderLoader::Step::ReadHead0 || s == CylinderLoader::Step::ReadHead1) ? costs_.read_us
                                                                                                  : costs_.encode_us);
                if (since_step_ && i == step_drive_ && u.loader.ready() == u.d.head().cylinder()) {
                    uint32_t took = core0_us_ - last_step_us_;
                    if (took > slowest_ready_us) slowest_ready_us = took;
                    since_step_ = false;
                }
            }
            if (s != CylinderLoader::Step::Idle) continue;
            if (costs_.draw_us && !loading && int32_t(next_frame_us_ - core0_us_) > 0 &&
                int32_t(now - next_frame_us_) > 0) {
                core0_us_ = next_frame_us_;  // idle until the next frame, before `now`
            } else {
                core0_us_ = now;
            }
        }
        tracker.update(now);
        drainSounds();
    }

    // What RDATA carries for `head` at track byte `pos`: the selected
    // drive's cells, or nothing (0) when no drive is sending.
    uint16_t rdataCells(uint32_t now, uint8_t head, uint32_t pos) const {
        for (const auto& u : units_)
            if (u->d.streams(now, u->loader.ready()))
                return rdata::headCells(u->slots[pos % u->slot.format().track_bytes], head);
        return 0;
    }

    // The WDATA capture: intervals the host writes, as the PIO program
    // delivers them (ticks less the edge ticks, 8 bits), into the ring. Kept
    // only while a capture is running, as on the board.
    void captureInterval(uint32_t ticks) {
        if (!gate.capturing()) return;
        ring_[produced_ % ring_.size()] = uint8_t(ticks - EDGE_TICKS);
        ++produced_;
    }

    // Pin 34 as the cable carries it: DISK CHANGE on a PC, READY on Shugart.
    bool pin34(uint32_t now) {
        for (auto& u : units_)
            if (fdd::asserted(drive::outputPins(u->d.outputs(now), interface_), cfg::PIN_DSKCHG)) return true;
        return false;
    }

    static constexpr uint32_t TICKS_PER_CELL = 16;
    static constexpr uint32_t EDGE_TICKS = 2;

    // Drive 0's, as before there were two.
    drive::Drive& d;
    MemorySource& source;
    ImageCache& cache;
    DiskSlot& slot;
    CylinderLoader& loader;
    BurstWriter& writer;

    CaptureStream stream;
    WriteGate gate;
    activity::Tracker tracker;
    std::vector<BurstResult> writes;      // every burst core 0 decoded
    std::vector<activity::Event> sounds;  // every sound event, in order
    unsigned frames = 0;                  // panel frames drawn
    uint32_t longest_step_us = 0;         // core 0's longest single step: audio waits that long
    // The slowest a cylinder was ready after the head's last step: the host
    // reads after its settle time (15 ms for a PC), and RDATA is silent until then.
    uint32_t slowest_ready_us = 0;

private:
    // Called while the members declared before units_ are built, and only those.
    std::vector<std::unique_ptr<Unit>> makeUnits(const bus::Setup& bus, const std::vector<const ibm::Format*>& formats,
                                                 uint8_t start_cylinder) {
        std::vector<std::unique_ptr<Unit>> units;
        for (unsigned i = 0; i < bus.drives() && i < formats.size(); ++i) {
            drive::Config c;
            c.lines = bus::lines(bus, i);
            c.interface = bus.interface;
            c.start_cylinder = start_cylinder;
            units.push_back(std::make_unique<Unit>(*formats[i], c, scratch_.data(), store_.data(), store_.size(),
                                                   fields_.data(), fields_.size()));
        }
        return units;
    }

    void sample(uint32_t now, uint32_t pins, const drive::StepEvent& e) {
        // The drive the host is using: the selected one, else the last.
        for (size_t i = 0; i < units_.size(); ++i)
            if (units_[i]->d.selected()) current_ = i;
        if (e.stepped && e.outcome != head::Step::AtOuterStop && e.outcome != head::Step::AtInnerStop) {
            last_step_us_ = now;
            since_step_ = true;
            step_drive_ = current_;
        }
        std::vector<drive::Drive*> models = driveModels();
        activity::DriveEvent out[activity::DriveObserver::MAX_EVENTS];
        unsigned n = observer_.observe(now, models.data(), unsigned(models.size()), e, out);
        for (unsigned i = 0; i < n; ++i) tracker.onDriveEvent(out[i]);
        drainSounds();

        // Core 1 marks where each burst starts and ends in the stream; core 0
        // decodes it from the ring as it arrives (runCore0).
        Unit& u = *units_[current_];
        switch (gate.onSample(now, pins, u.d, true, u.slot.format().dataRate(), burst_)) {
            case WriteGate::Action::Start:
                burst_.drive = uint8_t(current_);
                burst_drive_ = current_;
                marks_.push_back(CaptureStream::Mark{true, produced_, burst_});
                break;
            case WriteGate::Action::Stop:
                marks_.push_back(CaptureStream::Mark{false, produced_, WriteBurst{}});
                runCore0(now);  // core 0 is otherwise idle during a write
                break;
            default:
                break;
        }
    }

    void fill(Unit& u, TrackSource& disk) {
        u.cache.attach(disk);
        while (u.cache.fill()) {}
    }

    void busy(uint32_t us) {
        core0_us_ += us;
        if (us > longest_step_us) longest_step_us = us;
    }

    void drainSounds() {
        activity::Event ev;
        while (tracker.pop(ev)) sounds.push_back(ev);
    }

    bool nextMark(CaptureStream::Mark& m) {
        if (next_mark_ == marks_.size()) return false;
        m = marks_[next_mark_++];
        return true;
    }

    bus::Interface interface_;
    VirtualHost* host_ = nullptr;
    size_t current_ = 0;
    uint32_t produced_ = 0;
    std::vector<CaptureStream::Mark> marks_;
    size_t next_mark_ = 0;
    WriteBurst burst_{};
    size_t burst_drive_ = 0;
    activity::DriveObserver observer_;
    uint32_t core0_us_ = 0;
    uint32_t next_frame_us_ = 0;
    bool frames_started_ = false;
    uint32_t frame_seed_ = 1;
    uint32_t last_step_us_ = 0;
    bool since_step_ = false;  // a step whose cylinder isn't ready yet
    size_t step_drive_ = 0;
};
