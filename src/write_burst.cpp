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
    scanner_.reset(uint16_t(source_.format().sectorBytes()));
}

void BurstWriter::feed(const uint8_t* intervals, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        unsigned n = separator_.cells(uint32_t(intervals[i]) + edge_ticks_);
        for (unsigned k = 1; k < n; ++k) scanner_.addCell(false);
        scanner_.addCell(true);
    }
}

BurstResult BurstWriter::finish() {
    const ibm::Format& fmt = source_.format();
    BurstResult r;
    track_write::Update updates[32];
    r.decoded = track_write::interpret(fmt, burst_.cylinder, burst_.head, burst_.start_byte, scanner_, updates, 32);
    for (unsigned i = 0; i < r.decoded.updates; ++i) {
        const uint8_t* data = store_ + updates[i].data_offset;
        r.patched += loader_.writeSector(burst_.cylinder, burst_.head, updates[i].sector, data);
        r.persisted += source_.writeSector(burst_.cylinder, burst_.head, updates[i].sector, data);
    }
    return r;
}
