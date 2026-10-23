#include "ibm_track.h"

#include "crc16.h"
#include "mfm.h"

namespace ibm {

uint32_t Format::usedBytes() const {
    uint32_t iam = gap4a + PRESYNC_BYTES + 4 + gap1;
    uint32_t id = PRESYNC_BYTES + 4 + 4 + 2 + gap2;
    uint32_t dam = PRESYNC_BYTES + 4 + sectorBytes() + 2 + gap3;
    return iam + sectors * (id + dam);
}

// A1 A1 A1 <mark> <field> <CRC>: the CRC covers the sync bytes too.
static void syncedField(mfm::Encoder& e, uint8_t mark, const uint8_t* field, size_t n) {
    e.fill(0x00, PRESYNC_BYTES);
    for (int i = 0; i < 3; ++i) e.sync(mfm::SYNC_A1);
    e.byte(mark);
    e.bytes(field, n);
    uint16_t crc = crc16::INITIAL;
    for (int i = 0; i < 3; ++i) crc = crc16::update(crc, 0xA1);
    crc = crc16::update(crc, mark);
    crc = crc16::compute(field, n, crc);
    e.byte(uint8_t(crc >> 8));
    e.byte(uint8_t(crc));
}

bool buildTrack(const Format& fmt, uint8_t cylinder, uint8_t head, const uint8_t* data, uint16_t* out) {
    if (fmt.usedBytes() > fmt.track_bytes) return false;
    mfm::Encoder e(out, fmt.track_bytes);

    e.fill(GAP_BYTE, fmt.gap4a);
    e.fill(0x00, PRESYNC_BYTES);
    for (int i = 0; i < 3; ++i) e.sync(mfm::SYNC_C2);
    e.byte(MARK_IAM);
    e.fill(GAP_BYTE, fmt.gap1);

    for (uint8_t s = 0; s < fmt.sectors; ++s) {
        const uint8_t id[4] = {cylinder, head, uint8_t(s + 1), fmt.size_code};
        syncedField(e, MARK_IDAM, id, sizeof id);
        e.fill(GAP_BYTE, fmt.gap2);
        syncedField(e, MARK_DAM, data + s * fmt.sectorBytes(), fmt.sectorBytes());
        e.fill(GAP_BYTE, fmt.gap3);
    }

    e.fill(GAP_BYTE, fmt.track_bytes - e.size());
    return !e.overflowed() && e.size() == fmt.track_bytes;
}

}  // namespace ibm
