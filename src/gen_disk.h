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

// One track's sector data (fmt.trackDataBytes(cylinder, head) bytes), as
// ibm::buildTrack takes it: the 512-byte blocks at its place in the image,
// so a PC format's sectors are the LBAs they'd be on a real disk.
inline void track(const ibm::Format& fmt, uint8_t cylinder, uint8_t head, uint8_t* out) {
    uint32_t offset = fmt.trackOffset(cylinder, head), n = fmt.trackDataBytes(cylinder, head);
    uint8_t block[SECTOR_BYTES];
    for (uint32_t at = 0; at < n;) {
        uint32_t lba = (offset + at) / SECTOR_BYTES, skip = (offset + at) % SECTOR_BYTES;
        sector(lba, block);
        uint32_t take = SECTOR_BYTES - skip < n - at ? SECTOR_BYTES - skip : n - at;
        for (uint32_t i = 0; i < take; ++i) out[at + i] = block[skip + i];
        at += take;
    }
}

}  // namespace gen_disk
