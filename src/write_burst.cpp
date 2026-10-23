#include "write_burst.h"

BurstResult BurstWriter::apply(const WriteBurst& burst, uint32_t ticks_per_cell, uint32_t edge_ticks) {
    const ibm::Format& fmt = source_.format();
    mfm::DataSeparator separator(ticks_per_cell);
    scanner_.reset(uint16_t(fmt.sectorBytes()));
    for (size_t i = 0; i < burst.count; ++i) {
        unsigned n = separator.cells(uint32_t(burst.intervals[i]) + edge_ticks);
        for (unsigned k = 1; k < n; ++k) scanner_.addCell(false);
        scanner_.addCell(true);
    }

    BurstResult r;
    track_write::Update updates[32];
    r.decoded = track_write::interpret(fmt, burst.cylinder, burst.head, burst.start_byte, scanner_, updates, 32);
    for (unsigned i = 0; i < r.decoded.updates; ++i) {
        const uint8_t* data = store_ + updates[i].data_offset;
        r.patched += loader_.writeSector(burst.cylinder, burst.head, updates[i].sector, data);
        r.persisted += source_.writeSector(burst.cylinder, burst.head, updates[i].sector, data);
    }
    return r;
}
