#include "track_write.h"

#include "track_source.h"

namespace track_write {

// The sector a data field belongs to, or -1: the one whose data field the
// layout puts at `sync_byte`.
static int placeByPosition(const ibm::Track& t, const uint16_t* order, uint32_t sync_byte) {
    for (uint16_t slot = 0; slot < t.sectors; ++slot) {
        uint32_t expected = t.dataFieldStart(slot) + ibm::PRESYNC_BYTES;
        uint32_t d = sync_byte > expected ? sync_byte - expected : expected - sync_byte;
        if (d <= POSITION_SLACK_BYTES) return order[slot];
    }
    return -1;
}

Result interpret(const ibm::Format& fmt, int cylinder, uint8_t head, uint32_t start_byte,
                 const mfm::FieldScanner& fields, Update* out, size_t max) {
    Result r;
    bool on_track = cylinder >= 0 && hasTrack(fmt, uint8_t(cylinder), head);
    const ibm::Track& t = fmt.track(on_track ? uint8_t(cylinder) : 0, head);
    uint16_t order[ibm::MAX_SECTORS];
    if (on_track) t.sectorMap(uint8_t(cylinder), head, order);
    const uint8_t head_id = t.head_id < 0 ? head : uint8_t(t.head_id);

    const mfm::Field* last_id = nullptr;  // the last good ID field in this burst
    for (size_t i = 0; i < fields.count(); ++i) {
        const mfm::Field& f = fields.field(i);
        if (f.mark == mfm::MARK_IDAM) {
            last_id = f.crc_ok ? &f : nullptr;
            continue;
        }
        if (!f.crc_ok) {
            ++r.bad_crc;
            last_id = nullptr;
            continue;
        }
        int sector = -1;
        if (!on_track) {
            // Nowhere on the image to put it.
        } else if (last_id) {
            // A track write: the ID before it names the sector.
            const uint8_t* id = last_id->id;
            if (id[0] == cylinder && id[1] == head_id && id[3] == t.size_code && id[2] >= t.first_id &&
                id[2] - t.first_id < t.sectors && f.data_len == t.sectorBytes())
                sector = id[2] - t.first_id;
        } else if (f.data_len == t.sectorBytes()) {
            // A sector write: where it landed names the sector.
            sector = placeByPosition(t, order, start_byte + f.cell / 16);
        }
        last_id = nullptr;
        if (sector < 0) {
            ++r.unplaced;
            continue;
        }
        if (r.updates < max) out[r.updates++] = Update{uint8_t(sector), f.data_offset};
    }
    return r;
}

}  // namespace track_write
