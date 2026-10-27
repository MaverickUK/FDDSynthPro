// The drive as the firmware runs it, for tests that drive it end to end:
// the drive model, the cylinder loader, the write path and the sound
// events, with core 1's per-sample work and core 0's background work
// played out in simulated time. Pure C++ throughout; only the PIO programs
// are left out, and they're tested in the simulator.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

#include "activity.h"
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
    // Declared first: members are built in declaration order, and the loader
    // and writer below are built on these buffers (sized for the largest
    // format, as on the board).
    Core0Costs costs_;
    std::vector<uint32_t> slots_;
    std::vector<uint16_t> scratch_;
    std::vector<uint8_t> data0_, data1_;
    std::vector<uint8_t> store_;
    std::vector<mfm::Field> fields_;
    // The WDATA capture ring, as on the board (16 KB).
    std::vector<uint8_t> ring_ = std::vector<uint8_t>(16384);
    // The image cache's memory: PSRAM on the board.
    std::vector<uint8_t> cache_ram_ = std::vector<uint8_t>(1474560);

public:
    explicit Machine(const ibm::Format& fmt = ibm::HD_1440, Core0Costs costs = {}, uint8_t start_cylinder = 0,
                     bool writable = true)
        : costs_(costs),
          slots_(ibm::HD_1440.track_bytes),
          scratch_(ibm::HD_1440.track_bytes),
          data0_(ibm::HD_1440.trackDataBytes()),
          data1_(ibm::HD_1440.trackDataBytes()),
          store_(12 * 1024),
          fields_(64),
          d(config(start_cylinder)),
          source(fmt),
          cache(cache_ram_.data(), cache_ram_.size()),
          slot(cache),
          loader(slot, slots_.data(), scratch_.data(), data0_.data(), data1_.data()),
          writer(slot, loader, store_.data(), store_.size(), fields_.data(), fields_.size()),
          stream(ring_.data(), uint32_t(ring_.size()), writer) {
        source.writable_ = writable;
        // As at power-on: the image is cached, then cylinder 0 is loaded
        // before the host can ask.
        fillCache(source);
        while (loader.service() != CylinderLoader::Step::Idle) {}
        d.insertDisk(!writable);
    }

    const ibm::Format& format() const { return slot.format(); }
    uint32_t usPerByte() const { return 8000000u / format().dataRate(); }

    // A disk swap, as the firmware does it: eject (DSKCHG set, RDATA off),
    // then later insert another disk. Its cylinders load in the background;
    // RDATA stays off until the one under the head is ready.
    void eject() { d.ejectDisk(); }
    void insert(TrackSource& disk) {
        fillCache(disk);  // during the ejected pause, as the firmware does
        slot.insert(cache);
        loader.reset();
        d.insertDisk(!disk.writable());  // a fresh start: any write fault is cleared
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
    // at a time, each taking its cost. The loader sees the head's cylinder as
    // each step starts.
    void runCore0(uint32_t now) {
        // Writes first, as on the board: decode whatever has arrived.
        BurstResult r;
        while (stream.service(produced_, SIZE_MAX, [this](CaptureStream::Mark& m) { return nextMark(m); }, r)) {
            writes.push_back(r);
            if (lostWrite(r)) d.setWriteProtected(true);  // as core 1 does on the board
        }
        if (costs_.draw_us && !frames_started_) {
            next_frame_us_ = now;
            frames_started_ = true;
        }
        while (int32_t(now - core0_us_) > 0) {
            loader.request(d.head().cylinder());
            bool frame_due = costs_.draw_us && int32_t(core0_us_ - next_frame_us_) >= 0;
            if (frame_due && !loader.loading()) {
                busy(costs_.draw_us);
                frame_seed_ = frame_seed_ * 1103515245 + 12345;
                next_frame_us_ += costs_.frame_us + (frame_seed_ >> 8) % 3000;
                ++frames;
                continue;
            }
            CylinderLoader::Step s = loader.service();
            if (s != CylinderLoader::Step::Idle) {
                busy((s == CylinderLoader::Step::ReadHead0 || s == CylinderLoader::Step::ReadHead1) ? costs_.read_us
                                                                                                  : costs_.encode_us);
                if (since_step_ && loader.ready() == d.head().cylinder()) {
                    uint32_t took = core0_us_ - last_step_us_;
                    if (took > slowest_ready_us) slowest_ready_us = took;
                    since_step_ = false;
                }
            } else if (costs_.draw_us && !loader.loading() && int32_t(next_frame_us_ - core0_us_) > 0 &&
                       int32_t(now - next_frame_us_) > 0) {
                core0_us_ = next_frame_us_;  // idle until the next frame, before `now`
            } else {
                core0_us_ = now;
            }
        }
        tracker.update(now);
        drainSounds();
    }

    // What RDATA carries for `head` at track byte `pos`: the cells, or
    // nothing (0) when the drive holds RDATA released.
    uint16_t rdataCells(uint32_t now, uint8_t head, uint32_t pos) const {
        if (!d.streams(now, loader.ready())) return 0;
        return rdata::headCells(slots_[pos % format().track_bytes], head);
    }

    // The WDATA capture: intervals the host writes, as the PIO program
    // delivers them (ticks less the edge ticks, 8 bits), into the ring. Kept
    // only while a capture is running, as on the board.
    void captureInterval(uint32_t ticks) {
        if (!gate.capturing()) return;
        ring_[produced_ % ring_.size()] = uint8_t(ticks - EDGE_TICKS);
        ++produced_;
    }

    static constexpr uint32_t TICKS_PER_CELL = 16;
    static constexpr uint32_t EDGE_TICKS = 2;

    drive::Drive d;
    MemorySource source;  // the disk in the drive at power-on: the card's image
    ImageCache cache;     // the inserted image, in memory; writes go through to the card
    DiskSlot slot;        // the cache, always
    CylinderLoader loader;
    BurstWriter writer;
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
    static drive::Config config(uint8_t start_cylinder) {
        drive::Config c;
        c.start_cylinder = start_cylinder;
        return c;
    }

    void sample(uint32_t now, uint32_t pins, const drive::StepEvent& e) {
        if (e.stepped && e.outcome != head::Step::AtOuterStop && e.outcome != head::Step::AtInnerStop) {
            last_step_us_ = now;
            since_step_ = true;
        }
        activity::DriveEvent out[3];
        unsigned n = observer_.observe(now, d, e, out);
        for (unsigned i = 0; i < n; ++i) tracker.onDriveEvent(out[i]);
        drainSounds();

        // Core 1 marks where each burst starts and ends in the stream; core 0
        // decodes it from the ring as it arrives (runCore0).
        switch (gate.onSample(now, pins, d, true, format().dataRate(), burst_)) {
            case WriteGate::Action::Start:
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

    void fillCache(TrackSource& disk) {
        cache.attach(disk);
        while (cache.fill()) {}
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

    VirtualHost* host_ = nullptr;
    uint32_t produced_ = 0;
    std::vector<CaptureStream::Mark> marks_;
    size_t next_mark_ = 0;
    WriteBurst burst_{};
    activity::DriveObserver observer_;
    uint32_t core0_us_ = 0;
    uint32_t next_frame_us_ = 0;
    bool frames_started_ = false;
    uint32_t frame_seed_ = 1;
    uint32_t last_step_us_ = 0;
    bool since_step_ = false;  // a step whose cylinder isn't ready yet
};
