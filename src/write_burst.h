// Core 0's side of a write: a burst of captured WDATA intervals in, sectors
// out, into the live track (so the host reads them back at once) and the
// disk (so they last). Pure C++, host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "cylinder_loader.h"
#include "drive.h"
#include "mfm_decode.h"
#include "track_source.h"
#include "track_write.h"

// One burst: what the host wrote while WGATE was asserted.
struct WriteBurst {
    uint8_t cylinder;
    uint8_t head;
    uint32_t start_byte;      // where on the track WGATE was asserted, in bytes from the index
    const uint8_t* intervals; // as wdata_pio captured them: ticks, less the edge ticks
    size_t count;
};

struct BurstResult {
    track_write::Result decoded;
    unsigned patched = 0;    // sectors put into the live track
    unsigned persisted = 0;  // sectors written to the disk
    bool overrun = false;    // the capture outran core 0: nothing written
};

// Core 1's side of a write: when to start and stop capturing WDATA. A
// capture runs from WGATE asserted to WGATE released (or the drive
// deselected), and only while the drive is selected, has a disk and isn't
// write-protected. Pure C++, so the firmware and the tests share it.
class WriteGate {
public:
    enum class Action : uint8_t { None, Start, Stop, Missed };

    // Call on every sample. `buffer_free`: core 0 has finished with the last
    // burst. On Start, `burst` gets the cylinder, head and track position;
    // the caller starts the capture. On Stop, the caller stops it and hands
    // the burst over. Missed: WGATE came while the buffer was busy (the
    // burst is lost; counted once per assertion).
    Action onSample(uint32_t now_us, uint32_t pins, drive::Drive& d, bool buffer_free, uint32_t data_rate_bps,
                    WriteBurst& burst);

    bool capturing() const { return capturing_; }

private:
    bool capturing_ = false;
    bool missed_ = false;
};

class BurstWriter {
public:
    // `store` holds decoded data fields: a full track's worth (9216 bytes for
    // 1.44 MB) plus room for stray fields.
    BurstWriter(TrackSource& source, CylinderLoader& loader, uint8_t* store, size_t store_len, mfm::Field* fields,
                size_t max_fields)
        : source_(source), loader_(loader), scanner_(store, store_len, fields, max_fields), store_(store) {}

    // A whole burst at once.
    BurstResult apply(const WriteBurst& burst, uint32_t ticks_per_cell, uint32_t edge_ticks);

    // The same in pieces, as the capture arrives: begin() with the burst's
    // cylinder, head and position (its intervals and count are ignored),
    // feed() the intervals in any number of chunks, then finish().
    void begin(const WriteBurst& burst, uint32_t ticks_per_cell, uint32_t edge_ticks);
    void feed(const uint8_t* intervals, size_t count);
    BurstResult finish();

private:
    TrackSource& source_;
    CylinderLoader& loader_;
    mfm::FieldScanner scanner_;
    uint8_t* store_;
    mfm::DataSeparator separator_{16};
    WriteBurst burst_{};
    uint32_t edge_ticks_ = 0;
};

// Core 0's side of a streaming capture. The WDATA intervals arrive in a ring
// (the DMA writes round it endlessly; the PIO program runs only while WGATE
// is asserted), and core 1 marks where each burst starts and ends. This
// feeds each burst to a BurstWriter as its bytes arrive, a chunk per call,
// so no burst ever has to fit in memory at once and core 0 can do other
// work (audio) between chunks. Pure C++, host-tested.
//
// Positions are free-running byte counts: the ring index is the count
// modulo the ring size. If core 0 falls a whole ring behind, the bytes it
// hasn't read are gone: that burst is abandoned and counted, never decoded
// from overwritten data.
class CaptureStream {
public:
    // A mark from core 1: a burst began (with its cylinder, head and track
    // position) or ended, at byte `at` of the stream.
    struct Mark {
        bool start;
        uint32_t at;
        WriteBurst burst;  // start only
    };

    CaptureStream(const uint8_t* ring, uint32_t ring_bytes, BurstWriter& writer)
        : ring_(ring), mask_(ring_bytes - 1), writer_(writer) {}

    // Feeds up to `max_bytes` more of the current burst, up to `produced`
    // (how far the DMA has written). Takes marks from `next_mark` as it
    // reaches them; returns true and fills `done` when a burst finishes.
    // `next_mark(Mark&)` returns false when there are no more marks yet.
    template <typename NextMark>
    bool service(uint32_t produced, size_t max_bytes, NextMark next_mark, BurstResult& done);

    bool inBurst() const { return in_burst_; }
    uint32_t overruns() const { return overruns_; }

private:
    const uint8_t* ring_;
    uint32_t mask_;
    BurstWriter& writer_;
    uint32_t consumed_ = 0;
    bool in_burst_ = false;
    bool have_end_ = false;
    bool lost_ = false;  // this burst overran: skip to its end
    uint32_t end_ = 0;
    uint32_t overruns_ = 0;
    Mark pending_{};
    bool have_pending_ = false;

public:
    uint32_t ticks_per_cell = 16;
    uint32_t edge_ticks = 2;
};

template <typename NextMark>
bool CaptureStream::service(uint32_t produced, size_t max_bytes, NextMark next_mark, BurstResult& done) {
    for (;;) {
        if (!have_pending_) have_pending_ = next_mark(pending_);
        if (!in_burst_) {
            if (!have_pending_) return false;
            if (pending_.start) {
                writer_.begin(pending_.burst, ticks_per_cell, edge_ticks);
                consumed_ = pending_.at;
                in_burst_ = true;
                have_end_ = false;
                lost_ = false;
            }
            have_pending_ = false;  // a stray end mark is dropped
            continue;
        }
        if (!have_end_ && have_pending_ && !pending_.start) {
            end_ = pending_.at;
            have_end_ = true;
            have_pending_ = false;
        }
        uint32_t limit = have_end_ ? end_ : produced;
        if (produced - consumed_ > mask_ + 1) lost_ = true;  // overwritten before we read it
        if (!lost_) {
            uint32_t n = limit - consumed_;
            if (n > max_bytes) n = uint32_t(max_bytes);
            while (n) {
                uint32_t at = consumed_ & mask_;
                uint32_t run = mask_ + 1 - at < n ? mask_ + 1 - at : n;  // up to the ring's end
                writer_.feed(ring_ + at, run);
                consumed_ += run;
                n -= run;
                max_bytes -= run;
            }
        }
        if (have_end_ && (lost_ || consumed_ == end_)) {
            in_burst_ = false;
            if (lost_) {
                // Abandoned: never finish() it, so nothing it decoded is
                // written. The next begin() resets the writer.
                ++overruns_;
                consumed_ = end_;
                done = BurstResult{};
                done.overrun = true;
                return true;
            }
            done = writer_.finish();
            return true;
        }
        return false;  // waiting for more bytes, or out of budget for this call
    }
}
