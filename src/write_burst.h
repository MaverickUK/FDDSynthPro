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

    BurstResult apply(const WriteBurst& burst, uint32_t ticks_per_cell, uint32_t edge_ticks);

private:
    TrackSource& source_;
    CylinderLoader& loader_;
    mfm::FieldScanner scanner_;
    uint8_t* store_;
};
