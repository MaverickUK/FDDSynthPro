#include "write_burst.h"

#include "fdd_signals.h"

WriteGate::Action WriteGate::onSample(uint32_t now_us, uint32_t pins, drive::Drive& d, bool buffer_free,
                                      uint32_t data_rate_bps, WriteBurst& burst) {
    bool gate = fdd::asserted(pins, cfg::PIN_WGATE);
    if (!gate || !d.selected()) {
        missed_ = false;
        if (!capturing_) return Action::None;
        capturing_ = false;
        return Action::Stop;
    }
    if (capturing_ || missed_ || !d.diskPresent() || d.writeProtected()) return Action::None;
    if (!buffer_free) {
        missed_ = true;
        return Action::Missed;
    }
    // Where on the track the write begins: it places a sector write.
    uint32_t start_byte = uint32_t(uint64_t(d.spindle().phaseUs(now_us)) * data_rate_bps / 8000000u);
    burst = WriteBurst{d.head().cylinder(), d.side(), start_byte, burst.intervals, 0};
    capturing_ = true;
    return Action::Start;
}

BurstResult BurstWriter::apply(const WriteBurst& burst, uint32_t ticks_per_cell, uint32_t edge_ticks) {
    begin(burst, ticks_per_cell, edge_ticks);
    feed(burst.intervals, burst.count);
    return finish();
}

void BurstWriter::begin(const WriteBurst& burst, uint32_t ticks_per_cell, uint32_t edge_ticks) {
    burst_ = burst;
    edge_ticks_ = edge_ticks;
    separator_.reset(ticks_per_cell);
    // Data fields are this track's sector size, unless an ID field says otherwise.
    const ibm::Format& fmt = source_.format();
    amiga_disk_ = fmt.encoding == ibm::ENCODING_AMIGA;
    amiga_.reset();
    int cylinder = fmt.imageCylinder(burst.cylinder);
    scanner_.reset(uint16_t(fmt.track(cylinder < 0 ? 0 : uint8_t(cylinder), burst.head).sectorBytes()));
}

void BurstWriter::feed(const uint8_t* intervals, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        unsigned n = separator_.cells(uint32_t(intervals[i]) + edge_ticks_);
        if (amiga_disk_) {
            for (unsigned k = 1; k < n; ++k) amiga_.addCell(false);
            amiga_.addCell(true);
            continue;
        }
        for (unsigned k = 1; k < n; ++k) scanner_.addCell(false);
        scanner_.addCell(true);
    }
}

track_write::Result BurstWriter::interpretAmiga(const ibm::Format& fmt, int cylinder, uint8_t head,
                                                track_write::Update* out, size_t max) const {
    track_write::Result r;
    bool on_track = cylinder >= 0 && hasTrack(fmt, uint8_t(cylinder), head);
    unsigned sectors = on_track ? fmt.track(uint8_t(cylinder), head).sectors : 0;
    for (size_t i = 0; i < amiga_.count(); ++i) {
        const amiga::SectorScanner::Sector& s = amiga_.sector(i);
        if (!s.ok) {
            ++r.bad_crc;
            continue;
        }
        if (!on_track || s.track != cylinder * 2 + head || s.sector >= sectors) {
            ++r.unplaced;
            continue;
        }
        if (r.updates < max) out[r.updates++] = track_write::Update{s.sector, s.data_offset};
    }
    return r;
}

BurstResult BurstWriter::finish() {
    const ibm::Format& fmt = source_.format();
    BurstResult r;
    // The head's cylinder (the loader's), and the image's (the disk's).
    int cylinder = fmt.imageCylinder(burst_.cylinder);
    track_write::Update updates[64];
    if (amiga_disk_)
        r.decoded = interpretAmiga(fmt, cylinder, burst_.head, updates, 64);
    else
        r.decoded = track_write::interpret(fmt, cylinder, burst_.head, burst_.start_byte, scanner_, updates, 64);
    for (unsigned i = 0; i < r.decoded.updates; ++i) {
        const uint8_t* data = store_ + updates[i].data_offset;
        if (!source_.writeSector(uint8_t(cylinder), burst_.head, updates[i].sector, data)) {
            ++r.failed;
            continue;  // the live track keeps the old data: what the disk has
        }
        ++r.persisted;
        r.patched += loader_.writeSector(burst_.cylinder, burst_.head, updates[i].sector, data);
    }
    return r;
}
