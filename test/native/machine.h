// The drive as the firmware runs it, for tests that drive it end to end:
// the drive model, the cylinder loader, the write path and the sound
// events, with core 1's per-sample work and core 0's background work
// played out in simulated time. Pure C++ throughout; only the PIO programs
// are left out, and they're tested in the simulator.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "activity.h"
#include "cylinder_loader.h"
#include "drive.h"
#include "gen_disk.h"
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
    const ibm::Format& format() const override { return fmt_; }
    bool readTrack(uint8_t c, uint8_t h, uint8_t* out) override {
        if (c >= 80) return false;
        std::memcpy(out, &image_[imageTrackOffset(fmt_, c, h)], fmt_.trackDataBytes());
        return true;
    }
    bool writeSector(uint8_t c, uint8_t h, uint8_t s, const uint8_t* data) override {
        if (!writable_) return false;
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

private:
    const ibm::Format& fmt_;
    std::vector<uint8_t> image_;
};

// Core 0's costs per loader step: SDIO-speed reads, and an assumed encode
// time until the board measures it (test_loader.cpp uses the same).
struct Core0Costs {
    uint32_t read_us = 3000;
    uint32_t encode_us = 2500;
};

class Machine {
    // Declared first: members are built in declaration order, and the loader
    // and writer below are built on these buffers.
    const ibm::Format& fmt_;
    Core0Costs costs_;
    std::vector<uint32_t> slots_;
    std::vector<uint16_t> scratch_;
    std::vector<uint8_t> data0_, data1_;
    std::vector<uint8_t> store_;
    std::vector<mfm::Field> fields_;

public:
    explicit Machine(const ibm::Format& fmt = ibm::HD_1440, Core0Costs costs = {}, uint8_t start_cylinder = 0,
                     bool writable = true)
        : fmt_(fmt),
          costs_(costs),
          slots_(fmt.track_bytes),
          scratch_(fmt.track_bytes),
          data0_(fmt.trackDataBytes()),
          data1_(fmt.trackDataBytes()),
          store_(12 * 1024),
          fields_(64),
          d(config(start_cylinder)),
          source(fmt),
          loader(source, slots_.data(), scratch_.data(), data0_.data(), data1_.data()),
          writer(source, loader, store_.data(), store_.size(), fields_.data(), fields_.size()) {
        source.writable_ = writable;
        // As at power-on: cylinder 0 is loaded before the host can ask.
        while (loader.service() != CylinderLoader::Step::Idle) {}
        d.insertDisk(!writable);
    }

    const ibm::Format& format() const { return fmt_; }
    uint32_t usPerByte() const { return 8000000u / fmt_.dataRate(); }

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
    }

    // Core 0's background work, up to `now`: one loader step at a time, each
    // taking its cost. The loader sees the head's cylinder as each step starts.
    void runCore0(uint32_t now) {
        while (int32_t(now - core0_us_) > 0) {
            loader.request(d.head().cylinder());
            CylinderLoader::Step s = loader.service();
            if (s == CylinderLoader::Step::Idle) {
                core0_us_ = now;
            } else {
                core0_us_ += (s == CylinderLoader::Step::ReadHead0 || s == CylinderLoader::Step::ReadHead1)
                                 ? costs_.read_us
                                 : costs_.encode_us;
            }
        }
        tracker.update(now);
        drainSounds();
    }

    // What RDATA carries for `head` at track byte `pos`: the cells, or
    // nothing (0) when the drive holds RDATA released.
    uint16_t rdataCells(uint32_t now, uint8_t head, uint32_t pos) const {
        if (!d.streams(now, loader.ready())) return 0;
        return rdata::headCells(slots_[pos % fmt_.track_bytes], head);
    }

    // The WDATA capture: intervals the host writes, as the PIO program
    // delivers them (ticks less the edge ticks, 8 bits). Kept only while a
    // capture is running, as on the board.
    void captureInterval(uint32_t ticks) {
        if (gate.capturing()) capture_.push_back(uint8_t(ticks - EDGE_TICKS));
    }

    static constexpr uint32_t TICKS_PER_CELL = 16;
    static constexpr uint32_t EDGE_TICKS = 2;

    drive::Drive d;
    MemorySource source;
    CylinderLoader loader;
    BurstWriter writer;
    WriteGate gate;
    activity::Tracker tracker;
    std::vector<BurstResult> writes;      // every burst core 0 decoded
    std::vector<activity::Event> sounds;  // every sound event, in order

private:
    static drive::Config config(uint8_t start_cylinder) {
        drive::Config c;
        c.start_cylinder = start_cylinder;
        return c;
    }

    void sample(uint32_t now, uint32_t pins, const drive::StepEvent& e) {
        activity::DriveEvent out[3];
        unsigned n = observer_.observe(now, d, e, out);
        for (unsigned i = 0; i < n; ++i) tracker.onDriveEvent(out[i]);
        drainSounds();

        switch (gate.onSample(now, pins, d, true, fmt_.dataRate(), burst_)) {
            case WriteGate::Action::Start:
                capture_.clear();
                break;
            case WriteGate::Action::Stop:
                // Core 0 decodes it straight away (it's idle during writes).
                burst_.intervals = capture_.data();
                burst_.count = capture_.size();
                writes.push_back(writer.apply(burst_, TICKS_PER_CELL, EDGE_TICKS));
                break;
            default:
                break;
        }
    }

    void drainSounds() {
        activity::Event ev;
        while (tracker.pop(ev)) sounds.push_back(ev);
    }

    VirtualHost* host_ = nullptr;
    std::vector<uint8_t> capture_;
    WriteBurst burst_{};
    activity::DriveObserver observer_;
    uint32_t core0_us_ = 0;
};
