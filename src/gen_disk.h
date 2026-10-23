// The slice 2 test disk: every sector's content is computed from its LBA,
// so the disk needs no storage. Pure C++. The same pattern as HDDSynthPro's
// gen_disk, with the same known answers.
//
// host/fddproto/gen_disk.py is the Python twin used by the tests; both are
// checked against the same known answers, so change them together.
//
// Layout of sector `lba` (512 bytes):
//   bytes 0-3   lba, little-endian      (a misplaced sector names itself)
//   bytes 4-7   ~lba, little-endian
//   bytes 8-511 a byte stream from a 32-bit LCG seeded from lba: neighbouring
//               sectors and byte offsets differ, so swaps and shifts show up
// LBA order is the IMG file order: cylinder, then head, then sector.
#pragma once

#include <cstdint>

#include "ibm_track.h"

namespace gen_disk {

constexpr uint32_t SECTOR_BYTES = 512;

inline void sector(uint32_t lba, uint8_t out[SECTOR_BYTES]) {
    uint32_t inv = ~lba;
    for (int i = 0; i < 4; ++i) {
        out[i] = uint8_t(lba >> (8 * i));
        out[4 + i] = uint8_t(inv >> (8 * i));
    }
    uint32_t state = lba * 2654435761u + 0x12345678u;
    for (uint32_t i = 8; i < SECTOR_BYTES; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = uint8_t(state >> 24);
    }
}

inline uint32_t firstLba(const ibm::Format& fmt, uint8_t cylinder, uint8_t head) {
    return (uint32_t(cylinder) * 2 + head) * fmt.sectors;
}

// One track's sector data (fmt.trackDataBytes() bytes), as ibm::buildTrack takes it.
inline void track(const ibm::Format& fmt, uint8_t cylinder, uint8_t head, uint8_t* out) {
    uint32_t lba = firstLba(fmt, cylinder, head);
    for (uint8_t s = 0; s < fmt.sectors; ++s) sector(lba + s, out + s * SECTOR_BYTES);
}

}  // namespace gen_disk
